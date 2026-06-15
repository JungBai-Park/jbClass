# Per-Monitor V2 DPI Refactoring Plan

## Objective

Make `FrameBox` and `cConBox` fully Per-Monitor V2 DPI-aware so the application
renders correctly at any DPI setting and adapts dynamically when the window is
moved between monitors with different DPI.

Target API: `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2` (requires Windows 10 1703+).
Technical background: `Documents/DPI.md`.

---

## Decisions Made (binding for all sessions)

### D1. Coordinate convention in source code
All coordinates stored in source code (by the `Add*`/`OpenFrame` macros and `LayOutRewrite`)
are **96 DPI logical pixels**.

- **At window/control creation:** scale to physical pixels via `MulDiv(coord, current_dpi, 96)`.
- **At ReWrite commit:** convert back to logical via `MulDiv(phys, 96, current_dpi)` before writing.
- **Existing source coordinates** are already 96 DPI-based (the project was DPI-unaware before this
  work); no migration of existing call sites is needed.
- **User guideline:** prefer performing ReWrite at 96 DPI. Other DPI settings produce integer
  rounding but are still usable.

### D2. DPI activation method
Embed a manifest file (`jbClass.manifest`) declaring `PerMonitorV2` and reference it via
`<Manifest><AdditionalManifestFiles>` in vcxproj. The manifest is merged and embedded into
the EXE at link time; no runtime file is needed.

**Do NOT use the code-based API** (`SetProcessDpiAwarenessContext` in `InitInstance` or any
global constructor). When `UseOfMfc=Static`, the MFC CRT initializer calls `SetProcessDPIAware()`
(System-aware) before any user code runs, including global object constructors. All subsequent
API calls to change awareness fail with LastError=5. Only the manifest (read by the OS loader
before process creation) can override this. See `Learned.md §1.4` for diagnostics.

### D3. Edit-mode movement granularity
Arrow keys move/resize in **5 physical pixels** (current behavior, unchanged).
No logical-pixel snapping is applied on commit; rounding is acceptable per D1 guideline.

### D4. `Parasite::last_rect` — redefined and moved
`last_rect` is moved **out of `#ifdef _DEBUG`** (it is needed in release builds for DPI
repositioning). Its semantics change to: **stores the 96 DPI logical rect of the control**.

- **Edit entry:** `last_rect = MulDiv(GetWindowRect(), 96, current_dpi)` (physical → logical).
- **ESC cancel:** `MoveWindow(MulDiv(last_rect, current_dpi, 96))` (logical → physical).
- **WM_DPICHANGED:** if a control is in edit mode, cancel edit first (restore via `last_rect`),
  then proceed with DPI repositioning.
- **FrameBox DPI reposition:** `MoveWindow(MulDiv(layout->last_rect, new_dpi, 96))` for each
  registered child.
- `finish_child` stores `layout->last_rect = CRect(x0, y0, x1, y1)` (raw logical coords from
  the `Add*` macro arguments) before scaling the control to physical pixels.

The remaining debug-only members (`editing`, `drag_edge`, `file`, `line`) stay inside `#ifdef _DEBUG`.

### D5. `cConBox` font size unit
`set_efont(name, size)` and `set_kfont(name, size)` accept **points (pt)**.
`kfont size == 0` means auto-match: Korean font height is forced to match the English font height.
`cConBox` must store the original pt values in members so fonts can be rebuilt without
re-calling from user code when DPI changes.

### D6. System font delivery
`FrameBox` retrieves the DPI-correct system font via
`SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)`
and sends `ncm.lfMessageFont` via `WM_SETFONT` to each registered child control.
This must happen both at creation (Step 2) and on every DPI change (Step 3).

---

## Technical Reference (from DPI.md — key facts only)

- `WM_DPICHANGED`: sent to top-level and `WS_POPUP` windows.
  `wParam` = new DPI; `lParam` = `RECT*` OS-suggested rect → use with `SetWindowPos`.
- `WM_DPICHANGED_BEFOREPARENT` / `WM_DPICHANGED_AFTERPARENT`: sent to `WS_CHILD` windows.
  Both have `wParam`/`lParam` == 0; query DPI via `::GetDpiForWindow(m_hWnd)`.
  BEFORE = rebuild fonts; AFTER = reposition (parent already resized).
- `WS_POPUP` frames (`AddFrame` sub-dialogs) receive their own `WM_DPICHANGED` independently
  from the owner window (each must handle it in its own `WindowProc`).
- `GetDeviceCaps(hdc, LOGPIXELSY)` returns **primary monitor DPI only** — never use for
  per-monitor font sizing. Use `::GetDpiForWindow(m_hWnd)` (Windows 10 1607+).
- Scrollbar pixel constants must scale: `MulDiv(base_px, new_dpi, 96)`.

---

## Progress

| Step | Description | Status |
|------|-------------|--------|
| 1 | Activate Per-Monitor V2; show DPI in title bar | [x] Complete |
| 2 | Scale window and controls to match startup DPI | [x] Complete |
| 3 | Handle WM_DPICHANGED for live monitor switching | [ ] Not started |
| 4 | DPI-correct ReWrite (logical coord output) | [ ] Not started |
| 5 | cConBox DPI support | [ ] Not started |

---

## Step 1 — Activate Per-Monitor V2, verify DPI notification [x] COMPLETE

**What was done:**
- Created `Project/jbClass.manifest` with `<dpiAwareness>PerMonitorV2</dpiAwareness>`.
- Added `<Manifest><AdditionalManifestFiles>jbClass.manifest</AdditionalManifestFiles></Manifest>`
  to `Project/jbClass.vcxproj` (unconditional `ItemDefinitionGroup`, covers all configs).
- `cDemoBox::WindowProc` (in `DemoApp.cpp`) handles `WM_DPICHANGED`: updates title bar to
  show current DPI. `FrameBox::WindowProc` has no DPI handler yet (Step 3).
- `DemoMain()` shows initial DPI in title bar via `GetDpiForWindow` after window creation.
- Removed the 500ms timer and tick counter from `DemoMain`.
- Code-based API approach was attempted and failed — see `Learned.md §1.4`.

---

## Step 2 — Scale window and controls to match startup DPI [x] COMPLETE

**What was done:**
- `Parasite::last_rect` moved out of `#ifdef _DEBUG`; semantics redefined to 96 DPI logical rect.
  `enter_edit()` converts physical → logical; ESC cancel converts logical → physical.
- `FrameBox` gained `int dpi` (default 96) and `HFONT sys_font` members.
- Added `make_dpi_font(int)` helper using `SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS)`.
- `open_core()` determines target monitor DPI via `MonitorFromPoint` + `GetDpiForMonitor`
  BEFORE `CreateWindowExW`, then scales the window rect with `SetWindowPos` if `dpi != 96`.
  (`GetDpiForWindow` after creation was unreliable for owned popups on a different-DPI monitor —
  see `Learned.md §3.9`.)
- `finish_child()` stores raw logical coords in `layout->last_rect`, scales child to physical
  pixels via `MulDiv`, and sends `sys_font` via `WM_SETFONT`.
- `attach_external()` (AddAsItIs/AddNew) handles attach-only mode by converting current
  physical rect to logical for `last_rect`; non-attach mode scales coords to physical.
- Child `FrameBox` frames (`make_child`) inherit parent `dpi` and get their own `sys_font`.
- Added `#include <shellscalingapi.h>` and `#pragma comment(lib, "Shcore.lib")` to `FrameBox.cpp`.
- `DemoApp.cpp`: added `cDemoSub` class (FrameBox subclass) with `update_title()` showing
  current DPI and WM_DPICHANGED count; both main and sub frames display this in their title bars.

---

## Step 3 — Handle WM_DPICHANGED for live monitor switching [ ]

**Scope:** `Source/FrameBox.cpp`.
**Prerequisite:** Step 2 complete.
**Note:** ConBox is NOT changed in this step. ConBox window is resized by FrameBox (same as Step 2).

**Tasks:**

1. **`FrameBox::WindowProc` — replace Step 1's `WM_DPICHANGED` stub:**
   - If `Parasite::active() != nullptr` (a control is being edited): call `leave_edit(false)` to
     cancel and restore (this uses the logical `last_rect` already).
   - `dpi = (int)LOWORD(wParam);`
   - `SetWindowPos(nullptr, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOZORDER|SWP_NOACTIVATE);`
     where `r = reinterpret_cast<RECT*>(lParam)`.
   - Rebuild system font at new DPI (D6).
   - Iterate child registry: for each entry where `layout != nullptr`:
     `child->MoveWindow(MulDiv(layout->last_rect, dpi, 96));`
   - Send new font via `WM_SETFONT` to each registered control.
   - `Invalidate();`
   - Title bar update (from Step 1) can remain or be removed here.
   - Return 0 (do not call `DefWindowProc`).

2. **`FrameBox::WindowProc` — add `WM_DPICHANGED_AFTERPARENT`** (for `WS_CHILD` zone frames):
   - `int new_dpi = ::GetDpiForWindow(m_hWnd);`
   - `dpi = new_dpi;`
   - Resize self using own logical rect (stored in the parent registry's `layout->last_rect`).
     Because the zone's `Parasite` is in the parent's registry, the zone's `FrameBox` needs to
     resize itself: query its own logical rect from `layout->last_rect` via the Parasite pointer,
     or store it in its own member. Simplest: each zone's FrameBox stores its own logical rect in
     a member `CRect self_logic_rect` set at creation.
   - Reposition all own children as in task 1 above.
   - `Invalidate();`

**Expected result:**
- Drag window from 96 DPI monitor to 150 DPI monitor while app is running.
- Window resizes to OS-suggested dimensions; all controls reposition and resize proportionally;
  fonts rescale.

**Verification:** Move window between two monitors with different DPI settings while app runs.

---

## Step 4 — DPI-correct ReWrite [ ]

**Scope:** `Source/FrameBox.cpp` (`LayOutRewrite` and `leave_edit` commit path).
**Prerequisite:** Step 3 complete.

**Tasks:**

1. **Add `int rewrite_dpi` parameter to `LayOutRewrite(file, line, rect, rewrite_dpi)`.**

2. **In `Parasite::leave_edit(commit=true)`:**
   - `int current_dpi = ::GetDpiForWindow(target->m_hWnd);`
   - Pass `current_dpi` to `LayOutRewrite`.

3. **Inside `LayOutRewrite`, before assembling the replacement string:**
   - `CRect logical = MulDiv(phys_rect, 96, rewrite_dpi);`
   - Write `logical` coordinates to source instead of `phys_rect`.

4. **Update `last_rect` in `leave_edit` commit path:**
   - After rewrite: `layout->last_rect = logical;` (keep in sync).

**Expected result:**
- Edit a control at 150% DPI (144 DPI), move it right 5 physical pixels, commit.
- Source file shows the x0 increased by `MulDiv(5, 96, 144)` = 3 (rounded).
- Rebuild source and run at 96 DPI: control is 3 logical pixels further right.

**Verification:** At a non-96 DPI monitor, move a control and commit. Inspect source. Rebuild.

---

## Step 5 — cConBox DPI support [ ]

**Scope:** `Source/ConBox.h`, `Source/ConBox.cpp`.
**Prerequisite:** Step 3 complete.

**Tasks:**

1. **Fix `make_font()` (or wherever font DPI is queried):**
   - Replace `::GetDeviceCaps(hdc, LOGPIXELSY)` with `::GetDpiForWindow(m_hWnd)`.

2. **Store original pt values in `cConBox` members:**
   - Add `int efont_pt`, `std::string efont_name` (and kfont equivalents).
   - `set_efont` / `set_kfont` store their arguments before building fonts.

3. **Handle `WM_DPICHANGED_AFTERPARENT`** (ConBox is WS_CHILD, registered via `AddNew`):
   - `int new_dpi = ::GetDpiForWindow(m_hWnd);`
   - Rebuild English font: re-call `set_efont(efont_name, efont_pt)` (now uses `GetDpiForWindow` internally).
   - Rebuild Korean font: re-call `set_kfont(kfont_name, kfont_pt)` (0 = auto-match preserved).
   - Rescale overlay scrollbar constants: `SBAR_W` etc. → `MulDiv(BASE_SBAR_W, new_dpi, 96)`.
     Store base (96 DPI) constants separately so rescaling is always from the same origin.
   - `Invalidate();`

4. **If ConBox can be top-level or popup** (handle `WM_DPICHANGED` as well, same logic).

**Expected result:**
- On a high-DPI monitor, ConBox text is legible (not tiny).
- Moving the host FrameBox to a different-DPI monitor: ConBox font and scrollbar rescale dynamically.

**Verification:** Place a ConBox in a FrameBox. Move between monitors. Confirm text size and
scrollbar width scale proportionally.
