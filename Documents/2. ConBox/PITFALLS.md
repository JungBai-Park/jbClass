## ConBox

### 1. Font Measurement and Korean Matching

- The final measured height for the Korean matching font must be handled at the end of `calc_cell_size`.
- Do not build the Korean matching font early in `set_kfont`.
- If it is built before font match information is available, the GDI object is created with a default height of 0 and Korean glyphs render too small compared with English 2x output.
- In match mode (`set_kfont` size <= 0), `kfont_lf.lfHeight` is 0 at `set_kfont` call time, so the bold/italic/bold_italic variants built there also have `lfHeight=0` (GDI default = too large, overlaps adjacent lines). `calc_cell_size` corrects `kfont` but does NOT propagate the matched height back to the variants. Fix: at the end of `calc_cell_size`, after all font rebuilds, if `kfont_match_efont`, call `GetObjectW(kfont, &kfont_lf)` then rebuild the three variants via `MakeFontVariant`. The double variant (`kfont_double`) already uses `GetObjectW` internally via `MakeDoubleFont` so it was never affected.

### 2. SGR Double-Size Rendering

- Use standard SGR 8 for double-size on and SGR 28 for double-size off.
- Do not invent a private control code for this feature because conhost may drop unknown private sequences.
- Double-size glyphs bleed pixels into cells on the right.
- Paint each row from right to left so adjacent clear operations do not erase the bleed pixels and leave visual artifacts.

### 3. Cursor and Korean IME Commit Order

- If a navigation key or Enter is received while Korean IME composition is active, call `ImmNotifyIME(CPS_COMPLETE)` first.
- Send the special key only after the committed text is delivered.
- The child process must receive input in this order: committed text, then movement or control key.
- After Korean syllable commit, the emulation path advances the cursor one cell to the right.
- To compensate, swallow right-arrow input when needed and send the left-arrow sequence twice for left-arrow correction.

### 4. EMF Export Pitfalls

- In an EMF DC, `BeginPath` and `FillPath` do not reliably wrap `TextOutW`.
- If path-based filling is used, text can appear as black blocks.
- Set the foreground color with `SetTextColor(fg)` and draw text directly.
- Pass `NULL` as the media bounds rectangle pointer to `CreateEnhMetaFile` so GDI fits the output bounds tightly.
- If a `CDC` is attached to a target HDC, call `Detach()` before closing or destroying the HDC. Otherwise the CDC destructor can destroy the handle at the wrong time.

### 5. UTF-8 Chunk Boundaries

- Data from an external process pipe can split a multibyte UTF-8 character across chunks.
- Keep the incomplete trailing bytes and their length in temporary members such as `utf8_tail` and `utf8_tail_len`.
- Prefix the saved tail to the next chunk before decoding.
- Without this, display output and copied text can be corrupted.

### 6. ConPTY IND/NEL Behavior and Resize Limits

- If ESC D (IND) or ESC E (NEL) appears to do nothing, do not assume a terminal parser bug first.
- Microsoft ConPTY can intercept those control streams and rewrite them into normal cursor positioning sequences such as CUP before delivery.
- Runtime resize intentionally uses truncation and padding instead of reflow to keep the implementation lightweight.
- Avoid manual resize during a running shell session when preserving visual layout matters. Restart the shell if a clean layout is required.

### 7. Keyboard Input Not Working in ConBox

- `CWnd`-derived custom windows do not receive focus automatically on click (unlike standard controls such as `CEdit`).
- `OnLButtonDown` must call `SetFocus()` explicitly so the window can receive keyboard input.
- `FrameBox::PreTranslateMessage` unconditionally consumes `VK_RETURN` and `VK_ESCAPE` even in normal (non-edit) mode. A window that needs these keys (e.g. ConBox returning `DLGC_WANTALLKEYS`) will never receive them unless a guard is added.
- Fix: check `focus->SendMessage(WM_GETDLGCODE) & DLGC_WANTALLKEYS` before the ESC/Enter intercept and yield to `CWnd::PreTranslateMessage` when true.
- TRAP: the `editing_hwnd()` guard in `PreTranslateMessage` only yields to the Parasite subclass proc during control repositioning. It does NOT mean ESC/Enter are consumed only in edit mode -- they are always consumed in normal mode too (except for `DLGC_WANTALLKEYS` windows).

### 8. DemoApp Exit Code and MFC Memory Leak Message

- `CWinApp::ExitInstance()` returns `AfxGetCurrentMessage()->wParam` -- the last pumped message's wParam. When `InitInstance` drives its own modal loop and returns `FALSE`, this yields a meaningless non-zero exit code. Override `ExitInstance()` to `return 0` explicitly.
- "Detected memory leaks! Dumping objects -> Object dump complete." with an EMPTY dump:
  - The header fires when the `_NORMAL_BLOCK` count differs between the initial snapshot and shutdown, but `_CrtMemDumpAllObjectsSince` only prints `_CLIENT_BLOCK` (`DEBUG_NEW`) objects. An empty dump means no user-code leaks -- the difference is in `_NORMAL_BLOCK` (raw `::operator new`, e.g. STL allocators).
  - ROOT CAUSE (confirmed, not an unfixable MFC quirk): a global/static `CWnd`-derived object (e.g. a global `ConBox`) holds STL members (`std::deque`/`vector`/`string`: scrollback, screen grid, buffers). A global object's destructor runs at STATIC DESTRUCTION, which is AFTER the CRT leak snapshot. So those STL allocations are still live at snapshot time -> `_NORMAL_BLOCK` count differs -> header. They are not `_CLIENT_BLOCK`, so the dump is empty. Symptom matches exactly.
  - Calling `DestroyWindow()` on the global does NOT fix it: that frees only the HWND, not the C++ object's STL members (which live until static destruction).
  - FIX (implemented): do not keep such objects global. `new` them and let `~FrameBox()` `delete` them inside the host loop -- attach external windows via `AddNew(...)` and the root frame via `new` + `delete Top` before `InitInstance` returns (see 3.3) -- so all heap members are freed BEFORE the snapshot. This removes the warning entirely (verified: clean build + run, exit code 0, no leak dump). Prefer this over suppressing the check (`_CrtSetDbgFlag`), which would also hide real leaks.

### 9. Per-Monitor DPI: Preserve the Grid, Do Not Resize the PTY

- A DPI change must rescale only the cell pixels (fonts) and KEEP cols/rows fixed. The grid is a
  logical entity; if `update_metrics` recomputes cols/rows from the new window pixels on a DPI change
  and calls `resize_sink` -> `ResizePseudoConsole`, the child shell RE-EMITS its visible screen
  (ConPTY reflow, see 4.6). Our terminal then appended it -> scrollback duplicated/grew on every
  monitor switch; an intermediate wrong-size recompute also truncated content (lost lines).
- `build_font` runs BEFORE the window exists (set_efont/set_kfont via setup_from_ini, and the
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
