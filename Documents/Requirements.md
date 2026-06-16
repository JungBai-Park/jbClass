# Software Requirements Specification (SRS)

This document defines the module-level requirements for `Parasite` and `ConBox`.
Project-wide environment, encoding, folder, build, and coding rules are defined in `CLAUDE.md` and are intentionally not repeated here.

## 1. FrameBox

### 1.1 Purpose

- `Parasite` attaches to existing controls through `SetWindowSubclass` and provides runtime layout editing, including moving and resizing controls.
- When an edit is confirmed, `Parasite` rewrites the coordinate literals at the source location recorded by `__FILE__` and `__LINE__`.
- Debug-only behavior, including edit mode, source rewriting, and file/line tracking members, must be compiled out when `_DEBUG` is not defined. Release builds keep only signal interoperability behavior.

### 1.2 Supported Controls and Signal Detection

- Supported controls include `CEdit`, `CStatic`, `CButton`, `CComboBox`, and common dialog controls through the default signal map and `FrameBox::add_*` factories.
- `Static` and `Progress` controls do not emit signals by default.
- `SysLink` controls are excluded from support.
- Control type detection is based on the window class name from `GetClassNameW`.
- Signal codes can be overridden through the `set_signal_code` method family.
- `WM_NOTIFY` handling must compare the signed full notification code and must not truncate it with `LOWORD`, so negative notification codes work correctly.

### 1.3 Layout Editing Gestures

- Clicking the middle mouse button toggles edit mode.
- While edit mode is active, the target control's native mouse and keyboard input is suppressed. When the edited target is a `FrameBox` itself (self-edit), its child controls are also kept inert: `PreTranslateMessage` swallows their mouse input (no hover/focus steal, except `WM_MBUTTONDOWN` so edit can be toggled elsewhere) and routes edit keys (arrows/Enter/Esc) to the editing window regardless of which child holds focus.
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

- `Parasite` handles `WM_PARASITE_SURVEIL` to store the report target.
- When a signal occurs, `Parasite` sends `WM_PARASITE_REPORT` to the stored report target.
- Host reflection messages include `WM_COMMAND`, `WM_NOTIFY`, `WM_HSCROLL`, and `WM_VSCROLL`.
- `Parasite` does not own the target control. It stores a borrowed `CWnd*`.
- During destruction, `Parasite` removes subclassing if the target `HWND` is still valid. It must not forcibly destroy the target control.

### 1.5 FrameBox Host and Factory Macros

- `FrameBox` is a `CWnd`-derived host window that OWNS its child controls through a per-instance registry. It replaces the former `cModalFrame` (top-level host) and `cLayOutZone` (child container). Its background color is `COLOR_BTNFACE`.
- `FrameBox` is `CWnd`-based, not `CFrameWnd`-based, so a child (`WS_CHILD`) zone form is natural and `CFrameWnd::OnDestroy` cannot post `WM_QUIT`. One shared `"FrameBox"` window class is registered once and created via `AfxHookWindowCreate` + `CreateWindowExW` (same pattern as `ConBox`); the style passed at creation selects top-level vs child vs popup.
- The `Add*` member macros (`AddStatic`, `AddButton`, `AddEdit`, `AddCombo`, ..., `AddZone`, `AddFrame`, `AddNew`, `AddAsItIs`) and the `OpenFrame` macro inject `__FILE__`/`__LINE__` at the call site. For `Add*` macros, coordinates are ALWAYS the first four arguments. For `OpenFrame(p, x0,y0,x1,y1)`, the pointer comes first and coordinates start at the 2nd argument (the source rewriter special-cases the `OpenFrame` name). In release builds the macros pass `nullptr,0` instead.
- Each `Add*` control factory performs control creation, `Parasite` creation and attachment, sizing, positioning, system font propagation through `WM_SETFONT`, `initialize()`, and registration of `{ wnd, cLayOut }` in the frame's registry.
- `FrameBox::open()` has two overloads selected by the first argument type. `open(CWinApp* app, x0,y0,x1,y1, ...)` calls `attach(app)` then creates a top-level `WS_OVERLAPPEDWINDOW`; `open(CWnd* owner, x0,y0,x1,y1, ...)` creates an owned popup (`WS_POPUP | WS_CAPTION | WS_SYSMENU`) then calls `owner->EnableWindow(FALSE)`. Both attach a self-editing `Parasite`. Coordinates are SCREEN coordinates. A bare `0`/`nullptr` must not be passed (overloads would be ambiguous). Use the `OpenFrame(p, x0,y0,x1,y1)` macro so file/line are injected. Driving construction as a member call (not a constructor) allows `FrameBox` subclasses to override `WindowProc` etc.
- `FrameBox::attach(CWinApp*)` binds the frame as the app main window (`m_pMainWnd = this`) and clears it in `close()`/`~FrameBox`, so no `CFrameWnd`-style `WM_QUIT` posting can apply.
- `FrameBox::~FrameBox()` (= `close()`) tears the registry down in reverse registration order (children first): delete `Parasite` (removes subclass) -> `DestroyWindow` -> delete `wnd`. A child-frame entry's `delete wnd` recurses into its own `~FrameBox`. This replaces the former global `DeleteLayOutWindows()`.
- `FrameBox::timer()` supports control of the `listen()` wait loop.
- `FrameBox::PreTranslateMessage` intercepts ESC (post `WM_CLOSE`) and Enter (click focused push button) in both normal and edit mode, matching `CDialog` keyboard behaviour. Exception: if the focused window returns `DLGC_WANTALLKEYS`, both keys pass through unmodified so terminal-style child windows (e.g. `ConBox`) can receive raw Enter and Esc.
- `add_zone` (`AddZone`) creates a `WS_CHILD | WS_EX_CONTROLPARENT` child container frame drawn inside the parent (the former `cLayOutZone` role). `add_frame` (`AddFrame`) creates a `WS_POPUP` separate-window child frame. Both are owned by the parent's registry and destroyed recursively. A child's controls are reflected by the child (its `WindowProc` runs reflection regardless of `listen()` state), but the report target is whichever frame called `listen()`, so an ancestor frame can `listen()` controls living in a child zone with no relay. True-modal behaviour for `AddFrame` is the caller's responsibility (`EnableWindow(parent, FALSE)` around the child's `listen()`); alternatively `Sub.OpenFrame(parentFrame, ...)` (the `CWnd*` overload) builds a modal owned-popup sub-dialog that disables its owner automatically and re-enables + reactivates it in `close()`.
- Default control styles:
  - `add_static`: `SS_LEFT | SS_CENTERIMAGE`.
  - `add_button`: `BS_PUSHBUTTON`.
  - `add_edit`: `ES_MULTILINE | ES_AUTOVSCROLL`.
- `AlignText` maps a keypad-style placement value from 1 to 9 to the text alignment behavior appropriate for the window class.
- `AddAsItIs(x0,y0,x1,y1, wnd)` attaches `Parasite` to an already-created `CWnd` and moves it to the given rect (borrowed: the registry stores `Parasite` only, not the `CWnd`). When all four coordinates are `0`, it operates in attach-only mode: the window is not moved, and `last_rect` is populated from the current `GetWindowRect` result (parent-relative). Prefer `AddNew` for windows created for the UI; reserve `AddAsItIs` for borrowed windows whose lifetime is owned elsewhere.
- `AddNew(x0,y0,x1,y1, wnd)` behaves like `AddAsItIs` but transfers ownership of the `new`'d `wnd` to the registry, so `close()` also `DestroyWindow`s and `delete`s it (`wnd` must be heap-allocated with `new`). Externally-created `CWnd`-derived windows (e.g. `new ConBox`) use this form.
- The root `FrameBox` is a STACK LOCAL inside the modal-loop driver (e.g. `DemoMain()`): `FrameBox Top; Top.OpenFrame(&theApp, ...);`. `~FrameBox` runs at scope exit, releasing the root and all its heap members (controls, attached `ConBox`, `Parasite`s) before the CRT exit leak snapshot, so no global cleanup call and no global/static window object are needed.
- **DPI coordinate convention**: all coordinates passed to `Add*`/`OpenFrame` macros are **96 DPI logical pixels**. `FrameBox::open()` and `add_*` factories scale them to physical pixels using `MulDiv(coord, monitor_dpi, 96)` at creation time. The target monitor DPI is determined via `MonitorFromPoint` + `GetDpiForMonitor` on the center of the intended rect.
- `FrameBox` retrieves a DPI-correct system font via `SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS)` and sends it to every registered child control via `WM_SETFONT` at creation. The font is stored as `sys_font` and freed in `close()`.

## 2. ConBox

### 2.1 Purpose

- `ConBox` is a portable, single-class, `CWnd`-derived terminal control for MFC.
- `ConBox` supports two operating modes in the same class:
  - Pure terminal view: output through `print()` and input routing through `set_input_sink()`. If no input sink is installed, it can be used as a diagnostic log display.
  - ConPTY child runner: `start(cmdline)` creates the required pipes and pseudo console, then runs a console child process.
- Target programs are interactive stdio shells such as PowerShell and line-oriented stdio programs that emit a limited VT100 stream.
- Full-screen TUI programs are supported only on a best-effort basis.

### 2.2 Window and Grid Control

- `open(parent, left, top)` registers and creates the window using the `"ConBox"` class name.
- The initial pixel size is calculated from the configured row and column counts.
- Runtime resizing does not perform automatic line reflow.
- When resized, existing rows are padded or truncated to fit the new width.
- `ConBox` is per-monitor DPI aware: fonts/cell metrics use the window's own DPI and rebuild when it changes. A DPI change must rescale only the cell pixels and KEEP the grid (rows/cols) fixed, so the child pseudo-console is not resized (a resize would make the shell re-emit its screen and corrupt scrollback). Font sizes are stored as DPI-independent specs so they can be rebuilt at the new DPI; overlay scrollbar pixel metrics scale with DPI.
- Margins are 96 DPI LOGICAL padding used ONLY to compute the initial window size and to derive rows/cols on resize (scaled to physical via the window DPI); `adjust` padding stays in raw physical pixels. The grid is NOT drawn at the margin offset: it is centered in the client area, and when it overflows it is drawn from the top-left and clipped at the bottom/right.
- Because cell pixels do not scale by the exact DPI ratio (integer font rounding), a preserved grid may not fit the linearly-scaled window after a DPI change. The `snap_mode` ini option controls the response: 0 = centering only (clip if too small); 1 = grow the window only when the grid would be clipped; 2 (default) = always snap the window to the exact grid+margin size. Snapping keeps the upper-left corner fixed and moves the right/bottom edges, and never resizes the pseudo-console.

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
- Instead of the default `WS_VSCROLL`, which consumes client pixels, `ConBox` renders a custom rounded overlay scrollbar at the edge of the view.
- The overlay scrollbar fades out during normal idle state.
- The overlay scrollbar's rounded thumb and arrow buttons are antialiased, and its gutter strip is filled with a background-contrasting color (not just a faint hover-only groove) for the full time the bar is shown. Visual style matches TableBox's overlay scrollbar (see 3.4); ConBox has only the vertical axis.
- The mouse cursor remains `IDC_ARROW`, matching Windows Terminal behavior.
- The text cursor (block/underline/I-beam) is hidden when the window does not have keyboard focus (`WM_KILLFOCUS`). It reappears immediately on `WM_SETFOCUS` with the blink timer restarted.

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

## 3. TableBox

### 3.1 Purpose

- `TableBox` is a `CWnd`-derived, Excel-like virtual grid control.
- It does NOT own its data. It requests only the currently visible cells through a `(row, col)` callback and draws them (virtual grid).
- Designed for reuse and customization by arbitrary projects (maximum flexibility), not project-specific.
- Self-contained portability unit: `TableBox.h` + `TableBox.cpp`. It must NOT reference `ConBox` / `FrameBox` members; duplicate the needed logic in-module even if code repeats (overlay scrollbar, font argument parsing).

### 3.2 Grid Definition (public API)

- `open(CWnd* parent, int x0, int y0, int rows, int cols)`: creates the child window at `(x0,y0)`. `rows`/`cols` are the INITIAL VISIBLE cell count, not the grid's total row/col count, and are clamped to it. Call `set_cols`/`set_rows`/`set_fixed`/`set_callback`/`set_font` before `open()` so the first paint already has metrics ready.
- `set_cols(int width, int limit)`: uniform column width with `limit` columns. `width` is 96 DPI logical px.
- `set_cols(const std::vector<int>& widths)`: per-column widths; column count = vector size.
- `set_rows(int height, int limit)` / `set_rows(const std::vector<int>& heights)`: same for rows.
- Before any `set_cols`/`set_rows` call, the grid defaults to uniform 75x20 (96 DPI logical) cells, 10000 columns x 10000 rows, so `open()` works even if the host never calls them.
- `set_fixed(int rows, int cols)`: frozen header rows/cols (default `1, 1`), Excel-style (stay fixed while the body scrolls).
- `set_callback(const char* (*cb)(int row, int col, void* user), void* user = nullptr)`: text source for ALL cells (fixed and normal alike). `row`/`col` are 0-based. The returned `const char*` is UTF-8 and is consumed (copied) immediately during drawing, never stored, so a reused static buffer is allowed. `user` is the opaque context registered here and passed back on every call.
- `set_font(const char* name, float size, const char* option = nullptr)`: single `CFont`. Borrows ONLY ConBox's argument grammar (name / size in points / option attribute string such as B/I/U). No English/Korean split and no height matching (TableBox cell widths are explicit, so the monospace cell logic does not apply).

### 3.3 Rendering

- Double-buffered: cells are drawn into a back-buffer memory DC, then `BitBlt`ed to screen in `OnPaint`.
- Per-cell drawing goes through an overridable virtual:
  `virtual void draw_cell(CDC& dc, int row, int col, int x0, int y0, int x1, int y1)`.
  - `dc` is the back-buffer memory DC.
  - Coordinates are client-area PHYSICAL pixels (DPI already applied). `x1`/`y1` are exclusive (RECT convention).
  - The default implementation calls the callback and renders text: normal cells on white, fixed cells with an Excel-like raised/button look (not flat gray). Subclasses may override to fully replace cell rendering or delegate to the base.
- Only visible cells are drawn. A partially visible cell IS drawn, with its FULL logical rect; the paint loop applies `IntersectClipRect(x0,y0,x1,y1)` before each `draw_cell` so overflow is clipped automatically (overriders always draw the whole cell).
- Grid lines are drawn by the paint loop AFTER all cells (not by `draw_cell`), so overrides need not redraw borders and normal cells always keep their grid.
- A protected helper `cell_text(row, col)` wraps the callback for overrides that only need the text.

### 3.4 Layout and Scrollbars

- All coordinates/sizes passed to `set_*` are 96 DPI logical pixels, scaled to physical at the window DPI.
- When the whole grid is smaller than the client area, it is top-left aligned; the leftover area is filled dark gray with no grid lines.
- Custom dynamic overlay scrollbars on BOTH the vertical and horizontal axes (re-implemented in-module, not referencing ConBox). They auto-show then fade with an antialiased rounded thumb and arrow-button shapes, scale with DPI, and adapt their color to the body's actual rendered background (since `draw_cell` is virtual and an override may not paint white).
- Each bar has arrow buttons at both ends that move one row/col per click; clicking the empty track between the thumb and an arrow pages by 3/4 of the visible extent.
- The vertical and horizontal bars never both show at once: in the shared corner where their hit zones overlap, whichever bar is already showing keeps the interaction; vertical wins when neither (or both) is showing.

### 3.5 DPI Awareness

- Per-monitor DPI aware. The initial render uses the creating monitor's DPI; a runtime DPI change (window moved to a different-DPI monitor) rebuilds the font and recomputes cell pixels.
- The DPI change is handled in `OnSize` by comparing `GetDpiForWindow` to the stored DPI (same pattern as ConBox). Because rows/cols are fixed by `set_cols`/`set_rows` and there is no child process, the grid is simply re-rendered at the new DPI; ConBox's "preserve grid, do not resize the PTY" constraint does not apply.
