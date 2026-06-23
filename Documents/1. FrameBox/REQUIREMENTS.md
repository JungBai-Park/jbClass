## FrameBox

### 1 Purpose

- `Parasite` attaches to existing controls through `SetWindowSubclass` and provides runtime layout editing, including moving and resizing controls.
- When an edit is confirmed, `Parasite` rewrites the coordinate literals at the source location recorded by `__FILE__` and `__LINE__`.
- Debug-only behavior, including edit mode, source rewriting, and file/line tracking members, must be compiled out when `_DEBUG` is not defined. Release builds keep only signal interoperability behavior.

### 2 Supported Controls and Signal Detection

- Supported controls include `CEdit`, `CStatic`, `CButton`, `CComboBox`, and common dialog controls through the default signal map and `FrameBox::add_*` factories.
- `Static` and `Progress` controls do not emit signals by default.
- `SysLink` controls are excluded from support.
- Control type detection is based on the window class name from `GetClassNameW`.
- Signal codes can be overridden through the `set_signal_code` method family.
- `WM_NOTIFY` handling must compare the signed full notification code and must not truncate it with `LOWORD`, so negative notification codes work correctly.

### 3 Layout Editing Gestures

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
- **Zoom-invariant layout editing**: all physical↔logical pixel conversions in `Parasite` (`enter_edit`, `leave_edit` ESC restore, commit/source rewrite) use `eff_dpi()` (= `MulDiv(real_dpi, zoom_pm, 1000)`) so the written 96 DPI logical coordinates are correct regardless of active zoom level. `Parasite` holds `FrameBox* owner_frame` (set by every factory via `set_owner(this)`); `friend class Parasite` in `FrameBox` grants access to the protected `eff_dpi()`. Factory placement (`finish_child`, `make_child`, `attach_external`) also uses `eff_dpi()` for the initial `MoveWindow`. `make_child` propagates the parent's `zoom_pm` to the newly created child `FrameBox` so both share a consistent `eff_dpi()` at construction time.

### 4 Host Framework Interoperability

- `Parasite` handles `WM_PARASITE_SURVEIL` to store the report target.
- When a signal occurs, `Parasite` sends `WM_PARASITE_REPORT` to the stored report target.
- Host reflection messages include `WM_COMMAND`, `WM_NOTIFY`, `WM_HSCROLL`, and `WM_VSCROLL`.
- `Parasite` does not own the target control. It stores a borrowed `CWnd*`.
- During destruction, `Parasite` removes subclassing if the target `HWND` is still valid. It must not forcibly destroy the target control.

### 5 FrameBox Host and Factory Macros

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
- **Ctrl+Wheel zoom**: `FrameBox::PreTranslateMessage` intercepts `WM_MOUSEWHEEL` with `MK_CONTROL` before any child handles it. Each wheel detent changes `zoom_pm` by ±100 (clamped to [500, 3000]; 1000 = 1.0×). `eff_dpi() = MulDiv(dpi, zoom_pm, 1000)` is the effective DPI used for all child layout and font sizing. When the cursor is inside the frame, the pixel under the cursor is kept fixed on screen (cursor-anchor mode).
- `WM_JBZOOM = WM_APP + 100`: sent to every registry child **before** `rescale_children()` → `MoveWindow` so ConBox/TableBox set their `zoom_pm` (and any needed flags) before `OnSize` fires. Each module header (`FrameBox.h`, `ConBox.h`, `TableBox.h`) defines the constant independently under an `#ifndef` guard with the same numeric value. Child-zone FrameBoxes receiving `WM_JBZOOM` forward it to their own children, rebuild `sys_font`, and call `rescale_children()` (no self-resize — parent's `rescale_children` handles their window size).
- `apply_zoom(int new_pm, bool cursor_anchor)` is `virtual protected` so subclasses can override it to add post-zoom actions (e.g. updating a title bar display) by calling `FrameBox::apply_zoom` then their own code.
- `dpi`, `zoom_pm`, `eff_dpi()`, and `apply_zoom` are in the `protected` section to allow subclass access without `friend` declarations.
- `set_margin(int margin_96)`: when `margin_96 >= 0`, after every `rescale_children()` call `fit_to_children()` reads each WS_CHILD's actual rect (`GetWindowRect` + `ScreenToClient`) to find the maximum right/bottom edge, then calls `SetWindowPos(SWP_NOMOVE)` to size FrameBox's client area to fit all children plus `MulDiv(margin_96, eff_dpi(), 96)` physical pixels of padding. `AdjustWindowRectEx` accounts for the non-client area. Default `-1` = disabled. Because `snap_to_grid()` inside ConBox runs synchronously within `MoveWindow`, `fit_to_children()` always sees the post-snap final child sizes.

### 6 Dynamic Menu Bar

- `add_menu(const char* popup_label, initializer_list<pair<const char*, function<void()>>> items)` adds a top-level popup to the menu bar at runtime without requiring a resource file. Labels are UTF-8. Pass `{"", nullptr}` for a separator. Returns the WM_COMMAND ID of the first non-separator item.
- `modify_menu_label(int id, const char* label)` changes the display label of a menu item by its WM_COMMAND ID (e.g. toggle "Start/Stop Logging").
- Menu actions are stored in `menu_popups` (vector of `MenuPopup`) and dispatched in `WindowProc` on `WM_COMMAND` with `lParam==0` and `HIWORD(wParam)==0`. No `BEGIN_MESSAGE_MAP` is needed in subclasses.
- The first `add_menu` call automatically compensates the window height so existing children are not clipped (measures client height delta before/after `SetMenu`).
- Menu bar DPI scaling (font, height) is handled automatically by Windows on `WM_DPICHANGED`.

### 7 Background Image and Frameless Mode

- `open_image(app/owner, x0, y0, id)` (macro `OpenImage`) opens a frame whose client area matches an `RT_RCDATA` image resource (JPEG/PNG/BMP auto-detected via GDI+ `Image::FromStream`). Window size is derived from the image pixel dimensions; only `x0,y0` (96 DPI logical screen coords) are given. Returns false if the resource is missing or the image format is invalid. The image is painted in `WM_ERASEBKGND`.
- The background image is **cached as a screen-compatible bitmap** pre-scaled to the current client size; `WM_ERASEBKGND` BitBlts the cache instead of re-running the GDI+ resample each paint (the per-paint resample stalled zoom 1-2 s — see PITFALLS). The cache is rebuilt only when the client size changes (zoom/DPI/resize) and freed in `close()` / on reload.
- `AddStatic` controls are transparent by default (`WM_CTLCOLORSTATIC` returns `NULL_BRUSH` + `SetBkMode(TRANSPARENT)`) so the background image/color shows through. (Therefore the frame must NOT use `WS_CLIPCHILDREN`, which would leave transparent-static rects unpainted.)
- `frameless(int option)` (macro `Frameless`) removes the OS title bar and draws owner-drawn Ubuntu-style round caption buttons in the top-right of the client area. Must be called AFTER `open()`/`open_image()` (requires a valid `m_hWnd`). Empty client area becomes draggable (`WM_NCHITTEST` -> `HTCAPTION`); button circles return `HTCLIENT` so clicks reach the frame. Returns 0 on success, -1 on invalid option, -2 if not yet created.
  - option 0: frameless, no buttons; 1: close only (right slot); 2: close + minimize (minimize takes the middle/maximize slot, not left blank); 3: close + minimize + maximize. Other values: no-op, -1.
  - Layout is right-aligned: rightmost slot is always Close. Button geometry (diameter, gap, margins) is a 96 DPI baseline scaled by `eff_dpi()` at draw/hit time, so it tracks zoom and DPI automatically.
  - Buttons are drawn with GDI+ anti-aliasing (no outline ring), a path-gradient top gloss plus a small specular highlight for a 3D feel, brighten on mouse-over (tracked via `WM_MOUSEMOVE` + `TrackMouseEvent`/`WM_MOUSELEAVE`), and dispatch on `WM_LBUTTONUP` (close -> `WM_CLOSE`, minimize -> `SW_MINIMIZE`, maximize -> toggle `SW_MAXIMIZE`/`SW_RESTORE`). All frameless `WindowProc` branches are gated on `fless_opt >= 0` so a normal titled window is unaffected.
