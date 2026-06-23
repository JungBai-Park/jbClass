// TableBox.h
// Copyright (c) 2026 JungBai Park. All rights reserved.
//
// Self-contained, CWnd-derived Excel-like virtual grid control. It does NOT own any cell
// data: the text callback is asked only for the currently visible cells and the result is
// drawn immediately (virtual grid). Designed for reuse by arbitrary projects.
//
// --- Requirements / dependencies ---
//   - MFC (Unicode or MBCS), Windows only. Depends on afxwin.h.
//   - Self-contained portability unit: TableBox.h + TableBox.cpp only. Must not include or
//     reference ConBox.h / FrameBox.h members (duplicate logic here if something is needed;
//     the overlay scrollbar below is a from-scratch reimplementation of the ConBox style).
//   - All coordinates passed to open()/set_cols/set_rows are 96 DPI logical px, scaled to
//     physical px at the window's own DPI (GetDpiForWindow), same convention as ConBox/FrameBox.
//   - Zoom (WM_JBZOOM from FrameBox): sets zoom_pm (x1000); fonts and cell geometry
//     scale with zoom like a DPI change (see build_font in TableBox.cpp).
//
// --- Usage ---
//     TableBox* table = new TableBox;
//     table->set_cols(100, 15);                 // uniform width 100 (96 DPI), 15 columns total
//     table->set_rows(25, 60);                   // uniform height 25 (96 DPI), 60 rows total
//     table->set_fixed(1, 1);                     // frozen header row/col (default)
//     table->set_text_callback(MyText, nullptr);  // text source for every cell
//     table->set_font("Malgun Gothic", 10);       // single font (ConBox name/size/option grammar)
//     table->set_edit_callback(MyEdit, nullptr);  // OPTIONAL, see set_edit_callback below
//     table->open(parent, x0, y0, rows, cols);
//     // rows/cols = initial VISIBLE cell count (NOT the grid's total row/col count -- that is
//     // set_rows()/set_cols()'s "limit"/vector size). Clamped to the grid's row/col count.
//     // Call set_cols/set_rows/set_fixed/set_text_callback/set_font BEFORE open() so the first paint
//     // already has metrics ready (same convention as ConBox::setup_from_ini -> open).
//     // A FrameBox host may then attach it like any CWnd*, e.g. Top.AddNew(0,0,0,0, table).
//
#pragma once

// Zoom message shared with FrameBox/ConBox (same numeric value in each module header).
#ifndef WM_JBZOOM
#define WM_JBZOOM  (WM_APP + 100)
#endif

// Self-contained: define these before afxwin.h pulls in windows.h, same as ConBox.h, in case
// this header is included without FrameBox.h/ConBox.h having run first.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006
#endif

#include <afxwin.h>
#include <vector>
#include <string>

// Implementation detail of in-place cell editing (Stage 5): an owner-draw dropdown popup defined
// inside TableBox.cpp. Forward-declared here only so TableBox can hold a pointer to it; callers
// never see its definition. (The plain-text editor is a raw Unicode EDIT window driven by a Win32
// subclass proc -- see start_text_edit / EditSubclassProc in TableBox.cpp -- not an MFC class,
// because an MBCS MFC build would make CEdit an ANSI window and break IME composition rendering.)
class TableComboPopup;

class TableBox : public CWnd
{
    // Tightly-coupled implementation detail of in-place editing (Stage 5): it needs to call back
    // into TableBox's private end_combo_edit and read/set ending_edit.
    friend class TableComboPopup;

public:
    TableBox();
    virtual ~TableBox();

    // Create the child window inside parent at (x0,y0). rows/cols give the INITIAL VISIBLE
    // cell count, clamped to the configured grid size. Window pixel size = visible cells'
    // actual (col_widths/row_heights) sum, 96 DPI logical scaled to the parent's DPI (WS_CHILD
    // inherits the parent's DPI under Per-Monitor V2, so GetDpiForWindow(parent) is authoritative).
    void open(CWnd* parent, int x0, int y0, int rows, int cols);

    // Uniform column width (96 DPI logical px) repeated for `limit` columns total.
    void set_cols(int width, int limit);
    // Per-column widths (96 DPI logical px); column count = widths.size().
    void set_cols(const std::vector<int>& widths);

    // Same as set_cols, for row heights.
    void set_rows(int height, int limit);
    void set_rows(const std::vector<int>& heights);

    // Frozen header rows/cols (Excel-style: stay fixed while the body scrolls). Default 1, 1.
    void set_fixed(int rows, int cols);

    // Text source for ALL cells (fixed and normal alike). row/col are 0-based. The returned
    // const char* is UTF-8 and is consumed (copied) immediately during drawing, never stored,
    // so a reused static buffer is allowed. user is an opaque context passed back on every call.
    void set_text_callback(const char* (*cb)(int row, int col, void* user), void* user = nullptr);

    // Dual-purpose write-back path (Stage 5). Optional: editing is unavailable if never called.
    // text == nullptr : QUERY mode -- TableBox asks "what kind of cell is this?". Return:
    //   nullptr (0)            : read-only; no editing.
    //   (const char*)(-1)     : CEdit-editable plain-text cell.
    //   "item0, item1, ..."   : combo (dropdown) cell; comma-separated list of item labels.
    //                           Use '\b' (0x08) in an item to represent a literal comma.
    // text != nullptr : COMMIT mode -- user has finished editing; store the new value.
    //   For a combo cell, text is the selected index as a decimal string ("0", "1", ...).
    //   For a CEdit cell, text is the raw UTF-8 string the user typed.
    //   The return value in commit mode is ignored by TableBox.
    // For combo cells, set_text_callback must return just the current index string ("0","1",...);
    // TableBox reads this in query/draw to know which item to display.
    void set_edit_callback(const char* (*cb)(int row, int col, const char* text, void* user), void* user = nullptr);

    // Inset offsets (96 DPI logical px, scaled to physical at the window DPI) for the in-place CEdit:
    //   CEdit rect = (x0+dx0, y0+dy0, x1-dx1, y1-dy1)
    // Use dy0/dy1 to vertically position the edit box (single-line CEdit cannot center
    // text programmatically). Horizontal alignment follows set_align(). Default: (1,1,0,0).
    void set_edit_adjust(int dx0, int dy0, int dx1, int dy1);

    // Single font. Borrows ONLY ConBox's set_efont/set_kfont argument grammar (name / size in
    // points / option attribute string: B/I/U/S/Q -- see ConBox.cpp ParseFontOpts). No English/
    // Korean split and no height matching: cell sizes are explicit (set_cols/set_rows), so the
    // monospace cell-width logic does not apply, and the width ratio ('W') option is not used.
    void set_font(const char* name, float size, const char* option = nullptr);

    // Body-cell text alignment, numpad layout (does not affect fixed/header cells):
    //   1=top-left  2=top-center  3=top-right
    //   4=mid-left  5=mid-center  6=mid-right
    //   7=bot-left  8=bot-center  9=bot-right
    // Default is 4 (mid-left). Can be called before or after open().
    void set_align(int num);

    // Inner padding (96 DPI logical px) applied to both left and right of the text rect inside
    // each cell. Also used as the gap between the combo label and the arrow button. Default is 4.
    void set_pad(int logical_px);

    // Grid line color (RGB). Default is RGB(191,191,191). Can be called before or after open().
    void set_grid_color(int red, int green, int blue);

protected:
    // Default rendering: normal body-cell white + text aligned per set_align(); fixed cell an
    // Excel-like raised/button look. Coordinates are client-area PHYSICAL px (DPI already
    // applied); x1/y1 exclusive. The caller (OnPaint) has already clipped the DC to [x0,y0,x1,y1),
    // so an override needs no extra clipping. Grid lines are drawn by the paint loop afterward,
    // not here -- an override need not redraw borders.
    virtual void draw_cell(CDC& dc, int row, int col, int x0, int y0, int x1, int y1);

    // Wraps the text callback; for overrides that only need the cell's text.
    const char* cell_text(int row, int col) const;

    // Enters in-place edit mode for body cell (row, col). Triggers: double-click, Enter on the
    // focused (non-editing) cell, or a printable character key (Edit-type cells only -- see
    // OnChar). Default implementation re-reads cell_text(row,col): a value starting with "\x1B"
    // marks a combo (dropdown) cell -- "\x1B index, item0, item1, ..." (comma-separated, each
    // item trimmed) opens an owner-draw popup list below the cell, sized to the cell's own width
    // and per-item height (CComboBox cannot do this -- its item height is font-fixed). Any other
    // value opens a borderless CEdit positioned over the cell with its text selected. No-op if
    // set_edit_callback was never called. Subclasses may override to fully replace the edit UI.
    virtual void edit_cell(int row, int col);

    // Selection / current-cell state (keyboard + mouse navigation). The default draw_cell
    // picks its background from cell_status(); overriding subclasses may call it too, or
    // ignore it. -1 = fixed (header) cell, 0 = normal cell, 1 = selected cell. A selection
    // collapsed to a single cell (anchor == cur, i.e. just the focused cell with no range)
    // reports 0, not 1 -- a 1x1 range is not considered "selected".
    int cell_status(int row, int col) const;

    // True only for the single focused cell (the one arrow keys move and that owns the green
    // border), AND only while the window has keyboard focus -- see has_focus. Independent of
    // cell_status: the focused cell reports status 1 when it is part of a real (>1 cell)
    // range, 0 otherwise.
    bool is_current(int row, int col) const;

    afx_msg void OnPaint();
    afx_msg void OnSize(UINT type, int cx, int cy);
    afx_msg BOOL OnEraseBkgnd(CDC* dc);
    afx_msg void OnSetFocus(CWnd* pOldWnd);
    afx_msg void OnKillFocus(CWnd* pNewWnd);
    afx_msg UINT OnGetDlgCode();
    afx_msg void OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags);
    afx_msg void OnChar(UINT nChar, UINT nRepCnt, UINT nFlags);
    afx_msg BOOL OnMouseWheel(UINT flags, short zDelta, CPoint pt);
    afx_msg void OnLButtonDown(UINT flags, CPoint pt);
    afx_msg void OnMouseMove(UINT flags, CPoint pt);
    afx_msg void OnLButtonUp(UINT flags, CPoint pt);
    afx_msg void OnLButtonDblClk(UINT flags, CPoint pt);
    afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);
    afx_msg void OnMouseLeave();
    afx_msg void OnTimer(UINT_PTR id);

    // Intercepts WM_IME_STARTCOMPOSITION so Hangul (which arrives as IME composition, not WM_CHAR)
    // opens the CEdit and continues there instead of leaking to the desktop -- see TableBox.cpp.
    virtual LRESULT WindowProc(UINT message, WPARAM wParam, LPARAM lParam) override;
    DECLARE_MESSAGE_MAP()

private:
    void ensure_back_buffer(CDC* ref, int w, int h);

    // Draw UTF-8 text aligned per cell_align/cell_pad. Caller must set up font/bkmode/textcolor.
    void draw_text_aligned(CDC& dc, const char* utf8, int x0, int y0, int x1, int y1);

    // (Re)build `font`/`font_tm` from font_name/font_size/font_opt at the current box_dpi.
    // Called by open(), set_font() (if the window already exists), and a DPI change in OnSize.
    void build_font();

    // Scale a 96 DPI logical px value to the control's current DPI (identity at 96).
    int to_px(int logical96) const;

    // Physical px sum of the frozen header columns/rows (top-left corner block size).
    int corner_w() const;
    int corner_h() const;

    // Exclusive end index such that drawing [first, end) covers at least avail_px (the last
    // cell may be only partially visible -- it IS included, per the partial-cell draw rule).
    int last_visible_row(int first, int avail_px) const;
    int last_visible_col(int first, int avail_px) const;

    // Largest valid scroll position (body start index) such that the body's content still fills
    // avail_px ending exactly at the bottom/right edge (no overscroll past the last row/col).
    // Returns fixed_rows/fixed_cols when the remaining content is smaller than avail_px (in which
    // case the body is pinned to the top/left and the leftover space stays background-filled).
    int max_scroll_row(int avail_px) const;
    int max_scroll_col(int avail_px) const;

    // Clamp scroll_row/scroll_col into their valid range for the current window size/grid.
    void clamp_scroll();

    // Map a client point to the grid cell under it. Always succeeds (clamps into the nearest
    // valid cell) as long as the grid has at least one row and one column, so it doubles as the
    // "clamp to visible cell" used when a drag-select moves past the rendered grid.
    bool hit_test(CPoint pt, int& row, int& col) const;

    // Last valid row/col index (>= fixed_rows/fixed_cols even when the grid is smaller).
    int last_row_index() const;
    int last_col_index() const;

    // Set anchor/cur (clamped to the body range) and Invalidate. Scrolls so cur is visible
    // unless scroll is false -- used by header-band whole-row/col selection, which places
    // focus on the row/col's first cell without jumping the viewport to it.
    void select_range(int anchor_r, int anchor_c, int cur_r, int cur_c, bool scroll = true);

    // Keyboard-driven move: keeps the anchor when extend is true (Shift+key), else collapses
    // the selection to the single target cell.
    void move_current(int row, int col, bool extend);

    // Adjust scroll_row/scroll_col (and re-clamp) so (row,col) is fully visible.
    void ensure_visible(int row, int col);

    // Clamp cur_row/cur_col/anchor_row/anchor_col after fixed_rows/fixed_cols or the grid size
    // changes (set_fixed/set_cols/set_rows can shrink the grid below the current selection).
    void clamp_cursor();

    // On-screen pixel rect of (row,col), in the same coordinate space as draw_cell. False if
    // the cell is not currently rendered (scrolled out of the fixed/body view).
    bool cell_rect(int row, int col, CRect& rc) const;

    // Stage 4: column/row border resize (vector-form axes only -- a no-op axis configured via
    // the uniform set_cols/set_rows overload has no border to grab, see cols_uniform/rows_uniform).
    // Border hit zones live in the header band: a column border is tested along y in [0,corner_h())
    // at each visible column boundary (fixed + currently scrolled body columns); a row border is
    // tested along x in [0,corner_w()) at each visible row boundary. Returns the index of the
    // column/row whose right/bottom border was hit, or -1.
    int hit_col_border(CPoint pt) const;
    int hit_row_border(CPoint pt) const;

    // Auto-fit: set col_widths[idx]/row_heights[idx] to the max text extent (GetTextExtentPoint32W)
    // over every currently VISIBLE cell on that column/row (the virtual grid cannot be scanned in
    // full), plus padding. No-op if the axis is uniform.
    void autofit_col(int idx);
    void autofit_row(int idx);

    // Excel-style group resize/autofit: if the dragged/double-clicked border's column (row) hc/hr
    // lies inside the current selection's column (row) span, the span covers every selected
    // column/row; otherwise it collapses to [hc,hc]/[hr,hr] (that single column/row only). Used by
    // both the drag-resize (same resulting size for the whole span) and the autofit double-click
    // (each column/row in the span fitted independently -- see OnLButtonDblClk).
    void col_selection_span(int hc, int& c0, int& c1) const;
    void row_selection_span(int hr, int& r0, int& r1) const;

    // Cancel the active edit session (edit_box or combo_popup) without committing. Safe to call
    // when no session is open. Used by OnMouseWheel, OnLButtonDown, and OnSize to discard a
    // floating editor before the table layout changes beneath it.
    void cancel_edit();

    // Stage 5: in-place cell editing. Starts the text-edit session over (row,col) with its current
    // text selected. The editor is a raw Unicode EDIT window (CreateWindowExW + SetWindowSubclass,
    // NOT an MFC CEdit) so IME composition renders correctly even in an MBCS build. If initial_char
    // != 0 it is forwarded to the new box right after creation (typed-key trigger: replaces the
    // selection, Excel-style overwrite-on-type).
    void start_text_edit(int row, int col, wchar_t initial_char = 0);

    // Ends the active text-edit session: commit=true reads the box's text and calls edit_cb; both
    // cases destroy the box and refocus the grid. move_dr/move_dc shift cur_row/cur_col after a
    // committed edit (Enter -> +1 row, Tab -> +1 col); ignored when commit is false.
    void end_text_edit(bool commit, int move_dr, int move_dc);

    // Win32 subclass proc for the edit_box EDIT window (dwRefData = this). Replaces the old
    // TableEditBox::PreTranslateMessage/OnKillFocus/OnGetDlgCode: handles Enter/Esc/Tab commit,
    // swallows the trailing WM_CHAR they post, reports DLGC_WANTALLKEYS, and commits on focus loss.
    static LRESULT CALLBACK EditSubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                             UINT_PTR id, DWORD_PTR ref);

    // Starts/ends the combo dropdown session over (row,col). end_combo_edit's new_index is the
    // item the user picked (ignored when commit is false); the resulting text handed to edit_cb
    // is re-encoded as "\x1B new_index, item0, item1, ...".
    void start_combo_edit(int row, int col);
    void end_combo_edit(bool commit, int new_index);

    // Physical-px rect of the dropdown arrow icon at a combo cell's right edge -- shared by
    // draw_cell (paints it) and OnLButtonDown (single-click-to-open hit test).
    CRect combo_arrow_rect(const CRect& cell_rc) const;

    // Paints the focused cell's green border as a final overlay, AFTER draw_grid_lines has
    // already run for the block. Cannot live in draw_cell: draw_grid_lines runs once for the
    // whole block right after draw_cell, so anything draw_cell paints on the cell's shared
    // edge would be overwritten by the plain gray grid line that follows.
    void draw_focus_border(CDC& dc);

    // Draw cells [r0,r1) x [c0,c1) with top-left pixel origin (x0,y0), advancing by each cell's
    // own physical size. Each cell is clipped to its own rect (SaveDC/IntersectClipRect/RestoreDC)
    // before calling draw_cell, so overflow is automatic and an override always sees its full rect.
    void draw_block(CDC& dc, int r0, int r1, int c0, int c1, int x0, int y0);

    // Draw grid lines for the same block, after all its cells (so overrides need not redraw borders).
    void draw_grid_lines(CDC& dc, int r0, int r1, int c0, int c1, int x0, int y0);

    // === Overlay scrollbar (re-implemented in-module; ConBox-style auto-fade) ===
    // No native WS_VSCROLL/WS_HSCROLL (would consume client pixels). Independent vertical/
    // horizontal bars drawn into the back buffer over the body's edges; shown on scroll/hover,
    // then fade. Geometry is derived on demand (no stored metrics). Each bar = a gutter strip
    // (filled with a background-contrasting color while shown) containing two arrow buttons
    // (step by 1 row/col) at the ends and a draggable thumb in the track between them; clicking
    // the track outside the thumb pages by 3/4 of the visible extent.
    struct SbarState {
        int   alpha;        // current opacity 0..255; 0 = hidden (not drawn)
        DWORD hold_until;    // GetTickCount() deadline; fade starts once past it (unless hover/drag)
        bool  hover;         // mouse is over the gutter (held visible while true)
        bool  dragging;      // dragging the thumb (held visible; SetCapture active)
        int   drag_off;      // px from thumb start to the grab point (so the thumb does not jump)
    };
    SbarState vsbar;
    SbarState hsbar;

    // gutter = full bar strip (background fill); track = gutter minus the two button squares
    // (where the thumb travels); btn_dec/btn_inc = the two arrow button squares (decrease/
    // increase index). false (all rects left untouched) when there is nothing to scroll.
    bool vsbar_geometry(CRect& gutter, CRect& track, CRect& thumb, CRect& btn_dec, CRect& btn_inc) const;
    bool hsbar_geometry(CRect& gutter, CRect& track, CRect& thumb, CRect& btn_dec, CRect& btn_inc) const;

    // Both bars' gutters span the full body edge, so they overlap in the bottom-right
    // SBAR_W x SBAR_W square; a point there satisfies both gutters' hit test. Called with each
    // bar's raw hit-test result (want_v/want_h) for the SAME point: if both are true, clears
    // whichever bar is not already showing (alpha == 0) so the two bars never both react to the
    // same point. Vertical wins when neither (or, degenerately, both) is already showing.
    void resolve_sbar_corner(bool& want_v, bool& want_h) const;

    void show_sbar(SbarState& s);

    // body_bg is the actual rendered body background, sampled by OnPaint from the back buffer
    // (draw_cell is virtual, so an override may paint something other than white) and used to
    // pick contrasting thumb/gutter colors -- see SbarColors() in TableBox.cpp.
    void draw_overlay_scrollbars(CDC& dc, COLORREF body_bg);
    void draw_sbar_axis(CDC& dc, const CRect& gutter, const CRect& thumb,
                         const CRect& btn_dec, const CRect& btn_inc, const SbarState& s, bool vertical,
                         COLORREF body_bg);

    // Grid definition (96 DPI logical px), one entry per column/row; size() is the grid's total
    // column/row count (the "limit" of the uniform set_cols/set_rows overload). Ctor default
    // (before any set_cols/set_rows call): uniform 75 wide x 20 tall, 10000 columns x 10000 rows,
    // so open() works even if the host never calls set_cols/set_rows.
    std::vector<int> col_widths;
    std::vector<int> row_heights;

    // True when the axis was last set through the uniform set_cols/set_rows overload (also true
    // for the ctor default); the vector-form overload clears it. Stage 4 resize/autofit are a
    // no-op while the axis's flag is true (uniform axes have no per-cell width/height to grab).
    bool cols_uniform;
    bool rows_uniform;

    // Stage 4 live border-drag state. resize_col/resize_row (-1 = not resizing) is the dragged
    // border's column/row; resize_start_pt/resize_start_sz are the mouse coordinate (physical px)
    // and that column/row's size (96 DPI logical px) at the moment the drag started, so OnMouseMove
    // can recompute the size from the cursor delta without accumulating rounding error across moves.
    // resize_group_c0/c1 (r0/r1) is the column/row span the computed size is applied to every move
    // -- see col_selection_span/row_selection_span; equals [resize_col,resize_col] outside a
    // multi-column/row selection, so the single-target case needs no special handling.
    int resize_col;
    int resize_row;
    int resize_start_pt;
    int resize_start_sz;
    int resize_group_c0, resize_group_c1;
    int resize_group_r0, resize_group_r1;

    // True while a fresh (non-Shift) header-band click is being dragged across other header
    // cells to multi-select columns/rows (Excel-style header drag-select); mutually exclusive
    // with `selecting` (body-cell range drag) and the Stage 4 border drags above, since each
    // starts its own SetCapture() from OnLButtonDown and they are tested in priority order there.
    bool header_drag_col;
    bool header_drag_row;

    int fixed_rows;   // frozen header row count (default 1)
    int fixed_cols;   // frozen header col count (default 1)

    int scroll_row;    // index of the first BODY row shown (>= fixed_rows)
    int scroll_col;    // index of the first BODY col shown (>= fixed_cols)

    int  cur_row, cur_col;        // active/focused cell (keyboard navigation target)
    int  anchor_row, anchor_col;  // selection-range start; equals cur_row/cur_col outside a range
    bool selecting;                // true while dragging a body-cell range selection (mouse captured)
    bool has_focus;                 // window has keyboard focus; the green focus border only
                                     // draws while true, but cur_row/cur_col are remembered
                                     // regardless so the border reappears where it was on refocus

    const char* (*text_cb)(int row, int col, void* user);
    void* text_cb_user;

    const char* (*edit_cb)(int row, int col, const char* text, void* user);
    void* edit_cb_user;

    // Stage 5 edit session state. edit_box (a raw Unicode EDIT HWND, see start_text_edit) and
    // combo_popup are mutually exclusive and non-null only while their respective session is open.
    // ending_edit guards against a re-entrant commit when WM_KILLFOCUS fires as a side effect of
    // our own DestroyWindow()/SetFocus() during commit/cancel (the box's real "lost focus to
    // another control" case is the one we DO want to commit on; the teardown-triggered one must
    // be ignored).
    HWND             edit_box;
    TableComboPopup* combo_popup;
    int  edit_row, edit_col;
    bool ending_edit;
    int  edit_adj_x0, edit_adj_y0, edit_adj_x1, edit_adj_y1;  // set by set_edit_adjust()

    // Font spec (96 DPI logical / DPI-independent), stored by set_font (and the ctor default) so
    // the font can be rebuilt at a new DPI without the host re-calling. build_font() does the GDI work.
    std::string font_name;
    float       font_size;
    std::string font_opt;
    CFont       font;
    TEXTMETRICW font_tm;

    int cell_align;    // numpad-style text alignment (1-9); set by set_align(), default 4 (mid-left)
    int cell_pad;      // inner left/right text padding (96 DPI logical px); set by set_pad(), default 4
    COLORREF grid_color; // grid line color; set by set_grid_color(), default RGB(191,191,191)
    int box_dpi;       // real monitor DPI; set in open() and updated in OnSize on a DPI change
    int zoom_pm;       // zoom x1000 (1000=1.0x); set via WM_JBZOOM from FrameBox
    int eff_dpi() const { int d = box_dpi > 0 ? box_dpi : 96; return max(1, ::MulDiv(d, zoom_pm, 1000)); }

    CDC      back_dc;          // persistent memory DC (double-buffered paint)
    CBitmap  back_bmp;         // back buffer bitmap (matches client size)
    CBitmap* back_bmp_saved;   // bitmap originally selected into back_dc
    int      back_w;           // current back_bmp size (change detection)
    int      back_h;
};
