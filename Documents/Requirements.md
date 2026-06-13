# Software Requirements Specification (SRS)

This document defines the module-level requirements for `cLayOut` and `cConBox`.
Project-wide environment, encoding, folder, build, and coding rules are defined in `CLAUDE.md` and are intentionally not repeated here.

## 1. LayOut

### 1.1 Purpose

- `cLayOut` attaches to existing controls through `SetWindowSubclass` and provides runtime layout editing, including moving and resizing controls.
- When an edit is confirmed, `cLayOut` rewrites the coordinate literals at the source location recorded by `__FILE__` and `__LINE__`.
- Debug-only behavior, including edit mode, source rewriting, and file/line tracking members, must be compiled out when `_DEBUG` is not defined. Release builds keep only signal interoperability behavior.

### 1.2 Supported Controls and Signal Detection

- Supported controls include `CEdit`, `CStatic`, `CButton`, `CComboLayOut`, and common dialog controls through the default signal map and `LayOut*` factories.
- `Static` and `Progress` controls do not emit signals by default.
- `SysLink` controls are excluded from support.
- Control type detection is based on the window class name from `GetClassNameW`.
- Signal codes can be overridden through the `set_signal_code` method family.
- `WM_NOTIFY` handling must compare the signed full notification code and must not truncate it with `LOWORD`, so negative notification codes work correctly.

### 1.3 Layout Editing Gestures

- Clicking the middle mouse button toggles edit mode.
- While edit mode is active, the target control's native mouse and keyboard input is suppressed.
- Keyboard movement rules:
  - Arrow key: move by 5 px.
  - Ctrl + Arrow key: move by 1 px.
  - Shift + Arrow key: resize.
  - Ctrl + Shift + Arrow key: resize by 1 px.
- Mouse drag rules:
  - A hit zone within 8 px of a border is treated as an edge or corner resize area.
  - Dragging inside the border hit zone resizes the control.
  - Dragging the inner area moves the control.
  - The cursor changes to match the current action, using `IDC_HAND` or directional resize cursors.
- Confirmation and cancellation rules:
  - Enter or left-button double click confirms the edit and rewrites the source code.
  - Escape restores the `last_rect` captured before entering edit mode and does not rewrite source code.
  - Leaving edit mode by clicking the middle mouse button again does not rewrite source code.
  - The Enter and Escape key events are fully consumed by the edit session. The trailing `WM_CHAR` (`'\r'` or `'\x1b'`) that `TranslateMessage` posts before the subclass proc runs is discarded via `PeekMessageW` so it does not reach the child window.

### 1.4 Host Framework Interoperability

- `cLayOut` handles `WM_EVOL_SURVEIL` to store the report target.
- When a signal occurs, `cLayOut` sends `WM_EVOL_REPORT` to the stored report target.
- Host reflection messages include `WM_COMMAND`, `WM_NOTIFY`, `WM_HSCROLL`, and `WM_VSCROLL`.
- `cLayOut` does not own the target control. It stores a borrowed `CWnd*`.
- During destruction, `cLayOut` removes subclassing if the target `HWND` is still valid. It must not forcibly destroy the target control.

### 1.5 Factory Macro and Helper Classes

- The `LayOut` macro performs `CWnd` creation, `cLayOut` creation and attachment, sizing, positioning, subclassing, system font propagation through `WM_SETFONT`, and `initialize()`.
- `cModalFrame` creates a modal frame whose own size and position can be edited live. Its background color is `COLOR_BTNFACE`.
- `cModalFrame::timer()` supports control of the `listen()` wait loop.
- `cModalFrame::PreTranslateMessage` intercepts ESC (post `WM_CLOSE`) and Enter (click focused push button) in both normal and edit mode, matching `CDialog` keyboard behaviour. Exception: if the focused window returns `DLGC_WANTALLKEYS`, both keys pass through unmodified so terminal-style child windows (e.g. `cConBox`) can receive raw Enter and Esc.
- `cLayOutZone` is a child container window with a `COLOR_BTNFACE` background and `WS_EX_CONTROLPARENT`. It acts as the parent layout region for child controls.
- Default styles:
  - `LayOutStatic`: `SS_LEFT | SS_CENTERIMAGE`.
  - `LayOutButton`: `BS_PUSHBUTTON`.
  - `LayOutEdit`: `ES_MULTILINE | ES_AUTOVSCROLL`.
- `AlignText` maps a keypad-style placement value from 1 to 9 to the text alignment behavior appropriate for the window class.
- `LayOut(AsItIs, wnd, x0,y0,x1,y1)` attaches `cLayOut` to an already-created `CWnd` and moves it to the given rect (borrowed: the registry owns only the `cLayOut`, not the `CWnd`). When all four coordinates are `0`, it operates in attach-only mode: the window is not moved, and `last_rect` is populated from the current `GetWindowRect` result (parent-relative). Prefer `LayOut(New, ...)` for windows created for the UI; reserve `LayOut(AsItIs, ...)` for borrowed windows whose lifetime is owned elsewhere, and never for a global/static instance.
- `LayOut(New, wnd, x0,y0,x1,y1)` behaves like `LayOut(AsItIs, ...)` but transfers ownership of the `new`'d `wnd` to the registry, so `DeleteLayOutWindows()` also `DestroyWindow`s and `delete`s it (`wnd` must therefore be heap-allocated with `new`). Externally-created `CWnd`-derived windows (e.g. `new cConBox`) must use this form rather than a global/static instance, so their heap members are released inside the host loop before the CRT exit leak snapshot.
- The host `CWinApp` subclass must call `m_pMainWnd = nullptr` then `DeleteLayOutWindows()` in `InitInstance()` immediately after the modal loop function returns. Nulling `m_pMainWnd` first prevents `CFrameWnd::OnDestroy` from posting `WM_QUIT` when it destroys the main window.

## 2. ConBox

### 2.1 Purpose

- `cConBox` is a portable, single-class, `CWnd`-derived terminal control for MFC.
- `cConBox` supports two operating modes in the same class:
  - Pure terminal view: output through `print()` and input routing through `set_input_sink()`. If no input sink is installed, it can be used as a diagnostic log display.
  - ConPTY child runner: `start(cmdline)` creates the required pipes and pseudo console, then runs a console child process.
- Target programs are interactive stdio shells such as PowerShell and line-oriented stdio programs that emit a limited VT100 stream.
- Full-screen TUI programs are supported only on a best-effort basis.

### 2.2 Window and Grid Control

- `open(parent, left, top)` registers and creates the window using the `"ConBox"` class name.
- The initial pixel size is calculated from the configured row and column counts.
- Runtime resizing does not perform automatic line reflow.
- When resized, existing rows are padded or truncated to fit the new width.

### 2.3 Font and Cell Metrics

- English font configuration is handled by `set_efont`.
- Korean font configuration is handled by `set_kfont`.
- If the Korean font size value is `<= 0`, Korean font height matching is enabled by default. In this mode, the Korean font's physical pixel height is forced to match the English font height so line heights remain consistent.
- Cell width calculation:
  - Measure English width as `w_e`.
  - Measure Korean width as `w_k`.
  - Apply the configured ratio option.
  - Calculate `cell_w` as `(max(2 * w_e, w_k) + 1) / 2` to round up and avoid glyph clipping.
- `adjust(left, top, right, bottom)` keeps the font size unchanged, adjusts per-side cell margins, and compensates the internal glyph drawing position so block and box-drawing characters can render without gaps.

### 2.4 Rendering and Viewport

- The cell grid uses a fully fixed-width unit.
- CJK characters, including Korean characters, occupy two cell columns: lead and trail.
- Instead of the default `WS_VSCROLL`, which consumes client pixels, `cConBox` renders a custom rounded overlay scrollbar at the edge of the view.
- The overlay scrollbar fades out during normal idle state.
- The mouse cursor remains `IDC_ARROW`, matching Windows Terminal behavior.

### 2.5 Output and VT Escape Parsing

- C0 control support includes `\r`, `\n`, `\b`, and `\t`.
- Tabs advance to 8-column tab stops.
- CSI support includes cursor control such as CUU, CUD, and CUP.
- CSI support includes erase operations such as ED, EL, and ECH.
- CSI support includes scroll region control.
- SGR attributes include Bold, Italic, Underline, Strikethrough, Blink, and Reverse.
- Blink uses a 500 ms cycle.
- SGR 8 enables double-size rendering, and SGR 28 disables it.
- Double-size rendering draws text on the logical grid at a 2x horizontal and vertical scale, overlapping upward and rightward. This uses the standard SGR 8 code because conhost does not discard it in the middle of the stream.
- 256-color and True Color output are supported by default.
- Two-byte ESC support includes ESC 7/8 (DECSC/DECRC), RI (`M`), and RIS (`c`).
- Private sequence extensions that are likely to cause incorrect behavior are excluded.

### 2.6 Input Mapping and IME Commit Handling

- Standard virtual keys and character combinations are translated into the corresponding VT escape sequence strings encoded as UTF-8 and sent to the input sink.
- If a special key event, such as an arrow key or Enter, arrives while IME composition is active, `ImmNotifyIME` must commit the IME text first.
- After IME commit, the special key sequence is sent to the child process.
- The child process must always receive data in this order: committed Korean text first, then the special key code.
- Horizontal arrow correction:
  - After IME commit, the child process automatically moves the cursor one cell to the right.
  - To keep mouse and manual cursor tracking aligned, the input module may swallow the right arrow or send the left-move sequence twice for a left-arrow input.

### 2.7 Clipboard and Drag-and-Drop

- Drag selection stores the selected region in the clipboard as Unicode plain text.
- Double click selects a word.
- Alt-drag selects a rectangular block.
- On `WM_DROPFILES`, the full path of each dropped file is typed into the child process through standard input.
- Paths containing spaces are wrapped in double quotes.

### 2.8 Export and Logging

- `save_emf(dir)` creates a series of page-based EMF vector files in the target directory, using `cfg_lines_per_paper` as the page line count.
- EMF export maps colors through `remap_paper_color` so output is converted to print-oriented paper colors.
- `save_pdf(path)` searches system devices for a PDF conversion driver and exports the grid as a high-quality PDF using paper-oriented inverse color mapping.
- `get_text_lines()` returns the scrollback and current output as a list of UTF-8 text lines after trimming horizontal trailing spaces.
- `save_log(file_name)` writes the raw stream bytes received from the child process to a log file without CRLF conversion, encoding conversion, or other transformation.
- When a logging session restarts, four newline characters and a unique date timestamp delimiter are inserted.
