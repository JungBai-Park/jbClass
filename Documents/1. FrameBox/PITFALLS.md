## FrameBox

### 1. Host Notification Reflection

- Notifications such as `BN_CLICKED`, `EN_CHANGE`, `CBN_SELCHANGE`, and scroll request codes are delivered to the parent window, not directly to the control.
- To let the `Parasite` subclass receive a reflected notification, the host must send the callback to the control HWND:

```cpp
::SendMessage(ctrlHwnd, WM_PARASITE_CALLBACK, 0, (LPARAM)(int)code);
```

- Do not call `wnd->WindowProc(...)` through a C++ object pointer for this path. It bypasses the `SetWindowSubclass` chain and loses the notification.
- Preserve signed notification codes. Do not truncate them with `LOWORD`.

### 2. Source Rewriter Limits

- The `Add*`/`OpenFrame` macros inject `__FILE__` and `__LINE__`.
- The rewriter is FORM-BASED (not heuristic): the call form on the target line fixes where the coordinates start. Both accepted forms are member calls (`.Xxx` or `->Xxx` where `Xxx` starts uppercase):
  - `obj.OpenFrame(p, x0,y0,x1,y1)`: identifier is exactly `OpenFrame`. Coordinates start at the 2nd argument (skip the pointer arg) -- `coord_anchor` is the 1st top-level comma after `(`.
  - `obj.AddXxx(x0,y0,x1,y1[, extra])`: any other uppercase-initial member call whose 1st arg is an integer literal. Coordinates start at the 1st argument -- `coord_anchor` is `(`.
  - From `coord_anchor`, three top-level commas delimit x0,y0,x1,y1; text before the first coord and after the fourth (init string / window pointer trailing arg) is preserved verbatim. Reuses `find_char` (paren/bracket/string-aware).
- The trailing argument (init string for controls, window pointer for `AddNew`/`AddAsItIs`) must be preserved; it is.
- The rewriter supports only single-line calls with integer literals.
- UTF-16 BOM source files are not supported. ANSI and UTF-8 files are supported.
- Create a `.bak` file before rewriting.

### 3. Lifetime and Ownership

- `Parasite` stores a borrowed reference and does not own the target control.
- Each `FrameBox` OWNS its children through a per-instance registry (the global `new_window_registry()` / `DeleteLayOutWindows()` are gone). `~FrameBox()` (= `close()`) processes the registry in reverse-registration order: `delete Parasite` (removes subclass) -> `DestroyWindow` -> `delete wnd`. A child-frame entry's `delete wnd` recurses into that child's `~FrameBox`, tearing down grandchildren. Do NOT call `DestroyWindow()` separately on registered controls -- it is redundant.
- `AddAsItIs(..., wnd)` registers `{ nullptr, Parasite }` (borrowed): teardown removes the subclass only. `AddNew(..., wnd)` registers `{ wnd, Parasite }` (owned): teardown also `DestroyWindow`s + `delete`s wnd, so wnd must be a heap (`new`) object. Both attach the editing subclass, so the window stays live-editable.
- The root `FrameBox` is a STACK LOCAL in the modal-loop driver (`DemoMain`): `FrameBox Top; Top.OpenFrame(&theApp, ...);`. `~FrameBox` runs at scope exit. `attach()` (called by the `CWinApp*` overload of `open()`) sets `m_pMainWnd = this`; `close()` clears it FIRST so destruction never leaves a dangling main window.
- `OpenFrame` is a MEMBER-CALL macro (not a ctor macro), so `FrameBox` subclasses can override `WindowProc` etc. and still use `OpenFrame` for setup. No C++17 copy-elision dependency; no `#error` guard in `FrameBox.h`.
- Because `FrameBox` is `CWnd`-based (not `CFrameWnd`), `OnDestroy` does not post `WM_QUIT`; the old `m_pMainWnd = nullptr` + `DeleteLayOutWindows()` dance in `InitInstance` is no longer needed.
- A child frame (`AddZone`/`AddFrame`) closing tears down only its OWN registry, so the former footgun -- a child calling the global `DeleteLayOutWindows()` and wiping the main window -- cannot happen anymore.
- If the parent window is destroyed first (`WM_NCDESTROY`), `~Parasite` skips `RemoveWindowSubclass` (HWND already gone) and never deletes the target, so teardown is safe in either order.
- `FrameBox::PostNcDestroy` must be a no-op (lifetime is owner-managed; a self-delete would double-free).
- `FrameBox` creates its window via the `AfxHookWindowCreate` + `CreateWindowExW` pattern (see 2.2), exactly like `ConBox`: it registers one shared `"FrameBox"` class and selects top-level / `WS_CHILD` zone / `WS_POPUP` purely by the style passed at creation.

### 4. Edit-Mode Key Commit and WM_CHAR Leak

- When `PreTranslateMessage` returns `FALSE` for a `WM_KEYDOWN` in edit mode, the MFC pump calls `TranslateMessage` **before** `DispatchMessage`. `TranslateMessage` posts `WM_CHAR` to the queue immediately -- before the subclass proc runs and sets `editing = false`.
- After `leave_edit()` returns, `editing` is false, so the queued `WM_CHAR` (`'\r'` for Enter, `'\x1b'` for ESC) passes through the subclass proc and reaches the child window as input.
- Fix: call `::PeekMessageW(&dummy, h, WM_CHAR, WM_CHAR, PM_REMOVE)` immediately after `leave_edit()` in both paths (Enter commit in `on_key`, ESC cancel in the `WM_KEYDOWN` dispatch). `TranslateMessage` has already run at that point so the char is in the queue and can be removed.
- Double-click commit does not have this issue -- mouse messages do not generate `WM_CHAR`.

### 5. CEdit Vertical Alignment

- `EM_SETRECT` is ignored by single-line edit controls.
- To adjust vertical margins, the edit control must include `ES_MULTILINE`.
- When calling `SetRect()`, first call `GetRect()` and change only the Y-axis top offset. This avoids corrupting horizontal inset margins and border rendering.
- If `PreTranslateMessage` intercepts Enter, always return `TRUE` after handling it. Otherwise an unintended `'\r'` can enter the edit view.

### 6. Popup Frame Coordinate System (WS_CHILD vs GetParent)

- A `WS_POPUP` frame's `GetParent()` returns its OWNER, not a real parent. Deciding "child vs top-level" by `GetParent() != NULL` therefore misclassifies an owned popup as a child.
- Symptom (popup self-edit): entering edit then pressing an arrow made the window jump off-screen; committing a small mouse move rewrote the source with off-screen coords (e.g. owner-client-relative values) so the next run created the popup outside the visible area.
- Root cause: `Parasite::get_rect` mapped screen coords to owner-client coords (because `GetParent` was non-NULL), and `leave_edit`/`on_key` then moved the popup with those wrong-space coords (a non-child `MoveWindow`/`SetWindowPos` expects SCREEN coords).
- Fix: decide the coordinate space by the `WS_CHILD` style bit (as `on_drag` already did), not by `GetParent`. Only real children (`WS_CHILD`) use parent-client coords; top-level and popup frames stay in screen coords throughout (create/move/capture/rewrite).

### 7. Modal Sub-Dialog Close: Z-Order and Owner Reactivation

- `EnableWindow(owner, TRUE)` only restores input; it does NOT change activation/Z-order.
- Symptom: closing the modal sub-dialog dropped the owner one step down in Z-order (the system handed activation to whatever sat below the destroyed, still-active popup).
- Fix: in `close()`, BEFORE `DestroyWindow()` of the owned popup, call `owner->EnableWindow(TRUE)` then `owner->SetActiveWindow()` so the owner is already the active window when the popup is destroyed. Order matters (re-enable + reactivate, then destroy).

### 8. Per-Monitor V2 DPI Activation in MFC Static-Link Builds

- `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)` called from
  `InitInstance()` or even a global object constructor **always fails with LastError=5** when
  `UseOfMfc=Static` (MFC statically linked).
- Root cause: the MFC static CRT initializer calls `SetProcessDPIAware()` (System-aware) before
  any user code, including global constructors. The system rejects any downgrade or change attempt.
- Only solution: embed a manifest declaring `PerMonitorV2` so the OS loader sets awareness
  before the process starts. Use `AdditionalManifestFiles` in vcxproj:
  ```xml
  <ItemDefinitionGroup>
    <Manifest>
      <AdditionalManifestFiles>jbClass.manifest</AdditionalManifestFiles>
    </Manifest>
  </ItemDefinitionGroup>
  ```
  The `.manifest` file is merged and embedded into the EXE at link time (`RT_MANIFEST`, type 24, ID 1).
  No separate file is needed at runtime.
- The manifest file (`jbBox.manifest`) declares BOTH the `PerMonitorV2` DPI awareness and the
  Common Controls 6.0 `<dependency>` (`processorArchitecture="*"`). The old
  `#pragma comment(linker, "/manifestdependency:...")` in source was removed in favor of the
  manifest entry.
- Diagnostic: log `GetThreadDpiAwarenessContext()` result at startup; `SYS=1` confirms the
  CRT initializer already set System-aware before the code-based API call.

### 9. Development Monitor Layout

- The dev machine uses two stacked 1920x1080 monitors: PRIMARY at the bottom (origin, so screen `y: 0..1080`, 125% scale = 120 DPI) and SECONDARY above it (screen `y: -1080..0`, 100% scale = 96 DPI).
- So negative Y coordinates (e.g. the demo main window / sub-frame) are ON the upper secondary monitor, not off-screen. Pick screen coords accordingly when placing top-level/popup frames (`OpenFrame` coords are screen-based).

### 10. GetDpiForWindow Returns Owner's DPI for Owned Popup Windows

- For a `WS_POPUP` window whose owner is on a different-DPI monitor, `GetDpiForWindow` called
  immediately after `CreateWindowExW` can return the **owner's** DPI, not the DPI of the monitor
  where the popup rect was actually placed.
- Symptom: a popup at `(929, -628)` on the 96 DPI secondary monitor appeared 25% too large and
  the font was too large, because the owner (main window) was on the 120 DPI primary monitor.
- Fix: determine the target DPI **before** creating the window via `MonitorFromPoint` (center of
  the intended rect) + `GetDpiForMonitor(MDT_EFFECTIVE_DPI)`. This is correct regardless of
  ownership. Requires `<shellscalingapi.h>` and `Shcore.lib` (added to FrameBox.cpp).
- `FrameBox::open_core` now uses this approach for all window creation paths.

### 11. Parasite::set_owner Must Be Called in Every Factory That Creates a Parasite

- `Parasite::eff_dpi()` returns `owner_frame->eff_dpi()` (zoom-aware) when `owner_frame != nullptr`,
  and falls back to `GetDpiForWindow(h)` (zoom-unaware) otherwise. The fallback is silent --
  no crash, but layout editing at zoom != 100% writes inflated/deflated coords to source.
- Every site in FrameBox.cpp that does `new Parasite` must call `p->set_owner(this)` immediately
  after `p->attach(wnd)`. Current sites: `finish_child`, `make_child`, `attach_external`,
  `open_core` (self_layout). If a new factory is added without `set_owner`, zoom-aware editing
  silently breaks for controls created by that factory.
- `make_child` additionally propagates `child->zoom_pm = zoom_pm` so the new child FrameBox
  shares the parent's effective DPI from the moment of creation.

### 12. CMenu Double-Free on WM_DESTROY (Dynamic Menu Bar)

- When the user closes the window (X button), Windows destroys the window's menu during `WM_DESTROY`, before `close()` is called. If `CMenu` still holds the `HMENU`, its destructor calls `DestroyMenu` again on the already-freed handle.
- `CMenu::GetSafeHmenu()` contains `ASSERT(this == NULL || IsMenu(m_hMenu))`. Calling it after `WM_DESTROY` fires the assert because `IsMenu(m_hMenu)` returns false for the dead handle.
- Fix: in `close()`, call `menu_bar.Detach()` (assert-free) to retrieve the raw HMENU, then `::IsMenu(hm)` to guard `::DestroyMenu(hm)`. This handles both paths: normal close (menu still valid, destroyed here) and user-close (menu already dead, `DestroyMenu` skipped).
- `CMenu::CreateMenu()` and `CMenu::CreatePopupMenu()` (no W suffix) must be used via `Attach(::CreateMenu())` in MBCS builds -- `CMenu::CreateMenuW` / `CMenu::CreatePopupMenuW` do not exist as MFC member functions.

### 13. Frame Self-Edit: Child Input Isolation

- A `FrameBox` self-edit subclasses the FRAME's HWND, but the frame's child controls are separate HWNDs. The frame's `Parasite::dispatch` eats input only for the frame window, so children still hover-highlighted and stole focus, and arrow keys went to the focused child instead of moving the frame.
- `editing_hwnd() == pMsg->hwnd` is false when a child holds focus (target is the child), so the old `PreTranslateMessage` guard did not catch this case.
- Fix (in `FrameBox::PreTranslateMessage`, only while `editing_hwnd()` is non-NULL): route edit keys (arrows/Enter/Esc) to the editing window via `SendMessageW(edit, WM_KEYDOWN, ...)` regardless of focus and consume them; swallow non-editing controls' mouse messages (`WM_MOUSEFIRST..WM_MOUSELAST`) so they stay inert. Let `WM_MBUTTONDOWN` pass so edit can still be toggled on another control. When the editing window itself is focused (`pMsg->hwnd == edit`), return FALSE to let its own subclass proc handle the key.

### 14. Background Image Re-Stretch Stalls Zoom (set_image)

- Drawing the GDI+ background `Image` directly in `WM_ERASEBKGND` (`Graphics::DrawImage(img, clientRect)`) re-runs the JPEG/PNG resample on EVERY erase. A Ctrl+Wheel zoom triggers many full-client erases (child `MoveWindow`, `Invalidate()`, `fit_to_children` resize, `CS_HREDRAW|CS_VREDRAW`), so the visible result was: background repainted instantly but the whole frame took 1-2 s to settle. `OpenFrame` (solid `FillRect`) had no lag, which isolated the cause to the image resample.
- Fix: cache the scaled background in a screen-compatible `HBITMAP` (`rebuild_bg_cache`) sized to the client; `WM_ERASEBKGND` `BitBlt`s the cache. Rebuild only when the client size changes (`bg_cache_w/h` mismatch) and free in `close()` / `load_bg_image()`. `frameless`'s `fless_draw` also blits the cache slice instead of re-stretching the full image.
- Note: a small-output `DrawImage` clipped to the button strip is NOT cheap -- GDI+ still transforms the entire source. Always source button-strip background from the cache, not a fresh stretch.
- Do NOT add `WS_CLIPCHILDREN` to speed this up: transparent `AddStatic` controls (`WM_CTLCOLORSTATIC` -> `NULL_BRUSH`) rely on the parent painting the background under them; clipping children would leave those rects unpainted.

### 15. Gdiplus::Graphics::DrawImage Overload Ambiguity with LONG

- `Graphics::DrawImage(Image*, x, y)` has both `(INT,INT)` and `(REAL,REAL)` overloads. Passing
  `CRect::left/top` (type `LONG`) is ambiguous (C2668). Cast explicitly: `DrawImage(&buf, (INT)x, (INT)y)`.

### 16. Frameless Mode (Custom Caption Buttons)

- `frameless()` strips `WS_CAPTION|WS_SYSMENU|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_BORDER|WS_DLGFRAME`, adds `WS_POPUP`, then `SetWindowPos(SWP_FRAMECHANGED)`. Must run after the window exists.
- Removing `WS_CAPTION` enlarges the client area (no title bar), so an `open_image` window's client grows by the former caption height and the cached background stretches to fill it -- acceptable, not corrected.
- GDI+ must be initialized even when no background image is used: `frameless()` calls `gdip_addref()` once (guarded by the `fless_opt < 0` -> first-call transition) and `close()` releases it via the `fless_gdip` flag. Without this, a frameless `OpenFrame` (no `open_image`) window would create GDI+ objects in `fless_draw` with GDI+ not started.
- `WM_NCHITTEST` returns `HTCAPTION` for empty client area (OS drag-move) and `HTCLIENT` over a button circle (so `WM_MOUSEMOVE`/`WM_LBUTTONUP` reach the frame). Hit test is circular (distance <= radius), not the bounding rect.
- All caption-button handling is gated on `fless_opt >= 0`; a normal titled window keeps its original `WindowProc` behavior untouched.
