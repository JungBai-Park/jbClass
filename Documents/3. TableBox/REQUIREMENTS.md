## TableBox

### 1. Purpose

- `TableBox` is a `CWnd`-derived, Excel-like grid control supporting two data modes:
  - Virtual mode (default): owns no data; requests only the visible cells through a `(row, col)` callback. Suited to large/dynamic data.
  - Owned mode (`alloc_text`): the table stores every cell's UTF-8 string itself, for small fixed tables (intended for < 100 cols x < 27 rows). Lower entry barrier -- no text callback required.
- Designed for reuse and customization by arbitrary projects (maximum flexibility), not project-specific.
- Self-contained portability unit: `TableBox.h` + `TableBox.cpp`. It must NOT reference `ConBox` / `FrameBox` members; duplicate the needed logic in-module even if code repeats (overlay scrollbar, font argument parsing).

### 2. Grid Definition (public API)

- `open(CWnd* parent, int x0, int y0, int rows, int cols)`: creates the child window at `(x0,y0)`. `rows`/`cols` are the INITIAL VISIBLE cell count, not the grid's total row/col count, and are clamped to it. Call `set_cols`/`set_rows`/`set_fixed`/`set_text_callback`/`set_font` before `open()` so the first paint already has metrics ready.
- `set_cols(int width, int limit)`: uniform column width with `limit` columns. No vector allocation; `limit` can be very large (thousands).
- `set_cols(std::initializer_list<int> pattern, int count = 1)`: repeats `pattern` `count` times to build the column-width vector. `count=1` (default) uses the pattern as-is.
- `set_rows(int height, int limit)` / `set_rows(std::initializer_list<int> pattern, int count = 1)`: same for rows.
- Before any `set_cols`/`set_rows` call, the grid defaults to uniform 75x20 (96 DPI logical) cells, 5 columns x 20 rows, so `open()` works even if the host never calls them.
- `set_fixed(int rows, int cols)`: frozen header rows/cols (default `1, 1`), Excel-style (stay fixed while the body scrolls).
- `set_text_callback(const char* (*cb)(int row, int col, void* context), void* context = nullptr)`: text source for ALL cells (fixed and normal alike). `row`/`col` are 0-based. The returned `const char*` is UTF-8 and is consumed (copied) immediately during drawing, never stored, so a reused static buffer is allowed. `context` is the opaque pointer registered here and passed back on every call; when omitted (nullptr) it defaults to `this`. For a combo (dropdown) cell the value is just the selected index as a decimal string (`"0"`, `"1"`, ...); the item labels come from `set_edit_callback` (see 3.8).
- Owned data mode (alternative to `set_text_callback`):
  - `col_count` / `row_count` are public (single source of truth for both modes); set them via `set_cols`/`set_rows`, not by direct assignment.
  - `std::string* alloc_text()`: enters owned mode. (Re)allocates a `col_count*row_count` 1-D string array (index = `row*col_count + col`), pre-fills the header band (row 0 = `"","A".."Z"` for cols 1..26, col >= 27 blank; col 0 = `"","1","2",...`), and returns the buffer start. Call `set_cols`/`set_rows` first so the size is known. In owned mode `text_cb` is ignored. Editing also works WITHOUT an `edit_cb`: every non-fixed cell is a plain CEdit cell and commits write directly into the owned buffer (an `edit_cb`, if set, is consulted for cell type as usual and takes precedence). Resizing the grid (`set_cols`/`set_rows`) in owned mode reallocates the buffer and re-fills headers (old content discarded).
  - `std::string& cell(int row, int col)`: owned-mode read/write access. Out-of-range index asserts (Debug) then clamps into range -- an out-of-range write deliberately hits the wrong cell so the mistake surfaces. Must not be called in virtual mode.
- `set_font(const char* name, float size, const char* option = nullptr)`: single `CFont`. Borrows ONLY ConBox's argument grammar (name / size in points / option attribute string such as B/I/U). No English/Korean split and no height matching (TableBox cell widths are explicit, so the monospace cell logic does not apply).
- `set_align(int num)`: body-cell text alignment, numpad layout (1=top-left ... 5=center ... 9=bot-right); does not affect fixed/header cells. Default 4 (mid-left). Combo labels follow it too.
- `set_pad(int logical_px)`: inner left/right text padding (96 DPI logical px) inside every cell, and the gap before a combo cell's arrow. Default 4.
- `set_edit_adjust(int dx0, int dy0, int dx1, int dy1)`: inset offsets (96 DPI logical px) for the in-place edit box rect `(x0+dx0, y0+dy0, x1-dx1, y1-dy1)`. Use `dy0`/`dy1` to vertically place the box (a single-line edit cannot center text); horizontal alignment follows `set_align`. Default `(1,1,0,0)`.

### 3. Rendering

- Double-buffered: cells are drawn into a back-buffer memory DC, then `BitBlt`ed to screen in `OnPaint`.
- Per-cell drawing goes through an overridable virtual:
  `virtual void draw_cell(CDC& dc, int row, int col, int x0, int y0, int x1, int y1)`.
  - `dc` is the back-buffer memory DC.
  - Coordinates are client-area PHYSICAL pixels (DPI already applied). `x1`/`y1` are exclusive (RECT convention).
  - The default implementation calls the callback and renders text: normal cells on white, fixed cells with a tan background (`RGB(236,233,210)`) and a 1px white inset line on the top/left edge, selected-range cells with a light blue tint (see 3.6). Subclasses may override to fully replace cell rendering or delegate to the base.
- Excel-style header highlighting: a column header cell (row < fixed_rows, col >= fixed_cols) is highlighted when `col` falls inside `[min(anchor_col,cur_col)..max(anchor_col,cur_col)]`; a row header cell (col < fixed_cols, row >= fixed_rows) similarly for the row axis. Highlighted headers use `RGB(224,221,200)` background and `RGB(128,128,128)` inset line (vs. default tan background and white inset). Corner cells (both row and col inside the fixed band) are never highlighted.
- Only visible cells are drawn. A partially visible cell IS drawn, with its FULL logical rect; the paint loop applies `IntersectClipRect(x0,y0,x1,y1)` before each `draw_cell` so overflow is clipped automatically (overriders always draw the whole cell).
- Grid lines are drawn by the paint loop AFTER all cells (not by `draw_cell`), so overrides need not redraw borders and normal cells always keep their grid.
- A protected helper `cell_text(row, col)` wraps the callback for overrides that only need the text.

### 4. Layout and Scrollbars

- All coordinates/sizes passed to `set_*` are 96 DPI logical pixels, scaled to physical at the window DPI.
- When the whole grid is smaller than the client area, it is top-left aligned; the leftover area is filled dark gray with no grid lines.
- Custom dynamic overlay scrollbars on BOTH the vertical and horizontal axes (re-implemented in-module, not referencing ConBox). They auto-show then fade with an antialiased rounded thumb and arrow-button shapes, scale with DPI, and adapt their color to the body's actual rendered background (since `draw_cell` is virtual and an override may not paint white).
- Each bar has arrow buttons at both ends that move one row/col per click; clicking the empty track between the thumb and an arrow pages by 3/4 of the visible extent.
- The vertical and horizontal bars never both show at once: in the shared corner where their hit zones overlap, whichever bar is already showing keeps the interaction; vertical wins when neither (or both) is showing.

### 5. DPI Awareness and Zoom

- Per-monitor DPI aware. The initial render uses the creating monitor's DPI; a runtime DPI change (window moved to a different-DPI monitor) rebuilds the font and recomputes cell pixels.
- The DPI change is handled in `OnSize` by comparing `GetDpiForWindow` to the stored DPI (same pattern as ConBox). Because rows/cols are fixed by `set_cols`/`set_rows` and there is no child process, the grid is simply re-rendered at the new DPI; ConBox's "preserve grid, do not resize the PTY" constraint does not apply.
- **Zoom via `WM_JBZOOM`**: On receipt, TableBox sets `zoom_pm`, cancels any active in-place editor, calls `build_font()` (which uses `eff_dpi() = MulDiv(box_dpi, zoom_pm, 1000)`), clamps scroll, and invalidates. The subsequent `MoveWindow` from FrameBox's `rescale_children` fires `OnSize`, which redraws all cell geometry through `to_px()` at the new effective DPI.

### 6. Selection and Keyboard Navigation

- Focused cell: the single cell that arrow keys move and that owns the green border. Set by a body-cell click, then movable by Left/Right/Up/Down, PageUp/PageDown (one screenful of body rows), Home/End (first/last column of its row), Ctrl+Home/Ctrl+End (first/last body cell).
- Selected cells: the cells inside the rectangular range defined by Shift+arrow keys or a mouse drag (anchor to focused cell). A 1x1 range (just the focused cell, no drag/Shift-extend) is NOT considered "selected".
- `int cell_status(int row, int col) const` reports which of 3 cases a cell falls into, for rendering: `-1` fixed (header), `0` normal, `1` selected (a real, more-than-1-cell range only).
- When a real selection range exists, the focused cell renders exactly like any other selected cell (selection background), with the green border drawn on top of it -- it is not exempted into a separate plain look.
- The green focus border is visible only while the `TableBox` window itself has keyboard focus. Losing focus hides it; the focused cell position is remembered and the border reappears there when focus returns.
- Clicking a fixed column header selects that whole column; clicking a fixed row header selects that whole row. Either selects the full row/col range but places the focused cell at the FIRST body cell (right after the fixed header) and does NOT auto-scroll the viewport to it. A following arrow key scrolls normally from there.
- A fresh (non-Shift) header click also starts a drag: moving the mouse to another header cell while still pressed extends the selection across every column/row in between (Excel-style header drag-select), still without auto-scrolling.
- Clicking the top-left corner cell (intersection of the fixed row and fixed column band) selects the entire grid, same focus/no-auto-scroll rule as a header click.

### 7. Column/Row Resize and Auto-fit

- Only valid on an axis configured through the vector form of `set_cols`/`set_rows`; the uniform-width/height form has no per-cell size to grab, so resize/auto-fit is a no-op on that axis.
- Dragging a column/row border in the header band resizes it; double-clicking a border auto-fits it to the widest/tallest currently visible cell content on that column/row.
- Matches Excel's selection-aware behavior: if the dragged/double-clicked border belongs to a column/row that is part of the current selection, the action applies to every column/row in that selection's span, not just the one under the cursor. Otherwise it applies to that single column/row only.
  - Drag: every column/row in the span is set to the SAME resulting size (the one the drag produced).
  - Auto-fit (double-click): every column/row in the span is fitted INDEPENDENTLY to its own content (sizes may differ from each other).

### 8. In-Place Cell Editing

- `set_edit_callback(const char* (*cb)(int row, int col, const char* text, void* context), void* context = nullptr)`: optional, dual-purpose, separate from the read-only `set_text_callback`. `context`, when omitted (nullptr), defaults to `this`. In virtual mode, editing is unavailable (Enter/double-click/typed-char are no-ops on a body cell) until this is called; in owned mode, non-fixed cells are editable as plain CEdit cells even without it (commits write to the owned buffer).
  - QUERY mode (`text == nullptr`): TableBox asks the cell's type. Return `nullptr` (read-only, no editing), `(const char*)(-1)` (plain-text CEdit cell), or a comma-separated item list `"item0, item1, ..."` (combo/dropdown cell). Items are trimmed of surrounding spaces; `'\b'` (0x08) inside an item stands for a literal comma. The query is called per cell during draw, mouse-over, and edit entry, so it must be cheap.
  - COMMIT mode (`text != nullptr`): the user finished editing; store the new value. For a combo cell `text` is the selected index as a decimal string (`"0"`, `"1"`, ...); for a CEdit cell it is the raw UTF-8 string. The return value is ignored in commit mode. In owned mode the commit is written directly into the owned buffer and `edit_cb` is NOT invoked for commit (it is still used in query mode for cell type).
  - The cell's CURRENT value comes from `set_text_callback`, not from `edit_cb`: a combo cell's `text_cb` returns just the index string. TableBox caches nothing; the host's `text_cb` source must reflect the committed value on the next call.
- `virtual void edit_cell(int row, int col)`: overridable entry point into edit mode for a body cell, mirroring `draw_cell`'s overridability. The default implementation calls `edit_cb(row,col,nullptr)` (query) to decide between the two edit UIs below.
- Combo (dropdown) cell: identified by the QUERY return being a comma-separated item list (the old `"\x1B"`-prefixed `text_cb` encoding is gone). The default `draw_cell` renders the currently selected item's label (index from `text_cb`, clamped into range) plus the Unicode character `▼` (U+25BC) drawn right-aligned in the arrow column using the same cell font and `DrawTextW(DT_CENTER|DT_VCENTER|DT_SINGLELINE)`; the full arrow column rect is kept as the mouse hit-test zone regardless of the glyph's actual width.
- Entering edit mode (CEdit or combo) collapses any active range selection down to the editing cell.
- Edit triggers on a body cell, only when not already editing:
  - Double-click, or Enter on the focused cell: opens the CEdit (plain cell) or the dropdown (combo cell), matching whichever type the cell is.
  - A printable character key: starts the CEdit and forwards the typed character so it replaces the selected (full) text -- Excel's type-to-overwrite. Combo cells are excluded (no free-text meaning).
  - A single click on a combo cell's dropdown arrow (without Shift) opens the dropdown immediately, like clicking a real ComboBox's drop button.
- Plain-text edit: a borderless **raw Unicode EDIT window** (NOT an MFC `CEdit`) positioned exactly over the cell, current text pre-selected. It is created with `CreateWindowExW(L"EDIT")` + `SetWindowSubclass` (no MFC hook) so it stays a Unicode window even in an MBCS build, which is required for correct Hangul IME composition rendering (see Learned.md 5.9). Enter commits and moves the focused cell down one row; Tab commits and moves it right one column; Esc cancels (restores the original value, no callback call); losing focus to anything else commits with no move. `edit_cb` (commit mode) receives the box's text as-is. Starting Hangul input on the focused editable cell opens the box automatically (the IME composition that begins on the cell is handed to the new EDIT).
- Combo edit (owner-draw dropdown): a screen-positioned popup directly below the focused cell (so it can extend past TableBox's own client area, like a real combo dropdown), width = the cell's width, each item's row height = the cell's height (not achievable with `CComboBox`, whose per-item height is fixed by the font). The popup has `CS_DROPSHADOW` and a `WS_BORDER`; its window size is computed via `AdjustWindowRectEx` so the client area is exactly `cell_h * N` pixels (avoiding the 1-pixel-per-item shrinkage `WS_BORDER` would otherwise cause). Items are painted in two passes: backgrounds and text first, then separator lines (`RGB(191,191,191)`, same as grid lines) at `(i+1)*rh - 1` (the `-1` compensates for the 1px WS_BORDER top frame so separators align with the surrounding table grid). Clicking an item, or Enter on the highlighted item, commits; Up/Down move the highlight; Esc, or the popup losing activation (e.g. clicking elsewhere), cancels with no callback call. On commit, `edit_cb` (commit mode) receives the selected index as a decimal string (`"0"`, `"1"`, ...), matching what `set_text_callback` returns for a combo cell.
- Disruption cancellation: `OnMouseWheel`, `OnLButtonDown`, and `OnSize` call `cancel_edit()` at entry (before any scrolling, focus change, or layout update). `cancel_edit()` discards the active `edit_box` or `combo_popup` without committing, then the triggering action proceeds normally. This prevents the floating editor from becoming misaligned when the table layout changes beneath it.
- `TableBox` does not cache edited text itself; the host's `set_text_callback` source is expected to reflect the new value on the next `cell_text` call.
- Mouse-over cursor (works regardless of TableBox keyboard focus): over a CEdit-type editable cell the cursor is `IDC_HAND` across the whole cell; over a combo cell it is `IDC_HAND` only on the dropdown arrow zone; over a read-only cell it is the default arrow. While an overlay scrollbar is in its visible "hold" phase the cursor stays the default arrow table-wide (it may turn into a hand again once the bar starts fading).
