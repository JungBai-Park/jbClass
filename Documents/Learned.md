# Development Notes

This file records project-specific pitfalls and implementation details that are easy to forget. General project rules, coding conventions, encoding rules, folder layout, and default build policy are defined in `CLAUDE.md` and are not repeated here.

## 1. Build and Toolchain Pitfalls

### 1.1 Visual Studio Platform Names

- At solution level, the 32-bit platform name is `x86`.
- `Win32` is the platform name used inside `.vcxproj` files. Passing `Win32` to solution-level MSBuild can produce `MSB4126`.
- Known x64 Debug build command:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" "Project\jbClass.sln" /t:Build /p:Configuration=Debug /p:Platform=x64
```

- Build output uses configuration-specific suffix directories such as `Project\Debug.64\` and `Project\Debug.32\`.

### 1.2 Bash MSBuild Option Issue

- Running MSBuild from Bash can corrupt slash-prefixed options such as `/t:` by treating them as POSIX-style paths.
- Use PowerShell for MSBuild commands that pass Visual Studio properties or targets.

### 1.3 Temp Directory Handling

- The project `Temp\` directory can be automatically reset at session boundaries.
- Do not recursively delete `Temp\` during a live development session because harness outputs may still depend on files under it.

### 1.4 Per-Monitor V2 DPI Activation in MFC Static-Link Builds

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
- The manifest file itself only contains the DPI awareness declaration. Common Controls 6.0
  dependency stays in source code via `#pragma comment(linker, "/manifestdependency:...")`.
- Diagnostic: log `GetThreadDpiAwarenessContext()` result at startup; `SYS=1` confirms the
  CRT initializer already set System-aware before the code-based API call.

### 1.5 Development Monitor Layout

- The dev machine uses two stacked 1920x1080 monitors: PRIMARY at the bottom (origin, so screen `y: 0..1080`, 125% scale = 120 DPI) and SECONDARY above it (screen `y: -1080..0`, 100% scale = 96 DPI).
- So negative Y coordinates (e.g. the demo main window / sub-frame) are ON the upper secondary monitor, not off-screen. Pick screen coords accordingly when placing top-level/popup frames (`OpenFrame` coords are screen-based).

## 2. Source Compatibility Pitfalls

### 2.1 Comment Encoding and Korean Literals

- Non-ASCII comments in `Source/*.h` and `Source/*.cpp` can trigger `C4819`.
- For wide literals used as fixed code-point probes, prefer explicit escapes such as `L"\xac00"` so the value does not depend on source decoding.
- For user-visible narrow string literals that are intentionally UTF-8, direct Korean text is allowed when the source file is saved as UTF-8 with BOM.

### 2.2 Explicit Win32 APIs in MBCS Builds

- `CFont::CreateFontIndirectW` is not available in MBCS builds. Use `CFont::Attach(::CreateFontIndirectW(&lf))`.
- `CFont::GetLogFont(LOGFONTW*)` can map internally to `GetObjectA`, causing a mismatch between 60-byte and 92-byte structures. Use `::GetObjectW(hFont, sizeof(LOGFONTW), &lf)`.
- Use `::TextOutW(dc.GetSafeHdc(), ...)` instead of `CDC::TextOutW`.
- Use `::CreateEnhMetaFileW` explicitly because `::CreateEnhMetaFile` maps to the `A` variant in MBCS builds.
- When converting paths from `CString`, force the wide constructor with `CStringW wpath(str.GetString())` before calling `WideCharToMultiByte`.
- `IDC_ARROW` and other `IDC_*` cursor constants expand to `MAKEINTRESOURCEA` (= `LPCSTR`) in MBCS. Cast to `(LPCWSTR)` when passing to `LoadCursorW`.
- `CDC::GetTextMetricsW` does not exist in MBCS builds. Use `::GetTextMetricsW(dc.GetSafeHdc(), &tm)`.
- `CWnd::CreateEx` accepts `LPCTSTR` (= `LPCSTR` in MBCS), so a `const wchar_t*` class-name literal cannot be passed directly. Use `AfxHookWindowCreate(this)` + `::CreateWindowExW(...)` + `AfxUnhookWindowCreate()` to create a CWnd-derived window with a wide class name while keeping full MFC message-map support. `Attach(hwnd)` alone stores only the HWND with no WndProc replacement (message map never fires). `SubclassWindow(hwnd)` works but registers an "external subclassing" entry that can produce a phantom leak for global CWnd objects.

### 2.3 Bulk Replacement Hazards

- Short replacement patterns can accidentally modify longer identifiers or expressions.
- Example: replacing `dc.TextOutW(` can damage `cdc.TextOutW(`.
- Replace the longest and most specific patterns first, then manually inspect the result.

### 2.4 Factory Macro Hazards (FrameBox Add*/OpenFrame)

- The `Add*`/`OpenFrame` factory macros are global, function-like, case-sensitive textual substitutions defined at the bottom of `FrameBox.h`.
- `OpenFrame` takes 5 arguments and does NOT collide with MFC `Open(...)` members, so `afxpriv.h` no longer needs to precede `FrameBox.h`. Always include `"FrameBox.h"` first in `.cpp` files so `_WIN32_WINNT` is defined before `<afxwin.h>` is parsed (avoids the `_WIN32_WINNT not defined` warning).
- Do NOT write `Add*` (or any `...*/...`) inside a `/* */` block comment: the `*/` closes the comment early and everything after it compiles as code. Use `Add...` or a `//` line comment instead.

### 2.5 C++ Standard Version Check on MSVC

- MSVC pins `__cplusplus` at `199711L` unless `/Zc:__cplusplus` is passed, so a `#if __cplusplus >= 201703L` guard misfires even with `/std:c++17`.
- Use `_MSVC_LANG` (always reflects `/std`) when defined, else `__cplusplus`. (The `FrameBox.h` C++17 `#error` guard was removed when `NewFrame` copy-elision dependency was eliminated; this pitfall still applies to any new guards added.)

## 3. FrameBox

### 3.1 Host Notification Reflection

- Notifications such as `BN_CLICKED`, `EN_CHANGE`, `CBN_SELCHANGE`, and scroll request codes are delivered to the parent window, not directly to the control.
- To let the `Parasite` subclass receive a reflected notification, the host must send the callback to the control HWND:

```cpp
::SendMessage(ctrlHwnd, WM_PARASITE_CALLBACK, 0, (LPARAM)(int)code);
```

- Do not call `wnd->WindowProc(...)` through a C++ object pointer for this path. It bypasses the `SetWindowSubclass` chain and loses the notification.
- Preserve signed notification codes. Do not truncate them with `LOWORD`.

### 3.2 Source Rewriter Limits

- The `Add*`/`OpenFrame` macros inject `__FILE__` and `__LINE__`.
- The rewriter is FORM-BASED (not heuristic): the call form on the target line fixes where the coordinates start. Both accepted forms are member calls (`.Xxx` or `->Xxx` where `Xxx` starts uppercase):
  - `obj.OpenFrame(p, x0,y0,x1,y1)`: identifier is exactly `OpenFrame`. Coordinates start at the 2nd argument (skip the pointer arg) -- `coord_anchor` is the 1st top-level comma after `(`.
  - `obj.AddXxx(x0,y0,x1,y1[, extra])`: any other uppercase-initial member call whose 1st arg is an integer literal. Coordinates start at the 1st argument -- `coord_anchor` is `(`.
  - From `coord_anchor`, three top-level commas delimit x0,y0,x1,y1; text before the first coord and after the fourth (init string / window pointer trailing arg) is preserved verbatim. Reuses `find_char` (paren/bracket/string-aware).
- The trailing argument (init string for controls, window pointer for `AddNew`/`AddAsItIs`) must be preserved; it is.
- The rewriter supports only single-line calls with integer literals.
- UTF-16 BOM source files are not supported. ANSI and UTF-8 files are supported.
- Create a `.bak` file before rewriting.

### 3.3 Lifetime and Ownership

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

### 3.4 Edit-Mode Key Commit and WM_CHAR Leak

- When `PreTranslateMessage` returns `FALSE` for a `WM_KEYDOWN` in edit mode, the MFC pump calls `TranslateMessage` **before** `DispatchMessage`. `TranslateMessage` posts `WM_CHAR` to the queue immediately -- before the subclass proc runs and sets `editing = false`.
- After `leave_edit()` returns, `editing` is false, so the queued `WM_CHAR` (`'\r'` for Enter, `'\x1b'` for ESC) passes through the subclass proc and reaches the child window as input.
- Fix: call `::PeekMessageW(&dummy, h, WM_CHAR, WM_CHAR, PM_REMOVE)` immediately after `leave_edit()` in both paths (Enter commit in `on_key`, ESC cancel in the `WM_KEYDOWN` dispatch). `TranslateMessage` has already run at that point so the char is in the queue and can be removed.
- Double-click commit does not have this issue -- mouse messages do not generate `WM_CHAR`.

### 3.5 CEdit Vertical Alignment

- `EM_SETRECT` is ignored by single-line edit controls.
- To adjust vertical margins, the edit control must include `ES_MULTILINE`.
- When calling `SetRect()`, first call `GetRect()` and change only the Y-axis top offset. This avoids corrupting horizontal inset margins and border rendering.
- If `PreTranslateMessage` intercepts Enter, always return `TRUE` after handling it. Otherwise an unintended `'\r'` can enter the edit view.

### 3.6 Popup Frame Coordinate System (WS_CHILD vs GetParent)

- A `WS_POPUP` frame's `GetParent()` returns its OWNER, not a real parent. Deciding "child vs top-level" by `GetParent() != NULL` therefore misclassifies an owned popup as a child.
- Symptom (popup self-edit): entering edit then pressing an arrow made the window jump off-screen; committing a small mouse move rewrote the source with off-screen coords (e.g. owner-client-relative values) so the next run created the popup outside the visible area.
- Root cause: `Parasite::get_rect` mapped screen coords to owner-client coords (because `GetParent` was non-NULL), and `leave_edit`/`on_key` then moved the popup with those wrong-space coords (a non-child `MoveWindow`/`SetWindowPos` expects SCREEN coords).
- Fix: decide the coordinate space by the `WS_CHILD` style bit (as `on_drag` already did), not by `GetParent`. Only real children (`WS_CHILD`) use parent-client coords; top-level and popup frames stay in screen coords throughout (create/move/capture/rewrite).

### 3.7 Modal Sub-Dialog Close: Z-Order and Owner Reactivation

- `EnableWindow(owner, TRUE)` only restores input; it does NOT change activation/Z-order.
- Symptom: closing the modal sub-dialog dropped the owner one step down in Z-order (the system handed activation to whatever sat below the destroyed, still-active popup).
- Fix: in `close()`, BEFORE `DestroyWindow()` of the owned popup, call `owner->EnableWindow(TRUE)` then `owner->SetActiveWindow()` so the owner is already the active window when the popup is destroyed. Order matters (re-enable + reactivate, then destroy).

### 3.9 GetDpiForWindow Returns Owner's DPI for Owned Popup Windows

- For a `WS_POPUP` window whose owner is on a different-DPI monitor, `GetDpiForWindow` called
  immediately after `CreateWindowExW` can return the **owner's** DPI, not the DPI of the monitor
  where the popup rect was actually placed.
- Symptom: a popup at `(929, -628)` on the 96 DPI secondary monitor appeared 25% too large and
  the font was too large, because the owner (main window) was on the 120 DPI primary monitor.
- Fix: determine the target DPI **before** creating the window via `MonitorFromPoint` (center of
  the intended rect) + `GetDpiForMonitor(MDT_EFFECTIVE_DPI)`. This is correct regardless of
  ownership. Requires `<shellscalingapi.h>` and `Shcore.lib` (added to FrameBox.cpp).
- `FrameBox::open_core` now uses this approach for all window creation paths.

### 3.8 Frame Self-Edit: Child Input Isolation

- A `FrameBox` self-edit subclasses the FRAME's HWND, but the frame's child controls are separate HWNDs. The frame's `Parasite::dispatch` eats input only for the frame window, so children still hover-highlighted and stole focus, and arrow keys went to the focused child instead of moving the frame.
- `editing_hwnd() == pMsg->hwnd` is false when a child holds focus (target is the child), so the old `PreTranslateMessage` guard did not catch this case.
- Fix (in `FrameBox::PreTranslateMessage`, only while `editing_hwnd()` is non-NULL): route edit keys (arrows/Enter/Esc) to the editing window via `SendMessageW(edit, WM_KEYDOWN, ...)` regardless of focus and consume them; swallow non-editing controls' mouse messages (`WM_MOUSEFIRST..WM_MOUSELAST`) so they stay inert. Let `WM_MBUTTONDOWN` pass so edit can still be toggled on another control. When the editing window itself is focused (`pMsg->hwnd == edit`), return FALSE to let its own subclass proc handle the key.

## 4. ConBox

### 4.1 Font Measurement and Korean Matching

- The final measured height for the Korean matching font must be handled at the end of `calc_cell_size`.
- Do not build the Korean matching font early in `set_kfont`.
- If it is built before font match information is available, the GDI object is created with a default height of 0 and Korean glyphs render too small compared with English 2x output.
- In match mode (`set_kfont` size <= 0), `kfont_lf.lfHeight` is 0 at `set_kfont` call time, so the bold/italic/bold_italic variants built there also have `lfHeight=0` (GDI default = too large, overlaps adjacent lines). `calc_cell_size` corrects `kfont` but does NOT propagate the matched height back to the variants. Fix: at the end of `calc_cell_size`, after all font rebuilds, if `kfont_match_efont`, call `GetObjectW(kfont, &kfont_lf)` then rebuild the three variants via `MakeFontVariant`. The double variant (`kfont_double`) already uses `GetObjectW` internally via `MakeDoubleFont` so it was never affected.

### 4.2 SGR Double-Size Rendering

- Use standard SGR 8 for double-size on and SGR 28 for double-size off.
- Do not invent a private control code for this feature because conhost may drop unknown private sequences.
- Double-size glyphs bleed pixels into cells on the right.
- Paint each row from right to left so adjacent clear operations do not erase the bleed pixels and leave visual artifacts.

### 4.3 Cursor and Korean IME Commit Order

- If a navigation key or Enter is received while Korean IME composition is active, call `ImmNotifyIME(CPS_COMPLETE)` first.
- Send the special key only after the committed text is delivered.
- The child process must receive input in this order: committed text, then movement or control key.
- After Korean syllable commit, the emulation path advances the cursor one cell to the right.
- To compensate, swallow right-arrow input when needed and send the left-arrow sequence twice for left-arrow correction.

### 4.4 EMF Export Pitfalls

- In an EMF DC, `BeginPath` and `FillPath` do not reliably wrap `TextOutW`.
- If path-based filling is used, text can appear as black blocks.
- Set the foreground color with `SetTextColor(fg)` and draw text directly.
- Pass `NULL` as the media bounds rectangle pointer to `CreateEnhMetaFile` so GDI fits the output bounds tightly.
- If a `CDC` is attached to a target HDC, call `Detach()` before closing or destroying the HDC. Otherwise the CDC destructor can destroy the handle at the wrong time.

### 4.5 UTF-8 Chunk Boundaries

- Data from an external process pipe can split a multibyte UTF-8 character across chunks.
- Keep the incomplete trailing bytes and their length in temporary members such as `utf8_tail` and `utf8_tail_len`.
- Prefix the saved tail to the next chunk before decoding.
- Without this, display output and copied text can be corrupted.

### 4.6 ConPTY IND/NEL Behavior and Resize Limits

- If ESC D (IND) or ESC E (NEL) appears to do nothing, do not assume a terminal parser bug first.
- Microsoft ConPTY can intercept those control streams and rewrite them into normal cursor positioning sequences such as CUP before delivery.
- Runtime resize intentionally uses truncation and padding instead of reflow to keep the implementation lightweight.
- Avoid manual resize during a running shell session when preserving visual layout matters. Restart the shell if a clean layout is required.

### 4.7 Keyboard Input Not Working in ConBox

- `CWnd`-derived custom windows do not receive focus automatically on click (unlike standard controls such as `CEdit`).
- `OnLButtonDown` must call `SetFocus()` explicitly so the window can receive keyboard input.
- `FrameBox::PreTranslateMessage` unconditionally consumes `VK_RETURN` and `VK_ESCAPE` even in normal (non-edit) mode. A window that needs these keys (e.g. ConBox returning `DLGC_WANTALLKEYS`) will never receive them unless a guard is added.
- Fix: check `focus->SendMessage(WM_GETDLGCODE) & DLGC_WANTALLKEYS` before the ESC/Enter intercept and yield to `CWnd::PreTranslateMessage` when true.
- TRAP: the `editing_hwnd()` guard in `PreTranslateMessage` only yields to the Parasite subclass proc during control repositioning. It does NOT mean ESC/Enter are consumed only in edit mode -- they are always consumed in normal mode too (except for `DLGC_WANTALLKEYS` windows).

### 4.8 DemoApp Exit Code and MFC Memory Leak Message

- `CWinApp::ExitInstance()` returns `AfxGetCurrentMessage()->wParam` -- the last pumped message's wParam. When `InitInstance` drives its own modal loop and returns `FALSE`, this yields a meaningless non-zero exit code. Override `ExitInstance()` to `return 0` explicitly.
- "Detected memory leaks! Dumping objects -> Object dump complete." with an EMPTY dump:
  - The header fires when the `_NORMAL_BLOCK` count differs between the initial snapshot and shutdown, but `_CrtMemDumpAllObjectsSince` only prints `_CLIENT_BLOCK` (`DEBUG_NEW`) objects. An empty dump means no user-code leaks -- the difference is in `_NORMAL_BLOCK` (raw `::operator new`, e.g. STL allocators).
  - ROOT CAUSE (confirmed, not an unfixable MFC quirk): a global/static `CWnd`-derived object (e.g. a global `ConBox`) holds STL members (`std::deque`/`vector`/`string`: scrollback, screen grid, buffers). A global object's destructor runs at STATIC DESTRUCTION, which is AFTER the CRT leak snapshot. So those STL allocations are still live at snapshot time -> `_NORMAL_BLOCK` count differs -> header. They are not `_CLIENT_BLOCK`, so the dump is empty. Symptom matches exactly.
  - Calling `DestroyWindow()` on the global does NOT fix it: that frees only the HWND, not the C++ object's STL members (which live until static destruction).
  - FIX (implemented): do not keep such objects global. `new` them and let `~FrameBox()` `delete` them inside the host loop -- attach external windows via `AddNew(...)` and the root frame via `new` + `delete Top` before `InitInstance` returns (see 3.3) -- so all heap members are freed BEFORE the snapshot. This removes the warning entirely (verified: clean build + run, exit code 0, no leak dump). Prefer this over suppressing the check (`_CrtSetDbgFlag`), which would also hide real leaks.

### 4.9 Per-Monitor DPI: Preserve the Grid, Do Not Resize the PTY

- A DPI change must rescale only the cell pixels (fonts) and KEEP cols/rows fixed. The grid is a
  logical entity; if `update_metrics` recomputes cols/rows from the new window pixels on a DPI change
  and calls `resize_sink` -> `ResizePseudoConsole`, the child shell RE-EMITS its visible screen
  (ConPTY reflow, see 4.6). Our terminal then appended it -> scrollback duplicated/grew on every
  monitor switch; an intermediate wrong-size recompute also truncated content (lost lines).
- `make_font` runs BEFORE the window exists (set_efont/set_kfont via setup_from_ini, and the
  pre-create font build in `open()`), so `GetDpiForWindow(NULL)` returns 0 there. Resolve DPI as:
  `GetDpiForWindow(m_hWnd)` if the window exists, else `box_dpi` (seeded from the parent monitor in
  `open()`), else primary-monitor `GetDeviceCaps`. Naively swapping in `GetDpiForWindow(m_hWnd)`
  alone zeroes `lfHeight` and makes default-size (broken) fonts.
- DO the live DPI work in `OnSize`, NOT in `WM_DPICHANGED_BEFOREPARENT/AFTERPARENT`. Approaches that
  set a "lock grid" flag in those handlers FAILED: the order of AFTERPARENT vs the parent's
  follow-up `MoveWindow`->OnSize is NOT reliable (observed: the grid-recomputing OnSize ran while the
  lock was not yet set and fonts were still old -> grid collapsed to ~19 rows, content truncated).
  `OnSize` is the single point where the grid is computed and `GetDpiForWindow(m_hWnd)` is
  authoritative; compare it to `box_dpi`: if different it is a DPI change -> rebuild fonts + PRESERVE
  the grid (skip `update_metrics`, so no `resize_sink`); if equal it is a user resize -> normal
  recompute. Order-independent, no message-timing assumptions. (For a WS_CHILD ConBox the parent
  FrameBox's `rescale_children` MoveWindow is what fires that OnSize, and the child's DPI is already
  the new value by then.)
- SLACK/CLIP RESIDUAL -- RESOLVED. With cols/rows fixed, the window scales by the exact DPI ratio but
  cell_w/cell_h scale by ROUNDED integer font metrics (plus Korean match-mode leading), so the same
  grid needs a few more (or fewer) pixels than the linearly-scaled window -> deficit clips the last
  column(s)/row, surplus leaves slack. Confirmed: cell_w/cell_h at a higher DPI are NOT exactly the
  DPI-ratio multiple of the 96 DPI metrics (GDI rounds up), and the per-cell error accumulates over
  ~96 cols / ~25 rows into more than one cell (the user's ">1 cell" observation; it is whole cells,
  not pixels). Fix has two parts:
  - `margin` is now 96 DPI LOGICAL padding used ONLY to size the window (`open`) and derive cols/rows
    (`update_metrics`), scaled via `MulDiv(margin, box_dpi, 96)`. Drawing no longer uses margins.
    `adjust` stays raw physical px.
  - `recalc_origin()` CENTERS the grid: `origin = (client - grid) / 2`, clamped to 0 on overflow
    (draw top-left, clip bottom/right). All on-screen draw / hit_test / caret / IME read
    `origin_x/origin_y`; EMF/PDF export keeps its paper margins.
  - `snap_to_grid()` (OnSize DPI branch, after `relayout_for_dpi`) resizes the window to fit the
    preserved grid; ini `snap_mode` (default 2): 0=centering only, 1=grow-only when clipped, 2=always
    exact grid+margin. `SWP_NOMOVE` keeps the upper-left fixed (right/bottom edges move); non-client
    diff added so a bordered popup sizes its CLIENT. The snap's follow-up OnSize has `cur == box_dpi`
    and recomputes the IDENTICAL grid (same size -> no `resize_sink` -> no PTY resize), so even
    mode-2 shrink is corruption-safe.

## 5. TableBox

### 5.1 std::min/std::max Break Under windows.h

- `afxwin.h` pulls in `windows.h`, which (without `NOMINMAX`) defines `min`/`max` as macros.
  `std::min(...)`/`std::max(...)` then fail to compile (`std::` followed by an expanded macro
  is a syntax error: `C2589`/`C2059`), not a clean "ambiguous call" error.
- This project does not define `NOMINMAX`. Use a manual ternary (`a < b ? a : b`) instead of
  `std::min`/`std::max` in any new code; do not add `<algorithm>` for this purpose.

### 5.2 draw_grid_lines Runs AFTER draw_cell and Owns the Shared Boundary Pixel

- `draw_grid_lines` is called once per block, AFTER every `draw_cell` in that block has already
  run. Anything `draw_cell` paints on a cell's edge pixel is overwritten by the plain gray grid
  line that follows -- a cell-local border/inset line drawn exactly at `rc.left`/`rc.top`/
  `rc.right`/`rc.bottom` from inside `draw_cell` never survives.
- Hit this twice: the focused-cell green border (top/left edge looked thinner than bottom/right
  because the trailing grid-line pass ate into the boundary) and the fixed-header inset white
  line (invisible at `rc.left`/`rc.top`, only became visible 1px further in).
- Fix pattern: draw such overlays from `OnPaint` AFTER the `draw_block`/`draw_grid_lines` pair
  for the whole visible region (see `draw_focus_border`), or, if it must stay inside
  `draw_cell`, offset it 1px inward (`rc.left+1`/`rc.top+1`/...) so the later grid-line draw
  does not touch it (see the fixed-header inset line).
- Pixel-exact alignment detail: `draw_grid_lines`'s right/bottom line for a cell is drawn AT
  `rc.right`/`rc.bottom` (the next cell's left/top origin), not at `rc.right-1`/`rc.bottom-1`.
  A plain `CDC::Rectangle(rc)` with a 1px pen draws its right/bottom edge at `rc.right-1`/
  `rc.bottom-1` instead, one pixel off from the real grid line -- to overlay exactly on top of
  it, call `Rectangle(rc.left, rc.top, rc.right+1, rc.bottom+1)`.

### 5.3 Per-Zone Cursor Needs ON_WM_SETCURSOR, Not a SetCursor Call in OnMouseMove

- `TableBox`'s window class registers a static `hCursor` (`IDC_ARROW`). On every mouse move
  over the window, `DefWindowProc`'s default `WM_SETCURSOR` handling re-applies that class
  cursor, so a `::SetCursor()` call made from `OnMouseMove` gets immediately overwritten.
- Fix: add `ON_WM_SETCURSOR()` to the message map and override `OnSetCursor` to call
  `::SetCursor()` and return `TRUE` (suppressing the default handling) when the point is over a
  resize border (`hit_col_border`/`hit_row_border`); otherwise fall through to
  `CWnd::OnSetCursor`. `OnSetCursor` does not receive the point directly -- use
  `GetCursorPos` + `ScreenToClient`.
- `IDC_SIZEWE`/`IDC_SIZENS` need the same `(LPCWSTR)` cast as `IDC_ARROW` when passed to
  `LoadCursorW` in MBCS builds (see 2.2).

### 5.4 In-Place Editing: SetFocus() Must Not Be Called From Inside the WM_KILLFOCUS Path

- The plain-text editor is a raw Unicode EDIT driven by the static `TableBox::EditSubclassProc`
  (dwRefData = the `TableBox*`), NOT a `CEdit` subclass -- see 5.9 for why. `TableComboPopup` is
  still a private coupled class needing `TableBox`'s private `end_combo_edit`/`ending_edit`,
  granted via `friend class TableComboPopup;`. (The former `friend class TableEditBox;` is gone.)
- `end_text_edit`/`end_combo_edit` deliberately do NOT call `SetFocus()` themselves. Calling
  `SetFocus()` from code that can be reached while a `WM_KILLFOCUS` is being processed (Win32
  warns against this) is ambiguous: when a click elsewhere triggers `TableBox::OnLButtonDown`'s
  existing `SetFocus()` call, that synchronously raises `WM_KILLFOCUS` on the edit box, which
  commits -- calling `SetFocus()` again from inside that commit would be a reentrant call into
  an in-progress focus change. Instead, the CALLER decides: the EDIT subclass proc's `WM_KEYDOWN`
  handler (Enter/Esc/Tab, box still focused, no `WM_KILLFOCUS` involved) calls `self->SetFocus()`
  itself right after committing/cancelling; the `WM_KILLFOCUS` case does not call it at all (focus
  is already headed wherever the click/Tab-out sent it). Same split for the combo popup: mouse
  pick / Enter / Esc call `owner->SetFocus()` afterward, but `WM_ACTIVATE(WA_INACTIVE)` (the
  user activated something else) never does -- that would steal focus back from whatever the
  user actually clicked.
- Because `end_text_edit`/`end_combo_edit` do not touch focus and `TableBox::OnLButtonDown`'s
  pre-existing `SetFocus()` call already triggers the edit box's `WM_KILLFOCUS` synchronously,
  no extra "commit pending edit before processing this click" code is needed in
  `OnLButtonDown`/`OnLButtonDblClk` -- it falls out for free. Same for the combo popup via
  `WM_ACTIVATE`: clicking elsewhere deactivates the popup before the new click's own handler runs.

### 5.5 Single-Line EDIT: Text Is Top-Aligned, EM_SETRECT Has No Effect

- The in-place EDIT is created WITHOUT `ES_MULTILINE`. A single-line EDIT always top-aligns its
  text regardless of the control's height, and `EM_SETRECT`/`EM_SETRECTNP` have no effect on it.
  `EM_SETRECT` only works on `ES_MULTILINE` edits (section 3.5). The editor does not apply
  vertical centering; `set_edit_adjust(dx0,dy0,dx1,dy1)` lets the host nudge the box rect (use
  `dy0`/`dy1`) to place the text vertically.
- `WM_GETDLGCODE` returning `DLGC_WANTALLKEYS` (from `EditSubclassProc` for the EDIT and
  `OnGetDlgCode` for `TableComboPopup`) is required so `FrameBox::PreTranslateMessage`'s
  `CDialog`-like Enter/Esc interception (Requirements.md 1.3 / Learned.md 4.7) yields to them.
  For the EDIT, the subclass proc catches Enter/Esc/Tab in `WM_KEYDOWN` and swallows the trailing
  `WM_CHAR` (`\r`/`\x1b`/`\t`) `TranslateMessage` posts (Learned.md 3.4); `DLGC_WANTALLKEYS` is the
  safety net.

### 5.6 Combo Cell Type Comes From edit_cb, Not From text_cb

- Combo cells are identified by the dual-purpose `edit_cb(row,col,nullptr)` QUERY return (a
  comma-separated item list), NOT by a `"\x1B"`-prefixed `text_cb` value (that old encoding is
  gone). `text_cb` for a combo cell returns just the selected index string (`"0"`, `"1"`, ...).
  Static helpers: `ParseComboItems(str, items)` (splits the list, trims spaces, `'\b'` -> literal
  comma) and `ComboIndex(idx_text, max)` (clamped 0-based index from `text_cb`).
- Any code that DISPLAYS or MEASURES a combo cell (`draw_cell`, `autofit_col`/`autofit_row`) must
  resolve the label as `items[ComboIndex(cell_text(...), items.size())]`, never the raw text.
  The old `ComboDisplayLabel(raw)` helper (which decoded the `\x1B` spec) is gone; the logic is
  now inline in each of those sites guarded by an `edit_cb != nullptr` + `ParseComboItems` check.

### 5.7 Combo Popup Window Size: WS_BORDER Shrinks Client Area

- `WS_BORDER` subtracts its frame (1px top + 1px bottom) from the client area. If the window
  height is set to `cell_h * N`, the client height becomes `cell_h * N - 2`, so integer division
  yields `row_h() = cell_h - 1`. Every item is 1px shorter than the cell, separator lines drift
  1px per item (accumulating), and `N - 2` px of blank space remain at the bottom.
- Fix: compute the window rect from the desired client rect via
  `AdjustWindowRectEx(&adj, WS_POPUP|WS_BORDER, FALSE, WS_EX_TOOLWINDOW)` so the client area
  is exactly `cell_h * N` regardless of system border metrics.
- Separator alignment: client y=0 maps to screen y = window_top + 1 (top border), so a separator
  drawn at client y = `(i+1)*rh` lands 1px below the matching table grid line. Draw at
  `(i+1)*rh - 1` to compensate. This 1px correction is constant (no accumulation) once
  `row_h() = cell_h` is exact.
- Two-pass rendering: draw all item backgrounds and text first, then draw separator lines on top
  in a second pass -- same pattern as `draw_block` + `draw_grid_lines` -- so separator lines
  are never overwritten by a subsequent item's background fill.

### 5.8 Combo Popup External Cancel: Prevent Double-Destroy via ending_edit

- `end_combo_edit` is designed to be called by the popup AFTER it has already destroyed itself
  (`DestroyWindow()` in `OnLButtonUp`/`OnKeyDown`/`OnActivate`); it only clears the pointer,
  never calls `DestroyWindow` itself.
- When `cancel_edit()` (called from `OnMouseWheel`/`OnLButtonDown`/`OnSize`) must cancel the
  popup from outside, it must call `DestroyWindow` directly. During that destroy, Win32 may
  dispatch `WM_ACTIVATE(WA_INACTIVE)` to the popup, whose `OnActivate` handler would then call
  `DestroyWindow` a second time and `end_combo_edit` again -> double-destroy and double-cleanup.
- Fix: in `cancel_edit`, set `combo_popup = nullptr` and `ending_edit = true` BEFORE calling
  `DestroyWindow`, so any re-entrant `OnActivate` call sees the guard and returns immediately.
  `OnActivate` checks `owner->ending_edit` and skips its body when true. Reset `ending_edit`
  after `DestroyWindow` returns.

### 5.9 Combo Popup Self-Commit: A `closing` Flag Stops WA_INACTIVE From Eating the Commit

- A combo pick commits via the popup's own handler (`OnLButtonUp`/`OnKeyDown`): it calls
  `DestroyWindow()` and THEN `owner->end_combo_edit(true, chosen)`. But `DestroyWindow()`
  synchronously raises `WM_ACTIVATE(WA_INACTIVE)`, whose `OnActivate` ran the CANCEL path FIRST
  (`end_combo_edit(false,-1)`), clearing `owner->combo_popup`; the real commit that followed then
  hit the `combo_popup == nullptr` guard and was silently dropped (selected value never stored).
  `ending_edit` did NOT cover this -- it is false during a normal self-commit.
- Fix: a `bool closing` member on `TableComboPopup`, set true at the top of EVERY self-close path
  (`OnLButtonUp`, `OnKeyDown` Enter/Esc) before `DestroyWindow()`. `OnActivate` skips its CANCEL
  body when `closing` (or `owner->ending_edit`) is set, so the re-entrant WA_INACTIVE is a no-op
  and the pending commit lands. Order within a self-close: `closing = true;` -> `DestroyWindow()`
  -> `end_combo_edit(...)`.

### 5.10 Hangul IME In-Place Edit: Use a Raw Unicode EDIT + Win32 Subclass, Not an MFC CEdit

- Symptom path (each step a separately-debugged failure): typing Hangul to start editing a CEdit
  cell showed the lead jamo as `?`, then later as a different wrong glyph per jamo, then the leading
  jamo simply did not appear until the syllable completed. Root causes, in order:
- (a) ANSI EDIT window. `CEdit::Create` -- and even `AfxHookWindowCreate` + `CreateWindowExW` --
  bind the window to MFC's ANSI `AfxWndProc` in an MBCS build, so `IsWindowUnicode == 0`. An ANSI
  EDIT renders `WM_IME_COMPOSITION` text through CP949; an incomplete jamo (e.g. lead `U+3131`,
  not a precomposed syllable) has no CP949 form and paints as `?`. FIX: create the editor as a
  RAW Unicode EDIT with a plain `::CreateWindowExW(0, L"EDIT", ...)` (NO MFC hook) + a Win32
  `SetWindowSubclass(eh, EditSubclassProc, ...)`; that keeps `IsWindowUnicode == 1`. The subclass
  proc replaces the former `TableEditBox` (Enter/Esc/Tab commit, `WM_CHAR` swallow, `WM_KILLFOCUS`
  commit, `DLGC_WANTALLKEYS`, `WM_NCDESTROY` -> `RemoveWindowSubclass`). `edit_box` is now an `HWND`.
- (b) The IME starts composition while the CELL (TableBox) still has focus, so the FIRST
  `WM_IME_COMPOSITION` is delivered to TableBox even though `start_text_edit` already `SetFocus`-ed
  the EDIT; every later composition message goes straight to the EDIT. That one stray first message
  must be relayed to the EDIT (`SendMessageW`), else the lead jamo never paints (it lives in the
  HIMC, so it only shows once the syllable completes).
- (c) But TableBox is an ANSI window in an MBCS build, so that relayed `wParam` is an ANSI (CP949)
  code (e.g. `0xA4A1`, carried because `lParam` has `CS_INSERTCHAR`). Forwarding it verbatim makes
  the Unicode EDIT read `0xA4A1` as `U+A4A1` (a wrong glyph; the HIMC still holds the real jamo, so
  a later repaint corrects it). FIX: convert `wParam` from `CP_ACP` to UTF-16 (`MultiByteToWideChar`,
  `0xA4A1 -> U+3131`) before relaying.
- (d) TRAP: do NOT gate the conversion on `IsWindowUnicode(m_hWnd)`. TableBox registers its class
  with `RegisterClassExW` + `DefWindowProcW`, so `IsWindowUnicode(m_hWnd)` is TRUE even in an MBCS
  build, while the ANSI `AfxWndProc` is still what delivered the CP949 `wParam` -- the gate skipped
  the needed conversion and the `?`/wrong-glyph regressed. Always convert; in a Unicode build the
  IME routes the first message straight to the EDIT (this relay branch is not reached), so it never
  runs on an already-UTF-16 `wParam`. Verified working in BOTH MBCS and Unicode `Debug|x64` builds.
- Do NOT re-post `WM_IME_STARTCOMPOSITION` to the EDIT: `SetFocus` already connected the IME, and an
  empty (0,0) re-start corrupts the composition (first jamo paints as `?`).
