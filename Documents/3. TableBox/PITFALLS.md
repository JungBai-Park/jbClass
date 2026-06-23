## TableBox

### 1. Grid Axis Invariant: col_widths.empty() == Uniform, col_count Is Always Valid

Two invariant rules govern `col_widths`/`row_heights` and `col_count`/`row_count` -- breaking
either corrupts all cell geometry silently:

1. `col_widths.empty()` signals the uniform column axis (set by `set_cols(int,int)` or ctor);
   `row_heights.empty()` signals the uniform row axis. Do NOT push/assign to the vector without
   also switching the axis to non-uniform; do NOT clear the vector without intending to restore
   the uniform path.
2. `col_count` is the single source of truth for the total column count in ALL cases (both paths).
   `set_cols(initializer_list)` must update `col_count = col_widths.size()` after building the
   vector; the uniform overload must update `col_count = limit`. Same rule for `row_count`.

Cell-size access pattern (enforced throughout the code):
  `c < (int)col_widths.size() ? col_widths[c] : col_uniform_w`
Out-of-range index returns `col_uniform_w` as a soft fallback (intentional, not a bug guard).

### 2. std::min/std::max Break Under windows.h

- `afxwin.h` pulls in `windows.h`, which (without `NOMINMAX`) defines `min`/`max` as macros.
  `std::min(...)`/`std::max(...)` then fail to compile (`std::` followed by an expanded macro
  is a syntax error: `C2589`/`C2059`), not a clean "ambiguous call" error.
- This project does not define `NOMINMAX`. Use a manual ternary (`a < b ? a : b`) instead of
  `std::min`/`std::max` in any new code; do not add `<algorithm>` for this purpose.

### 2. draw_grid_lines Runs AFTER draw_cell and Owns the Shared Boundary Pixel

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

### 3. Per-Zone Cursor Needs ON_WM_SETCURSOR, Not a SetCursor Call in OnMouseMove

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

### 4. In-Place Editing: SetFocus() Must Not Be Called From Inside the WM_KILLFOCUS Path

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

### 5. Single-Line EDIT: Text Is Top-Aligned, EM_SETRECT Has No Effect

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

### 6. Combo Cell Type Comes From edit_cb, Not From text_cb

- Combo cells are identified by the dual-purpose `edit_cb(row,col,nullptr)` QUERY return (a
  comma-separated item list), NOT by a `"\x1B"`-prefixed `text_cb` value (that old encoding is
  gone). `text_cb` for a combo cell returns just the selected index string (`"0"`, `"1"`, ...).
  Static helpers: `ParseComboItems(str, items)` (splits the list, trims spaces, `'\b'` -> literal
  comma) and `ComboIndex(idx_text, max)` (clamped 0-based index from `text_cb`).
- Any code that DISPLAYS or MEASURES a combo cell (`draw_cell`, `autofit_col`/`autofit_row`) must
  resolve the label as `items[ComboIndex(cell_text(...), items.size())]`, never the raw text.
  The old `ComboDisplayLabel(raw)` helper (which decoded the `\x1B` spec) is gone; the logic is
  now inline in each of those sites guarded by an `edit_cb != nullptr` + `ParseComboItems` check.

### 7. Combo Popup Window Size: WS_BORDER Shrinks Client Area

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

### 8. Combo Popup External Cancel: Prevent Double-Destroy via ending_edit

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

### 9. Combo Popup Self-Commit: A `closing` Flag Stops WA_INACTIVE From Eating the Commit

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

### 10. Hangul IME In-Place Edit: Use a Raw Unicode EDIT + Win32 Subclass, Not an MFC CEdit

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

## 6. Cross-Module Zoom (WM_JBZOOM)

### 6.1 WM_JBZOOM Must Be Sent BEFORE rescale_children() / MoveWindow

- ConBox needs `zoom_pm` set and `zoom_resize = true` BEFORE `MoveWindow` triggers `OnSize`.
  `OnSize` reads `zoom_resize` at entry to decide between `relayout_for_dpi` (zoom/DPI path,
  no PTY resize) and `update_metrics` (user-resize path, may resize PTY). If `WM_JBZOOM` arrives
  after `MoveWindow`, `OnSize` already ran with `zoom_resize = false` and took the wrong path.
- Same principle for TableBox: `build_font()` inside `WM_JBZOOM` uses the new `eff_dpi()` so the
  font is correct when `OnSize` → `Invalidate` runs.
- Rule: always send `WM_JBZOOM` to all children BEFORE calling `rescale_children()`.

### 6.2 snap_to_grid Is Synchronous -- fit_to_children Reads the Final Size Immediately

- ConBox's `snap_to_grid()` calls `SetWindowPos(m_hWnd, ...)` which, for a same-thread window,
  runs synchronously and delivers `WM_SIZE` → `OnSize` before returning. This entire chain
  executes INSIDE the single `MoveWindow(ConBox)` call in `rescale_children()`.
- When `rescale_children()` returns, ConBox already has its final post-snap window size.
  `fit_to_children()` (called at the end of `rescale_children`) can therefore read the actual
  `GetWindowRect` of each child and compute the correct FrameBox size immediately -- no deferred
  message, timer, or callback is needed.

### 6.3 ConBox zoom_resize Flag: Why It Is Needed

- A zoom-triggered `OnSize` has `cur == box_dpi` (no real monitor DPI change), so the existing
  `cur != box_dpi` guard in `OnSize` would fall through to `update_metrics()` → potential PTY
  resize and grid recompute.
- `WM_JBZOOM` handler sets `zoom_resize = true` before the `MoveWindow` that fires `OnSize`.
  `OnSize` checks `dpi_changed || zoom_resize`; if either is true, clears `zoom_resize` and
  takes the `relayout_for_dpi` + `snap_to_grid` path (no PTY resize).

### 6.4 apply_zoom Must Be virtual+protected for Subclass Title-Bar Hooks

- Ctrl+Wheel calls `apply_zoom()` directly (not via a message), so subclasses cannot intercept
  it through `WindowProc`. Making `apply_zoom` `virtual protected` lets a subclass override it:
  call `FrameBox::apply_zoom(new_pm, anchor)` then update a title bar or status indicator.
- `dpi` and `zoom_pm` are also `protected` (not `private`) so the override can read them.

### 6.5 Naming Unification: ConBox build_font / to_px

- ConBox previously used `make_font` (TableBox uses `build_font`) and `sbar_px` (TableBox uses
  `to_px`) for the same concepts. Renamed to `build_font` / `to_px` in ConBox to match TableBox.
- If divergence reappears in future sessions, the canonical names are `build_font` and `to_px`.
