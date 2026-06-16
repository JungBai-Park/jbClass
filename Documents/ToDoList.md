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

## Stage 3+ - Proposed (to confirm/refine after Stage 2)

Order reflects dependencies; CORE vs OPTIONAL noted. Re-confirm scope after Stage 2 results.

- Stage 3 (CORE) - Selection & keyboard navigation: focus/current cell, arrow/PageUp-Down/
  Home/End/Ctrl moves, auto-scroll to keep the target visible, mouse click + drag range select,
  fixed-header click selects whole row/col, Excel-like selection highlight (default `draw_cell`
  reflects selection; overriders may ignore).
- Stage 4 (CORE, vector form only) - Runtime column/row resize: 8px border hit-zone + cursor
  change + drag to resize (FrameBox hit-zone style); ignored for uniform-form grids.
  Optional: double-click border = auto-fit to measured text width.
- Stage 5 (CORE) - In-place editing: double-click / Enter / F2 -> overlaid `CEdit`; write path
  separate from the read-only callback via `set_edit_callback(row,col,text,user)`; ESC cancel,
  Enter commit+down, Tab commit+right.
- Stage 6 (OPTIONAL) - Clipboard: Ctrl+C copies the selection as tab/newline-delimited text
  (Excel paste-compatible); optional Ctrl+V via the edit-callback path.
- Stage 7 (CORE) - Polish: background-color-driven grid/header contrast (scrollbar contrast
  already done in Stage 2), visible-cell + clipping performance pass (minimize callback calls),
  demo integration, doc updates (Requirements/Learned). NOTE: DPI handling is intentionally NOT
  here (folded into Stage 2).
