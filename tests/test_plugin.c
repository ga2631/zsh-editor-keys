/*
 * Automated syntax, keymap, and live forkpty() end-to-end test suite
 * for zsh-editor-keys.plugin.zsh.
 *
 * Build & run:
 *   macOS: cc -Wall -Wextra -Werror -std=c11 -O2 -o tests/test_plugin tests/test_plugin.c && ./tests/test_plugin
 *   Linux: cc -Wall -Wextra -Werror -std=c11 -O2 -o tests/test_plugin tests/test_plugin.c -lutil && ./tests/test_plugin
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#include <util.h>
#else
#include <pty.h>
#endif

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define OUT_CAP 65536
#define SCRIPT_CAP (PATH_MAX + 512)

static char g_plugin_path[PATH_MAX];

typedef struct {
    const char *key;
    const char *val;
} EnvVar;

static void resolve_plugin_path(const char *argv0) {
    char exe_dir[PATH_MAX - 64];
    const char *slash = strrchr(argv0, '/');
    if (slash != NULL) {
        size_t len = (size_t)(slash - argv0);
        if (len >= sizeof(exe_dir)) {
            fprintf(stderr, "Path too long\n");
            exit(1);
        }
        memcpy(exe_dir, argv0, len);
        exe_dir[len] = '\0';
        snprintf(g_plugin_path, sizeof(g_plugin_path), "%s/../zsh-editor-keys.plugin.zsh", exe_dir);
    } else {
        snprintf(g_plugin_path, sizeof(g_plugin_path), "zsh-editor-keys.plugin.zsh");
    }

    if (access(g_plugin_path, R_OK) != 0) {
        snprintf(g_plugin_path, sizeof(g_plugin_path), "zsh-editor-keys.plugin.zsh");
        if (access(g_plugin_path, R_OK) != 0) {
            fprintf(stderr, "Cannot locate zsh-editor-keys.plugin.zsh\n");
            exit(1);
        }
    }
}

static int run_zsh(const char *const argv[], const EnvVar *envs, size_t n_envs,
                   char *out_buf, size_t out_cap) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        exit(1);
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        for (size_t i = 0; i < n_envs; i++) {
            if (envs[i].val == NULL) {
                unsetenv(envs[i].key);
            } else {
                setenv(envs[i].key, envs[i].val, 1);
            }
        }

        execvp("zsh", (char *const *)argv);
        perror("execvp zsh");
        _exit(127);
    }

    close(pipefd[1]);
    size_t total = 0;
    while (total + 1 < out_cap) {
        ssize_t n = read(pipefd[0], out_buf + total, out_cap - 1 - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    out_buf[total] = '\0';
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) break;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static void assert_contains(const char *test_name, const char *haystack, const char *needle) {
    if (strstr(haystack, needle) == NULL) {
        fprintf(stderr, "[FAIL] %s: expected substring '%s'\nActual output:\n%s\n",
                test_name, needle, haystack);
        exit(1);
    }
}

static void assert_not_contains(const char *test_name, const char *haystack, const char *needle) {
    if (strstr(haystack, needle) != NULL) {
        fprintf(stderr, "[FAIL] %s: did NOT expect substring '%s'\nActual output:\n%s\n",
                test_name, needle, haystack);
        exit(1);
    }
}

static void trim_trailing_ws(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

/* ---------- PTY helpers for real interactive ZLE end-to-end tests ---------- */

static void pty_read_for(int fd, int ms, char *buf, size_t *len, size_t cap) {
    struct timeval start, now;
    gettimeofday(&start, NULL);
    for (;;) {
        gettimeofday(&now, NULL);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000L + (now.tv_usec - start.tv_usec) / 1000L;
        if (elapsed >= ms || *len + 1 >= cap) break;

        long rem = ms - elapsed;
        struct timeval tv;
        tv.tv_sec = rem / 1000L;
        tv.tv_usec = (rem % 1000L) * 1000L;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        int rv = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (rv <= 0) break;

        ssize_t n = read(fd, buf + *len, cap - 1 - *len);
        if (n <= 0) break;
        *len += (size_t)n;
        buf[*len] = '\0';
    }
}

static void pty_send(int fd, const char *s) {
    size_t n = strlen(s);
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, s + off, n - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
}

static void run_pty_session(const char *const inputs[], size_t n_inputs, char *out, size_t out_cap) {
    struct winsize ws = {24, 100, 0, 0};
    int master_fd = -1;
    pid_t pid = forkpty(&master_fd, NULL, NULL, &ws);
    if (pid < 0) {
        perror("forkpty");
        exit(1);
    }

    if (pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        setenv("PS1", "PROMPT> ", 1);
        setenv("ZSH_EDITOR_KEYS_CLIPBOARD", "none", 1);
        setenv("ZSH_EDITOR_KEYS_KITTY_PROTOCOL", "never", 1);
        execlp("zsh", "zsh", "-f", "-i", (char *)NULL);
        perror("execlp zsh");
        _exit(127);
    }

    size_t len = 0;
    out[0] = '\0';
    pty_read_for(master_fd, 250, out, &len, out_cap);

    char source_cmd[PATH_MAX + 32];
    snprintf(source_cmd, sizeof(source_cmd), "source \"%s\"\n", g_plugin_path);
    pty_send(master_fd, source_cmd);
    pty_read_for(master_fd, 250, out, &len, out_cap);

    /* Reset capture buffer so assertions only inspect test output */
    len = 0;
    out[0] = '\0';

    for (size_t i = 0; i < n_inputs; i++) {
        pty_send(master_fd, inputs[i]);
        pty_read_for(master_fd, 120, out, &len, out_cap);
    }

    pty_send(master_fd, "exit\n");
    pty_read_for(master_fd, 200, out, &len, out_cap);
    close(master_fd);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) break;
    }
}

/* ---------- Unit tests ---------- */

static void test_syntax(void) {
    char out[OUT_CAP];
    const char *argv[] = {"zsh", "-n", g_plugin_path, NULL};
    int rc = run_zsh(argv, NULL, 0, out, sizeof(out));
    if (rc != 0) {
        fprintf(stderr, "[FAIL] test_syntax (rc=%d):\n%s\n", rc, out);
        exit(1);
    }
    printf("[pass] zsh -n syntax check\n");
}

static void test_main_bindings(void) {
    char script[SCRIPT_CAP];
    char out[OUT_CAP];
    snprintf(script, sizeof(script), "source \"%s\"\nbindkey -M main\n", g_plugin_path);

    const char *argv[] = {"zsh", "-f", "-c", script, NULL};
    int rc = run_zsh(argv, NULL, 0, out, sizeof(out));
    if (rc != 0) {
        fprintf(stderr, "[FAIL] test_main_bindings (rc=%d):\n%s\n", rc, out);
        exit(1);
    }

    assert_contains("main:Shift+Left", out, "\"^[[1;2D\" editor-keys::backward-char");
    assert_contains("main:Shift+Right", out, "\"^[[1;2C\" editor-keys::forward-char");
    assert_contains("main:Shift+Up", out, "\"^[[1;2A\" editor-keys::up-line");
    assert_contains("main:Shift+Down", out, "\"^[[1;2B\" editor-keys::down-line");
    assert_contains("main:Shift+Opt+Left", out, "\"^[[1;4D\" editor-keys::backward-word");
    assert_contains("main:iTerm2+Shift+Opt+Left", out, "\"^[[1;10D\" editor-keys::backward-word");
    assert_contains("main:AppMode+Shift+Left", out, "\"^[O1;2D\" editor-keys::backward-char");
    assert_contains("main:Ctrl+Shift+Left", out, "\"^[[1;6D\" editor-keys::backward-word");
    assert_contains("main:Shift+Home", out, "\"^[[1;2H\" editor-keys::beginning-of-line");
    assert_contains("main:Shift+End", out, "\"^[[1;2F\" editor-keys::end-of-line");
    assert_contains("main:Shift+Enter(xterm)", out, "\"^[[27;2;13~\" editor-keys::newline");
    assert_contains("main:Shift+Enter(csi-u)", out, "\"^[[13;2u\" editor-keys::newline");
    assert_contains("main:SelectAll", out, "\"^[a\" editor-keys::select-all");
    assert_contains("main:Undo", out, "\"^[z\" editor-keys::undo");
    assert_contains("main:Redo", out, "\"^[Z\" editor-keys::redo");
    printf("[pass] main keymap selection, Select All, Undo/Redo & Shift+Enter bindings\n");
}

static void test_viins_bindings(void) {
    char script[SCRIPT_CAP];
    char out[OUT_CAP];
    snprintf(script, sizeof(script), "source \"%s\"\nbindkey -M viins\n", g_plugin_path);

    const char *argv[] = {"zsh", "-f", "-c", script, NULL};
    int rc = run_zsh(argv, NULL, 0, out, sizeof(out));
    if (rc != 0) {
        fprintf(stderr, "[FAIL] test_viins_bindings (rc=%d):\n%s\n", rc, out);
        exit(1);
    }

    assert_contains("viins:Shift+Left", out, "\"^[[1;2D\" editor-keys::backward-char");
    assert_contains("viins:SelectAll", out, "\"^[a\" editor-keys::select-all");
    assert_contains("viins:Undo", out, "\"^[z\" editor-keys::undo");
    printf("[pass] viins (Vi Insert mode) keymap bindings\n");
}

static void test_cached_capabilities(void) {
    char script[SCRIPT_CAP];
    char out[OUT_CAP];
    snprintf(script, sizeof(script),
             "source \"%s\"\n"
             "print -r -- \"KITTY=${_zsh_editor_keys_has_kitty} CLIP=${#_zsh_editor_keys_clip_cmd}\"\n",
             g_plugin_path);

    EnvVar envs[] = {
        {"TERM", "xterm-ghostty"},
        {"ZSH_EDITOR_KEYS_CLIPBOARD", "none"},
    };
    const char *argv[] = {"zsh", "-f", "-c", script, NULL};
    run_zsh(argv, envs, 2, out, sizeof(out));
    trim_trailing_ws(out);
    if (strcmp(out, "KITTY=1 CLIP=0") != 0) {
        fprintf(stderr, "[FAIL] cached capabilities expected 'KITTY=1 CLIP=0', got '%s'\n", out);
        exit(1);
    }
    printf("[pass] load-time cached Kitty & clipboard capabilities\n");
}

static void test_shift_select_keymap(void) {
    char script[SCRIPT_CAP];
    char out[OUT_CAP];
    snprintf(script, sizeof(script), "source \"%s\"\nbindkey -M shift-select\n", g_plugin_path);

    const char *argv[] = {"zsh", "-f", "-c", script, NULL};
    int rc = run_zsh(argv, NULL, 0, out, sizeof(out));
    if (rc != 0) {
        fprintf(stderr, "[FAIL] test_shift_select_keymap (rc=%d):\n%s\n", rc, out);
        exit(1);
    }

    assert_contains("keymap:Backspace", out, "\"^?\" editor-keys::kill-region");
    assert_contains("keymap:Opt+Backspace", out, "\"^[^?\" editor-keys::kill-region");
    assert_contains("keymap:Ctrl+W", out, "\"^W\" editor-keys::kill-region");
    assert_contains("keymap:Delete", out, "\"^[[3~\" editor-keys::kill-region");
    assert_contains("keymap:Cmd+C", out, "\"^[[99;9u\" editor-keys::copy-region");
    assert_contains("keymap:Ctrl+Shift+C", out, "\"^[[99;6u\" editor-keys::copy-region");
    assert_contains("keymap:Cmd+X", out, "\"^[[120;9u\" editor-keys::cut-region");
    assert_contains("keymap:Ctrl+Shift+X", out, "\"^[[120;6u\" editor-keys::cut-region");
    assert_contains("keymap:PasteReplace", out, "\"^[[200~\" editor-keys::paste-replace");
    assert_contains("keymap:EscCSIu", out, "\"^[[27u\" editor-keys::deselect");
    assert_contains("keymap:PlainLeft", out, "\"^[[D\" editor-keys::deselect-and-input");
    assert_contains("keymap:AltLeft", out, "\"^[[1;3D\" editor-keys::deselect-and-input");
    assert_contains("keymap:TmuxHome", out, "\"^[[1~\" editor-keys::deselect-and-input");
    printf("[pass] shift-select keymap (kill, Opt+Backspace, replace, copy, cut, paste, deselect)\n");
}

static void test_themes(void) {
    const struct {
        const char *name;
        const char *expected;
    } cases[] = {
        {"github-dark", "region:bg=#264f78"},
        {"catppuccin",  "region:bg=#45475a"},
        {"tokyonight",  "region:bg=#283457"},
        {"default",     ""},
    };

    char script[SCRIPT_CAP];
    char out[OUT_CAP];
    snprintf(script, sizeof(script),
             "source \"%s\"\nprint -r -- \"${zle_highlight[*]}\"\n",
             g_plugin_path);

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        EnvVar envs[] = {{"ZSH_EDITOR_KEYS_THEME", cases[i].name}};
        const char *argv[] = {"zsh", "-f", "-c", script, NULL};
        run_zsh(argv, envs, 1, out, sizeof(out));
        if (cases[i].expected[0] != '\0') {
            assert_contains(cases[i].name, out, cases[i].expected);
        } else {
            trim_trailing_ws(out);
            if (strstr(out, "region:bg=#") != NULL) {
                fprintf(stderr, "[FAIL] theme 'default' should not set hex bg, got '%s'\n", out);
                exit(1);
            }
        }
    }
    printf("[pass] selection highlight themes (github-dark, catppuccin, tokyonight, default)\n");
}

static void test_kitty_protocol_detection(void) {
    char script[SCRIPT_CAP];
    char out[OUT_CAP];
    snprintf(script, sizeof(script),
             "source \"%s\"\n_zsh_editor_keys_supports_kitty && echo YES || echo NO\n",
             g_plugin_path);
    const char *argv[] = {"zsh", "-f", "-c", script, NULL};

    EnvVar env_ghostty[] = {
        {"TERM", "xterm-ghostty"}, {"TERM_PROGRAM", ""}, {"GHOSTTY_RESOURCES_DIR", ""},
        {"KITTY_WINDOW_ID", ""}, {"WT_SESSION", ""}, {"TMUX", ""},
        {"ZSH_EDITOR_KEYS_KITTY_PROTOCOL", "auto"},
    };
    run_zsh(argv, env_ghostty, 7, out, sizeof(out));
    trim_trailing_ws(out);
    if (strcmp(out, "YES") != 0) {
        fprintf(stderr, "[FAIL] ghostty kitty detection expected YES, got '%s'\n", out);
        exit(1);
    }

    EnvVar env_vscode[] = {
        {"TERM", "xterm-256color"}, {"TERM_PROGRAM", "vscode"}, {"GHOSTTY_RESOURCES_DIR", ""},
        {"KITTY_WINDOW_ID", ""}, {"WT_SESSION", ""}, {"TMUX", ""},
        {"ZSH_EDITOR_KEYS_KITTY_PROTOCOL", "auto"},
    };
    run_zsh(argv, env_vscode, 7, out, sizeof(out));
    trim_trailing_ws(out);
    if (strcmp(out, "YES") != 0) {
        fprintf(stderr, "[FAIL] vscode kitty detection expected YES, got '%s'\n", out);
        exit(1);
    }

    EnvVar env_tmux[] = {
        {"TERM", "xterm-ghostty"}, {"TERM_PROGRAM", "ghostty"}, {"GHOSTTY_RESOURCES_DIR", "/app"},
        {"KITTY_WINDOW_ID", ""}, {"WT_SESSION", ""}, {"TMUX", "/tmp/tmux-501/default,123,0"},
        {"ZSH_EDITOR_KEYS_KITTY_PROTOCOL", "auto"},
    };
    run_zsh(argv, env_tmux, 7, out, sizeof(out));
    trim_trailing_ws(out);
    if (strcmp(out, "NO") != 0) {
        fprintf(stderr, "[FAIL] tmux auto guard expected NO, got '%s'\n", out);
        exit(1);
    }

    EnvVar env_dumb[] = {
        {"TERM", "dumb"}, {"TERM_PROGRAM", ""}, {"GHOSTTY_RESOURCES_DIR", ""},
        {"KITTY_WINDOW_ID", ""}, {"WT_SESSION", ""}, {"TMUX", ""},
        {"ZSH_EDITOR_KEYS_KITTY_PROTOCOL", "auto"},
    };
    run_zsh(argv, env_dumb, 7, out, sizeof(out));
    trim_trailing_ws(out);
    if (strcmp(out, "NO") != 0) {
        fprintf(stderr, "[FAIL] dumb terminal kitty detection expected NO, got '%s'\n", out);
        exit(1);
    }

    EnvVar env_always[] = {
        {"TERM", "dumb"}, {"TMUX", "/tmp/tmux-501/default,123,0"},
        {"ZSH_EDITOR_KEYS_KITTY_PROTOCOL", "always"},
    };
    run_zsh(argv, env_always, 3, out, sizeof(out));
    trim_trailing_ws(out);
    if (strcmp(out, "YES") != 0) {
        fprintf(stderr, "[FAIL] kitty=always expected YES, got '%s'\n", out);
        exit(1);
    }

    printf("[pass] Kitty keyboard protocol detection (Ghostty / VS Code / tmux guard / dumb / always)\n");
}

static void test_shift_enter_opt_out(void) {
    char script[SCRIPT_CAP];
    char out[OUT_CAP];
    snprintf(script, sizeof(script),
             "source \"%s\"\nbindkey -M main | grep newline || true\n",
             g_plugin_path);

    EnvVar envs[] = {{"ZSH_EDITOR_KEYS_SHIFT_ENTER", "0"}};
    const char *argv[] = {"zsh", "-f", "-c", script, NULL};
    run_zsh(argv, envs, 1, out, sizeof(out));
    trim_trailing_ws(out);
    if (out[0] != '\0') {
        fprintf(stderr, "[FAIL] SHIFT_ENTER=0 should not bind newline, got '%s'\n", out);
        exit(1);
    }
    printf("[pass] ZSH_EDITOR_KEYS_SHIFT_ENTER=0 opt-out\n");
}

/* ---------- Real forkpty() end-to-end behavioral tests ---------- */

static void test_pty_select_delete_and_replace(void) {
    char out[OUT_CAP];
    /* 1. Type "echo RES_1_OLD", select "_OLD" (4x Shift+Left), press Backspace -> "RES_1"
     * 2. Type "echo path/old", select subword "old" (Shift+Opt+Left), type "new" -> "path/new" */
    const char *inputs[] = {
        "echo RES_1_OLD",
        "\x1b[1;2D\x1b[1;2D\x1b[1;2D\x1b[1;2D",
        "\x7f",
        "\n",
        "echo path/old",
        "\x1b[1;4D",
        "new",
        "\n",
    };
    run_pty_session(inputs, sizeof(inputs) / sizeof(inputs[0]), out, sizeof(out));
    assert_contains("pty:select+delete", out, "\nRES_1\r\n");
    assert_contains("pty:subword+replace", out, "\npath/new\r\n");
    printf("[pass] PTY e2e: Shift+Left delete & Shift+Opt+Left subword replace\n");
}

static void test_pty_select_all_undo_and_shift_enter(void) {
    char out[OUT_CAP];
    /* 1. Type "echo RESTORED_OK", Alt+A (select all), Opt+Backspace (delete), Alt+Z (undo), Enter
     * 2. Type multiline command with Shift+Enter -> prints LINE_A then LINE_B without ;2;13~ */
    const char *inputs[] = {
        "echo RESTORED_OK",
        "\033a",
        "\033\x7f",
        "\033z",
        "\n",
        "echo LINE_A &&",
        "\033[27;2;13~",
        "echo LINE_B",
        "\n",
    };
    run_pty_session(inputs, sizeof(inputs) / sizeof(inputs[0]), out, sizeof(out));
    assert_contains("pty:select-all+undo", out, "\nRESTORED_OK\r\n");
    assert_contains("pty:shift-enter-lineA", out, "\nLINE_A\r\n");
    assert_contains("pty:shift-enter-lineB", out, "\nLINE_B\r\n");
    assert_not_contains("pty:no-shift-enter-leak", out, ";2;13~");
    printf("[pass] PTY e2e: Alt+A Select All, Opt+Backspace, Alt+Z Undo & Shift+Enter newline\n");
}

int main(int argc, char **argv) {
    (void)argc;
    resolve_plugin_path(argv[0]);

    test_syntax();
    test_main_bindings();
    test_viins_bindings();
    test_cached_capabilities();
    test_shift_select_keymap();
    test_themes();
    test_kitty_protocol_detection();
    test_shift_enter_opt_out();
    test_pty_select_delete_and_replace();
    test_pty_select_all_undo_and_shift_enter();

    printf("\nAll 10 tests passed!\n");
    return 0;
}
