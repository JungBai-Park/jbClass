## ConBox

### 1. Purpose

- `ConBox` is a portable, single-class, `CWnd`-derived terminal control for MFC.
- `ConBox` supports two operating modes in the same class:
  - Pure terminal view: output through `print()` and input routing through `set_input_sink()`. If no input sink is installed, it can be used as a diagnostic log display.
  - ConPTY child runner: `start(cmdline)` creates the required pipes and pseudo console, then runs a console child process.
- Target programs are interactive stdio shells such as PowerShell and line-oriented stdio programs that emit a limited VT100 stream.
- Full-screen TUI programs are supported only on a best-effort basis.

### 2. Window and Grid Control

- `open(parent, left, top)` registers and creates the window using the `"ConBox"` class name.
- The initial pixel size is calculated from the configured row and column counts.
- Runtime resizing does not perform automatic line reflow.
- When resized, existing rows are padded or truncated to fit the new width.
- `ConBox` is per-monitor DPI aware: fonts/cell metrics use the window's own DPI and rebuild when it changes. A DPI change must rescale only the cell pixels and KEEP the grid (rows/cols) fixed, so the child pseudo-console is not resized (a resize would make the shell re-emit its screen and corrupt scrollback). Font sizes are stored as DPI-independent specs so they can be rebuilt at the new DPI; overlay scrollbar pixel metrics scale with DPI.
- Margins are 96 DPI LOGICAL padding used ONLY to compute the initial window size and to derive rows/cols on resize (scaled to physical via the window DPI); `adjust` padding stays in raw physical pixels. The grid is NOT drawn at the margin offset: it is centered in the client area, and when it overflows it is drawn from the top-left and clipped at the bottom/right.
- Because cell pixels do not scale by the exact DPI ratio (integer font rounding), a preserved grid may not fit the linearly-scaled window after a DPI change. The `snap_mode` ini option controls the response: 0 = centering only (clip if too small); 1 = grow the window only when the grid would be clipped; 2 (default) = always snap the window to the exact grid+margin size. Snapping keeps the upper-left corner fixed and moves the right/bottom edges, and never resizes the pseudo-console.
- **Zoom via `WM_JBZOOM`**: On receipt, ConBox sets `zoom_pm` and `zoom_resize = true`. The next `OnSize` (triggered by FrameBox `rescale_children` → `MoveWindow`) detects `zoom_resize` and takes the `relayout_for_dpi()` + `snap_to_grid()` path — identical to a real DPI change — preserving the logical grid without calling `update_metrics` or `resize_sink` (no PTY resize). `build_font()` applies `zoom_pm` through `eff_dpi()` so fonts scale with zoom exactly as they do with DPI changes.
- `start_x`/`start_y` INI keys (host-consumed, read via `config_start_x()`/`config_start_y()`): the
  host's top-level window start position. ConBox does not position any window itself -- it only parses
  and stores the values so the host can pass them to its own `CreateWindow` call. Absent/empty resolves
  to `CW_USEDEFAULT`, same as the previous hardcoded default.

### 3. Font and Cell Metrics

- English font configuration is handled by `set_efont`.
- Korean font configuration is handled by `set_kfont`.
- If the Korean font size value is `<= 0`, Korean font height matching is enabled by default. In this mode, the Korean font's physical pixel height is forced to match the English font height so line heights remain consistent.
- Cell width calculation:
  - Measure English width as `w_e`.
  - Measure Korean width as `w_k`.
  - Apply the configured ratio option.
  - Calculate `cell_w` as `(max(2 * w_e, w_k) + 1) / 2` to round up and avoid glyph clipping.
- `adjust(left, top, right, bottom)` keeps the font size unchanged, adjusts per-side cell margins, and compensates the internal glyph drawing position so block and box-drawing characters can render without gaps.
- Symbol fallback: `TextOutW` does not do font fallback, so a glyph missing from `efont`/`kfont` (e.g. Miscellaneous Technical symbols) would render as a hollow box. Every glyph-drawing site (screen paint, EMF export, PDF export) checks glyph existence and substitutes a configurable fallback font (`fallback_font_name` INI key, default `"Segoe UI Symbol"`) when needed.

### 4. Rendering and Viewport

- The cell grid uses a fully fixed-width unit.
- CJK characters, including Korean characters, occupy two cell columns: lead and trail.
- Instead of the default `WS_VSCROLL`, which consumes client pixels, `ConBox` renders a custom rounded overlay scrollbar at the edge of the view.
- The overlay scrollbar is wt.exe-like: while scrollable it is ALWAYS visible (except in the thumbless mouse-reporting mode, see section 12, where it shows on hover only), but only as a slim bar at rest (a thin translucent bar hugging the right edge, no gutter/arrows). On hover, drag, or a scroll it EXPANDS to the full gutter + rounded thumb + arrow buttons for a hold window, then collapses again. (TableBox's overlay still uses the older auto-show-then-fade-to-nothing behavior; this slim/expand idle state is ConBox-specific.)
- The expand/collapse is a single-shape MORPH, not a cross-fade of two shapes: the bar itself interpolates width / x-position / opacity between the slim bar and the full thumb, while the gutter strip and arrow buttons fade in/out. Hit-testing always uses the full gutter width even in the slim state, so the thin bar stays easy to grab.
- The overlay scrollbar's rounded thumb and arrow buttons are antialiased, and its gutter strip is filled with a background-contrasting color. ConBox has only the vertical axis.
- The mouse cursor remains `IDC_ARROW`, matching Windows Terminal behavior.
- The text cursor (block/underline/I-beam) is hidden when the window does not have keyboard focus (`WM_KILLFOCUS`). It reappears immediately on `WM_SETFOCUS` with the blink timer restarted.

### 5. Output and VT Escape Parsing

- C0 control support includes `\r`, `\n`, `\b`, and `\t`.
- Tabs advance to 8-column tab stops.
- CSI support includes cursor control such as CUU, CUD, and CUP.
- CSI support includes erase operations such as ED, EL, and ECH.
- CSI support includes scroll region control.
- SGR attributes include Bold, Dim, Italic, Underline, Strikethrough, Blink, and Reverse. Dim (SGR 2) blends the foreground color halfway toward the background color at write time (not toward black), so contrast is reduced consistently on light and dark backgrounds alike; SGR 22 clears both Bold and Dim, per the "normal intensity" standard.
- Blink uses a 500 ms cycle.
- SGR 8 enables double-size rendering, and SGR 28 disables it.
- Double-size rendering draws text on the logical grid at a 2x horizontal and vertical scale, overlapping upward and rightward. This uses the standard SGR 8 code because conhost does not discard it in the middle of the stream.
- 256-color and True Color output are supported by default.
- Two-byte ESC support includes ESC 7/8 (DECSC/DECRC), RI (`M`), and RIS (`c`).
- Private sequence extensions that are likely to cause incorrect behavior are excluded.
- OSC 0/2 (set window title) is parsed; the decoded UTF-8 title text is forwarded to the host via `set_title_cb`. OSC 52 (clipboard set/query) is parsed; see section 12. Other OSC codes (icon name, color queries, hyperlinks, etc.) are dropped.
- Terminal queries are answered: DSR (`ESC[6n` -> cursor position report, `ESC[5n` -> status OK), Primary DA (`ESC[c` -> `ESC[?62;22c`, VT220 class + ANSI color -- lists only genuinely supported extensions, e.g. no sixel/ReGIS/DRCS/selective-erase), and XTVERSION (`ESC[>0q` -> a DCS reply naming ConBox and its version). The XTVERSION reply deliberately never impersonates a known terminal (xterm, etc.): doing so would make capability-sniffing TUIs assume features ConBox lacks (sixel, kitty keyboard protocol) and then draw garbage.

### 6. Input Mapping and IME Commit Handling

- Standard virtual keys and character combinations are translated into the corresponding VT escape sequence strings encoded as UTF-8 and sent to the input sink.
- If a special key event, such as an arrow key or Enter, arrives while IME composition is active, `ImmNotifyIME` must commit the IME text first.
- After IME commit, the special key sequence is sent to the child process.
- The child process must always receive data in this order: committed Korean text first, then the special key code.
- Horizontal arrow correction:
  - After IME commit, the child process automatically moves the cursor one cell to the right.
  - To keep mouse and manual cursor tracking aligned, the input module may swallow the right arrow or send the left-move sequence twice for a left-arrow input.
- Enter variants are distinguished by the byte sent, not by a VT modifier sequence: plain Enter and Shift+Enter both arrive as `WM_CHAR` with `ch == '\r'` (Windows does not vary Enter's translated char with Shift), so Shift is detected by querying `GetKeyState(VK_SHIFT)` inside the `'\r'` branch and sending LF instead of CR when held. Ctrl+Enter and Ctrl+J both arrive as `ch == '\n'` already (Windows' own key translation, not app logic) and are sent as raw LF. Net effect: Enter -> CR (submit); Shift+Enter / Ctrl+Enter / Ctrl+J -> LF (newline, no submit) for child programs that distinguish the two.

### 7. Clipboard and Drag-and-Drop

- Drag selection stores the selected region in the clipboard as Unicode plain text.
- Double click selects a word.
- Alt-drag selects a rectangular block.
- On `WM_DROPFILES`, the full path of each dropped file is typed into the child process through standard input.
- Paths containing spaces are wrapped in double quotes.

### 8. Export and Logging

- `save_emf(dir)` creates a series of page-based EMF vector files in the target directory, using `cfg_lines_per_paper` as the page line count.
- EMF export maps colors through `remap_paper_color` so output is converted to print-oriented paper colors.
- `save_pdf(path)` searches system devices for a PDF conversion driver and exports the grid as a high-quality PDF using paper-oriented inverse color mapping.
- `get_text_lines()` returns the scrollback and current output as a list of UTF-8 text lines after trimming horizontal trailing spaces.
- `save_log(file_name)` writes the raw stream bytes received from the child process to a log file without CRLF conversion, encoding conversion, or other transformation.
- When a logging session restarts, four newline characters and a unique date timestamp delimiter are inserted.

### 9. Child Process Lifecycle and Host-Close Cooperation

- `stop()` closes the pseudo console (`ClosePseudoConsole`) and all pipe/process/thread handles, but does NOT force the child process to exit -- it relies on the child noticing the closed console and exiting on its own, which is not guaranteed (see PITFALLS).
- `terminate()`: last-resort cleanup. `::TerminateProcess`s the child if a handle exists, then calls `stop()`. Idempotent / a no-op if no child is running.
- `is_running()` reports whether a child is currently alive.
- **Host-close cooperation (`WM_JBCLOSEQUERY`, see `Documents/1. FrameBox/REQUIREMENTS.md` #8)**: `ConBox::OnCloseQuery` returns non-zero while `is_running()`, so a host `FrameBox` hides instead of destroying itself on `WM_CLOSE`. On the FIRST such query it starts a one-shot grace timer (`CLOSE_TIMER`, duration = `cfg_close_kill_timeout_ms`, from the `close_kill_timeout_ms` INI key, default 250). Whichever happens first:
  - the child exits naturally (`handle_child_exit`) -> the pending close is completed immediately (re-posts `WM_CLOSE` to the parent), or
  - the timer expires -> `terminate()` force-kills the child, then the close is completed the same way.
- This is automatic for any `ConBox` registered as a `FrameBox` child (`AddNew`) -- no host application code is required. Multiple `ConBox` instances under one `FrameBox` are handled independently (each answers `WM_JBCLOSEQUERY` for itself).
- **System shutdown/logoff (`WM_ENDSESSION`)**: the `CLOSE_TIMER` grace period above is asynchronous and may never elapse if Windows tears the process down before it fires, leaving the child as an orphan. This is NOT automatic -- a host must call `terminate()` synchronously from its own `WM_ENDSESSION` handler when `wParam != 0` (session actually ending). See `Build/jbTerm/main.cpp` (`cJbTermFrame::WindowProc`) for the pattern. `WM_QUERYENDSESSION` is deliberately left unhandled (default allows the session to end) so a shutdown later cancelled by another app does not still kill the child.

### 10. Host Notification Callbacks

- `set_exit_callback`, `set_title_cb`, and `set_titlebar_color_cb` take no opaque `user` context argument (unlike `set_input_sink`/`set_resize_sink`, which keep theirs since `start()` reuses them internally via static thunks).
- `set_titlebar_color_cb(caption, text, border)` is fed from the `[titlebar]` INI colors; a color not set in the INI is `CLR_INVALID`. `ConBox` has no title bar of its own -- applying the values (e.g. via `DwmSetWindowAttribute`) is entirely up to the host. The callback fires once from `setup()`/`setup_from_ini()` and again immediately on registration, so it never misses the current values regardless of call order.
- `CreateDefaultIni` writes the `[titlebar]` block into a freshly created INI only if `set_titlebar_color_cb` is already registered at that point -- so hosts that want it must register callbacks BEFORE calling `setup()`/`setup_from_ini()`.

### 11. Keyboard Macros ([macros]) and Line-Triggered Auto-Input ([triggers])

- `[macros]`: an F1..F12 INI key holds literal text (escape-decoded, see below) sent verbatim to the
  child when that key is pressed, instead of its normal VT function-key sequence. Modifier state
  (Shift/Ctrl/Alt) is not distinguished -- any combination fires the same macro. F10 is not
  supported: it is delivered as `WM_SYSKEYDOWN`, not `WM_KEYDOWN` (see PITFALLS), so it never reaches
  `terminal_keydown()` regardless of INI content.
- `[triggers]`: repeated `match=`/`send=`/`cool=` line groups (NOT numbered keys -- a new group starts
  at each `match=` line; `send`/`cool` lines until the next `match=` belong to it). After every
  output chunk (`print()`), if the current line's text from column 0 to the cursor ends with a
  group's `match` (suffix match), that group's `send` is sent to the child verbatim -- e.g.
  auto-filling an SSH password prompt. A group with an empty/missing `match` or `send` is not
  registered (same "empty INI value == unset" convention as `work_directory` etc.).
  - `cool` (ms): absent or negative = fire once then permanently inactive; `0` = fire on every match;
    positive = minimum ms between fires (checked against `GetTickCount64()`).
  - `active_trigger_count` short-circuits `check_triggers()` to a no-op once every trigger has either
    never been defined or is a spent one-shot -- so a config with no (or exhausted) triggers pays no
    per-chunk cost building/comparing the current line's text.
  - Unlike every other setting, `[triggers]` groups from a layered `setup()`/`setup_from_ini()` call
    are APPENDED, never replacing or overriding ones an earlier layer already registered -- this lets
    a host load a git-tracked base INI plus a separate untracked one for secrets (e.g. `send` holding
    a real password) without the second file being able to accidentally blank out the first's rules.
- Escape decoding (`DecodeMacroEscapes`, shared by both `[macros]` values and `[triggers]`
  `match`/`send`): `\r \b \a \t \n \f \v \\ \" \' \?` and `\xHH` (2-digit hex, case-insensitive, any
  byte -- the only way to embed a literal `;` in a value, since the INI parser cuts a value at the
  first raw `;` before this decoding ever runs). No octal escapes. Byte-wise, so multi-byte UTF-8
  (Korean etc.) passes through unaffected.

### 12. Mouse Reporting (xterm) and OSC 52 Clipboard

- When the child enables xterm mouse tracking (`?1000`/`?1002`/`?1003`) together with SGR encoding
  (`?1006`, the only encoding ConBox emits), left/middle clicks, drags, motion, and wheel notches are
  forwarded to the child as SGR mouse sequences instead of driving local selection/scrollback --
  letting a full-screen TUI (Claude Code, vim, htop) scroll and select on its own.
- Holding Shift forces the local behavior back (xterm/wt.exe convention); right click always pastes
  locally regardless of mode.
- The overlay scrollbar goes THUMBLESS while mouse reporting is active (matching wt.exe), instead of
  being hidden: the child owns the scroll position -- it moves its own view in response to forwarded
  notches -- so ConBox knows neither the total extent nor where the view sits in it, and any thumb it
  drew would be a lie. `sbar_geometry()` therefore returns a valid track/arrow geometry with an EMPTY
  thumb rect, which is the flag every caller keys on.
  - Nothing is drawn at rest (there is no slim idle bar without a thumb); hovering the gutter fades in
    the gutter strip + arrow buttons, and leaving fades them back out through the normal hold/fade path.
  - Arrow and track presses become forwarded wheel notches rather than local `view_top` moves: one
    notch for an arrow, `rows / 4` (min 1) for a track half. A notch is worth ~3 lines by the usual
    convention (the local wheel path also uses 3), so a track half lands just under one screen; sending
    a full `rows` would overshoot by ~3x. An arrow cannot be made finer than one notch -- that is the
    protocol's smallest step, so it does not match the 1-line arrow of the local path.
  - The gutter is live for clicks ONLY while it is actually on screen (`sbar_hover`), so an invisible
    strip never steals a click the TUI expects in its own last column. Motion is not reported to the
    child while the pointer is over the gutter.
- OSC 52 lets the child read (`?` payload) or write (base64 payload) the Windows clipboard on its own
  behalf -- how a TUI that owns the mouse copies a selection it drew itself, since it has no OS
  clipboard access of its own.
