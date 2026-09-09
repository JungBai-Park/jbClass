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
      <AdditionalManifestFiles>jbBox.manifest</AdditionalManifestFiles>
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

### 16. Dialog-Style Esc/Enter Bypass for a Host Whose Only Child Wants All Keys

- `FrameBox::PreTranslateMessage` implements dialog-like defaults: Esc posts `WM_CLOSE`,
  Enter simulates a button click (or yields to an `ES_MULTILINE|ES_WANTRETURN` edit). Both
  branches are only reached when `GetFocus()`'s `WM_GETDLGCODE` does NOT include
  `DLGC_WANTALLKEYS` -- when it does (e.g. `ConBox::OnGetDlgCode` always returns it), the base
  class already forwards the key normally before reaching them.
- A host whose ONLY child needs Esc/Enter unconditionally (jbTerm/ConBox) is still exposed
  whenever focus itself drifts off that child: right after `OpenFrame` (a `CWnd`-based frame has
  no `CDialog`-style `GotoDlgCtrl`, so nothing gives the child initial focus), or after a
  system-menu common dialog closes with `hwndOwner == m_hWnd` (focus returns to the frame, not
  to the child that had it). In that state Esc closes the whole app and Enter is silently
  swallowed.
- Fix pattern (see `cJbTermFrame::PreTranslateMessage` in `Build/jbTerm/main.cpp`): override
  `PreTranslateMessage` in the host subclass and, for `WM_KEYDOWN`/`WM_CHAR`, force focus onto
  the child with `SetFocus()` whenever `GetFocus()` is not already it, THEN fall through to
  `FrameBox::PreTranslateMessage` as usual. With focus guaranteed to be on the child and
  `DLGC_WANTALLKEYS` always reported, FrameBox's own check now always takes its
  `CWnd::PreTranslateMessage` path (normal `TranslateMessage`/`DispatchMessage`) instead of ever
  reaching the Esc/Enter branches -- delivery no longer depends on whatever state focus happened
  to be in, without needing to special-case either key.
- Trap already hit once: do NOT try to fix this by `SendMessage`-ing `WM_KEYDOWN` straight to the
  child and returning `TRUE` from `PreTranslateMessage` to consume the original message. That
  skips `TranslateMessage` entirely, so `WM_CHAR` never fires for that keystroke -- and `ConBox`
  sends the actual Esc (`0x1B`)/Enter (`\r`) bytes to the child from `OnChar` (`WM_CHAR`), not
  `OnKeyDown` (`terminal_keydown()` has no case for `VK_ESCAPE`/`VK_RETURN` and returns `false`
  for both). The symptom is `OnKeyDown` running (IME/cursor bookkeeping looks fine) while Esc and
  Enter never reach the child at all. Deferring to the normal dispatch path (above) avoids this
  because it lets `TranslateMessage` produce `WM_CHAR` as usual.

### 17. Frameless Mode (Custom Caption Buttons)

- `frameless()` strips `WS_CAPTION|WS_SYSMENU|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_BORDER|WS_DLGFRAME`, adds `WS_POPUP`, then `SetWindowPos(SWP_FRAMECHANGED)`. Must run after the window exists.
- Removing `WS_CAPTION` enlarges the client area (no title bar), so an `open_image` window's client grows by the former caption height and the cached background stretches to fill it -- acceptable, not corrected.
- GDI+ must be initialized even when no background image is used: `frameless()` calls `gdip_addref()` once (guarded by the `fless_opt < 0` -> first-call transition) and `close()` releases it via the `fless_gdip` flag. Without this, a frameless `OpenFrame` (no `open_image`) window would create GDI+ objects in `fless_draw` with GDI+ not started.
- `WM_NCHITTEST` returns `HTCAPTION` for empty client area (OS drag-move) and `HTCLIENT` over a button circle (so `WM_MOUSEMOVE`/`WM_LBUTTONUP` reach the frame). Hit test is circular (distance <= radius), not the bounding rect.
- All caption-button handling is gated on `fless_opt >= 0`; a normal titled window keeps its original `WindowProc` behavior untouched.

### 18. Parent WM_ERASEBKGND Flash Behind a Fully-Covering Child

- `FrameBox::WindowProc`'s default `WM_ERASEBKGND` (no `bg_image`/`bg_color_set`) fills the
  whole client rect with `GetSysColorBrush(COLOR_BTNFACE)` (dialog-face gray). A child that
  fully covers the client area (e.g. `ConBox` in jbTerm, margin 0) hides this normally, but
  the frame and the child are separate HWNDs repainted asynchronously: whenever the FRAME's
  own region gets invalidated (e.g. uncovered by another window, a resize/zoom full
  repaint), its gray fill can reach the screen before the child's own repaint catches up,
  producing a brief white/gray flash. The child's own painting is not the cause even if it
  is already flicker-free (double-buffered, `OnEraseBkgnd` a no-op) -- the flash is the
  PARENT's erase showing through for one frame.
- `WS_CLIPCHILDREN` on the frame is the standard fix (excludes child-covered pixels from the
  parent's own paint/erase region entirely), but is NOT applied to `FrameBox` itself: pitfall
  #14's transparent `AddStatic` controls rely on the parent painting their background, which
  `WS_CLIPCHILDREN` would leave unpainted.
- Fix pattern for a host whose child(ren) fully tile the client area with no transparent
  controls (see `main.cpp`'s `WS_MINIMIZEBOX`/`WS_THICKFRAME` style block in jbTerm): OR in
  `WS_CLIPCHILDREN` on that specific window's `GWL_STYLE` after `OpenFrame`, not in
  `FrameBox` itself.

### 19. WM_ACTIVATE Focus Restore Must Run AFTER the Base WindowProc Call

- `FrameBox` is a plain `CWnd` (not `CFrameWnd`), so it has no built-in save/restore of the
  last-focused child across activation. A host that wants "clicking the title bar always
  refocuses my one real child control" (e.g. jbTerm's `ConBox`) must add this itself in a
  `WindowProc` override on `WM_ACTIVATE` (`LOWORD(wParam) != WA_INACTIVE`).
- Calling the child's `SetFocus()` BEFORE forwarding the message to `FrameBox::WindowProc`
  (-> eventually `DefWindowProc`) does not stick: `DefWindowProc`'s own default `WM_ACTIVATE`
  handling sets focus back to the top-level window itself, silently undoing it. Confirmed by
  sending a synthetic `WM_ACTIVATE(WA_CLICKACTIVE)` to a running instance and reading
  `GetGUIThreadInfo().hwndFocus` before/after each ordering.
- Fix: call the base `WindowProc` FIRST, keep its result, THEN call the child's `SetFocus()`,
  then return the saved result.
- This ordering issue is specific to real `WM_ACTIVATE` delivery through the full message
  chain (synthetic or genuine). It does not, by itself, guarantee focus at initial window
  creation -- the first `ShowWindow(SW_SHOW)` (e.g. inside `FrameBox::wait()`/`show()`) was
  observed to leave focus on the top-level window even with this ordering fixed, so a host
  that wants the child focused immediately at startup should also call the child's
  `SetFocus()` explicitly once, right after attaching it, rather than relying on `WM_ACTIVATE`
  alone.
