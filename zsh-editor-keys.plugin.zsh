# zsh-editor-keys.plugin.zsh
# Bring modern GUI / VS Code text-editing keybindings to Zsh (ZLE):
#   - Shift + Arrows (char, smart subword, line, multi-line & buffer selection)
#   - Backspace / Delete / typing replaces highlighted text
#   - Cross-platform OS clipboard Cmd+C / Cmd+X / Cmd+V (macOS, Wayland, X11, WSL)
#   - Scoped Kitty keyboard protocol (Mode 1) so Cmd+C works alongside mouse selection
#   - Select All (Alt+A / Ctrl+Shift+A), Undo/Redo (Alt+Z / Alt+Shift+Z), Shift+Enter newline
#   - Zero normal-typing overhead, compatible with emacs & viins keymaps
#
# Repository: https://github.com/ga2631/zsh-editor-keys
# License: MIT

() {
  # Defaults (override in ~/.zshrc before loading this plugin)
  : "${ZSH_EDITOR_KEYS_THEME:=github-dark}"       # github-dark | catppuccin | tokyonight | default
  : "${ZSH_EDITOR_KEYS_CLIPBOARD:=auto}"          # auto | none
  : "${ZSH_EDITOR_KEYS_KITTY_PROTOCOL:=auto}"     # auto | always | never
  : "${ZSH_EDITOR_KEYS_WORD_STYLE:=subword}"      # subword | default
  : "${ZSH_EDITOR_KEYS_SHIFT_ENTER:=1}"           # 1 | 0

  # 1. Selection highlight theme
  case "$ZSH_EDITOR_KEYS_THEME" in
    github-dark) zle_highlight+=(region:bg=#264f78) ;;
    catppuccin)  zle_highlight+=(region:bg=#45475a) ;;
    tokyonight)  zle_highlight+=(region:bg=#283457) ;;
    default)     ;;
  esac

  # 2. Compute terminal & clipboard capabilities ONCE at load time (zero per-keypress overhead)
  typeset -gi _zsh_editor_keys_kitty_pushed=0
  typeset -gi _zsh_editor_keys_has_kitty=0

  case "$ZSH_EDITOR_KEYS_KITTY_PROTOCOL" in
    always)
      _zsh_editor_keys_has_kitty=1
      ;;
    never)
      _zsh_editor_keys_has_kitty=0
      ;;
    *)
      if [[ -z "${TMUX:-}" ]]; then
        case "${TERM:-}" in
          *ghostty*|*kitty*|*wezterm*|*alacritty*|*foot*|*rio*|*contour*) _zsh_editor_keys_has_kitty=1 ;;
        esac
        case "${TERM_PROGRAM:-}" in
          ghostty|WezTerm|kitty|iTerm.app|Alacritty|Rio|vscode) _zsh_editor_keys_has_kitty=1 ;;
        esac
        if [[ -n "${GHOSTTY_RESOURCES_DIR:-}" || -n "${KITTY_WINDOW_ID:-}" || -n "${WT_SESSION:-}" ]]; then
          _zsh_editor_keys_has_kitty=1
        fi
      fi
      ;;
  esac

  typeset -ga _zsh_editor_keys_clip_cmd=()
  if [[ "$ZSH_EDITOR_KEYS_CLIPBOARD" != "none" ]]; then
    if command -v pbcopy >/dev/null 2>&1; then
      _zsh_editor_keys_clip_cmd=(pbcopy)
    elif [[ -n "${WAYLAND_DISPLAY:-}" ]] && command -v wl-copy >/dev/null 2>&1; then
      _zsh_editor_keys_clip_cmd=(wl-copy)
    elif command -v xclip >/dev/null 2>&1; then
      _zsh_editor_keys_clip_cmd=(xclip -selection clipboard)
    elif command -v xsel >/dev/null 2>&1; then
      _zsh_editor_keys_clip_cmd=(xsel --clipboard --input)
    elif command -v clip.exe >/dev/null 2>&1; then
      _zsh_editor_keys_clip_cmd=(clip.exe)
    fi
  fi

  function _zsh_editor_keys_supports_kitty() {
    (( _zsh_editor_keys_has_kitty ))
  }

  function _zsh_editor_keys_kitty_push() {
    if (( _zsh_editor_keys_has_kitty && ! _zsh_editor_keys_kitty_pushed )); then
      printf '\e[>1u' > /dev/tty 2>/dev/null
      _zsh_editor_keys_kitty_pushed=1
    fi
  }

  function _zsh_editor_keys_kitty_pop() {
    if (( _zsh_editor_keys_kitty_pushed )); then
      printf '\e[<u' > /dev/tty 2>/dev/null
      _zsh_editor_keys_kitty_pushed=0
    fi
  }

  function _zsh_editor_keys_pbcopy() {
    (( ${#_zsh_editor_keys_clip_cmd} )) && printf '%s' "$1" | "${_zsh_editor_keys_clip_cmd[@]}"
  }

  # Always restore terminal keyboard mode on prompt / command execution
  precmd_functions+=(_zsh_editor_keys_kitty_pop)
  preexec_functions+=(_zsh_editor_keys_kitty_pop)

  # 3. Core ZLE selection & editing widgets
  function editor-keys::select() {
    if (( ! REGION_ACTIVE )); then
      zle set-mark-command
      zle -K shift-select
      _zsh_editor_keys_kitty_push
    fi
    local target="${WIDGET#editor-keys::}"
    if [[ "$ZSH_EDITOR_KEYS_WORD_STYLE" == "subword" && "$target" == (backward|forward)-word ]]; then
      local WORDCHARS=''
      zle "$target"
    else
      zle "$target"
    fi
  }

  function editor-keys::select-all() {
    MARK=0
    CURSOR=${#BUFFER}
    REGION_ACTIVE=1
    zle -K shift-select
    _zsh_editor_keys_kitty_push
  }

  function editor-keys::deselect() {
    REGION_ACTIVE=0
    _zsh_editor_keys_kitty_pop
    zle -K main
  }

  function editor-keys::deselect-and-input() {
    REGION_ACTIVE=0
    _zsh_editor_keys_kitty_pop
    zle -K main
    zle -U -- "$KEYS"
  }

  function editor-keys::kill-region() {
    zle kill-region -w
    _zsh_editor_keys_kitty_pop
    zle -K main
  }

  function editor-keys::replace-region() {
    zle kill-region -w
    _zsh_editor_keys_kitty_pop
    zle -K main
    zle self-insert
  }

  function editor-keys::copy-region() {
    if (( REGION_ACTIVE )); then
      local start=$(( MARK < CURSOR ? MARK : CURSOR ))
      local end=$(( MARK > CURSOR ? MARK : CURSOR ))
      local selected="${BUFFER:$start:$((end - start))}"
      CUTBUFFER="$selected"
      _zsh_editor_keys_pbcopy "$selected"
    fi
    REGION_ACTIVE=0
    _zsh_editor_keys_kitty_pop
    zle -K main
  }

  function editor-keys::cut-region() {
    if (( REGION_ACTIVE )); then
      local start=$(( MARK < CURSOR ? MARK : CURSOR ))
      local end=$(( MARK > CURSOR ? MARK : CURSOR ))
      local selected="${BUFFER:$start:$((end - start))}"
      _zsh_editor_keys_pbcopy "$selected"
      zle kill-region -w
    fi
    _zsh_editor_keys_kitty_pop
    zle -K main
  }

  function editor-keys::paste-replace() {
    zle kill-region -w
    _zsh_editor_keys_kitty_pop
    zle -K main
    zle bracketed-paste
  }

  function editor-keys::undo() {
    if (( REGION_ACTIVE )); then
      REGION_ACTIVE=0
      _zsh_editor_keys_kitty_pop
      zle -K main
    fi
    zle undo
  }

  function editor-keys::redo() {
    if (( REGION_ACTIVE )); then
      REGION_ACTIVE=0
      _zsh_editor_keys_kitty_pop
      zle -K main
    fi
    zle redo
  }

  function editor-keys::newline() {
    if (( REGION_ACTIVE )); then
      zle kill-region -w
      _zsh_editor_keys_kitty_pop
      zle -K main
    fi
    LBUFFER+=$'\n'
  }

  # 4. Dedicated non-intrusive `shift-select` keymap
  bindkey -N shift-select
  bindkey -M shift-select -R '^@'-'^?' editor-keys::deselect-and-input
  bindkey -M shift-select -R ' '-'~' editor-keys::replace-region
  bindkey -M shift-select -R '\M-^@'-'\M-^?' editor-keys::replace-region

  local w
  for w in select-all deselect deselect-and-input kill-region replace-region \
           copy-region cut-region paste-replace undo redo newline; do
    zle -N "editor-keys::$w"
  done

  # Delete / Backspace / Esc / Copy / Cut / Paste while highlighted
  bindkey -M shift-select '^?' editor-keys::kill-region          # Backspace
  bindkey -M shift-select '^H' editor-keys::kill-region          # Ctrl+H / Backspace
  bindkey -M shift-select '^[^?' editor-keys::kill-region        # Option+Backspace
  bindkey -M shift-select '^W' editor-keys::kill-region          # Ctrl+W
  bindkey -M shift-select '^[[3~' editor-keys::kill-region       # Delete (Fn+Backspace)
  bindkey -M shift-select '^[' editor-keys::deselect             # Esc (legacy)
  bindkey -M shift-select '^[[27u' editor-keys::deselect         # Esc (Kitty CSI-u)
  bindkey -M shift-select '^[[99;9u' editor-keys::copy-region    # Cmd + C (macOS)
  bindkey -M shift-select '^[[99;6u' editor-keys::copy-region    # Ctrl + Shift + C (Linux)
  bindkey -M shift-select '^[c' editor-keys::copy-region         # Alt + C (fallback)
  bindkey -M shift-select '^[[120;9u' editor-keys::cut-region    # Cmd + X (macOS)
  bindkey -M shift-select '^[[120;6u' editor-keys::cut-region    # Ctrl + Shift + X (Linux)
  bindkey -M shift-select '^[x' editor-keys::cut-region          # Alt + X (fallback)
  bindkey -M shift-select '^[[200~' editor-keys::paste-replace   # Cmd + V / Bracketed Paste

  # Plain navigation keys deselect cleanly and move cursor normally
  local k
  for k in '^[[D' '^[[C' '^[[A' '^[[B' '^[OD' '^[OC' '^[OA' '^[OB' \
           '^[[H' '^[[F' '^[OH' '^[OF' '^[[1~' '^[[4~' \
           '^[b' '^[f' '^[[1;3D' '^[[1;3C' '^[[1;5D' '^[[1;5C' '^[[1;9D' '^[[1;9C'; do
    bindkey -M shift-select "$k" editor-keys::deselect-and-input
  done

  # Keymaps to register editor shortcuts on (supports both Emacs and Vi Insert mode)
  local -a target_keymaps=(main emacs viins shift-select)
  local km

  # Select All & Undo/Redo
  for km in "${target_keymaps[@]}"; do
    bindkey -M "$km" '^[a' editor-keys::select-all
    bindkey -M "$km" '^[[97;6u' editor-keys::select-all
    bindkey -M "$km" '^[z' editor-keys::undo
    bindkey -M "$km" '^[[122;9u' editor-keys::undo
    bindkey -M "$km" '^[Z' editor-keys::redo
    bindkey -M "$km" '^[[122;10u' editor-keys::redo
  done

  # Shift + Enter literal newline (works in Zsh without breaking Neovim/TUIs)
  if (( ZSH_EDITOR_KEYS_SHIFT_ENTER )); then
    local nl_seq
    for nl_seq in '^[[27;2;13~' '^[[13;2u'; do
      for km in "${target_keymaps[@]}"; do
        bindkey -M "$km" "$nl_seq" editor-keys::newline
      done
    done
  fi

  # Shifted navigation bindings (macOS + Linux/PC + iTerm2 + tmux + multi-line buffer)
  local -A bindings=(
    '^[[1;2D' backward-char                   # Shift + Left
    '^[[1;2C' forward-char                    # Shift + Right
    '^[[1;2A' up-line                         # Shift + Up (multi-line selection)
    '^[[1;2B' down-line                       # Shift + Down (multi-line selection)
    '^[O1;2D' backward-char                   # Shift + Left (app-cursor mode)
    '^[O1;2C' forward-char                    # Shift + Right (app-cursor mode)
    '^[O1;2A' up-line                         # Shift + Up (app-cursor mode)
    '^[O1;2B' down-line                       # Shift + Down (app-cursor mode)
    '^[[1;4D' backward-word                   # Shift + Option + Left (macOS)
    '^[[1;4C' forward-word                    # Shift + Option + Right (macOS)
    '^[[1;10D' backward-word                  # Shift + Option + Left (iTerm2/Meta)
    '^[[1;10C' forward-word                   # Shift + Option + Right (iTerm2/Meta)
    '^[[1;6D' backward-word                   # Ctrl + Shift + Left (Linux/PC)
    '^[[1;6C' forward-word                    # Ctrl + Shift + Right (Linux/PC)
    '^[[1;2H' beginning-of-line               # Shift + Home / Shift + Cmd + Left
    '^[[1;2F' end-of-line                     # Shift + End  / Shift + Cmd + Right
    '^[O1;2H' beginning-of-line               # Shift + Home (app-cursor mode)
    '^[O1;2F' end-of-line                     # Shift + End  (app-cursor mode)
    '^[[1;6H' beginning-of-buffer-or-history  # Ctrl + Shift + Home
    '^[[1;6F' end-of-buffer-or-history        # Ctrl + Shift + End
  )

  local seq widget
  for seq widget in "${(@kv)bindings}"; do
    zle -N "editor-keys::$widget" editor-keys::select
    for km in "${target_keymaps[@]}"; do
      bindkey -M "$km" "$seq" "editor-keys::$widget"
    done
  done

  # Clear zsh-autosuggestions cleanly while editing/selecting
  typeset -ga ZSH_AUTOSUGGEST_CLEAR_WIDGETS
  ZSH_AUTOSUGGEST_CLEAR_WIDGETS+=(
    editor-keys::backward-char
    editor-keys::forward-char
    editor-keys::up-line
    editor-keys::down-line
    editor-keys::backward-word
    editor-keys::forward-word
    editor-keys::beginning-of-line
    editor-keys::end-of-line
    editor-keys::beginning-of-buffer-or-history
    editor-keys::end-of-buffer-or-history
    editor-keys::select-all
    editor-keys::kill-region
    editor-keys::replace-region
    editor-keys::copy-region
    editor-keys::cut-region
    editor-keys::paste-replace
    editor-keys::undo
    editor-keys::redo
    editor-keys::newline
  )
}
