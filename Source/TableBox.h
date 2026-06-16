// TableBox.h
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
//
// --- Usage ---
//     TableBox* table = new TableBox;
//     table->set_cols(100, 15);                 // uniform width 100 (96 DPI), 15 columns total
//     table->set_rows(25, 60);                   // uniform height 25 (96 DPI), 60 rows total
//     table->set_fixed(1, 1);                     // frozen header row/col (default)
//     table->set_callback(MyText, nullptr);       // text source for every cell
//     table->set_font("Malgun Gothic", 10);       // single font (ConBox name/size/option grammar)
//     table->open(parent, x0, y0, rows, cols);
//     // rows/cols = initial VISIBLE cell count (NOT the grid's total row/col count -- that is
//     // set_rows()/set_cols()'s "limit"/vector size). Clamped to the grid's row/col count.
//     // Call set_cols/set_rows/set_fixed/set_callback/set_font BEFORE open() so the first paint
//     // already has metrics ready (same convention as ConBox::setup_from_ini -> open).
//     // A FrameBox host may then attach it like any CWnd*, e.g. Top.AddNew(0,0,0,0, table).
//
#pragma once

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

class TableBox : public CWnd
{
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
    void set_callback(const char* (*cb)(int row, int col, void* user), void* user = nullptr);

    // Single font. Borrows ONLY ConBox's set_efont/set_kfont argument grammar (name / size in
    // points / option attribute string: B/I/U/S/Q -- see ConBox.cpp ParseFontOpts). No English/
    // Korean split and no height matching: cell sizes are explicit (set_cols/set_rows), so the
    // monospace cell-width logic does not apply, and the width ratio ('W') option is not used.
    void set_font(const char* name, float size, const char* option = nullptr);

protected:
    // Default rendering: normal cell white + left-aligned/vertically-centered text; fixed cell
    // an Excel-like raised/button look. Coordinates are client-area PHYSICAL px (DPI already
    // applied); x1/y1 exclusive. The caller (OnPaint) has already clipped the DC to [x0,y0,x1,y1),
    // so an override needs no extra clipping. Grid lines are drawn by the paint loop afterward,
    // not here -- an override need not redraw borders.
    virtual void draw_cell(CDC& dc, int row, int col, int x0, int y0, int x1, int y1);

    // Wraps the text callback; for overrides that only need the cell's text.
    const char* cell_text(int row, int col) const;

    afx_msg void OnPaint();
    afx_msg void OnSize(UINT type, int cx, int cy);
    afx_msg BOOL OnEraseBkgnd(CDC* dc);
    afx_msg BOOL OnMouseWheel(UINT flags, short zDelta, CPoint pt);
    afx_msg void OnLButtonDown(UINT flags, CPoint pt);
    afx_msg void OnMouseMove(UINT flags, CPoint pt);
    afx_msg void OnLButtonUp(UINT flags, CPoint pt);
    afx_msg void OnMouseLeave();
    afx_msg void OnTimer(UINT_PTR id);
    DECLARE_MESSAGE_MAP()

private:
    void ensure_back_buffer(CDC* ref, int w, int h);

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

    int fixed_rows;   // frozen header row count (default 1)
    int fixed_cols;   // frozen header col count (default 1)

    int scroll_row;    // index of the first BODY row shown (>= fixed_rows)
    int scroll_col;    // index of the first BODY col shown (>= fixed_cols)

    const char* (*text_cb)(int row, int col, void* user);
    void* text_cb_user;

    // Font spec (96 DPI logical / DPI-independent), stored by set_font (and the ctor default) so
    // the font can be rebuilt at a new DPI without the host re-calling. build_font() does the GDI work.
    std::string font_name;
    float       font_size;
    std::string font_opt;
    CFont       font;
    TEXTMETRICW font_tm;

    int box_dpi;   // control's current DPI; set in open() and updated in OnSize on a DPI change

    CDC      back_dc;          // persistent memory DC (double-buffered paint)
    CBitmap  back_bmp;         // back buffer bitmap (matches client size)
    CBitmap* back_bmp_saved;   // bitmap originally selected into back_dc
    int      back_w;           // current back_bmp size (change detection)
    int      back_h;
};
