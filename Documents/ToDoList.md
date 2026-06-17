# ToDoList - TableBox

Staged plan for the TableBox module (see Requirements.md sec 3 for the confirmed spec).
At the start of each session, find the first stage not marked DONE and continue there.

Convention reminders (full rules in CLAUDE.md): all `set_*` coordinates are 96 DPI
logical px; per-cell drawing goes through the overridable `draw_cell(CDC&, row, col, x0,y0,x1,y1)`
(back-buffer memory DC, physical px, x1/y1 exclusive); callback is read-only and supplies
ALL cells; single `CFont`. TableBox must not reference ConBox/FrameBox members.

## Stage 1 - Skeleton -- DONE

`TableBox : public CWnd` skeleton + double-buffered `OnPaint`, placed in the FrameBox demo
via `AddNew`. Verified: window appears with the expected client area.

## Stage 2 - Excel-like virtual grid render (CORE) -- DONE

Implemented: `open(parent,x0,y0,rows,cols)` (rows/cols = initial visible cell count, see
Requirements.md 3.2); `set_cols`/`set_rows` (uniform + vector)/`set_fixed`/`set_callback`/
`set_font`; per-cell clipped `draw_cell` (default: white/raised-header look + text) + grid
lines + overlay scrollbars (vertical + horizontal, auto-fade); cell-index based scrolling
with a no-overscroll clamp (last row/col aligns exactly with the body edge); DPI change
handled in `OnSize` (rebuild font, rows/cols stay fixed).
Verified by screenshot: grid renders with frozen header row/col, gridlines, and `[row:col]`
text; mouse-wheel vertical AND horizontal (Shift+wheel) scroll both move the body correctly
while the header stays fixed, with correct clamp/scrollbar-thumb position. The overlay
scrollbars were refined further: antialiased thumb/arrow buttons, an always-visible
background-contrasting gutter, arrow click = 1 row/col, track click = 3/4 page, the two bars
never both show in their shared corner (vertical wins by default), and the adaptive color
samples the actually-rendered body background (not an assumed white) so a `draw_cell`
override is still handled correctly. ConBox's overlay scrollbar was updated to the same
visual style for consistency. The cross-monitor DPI rescale was not separately exercised
(code path reuses the ConBox-proven OnSize pattern) -- worth a real check early in Stage 3
if anything looks off.

## Stage 3 - Selection & keyboard navigation (CORE) -- DONE

Implemented: `cur_row`/`cur_col` (focused cell) + `anchor_row`/`anchor_col` (range start);
`hit_test`/`select_range`/`move_current`/`ensure_visible`/`clamp_cursor`; `OnGetDlgCode`
(`DLGC_WANTARROWS|DLGC_WANTALLKEYS`) so `FrameBox` does not steal arrows/Enter/Esc; `OnKeyDown`
(arrows, PageUp/Down, Home/End, Ctrl+Home/End, Shift to extend); mouse drag range-select in the
body; header-band click selects the whole row/col. `cell_status(row,col)` (-1 fixed/0 normal/
1 selected) drives `draw_cell`'s background (see Requirements.md 3.6). `draw_focus_border()`
draws the green focus border as a final overlay AFTER grid lines, so it overwrites the gray
grid line itself instead of being overwritten by it. Fixed-cell rendering changed from the old
3D raised look to `RGB(236,233,210)` + a 1px white inset line on the top/left edge.
Refined after user review: the focus border only shows while the window has keyboard focus
(`OnSetFocus`/`OnKillFocus`; `cur_row`/`cur_col` are remembered across a focus loss so it
reappears in place); header-click whole-row/col selection places focus on the first body cell
without auto-scrolling to the far end. Verified by the user running the demo (build success +
interactive review across several rounds of fixes); no separate screenshot pass recorded.

## Stage 4 - Runtime column/row resize (CORE, vector form only) -- DONE

Implemented: header-band border drag-resize and double-click auto-fit, vector-form axes only
(`cols_uniform`/`rows_uniform` gate; see Requirements.md 3.7). `hit_col_border`/`hit_row_border`
find the border under the cursor (+-4 logical px); `OnSetCursor` switches to `IDC_SIZEWE`/
`IDC_SIZENS` over a border; `OnLButtonDown`/`OnMouseMove`/`OnLButtonUp` drive the live drag
(`SetCapture`, delta from drag-start size, 10 logical px minimum); `OnLButtonDblClk` auto-fits
via `GetTextExtentPoint32W` over every currently visible cell on that column/row.
Extended (per user request) to match Excel's selection-aware resize: `col_selection_span`/
`row_selection_span` find the dragged/double-clicked border's selection span (the single
column/row if it is not part of the current selection); drag sets every column/row in the span
to the SAME resulting size, double-click auto-fits each one in the span INDEPENDENTLY. Also
added: clicking the top-left corner cell selects the entire grid; a fresh header click now
starts a drag that extends the row/column selection across header cells (Excel-style header
drag-select) instead of being one-shot.
Verified: build + demo launch only; the actual drag/double-click/cursor interaction was not
exercised by the AI (screenshot-review territory) -- worth a user pass before Stage 5.

## Stage 5 - In-place editing (CORE) -- DONE

Implemented; see Requirements.md 3.8 for the confirmed spec. `set_edit_callback`,
`virtual edit_cell(row,col)`, the plain-text editor and `TableComboPopup` (owner-draw dropdown,
genuine `WS_POPUP`) are all in `TableBox.cpp`; `DemoApp.cpp` wires `set_edit_callback` back into
`table_text` and seeds a combo demo cell.
Final design (after several user tuning + redesign rounds): combo arrow U+25BC via cell font;
popup sized with AdjustWindowRectEx (client = cell_h*N exact); two-pass item paint; CS_DROPSHADOW;
`cancel_edit()` from OnMouseWheel/OnLButtonDown/OnSize; `set_align`/`set_pad`/`set_edit_adjust`;
mouse-over hand cursor on editable cells.
Two later reworks (this stage's spec was redesigned mid-development):
- `set_callback` -> `set_text_callback`; combo cells now identified by the dual-purpose
  `edit_cb(row,col,nullptr)` QUERY (item list) instead of a `"\x1B"`-prefixed `text_cb`; combo
  `text_cb` returns just the index string; `edit_cb` return type is `const char*`
  (query=type / commit=store). See Learned.md 5.6, 5.8.
- Hangul IME: the plain-text editor is now a raw Unicode EDIT + Win32 `EditSubclassProc` (not an
  MFC CEdit), with the first stray `WM_IME_COMPOSITION` relayed CP949->UTF-16. Verified in BOTH
  MBCS and Unicode Debug|x64. See Learned.md 5.5, 5.9, 5.10.

## Stage 6 - Clipboard (OPTIONAL) -- DROPPED

Decided not to implement (user decision). Clipboard copy/paste is out of scope for TableBox.

## Stage 7 - Polish (CORE) -- DONE

- [DONE] Excel-style header highlighting.
- [DONE] Grid line color: `set_grid_color(r,g,b)` added; default RGB(191,191,191) unchanged.
  Background-contrast dynamic logic was evaluated and deferred (user controls color explicitly).
- [DONE] Performance: `OnTimer` now skips `Invalidate` when alpha is unchanged (hold phase).
  Other candidates (`last_visible_*` caching, SaveDC loop) reviewed and not worth the complexity.
- [DONE] Demo integration: verified by user.
