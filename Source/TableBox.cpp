// TableBox.cpp
//
// Implementation of TableBox. See TableBox.h for the public contract; read this file only
// when modifying behavior.
//
// === File layout ===
//   helpers (static) : ParseFontOpts, SbarColors, DrawAA, DrawRoundedThumb, DrawTriangle,
//                       FillTranslucent, ParseComboSpec, BuildComboSpec
//   message map       : BEGIN_MESSAGE_MAP ... END_MESSAGE_MAP
//   ctor/dtor         : TableBox, ~TableBox
//   window/font       : open, build_font
//   grid definition   : set_cols x2, set_rows x2, set_fixed, set_callback, set_font,
//                        set_edit_callback
//   layout            : to_px, corner_w, corner_h, last_visible_row/col, max_scroll_row/col,
//                        clamp_scroll
//   painting          : ensure_back_buffer, draw_block, draw_grid_lines, draw_cell, cell_text,
//                        combo_arrow_rect, OnPaint
//   overlay scrollbar : vsbar_geometry, hsbar_geometry, resolve_sbar_corner, show_sbar,
//                        draw_sbar_axis, draw_overlay_scrollbars
//   selection         : hit_test, last_row_index/col_index, select_range, move_current,
//                        ensure_visible, clamp_cursor, cell_status, is_current, cell_rect,
//                        draw_focus_border
//   resize (Stage 4)  : hit_col_border, hit_row_border, col_selection_span, row_selection_span,
//                        autofit_col, autofit_row
//   editing (Stage 5) : TableEditBox class, TableComboPopup class, edit_cell, start_text_edit,
//                        end_text_edit, start_combo_edit, end_combo_edit, cancel_edit
//   input             : OnSetFocus, OnKillFocus, OnGetDlgCode, OnKeyDown, OnChar, OnMouseWheel,
//                        OnLButtonDown, OnMouseMove, OnLButtonUp, OnLButtonDblClk, OnSetCursor,
//                        OnMouseLeave, OnTimer
//   window messages   : OnSize, OnEraseBkgnd
//
#include "TableBox.h"
#include <afxpriv.h>   // AfxHookWindowCreate / AfxUnhookWindowCreate (MFC window creation hook)
#include <cmath>

// Convert a font name/size + option string to a LOGFONTW. Duplicated from ConBox.cpp's
// ParseFontOpts (self-contained module -- see TableBox.h), trimmed to the options TableBox
// actually uses: no width-ratio ('W'), since cell widths are explicit here, not font-derived.
// option grammar: a number applies to the attribute letter that follows it.
//   B=Bold (with a number = lfWeight), I=Italic, U=Underline, S=Strikeout, Q=Quality (0..6).
static LOGFONTW ParseFontOpts(const char* name, float size_pt, const char* option, int dpi_y)
{
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));

    lf.lfHeight = -(LONG)(size_pt * dpi_y / 72.0f + 0.5f);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = DEFAULT_QUALITY;

    if (name != nullptr)
        ::MultiByteToWideChar(CP_UTF8, 0, name, -1, lf.lfFaceName, LF_FACESIZE);

    int num = 0;
    bool has_num = false;
    for (const char* p = option; p != nullptr && *p != '\0'; ++p) {
        char c = *p;
        if (c >= '0' && c <= '9') {
            num = num * 10 + (c - '0');
            has_num = true;
            continue;
        }
        switch (c) {
        case 'B': case 'b': lf.lfWeight = has_num ? num : FW_BOLD; break;
        case 'I': case 'i': lf.lfItalic = 1; break;
        case 'U': case 'u': lf.lfUnderline = 1; break;
        case 'S': case 's': lf.lfStrikeOut = 1; break;
        case 'Q': case 'q': lf.lfQuality = (has_num && num >= 0 && num <= 6) ? (BYTE)num : DEFAULT_QUALITY; break;
        default: break;
        }
        num = 0;
        has_num = false;
    }
    return lf;
}

// Overlay scrollbar visual constants (ConBox-style auto-fade, reimplemented in-module).
static const UINT_PTR SBAR_TIMER       = 1;
static const int      SBAR_W           = 14;   // gutter (hit-test) thickness
static const int      SBAR_BTN_H       = SBAR_W;  // arrow button square side (== gutter thickness)
static const int      SBAR_THUMB_W     = 6;    // visible thumb thickness
static const int      SBAR_INSET       = 3;    // gap from the client edge to the thumb
static const int      SBAR_MIN_THUMB   = 24;   // minimum thumb length so it stays grabbable
static const int      SBAR_RADIUS      = 3;    // thumb corner radius (rounded ends)
static const DWORD    SBAR_HOLD_MS     = 700;  // stay fully visible this long after last activity
static const UINT     SBAR_FADE_TICK_MS = 33;  // fade animation tick (~30 fps)
static const int      SBAR_FADE_STEP   = 24;   // alpha removed per tick once fading
static const int      SBAR_OP_IDLE     = 130;  // max thumb/button opacity when shown (translucent)
static const int      SBAR_OP_HOVER    = 215;  // brighter when hovering / dragging
static const int      SBAR_GUTTER_OP   = 70;   // gutter strip opacity (scaled by the fade alpha)

// Stage 4 column/row resize constants (96 DPI logical px).
static const int      RESIZE_ZONE_HALF = 4;    // border hit-zone half-width (FrameBox's 8px convention, centered on the line)
static const int      MIN_CELL_SIZE    = 10;   // minimum column width / row height while dragging or auto-fitting
static const int      AUTOFIT_PAD      = 8;    // padding added to the measured text extent

// Pick thumb/gutter colors that contrast with `bg` (luminance test), so the bar stays visible
// regardless of whether the body background is light or dark. `bg` is sampled from the actual
// rendered back buffer (see OnPaint), not assumed -- draw_cell is virtual, so an override may
// paint a background other than the default white.
static void SbarColors(COLORREF bg, COLORREF& thumb_color, COLORREF& gutter_color)
{
    int lum = (GetRValue(bg) * 299 + GetGValue(bg) * 587 + GetBValue(bg) * 114) / 1000;
    bool light = lum > 128;
    thumb_color  = light ? RGB(90, 90, 90)    : RGB(210, 210, 210);
    gutter_color = light ? RGB(190, 190, 190) : RGB(60, 60, 60);
}

// Render an antialiased shape into rc via 4x4 supersampling: inside(x,y) tests a point in LOCAL
// pixel coordinates (0,0 = rc's top-left, sub-pixel positions allowed). Used for the rounded
// thumb and arrow triangles so their curved/diagonal edges are smooth, not a hard pixel staircase.
// baseAlpha = per-pixel opacity when fully shown; fadeAlpha = the current fade level (AlphaBlend's
// SourceConstantAlpha).
template <typename InsideTest>
static void DrawAA(CDC& dc, const CRect& rc, COLORREF color, int baseAlpha, BYTE fadeAlpha, InsideTest inside)
{
    int w = rc.Width(), h = rc.Height();
    if (w <= 0 || h <= 0)
        return;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = ::CreateDIBSection(dc.GetSafeHdc(), &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (dib == NULL)
        return;

    const int SS = 4;   // 4x4 = 16 samples per output pixel
    int r = GetRValue(color), g = GetGValue(color), b = GetBValue(color);
    DWORD* px = (DWORD*)bits;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int hit = 0;
            for (int sy = 0; sy < SS; ++sy) {
                double fy = y + (sy + 0.5) / SS;
                for (int sx = 0; sx < SS; ++sx) {
                    double fx = x + (sx + 0.5) / SS;
                    if (inside(fx, fy)) hit++;
                }
            }
            int a = baseAlpha * hit / (SS * SS);
            int pr = r * a / 255, pg = g * a / 255, pb = b * a / 255;
            px[y * w + x] = ((DWORD)a << 24) | ((DWORD)pr << 16) | ((DWORD)pg << 8) | (DWORD)pb;
        }
    }

    CDC mem;
    mem.CreateCompatibleDC(&dc);
    HGDIOBJ old = ::SelectObject(mem.GetSafeHdc(), dib);
    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = fadeAlpha;
    bf.AlphaFormat = AC_SRC_ALPHA;
    dc.AlphaBlend(rc.left, rc.top, w, h, &mem, 0, 0, w, h, bf);
    ::SelectObject(mem.GetSafeHdc(), old);
    ::DeleteObject(dib);
}

// Antialiased rounded-rect fill (thumb). Standard rounded-box inside test: clamp the point's
// offset from center into the corner region, then circle-test there (degenerates to a flat
// half-plane test away from the corners).
static void DrawRoundedThumb(CDC& dc, const CRect& rc, COLORREF color, int baseAlpha, int radius, BYTE fadeAlpha)
{
    int w = rc.Width(), h = rc.Height();
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    if (radius < 0)     radius = 0;
    double hw = w / 2.0, hh = h / 2.0, rad = radius;
    DrawAA(dc, rc, color, baseAlpha, fadeAlpha, [hw, hh, rad](double x, double y) {
        double ax = std::fabs(x - hw) - (hw - rad); if (ax < 0) ax = 0;
        double ay = std::fabs(y - hh) - (hh - rad); if (ay < 0) ay = 0;
        return ax * ax + ay * ay <= rad * rad;
    });
}

// Arrow button direction (which way the triangle points = which way that button scrolls).
enum class SbarDir { UP, DOWN, LEFT, RIGHT };

// Antialiased filled triangle (arrow button), tip pointing `dir`, padded 3px from rc's edges.
static void DrawTriangle(CDC& dc, const CRect& rc, COLORREF color, int baseAlpha, BYTE fadeAlpha, SbarDir dir)
{
    int w = rc.Width(), h = rc.Height();
    const double pad = 3.0;
    double v0x, v0y, v1x, v1y, v2x, v2y;   // v0 = tip, v1/v2 = base corners
    switch (dir) {
    case SbarDir::UP:    v0x = w/2.0;     v0y = pad;       v1x = pad;       v1y = h-1-pad; v2x = w-1-pad;   v2y = h-1-pad; break;
    case SbarDir::DOWN:  v0x = w/2.0;     v0y = h-1-pad;   v1x = pad;       v1y = pad;     v2x = w-1-pad;   v2y = pad;     break;
    case SbarDir::LEFT:  v0x = pad;       v0y = h/2.0;     v1x = w-1-pad;   v1y = pad;     v2x = w-1-pad;   v2y = h-1-pad; break;
    default:             v0x = w-1-pad;   v0y = h/2.0;     v1x = pad;       v1y = pad;     v2x = pad;       v2y = h-1-pad; break;
    }
    DrawAA(dc, rc, color, baseAlpha, fadeAlpha, [=](double x, double y) {
        double d0 = (v1x-v0x)*(y-v0y) - (v1y-v0y)*(x-v0x);
        double d1 = (v2x-v1x)*(y-v1y) - (v2y-v1y)*(x-v1x);
        double d2 = (v0x-v2x)*(y-v2y) - (v0y-v2y)*(x-v2x);
        return (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
    });
}

// Flat translucent fill (the gutter strip background) -- a 1x1 source stretched over rc via
// AlphaBlend, same technique ConBox used for its scrollbar groove. No AA needed: a gutter is
// always an axis-aligned rect, so its edges are already pixel-exact.
static void FillTranslucent(CDC& dc, const CRect& rc, COLORREF color, BYTE alpha)
{
    if (rc.Width() <= 0 || rc.Height() <= 0 || alpha == 0)
        return;
    CDC src;
    src.CreateCompatibleDC(&dc);
    CBitmap bmp;
    bmp.CreateCompatibleBitmap(&dc, 1, 1);
    CBitmap* old = src.SelectObject(&bmp);
    src.SetPixel(0, 0, color);
    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.AlphaFormat = 0;
    bf.SourceConstantAlpha = alpha;
    dc.AlphaBlend(rc.left, rc.top, rc.Width(), rc.Height(), &src, 0, 0, 1, 1, bf);
    src.SelectObject(old);
}

// Combo-cell spec sentinel (Stage 5): a cell whose text starts with this byte is a dropdown
// (combo) cell, not plain text. ESC (0x1B) is a non-printable control byte that can never
// collide with real cell data, unlike a printable marker.
static const char COMBO_SENTINEL = '\x1B';

// Parses "\x1Bindex, item0, item1, ..." (selected index then comma-separated items, each
// trimmed of surrounding spaces). Returns false if text does not start with the sentinel (plain
// text cell). On success, sel_index is clamped into [0, items.size()-1], or -1 if items is empty.
static bool ParseComboSpec(const char* text, int& sel_index, std::vector<std::string>& items)
{
    items.clear();
    sel_index = -1;
    if (text == nullptr || text[0] != COMBO_SENTINEL)
        return false;

    const char* p = text + 1;
    while (*p == ' ') p++;
    int idx = 0;
    bool has_digit = false;
    while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); has_digit = true; p++; }
    while (*p == ' ') p++;
    if (*p == ',') p++;

    while (*p != '\0') {
        while (*p == ' ') p++;
        std::string item;
        while (*p != '\0' && *p != ',') item += *p++;
        while (!item.empty() && item.back() == ' ') item.pop_back();
        items.push_back(item);
        if (*p == ',') p++;
    }

    if (!items.empty()) {
        if (!has_digit || idx < 0) idx = 0;
        if (idx >= (int)items.size()) idx = (int)items.size() - 1;
        sel_index = idx;
    }
    return true;
}

// Re-encodes a combo spec after the user picks a new item, so the host's data shape (as fed
// back through set_edit_callback) stays identical to what set_callback originally returned.
static std::string BuildComboSpec(int sel_index, const std::vector<std::string>& items)
{
    std::string s(1, COMBO_SENTINEL);
    char buf[16];
    sprintf_s(buf, sizeof(buf), "%d", sel_index);
    s += buf;
    for (size_t i = 0; i < items.size(); ++i) {
        s += ", ";
        s += items[i];
    }
    return s;
}

// The visible label for a cell -- the selected item for a combo cell, the text itself
// otherwise. Used by autofit_col/autofit_row so they measure what is actually rendered, not
// the raw "\x1Bindex, items..." encoding.
static std::string ComboDisplayLabel(const char* raw)
{
    int sel;
    std::vector<std::string> items;
    if (ParseComboSpec(raw, sel, items))
        return (sel >= 0) ? items[sel] : std::string();
    return raw ? std::string(raw) : std::string();
}

BEGIN_MESSAGE_MAP(TableBox, CWnd)
    ON_WM_PAINT()
    ON_WM_SIZE()
    ON_WM_ERASEBKGND()
    ON_WM_SETFOCUS()
    ON_WM_KILLFOCUS()
    ON_WM_GETDLGCODE()
    ON_WM_KEYDOWN()
    ON_WM_CHAR()
    ON_WM_MOUSEWHEEL()
    ON_WM_LBUTTONDOWN()
    ON_WM_MOUSEMOVE()
    ON_WM_LBUTTONUP()
    ON_WM_LBUTTONDBLCLK()
    ON_WM_SETCURSOR()
    ON_WM_MOUSELEAVE()
    ON_WM_TIMER()
END_MESSAGE_MAP()

TableBox::TableBox()
{
    col_widths.assign(10000, 75);
    row_heights.assign(10000, 20);
    cols_uniform = true;
    rows_uniform = true;

    resize_col = -1;
    resize_row = -1;
    resize_start_pt = 0;
    resize_start_sz = 0;
    resize_group_c0 = resize_group_c1 = 0;
    resize_group_r0 = resize_group_r1 = 0;
    header_drag_col = false;
    header_drag_row = false;

    fixed_rows = 1;
    fixed_cols = 1;
    scroll_row = fixed_rows;
    scroll_col = fixed_cols;

    cur_row = fixed_rows; cur_col = fixed_cols;
    anchor_row = cur_row; anchor_col = cur_col;
    selecting = false;
    has_focus = false;

    text_cb = nullptr;
    text_cb_user = nullptr;

    edit_cb = nullptr;
    edit_cb_user = nullptr;
    edit_box = nullptr;
    combo_popup = nullptr;
    edit_row = -1;
    edit_col = -1;
    ending_edit = false;

    font_name = "Malgun Gothic";
    font_size = 9.0f;
    font_opt.clear();
    ZeroMemory(&font_tm, sizeof(font_tm));

    box_dpi = 0;

    back_bmp_saved = nullptr;
    back_w = 0;
    back_h = 0;

    vsbar.alpha = 0; vsbar.hold_until = 0; vsbar.hover = false; vsbar.dragging = false; vsbar.drag_off = 0;
    hsbar.alpha = 0; hsbar.hold_until = 0; hsbar.hover = false; hsbar.dragging = false; hsbar.drag_off = 0;
}

TableBox::~TableBox()
{
}

void TableBox::open(CWnd* parent, int x0, int y0, int rows, int cols)
{
    // Register the "TableBox" window class once. CreateWindowExW below keeps the class name
    // wide in both MBCS and Unicode builds (CWnd::CreateEx takes LPCTSTR, which is LPCSTR in
    // MBCS); AfxHookWindowCreate/AfxUnhookWindowCreate wire the HWND to this CWnd and its
    // message map exactly as CWnd::CreateEx would.
    WNDCLASSEXW wc = {};
    if (!::GetClassInfoExW(AfxGetInstanceHandle(), L"TableBox", &wc))
    {
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc   = ::DefWindowProcW;
        wc.hInstance     = AfxGetInstanceHandle();
        wc.hCursor       = ::LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = L"TableBox";
        ::RegisterClassExW(&wc);
    }

    box_dpi = (int)::GetDpiForWindow(parent->GetSafeHwnd());

    scroll_row = fixed_rows;
    scroll_col = fixed_cols;

    // Initial visible viewport: rows/cols cells, clamped to the configured grid size. Summed
    // per-cell (not a scaled total) so the window matches exactly what will be rendered.
    int vis_rows = rows < (int)row_heights.size() ? rows : (int)row_heights.size();
    int vis_cols = cols < (int)col_widths.size()  ? cols : (int)col_widths.size();
    int pw = 0; for (int c = 0; c < vis_cols; ++c) pw += to_px(col_widths[c]);
    int ph = 0; for (int r = 0; r < vis_rows; ++r) ph += to_px(row_heights[r]);
    CRect rc(x0, y0, x0 + pw, y0 + ph);

    AfxHookWindowCreate(this);
    HWND hwnd = ::CreateWindowExW(0, L"TableBox", L"",
        WS_CHILD | WS_VISIBLE,
        rc.left, rc.top, rc.Width(), rc.Height(),
        parent->GetSafeHwnd(), nullptr, AfxGetInstanceHandle(), nullptr);
    if (!AfxUnhookWindowCreate())
        PostNcDestroy();

    build_font();
    clamp_scroll();
}

// (Re)build the font + cached TEXTMETRICW from the stored spec at the current box_dpi. Called by
// open(), set_font() (if the window already exists), and OnSize on a DPI change.
void TableBox::build_font()
{
    int dpi = box_dpi > 0 ? box_dpi : 96;
    LOGFONTW lf = ParseFontOpts(font_name.c_str(), font_size, font_opt.c_str(), dpi);
    font.DeleteObject();
    font.Attach(::CreateFontIndirectW(&lf));

    CDC dc;
    dc.CreateCompatibleDC(NULL);
    CFont* old = dc.SelectObject(&font);
    ::GetTextMetricsW(dc.GetSafeHdc(), &font_tm);
    dc.SelectObject(old);
}

void TableBox::set_cols(int width, int limit)
{
    col_widths.assign(limit, width);
    cols_uniform = true;
    clamp_cursor();
    if (::IsWindow(m_hWnd)) { clamp_scroll(); Invalidate(); }
}

void TableBox::set_cols(const std::vector<int>& widths)
{
    col_widths = widths;
    cols_uniform = false;
    clamp_cursor();
    if (::IsWindow(m_hWnd)) { clamp_scroll(); Invalidate(); }
}

void TableBox::set_rows(int height, int limit)
{
    row_heights.assign(limit, height);
    rows_uniform = true;
    clamp_cursor();
    if (::IsWindow(m_hWnd)) { clamp_scroll(); Invalidate(); }
}

void TableBox::set_rows(const std::vector<int>& heights)
{
    row_heights = heights;
    rows_uniform = false;
    clamp_cursor();
    if (::IsWindow(m_hWnd)) { clamp_scroll(); Invalidate(); }
}

void TableBox::set_fixed(int rows, int cols)
{
    fixed_rows = rows;
    fixed_cols = cols;
    if (scroll_row < fixed_rows) scroll_row = fixed_rows;
    if (scroll_col < fixed_cols) scroll_col = fixed_cols;
    clamp_cursor();
    if (::IsWindow(m_hWnd)) { clamp_scroll(); Invalidate(); }
}

void TableBox::set_callback(const char* (*cb)(int row, int col, void* user), void* user)
{
    text_cb = cb;
    text_cb_user = user;
    if (::IsWindow(m_hWnd)) Invalidate();
}

void TableBox::set_edit_callback(void (*cb)(int row, int col, const char* text, void* user), void* user)
{
    edit_cb = cb;
    edit_cb_user = user;
}

void TableBox::set_font(const char* name, float size, const char* option)
{
    font_name = name ? name : "";
    font_size = size;
    font_opt  = option ? option : "";
    if (::IsWindow(m_hWnd)) { build_font(); Invalidate(); }
}

int TableBox::to_px(int logical96) const
{
    return ::MulDiv(logical96, box_dpi > 0 ? box_dpi : 96, 96);
}

int TableBox::corner_w() const
{
    int w = 0;
    int n = fixed_cols < (int)col_widths.size() ? fixed_cols : (int)col_widths.size();
    for (int i = 0; i < n; ++i) w += to_px(col_widths[i]);
    return w;
}

int TableBox::corner_h() const
{
    int h = 0;
    int n = fixed_rows < (int)row_heights.size() ? fixed_rows : (int)row_heights.size();
    for (int i = 0; i < n; ++i) h += to_px(row_heights[i]);
    return h;
}

int TableBox::last_visible_row(int first, int avail_px) const
{
    int total = (int)row_heights.size();
    if (first >= total) return first;
    int acc = 0, idx = first;
    while (idx < total) {
        acc += to_px(row_heights[idx]);
        idx++;
        if (acc >= avail_px) break;
    }
    return idx;
}

int TableBox::last_visible_col(int first, int avail_px) const
{
    int total = (int)col_widths.size();
    if (first >= total) return first;
    int acc = 0, idx = first;
    while (idx < total) {
        acc += to_px(col_widths[idx]);
        idx++;
        if (acc >= avail_px) break;
    }
    return idx;
}

// Walk backward from the last row so the body [result, total) fills at least avail_px, i.e. the
// last row aligns with the bottom edge with no overscroll. Returns fixed_rows if the remaining
// content is smaller than avail_px (pinned to the top; leftover stays background-filled).
int TableBox::max_scroll_row(int avail_px) const
{
    int total = (int)row_heights.size();
    if (total <= fixed_rows) return fixed_rows;
    int acc = 0, idx = total - 1;
    while (idx > fixed_rows) {
        acc += to_px(row_heights[idx]);
        if (acc >= avail_px) break;
        idx--;
    }
    return idx;
}

int TableBox::max_scroll_col(int avail_px) const
{
    int total = (int)col_widths.size();
    if (total <= fixed_cols) return fixed_cols;
    int acc = 0, idx = total - 1;
    while (idx > fixed_cols) {
        acc += to_px(col_widths[idx]);
        if (acc >= avail_px) break;
        idx--;
    }
    return idx;
}

void TableBox::clamp_scroll()
{
    if (!::IsWindow(m_hWnd)) {
        scroll_row = fixed_rows;
        scroll_col = fixed_cols;
        return;
    }
    CRect rc;
    GetClientRect(&rc);
    int mr = max_scroll_row(rc.Height() - corner_h());
    int mc = max_scroll_col(rc.Width()  - corner_w());

    if (scroll_row < fixed_rows) scroll_row = fixed_rows;
    if (scroll_row > mr)         scroll_row = mr;
    if (scroll_col < fixed_cols) scroll_col = fixed_cols;
    if (scroll_col > mc)         scroll_col = mc;
}

// ===== Selection / current-cell navigation (Stage 3) =====

int TableBox::last_row_index() const
{
    int total = (int)row_heights.size();
    return total > fixed_rows ? total - 1 : fixed_rows;
}

int TableBox::last_col_index() const
{
    int total = (int)col_widths.size();
    return total > fixed_cols ? total - 1 : fixed_cols;
}

bool TableBox::hit_test(CPoint pt, int& row, int& col) const
{
    int total_rows = (int)row_heights.size();
    int total_cols = (int)col_widths.size();
    if (total_rows <= 0 || total_cols <= 0)
        return false;

    CRect rc;
    GetClientRect(&rc);
    if (pt.x < 0) pt.x = 0;
    if (pt.y < 0) pt.y = 0;
    if (rc.right  > 0 && pt.x >= rc.right)  pt.x = rc.right - 1;
    if (rc.bottom > 0 && pt.y >= rc.bottom) pt.y = rc.bottom - 1;

    int ch = corner_h(), cw = corner_w();

    int fr1 = fixed_rows < total_rows ? fixed_rows : total_rows;
    int y, r, row_limit;
    if (pt.y < ch) { y = 0;  r = 0;         row_limit = fr1; }
    else           { y = ch; r = scroll_row; row_limit = total_rows; }
    row = total_rows - 1;   // fallback: clamp into the grid if the loop runs past rendered rows
    for (; r < row_limit; ++r) {
        int rh = to_px(row_heights[r]);
        if (pt.y < y + rh) { row = r; break; }
        y += rh;
    }

    int fc1 = fixed_cols < total_cols ? fixed_cols : total_cols;
    int x, c, col_limit;
    if (pt.x < cw) { x = 0;  c = 0;         col_limit = fc1; }
    else           { x = cw; c = scroll_col; col_limit = total_cols; }
    col = total_cols - 1;
    for (; c < col_limit; ++c) {
        int cwid = to_px(col_widths[c]);
        if (pt.x < x + cwid) { col = c; break; }
        x += cwid;
    }

    return true;
}

void TableBox::select_range(int ar, int ac, int cr, int cc, bool scroll)
{
    int max_row = last_row_index();
    int max_col = last_col_index();

    if (ar < fixed_rows) ar = fixed_rows; if (ar > max_row) ar = max_row;
    if (cr < fixed_rows) cr = fixed_rows; if (cr > max_row) cr = max_row;
    if (ac < fixed_cols) ac = fixed_cols; if (ac > max_col) ac = max_col;
    if (cc < fixed_cols) cc = fixed_cols; if (cc > max_col) cc = max_col;

    anchor_row = ar; anchor_col = ac;
    cur_row = cr;    cur_col = cc;
    if (scroll)
        ensure_visible(cur_row, cur_col);
    Invalidate();
}

void TableBox::move_current(int row, int col, bool extend)
{
    if (extend) select_range(anchor_row, anchor_col, row, col);
    else        select_range(row, col, row, col);
}

void TableBox::ensure_visible(int row, int col)
{
    if (!::IsWindow(m_hWnd))
        return;

    CRect rc;
    GetClientRect(&rc);
    int avail_h = rc.Height() - corner_h();
    int avail_w = rc.Width()  - corner_w();

    if (row < scroll_row) {
        scroll_row = row;
    } else {
        while (scroll_row < row && row >= last_visible_row(scroll_row, avail_h))
            scroll_row++;
    }
    if (col < scroll_col) {
        scroll_col = col;
    } else {
        while (scroll_col < col && col >= last_visible_col(scroll_col, avail_w))
            scroll_col++;
    }
    clamp_scroll();
}

void TableBox::clamp_cursor()
{
    int max_row = last_row_index();
    int max_col = last_col_index();

    if (cur_row    < fixed_rows) cur_row    = fixed_rows; if (cur_row    > max_row) cur_row    = max_row;
    if (cur_col    < fixed_cols) cur_col    = fixed_cols; if (cur_col    > max_col) cur_col    = max_col;
    if (anchor_row < fixed_rows) anchor_row = fixed_rows; if (anchor_row > max_row) anchor_row = max_row;
    if (anchor_col < fixed_cols) anchor_col = fixed_cols; if (anchor_col > max_col) anchor_col = max_col;
}

int TableBox::cell_status(int row, int col) const
{
    if (row < fixed_rows || col < fixed_cols)
        return -1;
    if (anchor_row == cur_row && anchor_col == cur_col)
        return 0;   // collapsed to a single cell: not a "selected" range

    int r0 = anchor_row < cur_row ? anchor_row : cur_row;
    int r1 = anchor_row < cur_row ? cur_row : anchor_row;
    int c0 = anchor_col < cur_col ? anchor_col : cur_col;
    int c1 = anchor_col < cur_col ? cur_col : anchor_col;
    return (row >= r0 && row <= r1 && col >= c0 && col <= c1) ? 1 : 0;
}

bool TableBox::is_current(int row, int col) const
{
    return has_focus && row == cur_row && col == cur_col;
}

bool TableBox::cell_rect(int row, int col, CRect& rc) const
{
    CRect client;
    GetClientRect(&client);
    int cw = corner_w(), ch = corner_h();

    int y0;
    if (row < fixed_rows) {
        y0 = 0;
        for (int r = 0; r < row; ++r) y0 += to_px(row_heights[r]);
    } else {
        if (row < scroll_row) return false;
        y0 = ch;
        for (int r = scroll_row; r < row; ++r) y0 += to_px(row_heights[r]);
        if (y0 >= client.Height()) return false;
    }

    int x0;
    if (col < fixed_cols) {
        x0 = 0;
        for (int c = 0; c < col; ++c) x0 += to_px(col_widths[c]);
    } else {
        if (col < scroll_col) return false;
        x0 = cw;
        for (int c = scroll_col; c < col; ++c) x0 += to_px(col_widths[c]);
        if (x0 >= client.Width()) return false;
    }

    rc.SetRect(x0, y0, x0 + to_px(col_widths[col]), y0 + to_px(row_heights[row]));
    return true;
}

// ===== Column/row border resize and auto-fit (Stage 4) =====

int TableBox::hit_col_border(CPoint pt) const
{
    if (cols_uniform)
        return -1;
    int ch = corner_h();
    if (ch <= 0 || pt.y < 0 || pt.y >= ch)
        return -1;

    int half = to_px(RESIZE_ZONE_HALF);
    CRect rc;
    GetClientRect(&rc);
    int total_cols = (int)col_widths.size();
    int fc1 = fixed_cols < total_cols ? fixed_cols : total_cols;
    int cw = corner_w();

    int x = 0;
    for (int c = 0; c < fc1; ++c) {
        x += to_px(col_widths[c]);
        int d = pt.x - x; if (d < 0) d = -d;
        if (d <= half) return c;
    }
    int c1 = last_visible_col(scroll_col, rc.Width() - cw);
    x = cw;
    for (int c = scroll_col; c < c1; ++c) {
        x += to_px(col_widths[c]);
        int d = pt.x - x; if (d < 0) d = -d;
        if (d <= half) return c;
    }
    return -1;
}

int TableBox::hit_row_border(CPoint pt) const
{
    if (rows_uniform)
        return -1;
    int cw = corner_w();
    if (cw <= 0 || pt.x < 0 || pt.x >= cw)
        return -1;

    int half = to_px(RESIZE_ZONE_HALF);
    CRect rc;
    GetClientRect(&rc);
    int total_rows = (int)row_heights.size();
    int fr1 = fixed_rows < total_rows ? fixed_rows : total_rows;
    int ch = corner_h();

    int y = 0;
    for (int r = 0; r < fr1; ++r) {
        y += to_px(row_heights[r]);
        int d = pt.y - y; if (d < 0) d = -d;
        if (d <= half) return r;
    }
    int r1 = last_visible_row(scroll_row, rc.Height() - ch);
    y = ch;
    for (int r = scroll_row; r < r1; ++r) {
        y += to_px(row_heights[r]);
        int d = pt.y - y; if (d < 0) d = -d;
        if (d <= half) return r;
    }
    return -1;
}

void TableBox::col_selection_span(int hc, int& c0, int& c1) const
{
    int s0 = anchor_col < cur_col ? anchor_col : cur_col;
    int s1 = anchor_col < cur_col ? cur_col : anchor_col;
    if (hc >= s0 && hc <= s1) { c0 = s0; c1 = s1; }
    else                      { c0 = hc; c1 = hc; }
}

void TableBox::row_selection_span(int hr, int& r0, int& r1) const
{
    int s0 = anchor_row < cur_row ? anchor_row : cur_row;
    int s1 = anchor_row < cur_row ? cur_row : anchor_row;
    if (hr >= s0 && hr <= s1) { r0 = s0; r1 = s1; }
    else                      { r0 = hr; r1 = hr; }
}

// Measures every currently visible cell on column idx (fixed rows + visible body rows -- the
// virtual grid cannot be scanned in full) and sets col_widths[idx] to the max text extent + padding.
void TableBox::autofit_col(int idx)
{
    if (cols_uniform || idx < 0 || idx >= (int)col_widths.size())
        return;

    CRect rc;
    GetClientRect(&rc);
    int ch = corner_h();
    int total_rows = (int)row_heights.size();
    int fr1 = fixed_rows < total_rows ? fixed_rows : total_rows;
    int r1 = last_visible_row(scroll_row, rc.Height() - ch);

    CDC dc;
    dc.CreateCompatibleDC(NULL);
    CFont* old = dc.SelectObject(&font);

    int max_w = 0;
    for (int pass = 0; pass < 2; ++pass) {
        int r0 = pass == 0 ? 0 : scroll_row;
        int rN = pass == 0 ? fr1 : r1;
        for (int r = r0; r < rN; ++r) {
            std::string disp = ComboDisplayLabel(cell_text(r, idx));
            if (disp.empty()) continue;
            wchar_t wbuf[256];
            int n = ::MultiByteToWideChar(CP_UTF8, 0, disp.c_str(), -1, wbuf, 256);
            if (n <= 0) continue;
            n--;
            SIZE sz;
            ::GetTextExtentPoint32W(dc.GetSafeHdc(), wbuf, n, &sz);
            if (sz.cx > max_w) max_w = sz.cx;
        }
    }
    dc.SelectObject(old);

    int new_w_px = max_w + to_px(AUTOFIT_PAD);
    int new_w = ::MulDiv(new_w_px, 96, box_dpi > 0 ? box_dpi : 96);
    if (new_w < MIN_CELL_SIZE) new_w = MIN_CELL_SIZE;

    col_widths[idx] = new_w;
    clamp_scroll();
    Invalidate();
}

// Same idea transposed: measures text height instead of width, over every visible column.
void TableBox::autofit_row(int idx)
{
    if (rows_uniform || idx < 0 || idx >= (int)row_heights.size())
        return;

    CRect rc;
    GetClientRect(&rc);
    int cw = corner_w();
    int total_cols = (int)col_widths.size();
    int fc1 = fixed_cols < total_cols ? fixed_cols : total_cols;
    int c1 = last_visible_col(scroll_col, rc.Width() - cw);

    CDC dc;
    dc.CreateCompatibleDC(NULL);
    CFont* old = dc.SelectObject(&font);

    int max_h = 0;
    for (int pass = 0; pass < 2; ++pass) {
        int c0 = pass == 0 ? 0 : scroll_col;
        int cN = pass == 0 ? fc1 : c1;
        for (int c = c0; c < cN; ++c) {
            std::string disp = ComboDisplayLabel(cell_text(idx, c));
            if (disp.empty()) continue;
            wchar_t wbuf[256];
            int n = ::MultiByteToWideChar(CP_UTF8, 0, disp.c_str(), -1, wbuf, 256);
            if (n <= 0) continue;
            n--;
            SIZE sz;
            ::GetTextExtentPoint32W(dc.GetSafeHdc(), wbuf, n, &sz);
            if (sz.cy > max_h) max_h = sz.cy;
        }
    }
    dc.SelectObject(old);

    int new_h_px = max_h + to_px(AUTOFIT_PAD);
    int new_h = ::MulDiv(new_h_px, 96, box_dpi > 0 ? box_dpi : 96);
    if (new_h < MIN_CELL_SIZE) new_h = MIN_CELL_SIZE;

    row_heights[idx] = new_h;
    clamp_scroll();
    Invalidate();
}

// The 2px-thick green border is two 1px rectangles: the outer one at exactly the same pixel
// columns/rows as draw_grid_lines' own MoveTo/LineTo calls (rc.left/top and rc.right/bottom,
// not rc.right-1/bottom-1 -- draw_grid_lines draws the shared edge AT the next cell's origin,
// not one pixel inside this cell), so it overwrites the gray grid line; the inner one is 1px
// further in.
void TableBox::draw_focus_border(CDC& dc)
{
    if (!has_focus)
        return;

    CRect rc;
    if (!cell_rect(cur_row, cur_col, rc))
        return;

    CPen pen(PS_SOLID, 1, RGB(33, 115, 70));
    CPen* old_pen = dc.SelectObject(&pen);
    CBrush* old_brush = (CBrush*)dc.SelectStockObject(NULL_BRUSH);

    dc.Rectangle(rc.left, rc.top, rc.right + 1, rc.bottom + 1);
    dc.Rectangle(rc.left + 1, rc.top + 1, rc.right, rc.bottom);

    dc.SelectObject(old_brush);
    dc.SelectObject(old_pen);
}

// ===== In-place cell editing (Stage 5) =====

// Borderless CEdit positioned exactly over a body cell, single-line (no ES_MULTILINE) so Win32
// vertically centers its text by itself -- no EM_SETRECT trick needed (that trick only applies
// to multiline edits, see Learned.md 3.5; a plain single-line CEdit already centers). CEdit has
// no virtual hook for Enter/Esc/Tab, so PreTranslateMessage is overridden to catch them BEFORE
// TranslateMessage would post a WM_CHAR for them (Learned.md 3.4) and to notify the owner.
// OnGetDlgCode returns DLGC_WANTALLKEYS so FrameBox's CDialog-like Enter/Esc interception
// (Requirements.md 1.3) yields to this box instead.
class TableEditBox : public CEdit
{
public:
    TableBox* owner;

    TableEditBox() : owner(nullptr) {}

    BOOL PreTranslateMessage(MSG* pMsg) override
    {
        if (pMsg->message == WM_KEYDOWN) {
            // The box still has focus at this point (no WM_KILLFOCUS involved), so an explicit
            // SetFocus() back to the grid is needed here -- end_text_edit itself does not call
            // it (see its own comment: ambiguous to call SetFocus from inside WM_KILLFOCUS).
            TableBox* o = owner;
            if (pMsg->wParam == VK_RETURN) { o->end_text_edit(true, 1, 0);  o->SetFocus(); return TRUE; }
            if (pMsg->wParam == VK_ESCAPE) { o->end_text_edit(false, 0, 0); o->SetFocus(); return TRUE; }
            if (pMsg->wParam == VK_TAB)    { o->end_text_edit(true, 0, 1);  o->SetFocus(); return TRUE; }
        }
        return CEdit::PreTranslateMessage(pMsg);
    }

protected:
    afx_msg UINT OnGetDlgCode() { return DLGC_WANTALLKEYS; }
    afx_msg void OnKillFocus(CWnd* pNewWnd)
    {
        CEdit::OnKillFocus(pNewWnd);
        // DestroyWindow() (called by end_text_edit itself while tearing down) also raises
        // WM_KILLFOCUS synchronously; owner->ending_edit guards against committing twice. No
        // SetFocus() here -- focus is already moving to wherever the click/Tab-out sent it
        // (commonly TableBox itself via its own OnLButtonDown, which already calls SetFocus()).
        if (!owner->ending_edit)
            owner->end_text_edit(true, 0, 0);
    }
    DECLARE_MESSAGE_MAP()
};

BEGIN_MESSAGE_MAP(TableEditBox, CEdit)
    ON_WM_GETDLGCODE()
    ON_WM_KILLFOCUS()
END_MESSAGE_MAP()

// Owner-draw dropdown for a combo cell. A genuine WS_POPUP (not a TableBox child) positioned in
// SCREEN coordinates directly below the focused cell, so it can extend past TableBox's own
// client area like a real combo box dropdown (Requirements.md 3.x). Width = the cell's width;
// each item's row height = the cell's height (CComboBox cannot do this -- its per-item height
// is fixed by the font). Click commits, Up/Down moves the hover highlight, Enter commits the
// highlighted item, Esc or losing activation cancels.
class TableComboPopup : public CWnd
{
public:
    TableBox* owner;
    std::vector<std::string> items;
    int hover;
    HFONT box_font;

    TableComboPopup() : owner(nullptr), hover(0), box_font(NULL) {}

    void open(TableBox* o, CRect screen_rc, const std::vector<std::string>& list, int sel, HFONT f)
    {
        owner = o;
        items = list;
        hover = sel >= 0 ? sel : 0;
        box_font = f;

        WNDCLASSEXW wc = {};
        if (!::GetClassInfoExW(AfxGetInstanceHandle(), L"TableBoxCombo", &wc)) {
            wc.cbSize = sizeof(wc);
            wc.style = CS_DBLCLKS | CS_DROPSHADOW;
            wc.lpfnWndProc = ::DefWindowProcW;
            wc.hInstance = AfxGetInstanceHandle();
            wc.hCursor = ::LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
            wc.lpszClassName = L"TableBoxCombo";
            ::RegisterClassExW(&wc);
        }

        AfxHookWindowCreate(this);
        HWND hwnd = ::CreateWindowExW(WS_EX_TOOLWINDOW, L"TableBoxCombo", L"",
            WS_POPUP | WS_BORDER,
            screen_rc.left, screen_rc.top, screen_rc.Width(), screen_rc.Height(),
            owner->GetSafeHwnd(), nullptr, AfxGetInstanceHandle(), nullptr);
        if (!AfxUnhookWindowCreate())
            PostNcDestroy();

        ::ShowWindow(hwnd, SW_SHOW);
        SetFocus();
    }

    int row_h() const { return items.empty() ? 1 : (int)(GetClientRectHeight() / (int)items.size()); }
    int GetClientRectHeight() const { CRect rc; GetClientRect(&rc); return rc.Height(); }

protected:
    afx_msg void OnPaint()
    {
        CPaintDC dc(this);
        CRect rc;
        GetClientRect(&rc);
        int rh = row_h();

        // Pass 1: backgrounds and text (same two-pass order as draw_cell + draw_grid_lines).
        CFont* old_font = box_font ? dc.SelectObject(CFont::FromHandle(box_font)) : nullptr;
        dc.SetBkMode(OPAQUE);
        TEXTMETRICW tm = {};
        ::GetTextMetricsW(dc.GetSafeHdc(), &tm);
        for (size_t i = 0; i < items.size(); ++i) {
            CRect item_rc(0, (int)i * rh, rc.Width(), (int)i * rh + rh);
            bool hi = ((int)i == hover);
            dc.FillSolidRect(&item_rc, hi ? ::GetSysColor(COLOR_HIGHLIGHT) : RGB(255, 255, 255));
            dc.SetTextColor(hi ? ::GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(0, 0, 0));
            dc.SetBkColor(hi ? ::GetSysColor(COLOR_HIGHLIGHT) : RGB(255, 255, 255));

            wchar_t wbuf[256];
            int n = ::MultiByteToWideChar(CP_UTF8, 0, items[i].c_str(), -1, wbuf, 256);
            if (n > 0) {
                n--;
                int ty = item_rc.top + (rh - tm.tmHeight) / 2;
                ::TextOutW(dc.GetSafeHdc(), item_rc.left + 4, ty, wbuf, n);
            }
        }
        if (old_font) dc.SelectObject(old_font);

        // Pass 2: separator lines drawn on top, at the boundary pixel between items --
        // mirrors draw_grid_lines running after draw_block so lines are never overdrawn.
        CPen sep_pen(PS_SOLID, 1, RGB(191, 191, 191));
        CPen* old_pen = dc.SelectObject(&sep_pen);
        // Subtract 1 to compensate for the WS_BORDER top frame (1px): client y=0 is 1px below
        // the window top, so client y=(i+1)*rh lands 1px lower than the matching table grid line.
        for (size_t i = 0; i + 1 < items.size(); ++i) {
            int y = (int)(i + 1) * rh - 1;
            dc.MoveTo(0, y);
            dc.LineTo(rc.Width(), y);
        }
        dc.SelectObject(old_pen);
    }

    afx_msg void OnMouseMove(UINT, CPoint pt)
    {
        int rh = row_h();
        int idx = rh > 0 ? pt.y / rh : 0;
        if (idx < 0) idx = 0;
        if (idx >= (int)items.size()) idx = (int)items.size() - 1;
        if (idx != hover) { hover = idx; Invalidate(); }
    }

    afx_msg void OnLButtonUp(UINT, CPoint pt)
    {
        int rh = row_h();
        int idx = rh > 0 ? pt.y / rh : 0;
        if (idx >= 0 && idx < (int)items.size())
            hover = idx;
        TableBox* o = owner;
        int chosen = hover;
        DestroyWindow();
        o->end_combo_edit(true, chosen);
        o->SetFocus();   // mouse pick: nothing else claims focus, so reclaim it for the grid
    }

    afx_msg UINT OnGetDlgCode() { return DLGC_WANTALLKEYS; }

    afx_msg void OnKeyDown(UINT nChar, UINT, UINT)
    {
        if (nChar == VK_DOWN) { if (hover < (int)items.size() - 1) hover++; Invalidate(); return; }
        if (nChar == VK_UP)   { if (hover > 0) hover--; Invalidate(); return; }
        if (nChar == VK_RETURN) {
            TableBox* o = owner;
            int chosen = hover;
            DestroyWindow();
            o->end_combo_edit(true, chosen);
            o->SetFocus();
            return;
        }
        if (nChar == VK_ESCAPE) {
            TableBox* o = owner;
            DestroyWindow();
            o->end_combo_edit(false, -1);
            o->SetFocus();
            return;
        }
    }

    afx_msg void OnActivate(UINT state, CWnd* w, BOOL minimized)
    {
        // WA_INACTIVE means the user activated some OTHER window (could be TableBox, could be
        // something else entirely) -- never SetFocus() back here, that would steal it away.
        // Skip if owner->ending_edit: cancel_edit() is already destroying us and setting the
        // flag; re-entering DestroyWindow() here would be a double-destroy.
        if (state == WA_INACTIVE && !owner->ending_edit) {
            TableBox* o = owner;
            DestroyWindow();
            o->end_combo_edit(false, -1);
        }
    }

    virtual void PostNcDestroy() override { delete this; }

    DECLARE_MESSAGE_MAP()
};

BEGIN_MESSAGE_MAP(TableComboPopup, CWnd)
    ON_WM_PAINT()
    ON_WM_MOUSEMOVE()
    ON_WM_LBUTTONUP()
    ON_WM_GETDLGCODE()
    ON_WM_KEYDOWN()
    ON_WM_ACTIVATE()
END_MESSAGE_MAP()

void TableBox::edit_cell(int row, int col)
{
    if (edit_cb == nullptr || row < fixed_rows || col < fixed_cols)
        return;
    if (edit_box != nullptr || combo_popup != nullptr)
        return;

    const char* text = cell_text(row, col);
    int sel_idx;
    std::vector<std::string> combo_items;
    if (ParseComboSpec(text, sel_idx, combo_items))
        start_combo_edit(row, col);
    else
        start_text_edit(row, col, 0);
}

void TableBox::start_text_edit(int row, int col, wchar_t initial_char)
{
    if (edit_box != nullptr || combo_popup != nullptr)
        return;

    CRect rc;
    if (!cell_rect(row, col, rc))
        return;

    TableEditBox* box = new TableEditBox;
    box->owner = this;
    box->Create(WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, rc, this, 0);
    box->SetFont(&font);

    const char* text = cell_text(row, col);
    wchar_t wbuf[1024] = L"";
    if (text != nullptr && *text != '\0')
        ::MultiByteToWideChar(CP_UTF8, 0, text, -1, wbuf, _countof(wbuf));
    ::SetWindowTextW(box->GetSafeHwnd(), wbuf);
    box->SetSel(0, -1);

    edit_box = box;
    edit_row = row;
    edit_col = col;
    box->SetFocus();

    if (initial_char != 0)
        ::SendMessageW(box->GetSafeHwnd(), WM_CHAR, (WPARAM)initial_char, 1);

    Invalidate();
}

void TableBox::end_text_edit(bool commit, int move_dr, int move_dc)
{
    if (edit_box == nullptr || ending_edit)
        return;
    ending_edit = true;

    if (commit && edit_cb) {
        wchar_t wbuf[1024];
        ::GetWindowTextW(edit_box->GetSafeHwnd(), wbuf, _countof(wbuf));
        char utf8[1024 * 3];
        ::WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, utf8, sizeof(utf8), NULL, NULL);
        edit_cb(edit_row, edit_col, utf8, edit_cb_user);
    }

    TableEditBox* box = edit_box;
    edit_box = nullptr;
    box->DestroyWindow();
    delete box;

    ending_edit = false;
    // No SetFocus() here -- see TableEditBox::PreTranslateMessage / OnKillFocus for why the
    // caller, not end_text_edit itself, is responsible for moving focus back to the grid.

    if (commit && (move_dr != 0 || move_dc != 0))
        move_current(cur_row + move_dr, cur_col + move_dc, false);

    Invalidate();
}

void TableBox::start_combo_edit(int row, int col)
{
    if (edit_box != nullptr || combo_popup != nullptr)
        return;

    CRect rc;
    if (!cell_rect(row, col, rc))
        return;

    const char* text = cell_text(row, col);
    int sel_idx;
    std::vector<std::string> items;
    if (!ParseComboSpec(text, sel_idx, items))
        return;

    // Compute window size so the CLIENT area is exactly cell_h * N pixels tall.
    // WS_BORDER subtracts its frame from the client area; AdjustWindowRectEx adds it back.
    int n = (int)(items.empty() ? 1 : items.size());
    RECT adj = { 0, 0, rc.Width(), rc.Height() * n };
    ::AdjustWindowRectEx(&adj, WS_POPUP | WS_BORDER, FALSE, WS_EX_TOOLWINDOW);
    CRect screen_rc(rc.left, rc.bottom,
        rc.left + (adj.right - adj.left), rc.bottom + (adj.bottom - adj.top));
    ClientToScreen(&screen_rc);

    edit_row = row;
    edit_col = col;

    TableComboPopup* popup = new TableComboPopup;
    popup->open(this, screen_rc, items, sel_idx, (HFONT)font.GetSafeHandle());
    combo_popup = popup;
    Invalidate();
}

void TableBox::end_combo_edit(bool commit, int new_index)
{
    if (combo_popup == nullptr || ending_edit)
        return;
    ending_edit = true;

    if (commit && edit_cb) {
        const char* text = cell_text(edit_row, edit_col);
        int sel_idx;
        std::vector<std::string> items;
        if (ParseComboSpec(text, sel_idx, items) && !items.empty()) {
            if (new_index < 0) new_index = 0;
            if (new_index >= (int)items.size()) new_index = (int)items.size() - 1;
            std::string spec = BuildComboSpec(new_index, items);
            edit_cb(edit_row, edit_col, spec.c_str(), edit_cb_user);
        }
    }

    combo_popup = nullptr;
    ending_edit = false;
    // No SetFocus() here -- see TableComboPopup's handlers for why the caller decides.
    Invalidate();
}

void TableBox::cancel_edit()
{
    if (edit_box != nullptr) end_text_edit(false, 0, 0);
    if (combo_popup != nullptr) {
        // end_combo_edit is designed for the popup calling it AFTER self-destroying; it clears
        // the pointer but never destroys the window. Here we must destroy it ourselves.
        // Set ending_edit first so the popup's WM_ACTIVATE(WA_INACTIVE) handler (which fires
        // during DestroyWindow) is a no-op and does not double-destroy.
        TableComboPopup* p = combo_popup;
        combo_popup = nullptr;
        ending_edit = true;
        p->DestroyWindow();   // PostNcDestroy -> delete p; p is dead after this returns
        ending_edit = false;
        Invalidate();
    }
}

// Lazily prepare the double-buffer memory DC/bitmap (same pattern as ConBox::ensure_back_buffer).
void TableBox::ensure_back_buffer(CDC* ref, int w, int h)
{
    if (back_dc.GetSafeHdc() == nullptr)
        back_dc.CreateCompatibleDC(ref);

    if (back_bmp.GetSafeHandle() == nullptr || w != back_w || h != back_h) {
        if (back_bmp_saved) {
            back_dc.SelectObject(back_bmp_saved);
            back_bmp_saved = nullptr;
        }
        back_bmp.DeleteObject();

        back_bmp.CreateCompatibleBitmap(ref, w, h);
        back_bmp_saved = back_dc.SelectObject(&back_bmp);
        back_w = w;
        back_h = h;
    }
}

void TableBox::draw_block(CDC& dc, int r0, int r1, int c0, int c1, int x0, int y0)
{
    int y = y0;
    for (int r = r0; r < r1; ++r) {
        int rh = to_px(row_heights[r]);
        int x = x0;
        for (int c = c0; c < c1; ++c) {
            int cw = to_px(col_widths[c]);
            dc.SaveDC();
            dc.IntersectClipRect(x, y, x + cw, y + rh);
            draw_cell(dc, r, c, x, y, x + cw, y + rh);
            dc.RestoreDC(-1);
            x += cw;
        }
        y += rh;
    }
}

void TableBox::draw_grid_lines(CDC& dc, int r0, int r1, int c0, int c1, int x0, int y0)
{
    int total_w = 0; for (int c = c0; c < c1; ++c) total_w += to_px(col_widths[c]);
    int total_h = 0; for (int r = r0; r < r1; ++r) total_h += to_px(row_heights[r]);
    if (total_w <= 0 || total_h <= 0)
        return;

    CPen pen(PS_SOLID, 1, RGB(191, 191, 191));   // Excel-like light gray gridlines
    CPen* old = dc.SelectObject(&pen);

    int x = x0;
    for (int c = c0; c <= c1; ++c) {
        dc.MoveTo(x, y0);
        dc.LineTo(x, y0 + total_h);
        if (c < c1) x += to_px(col_widths[c]);
    }
    int y = y0;
    for (int r = r0; r <= r1; ++r) {
        dc.MoveTo(x0, y);
        dc.LineTo(x0 + total_w, y);
        if (r < r1) y += to_px(row_heights[r]);
    }

    dc.SelectObject(old);
}

const char* TableBox::cell_text(int row, int col) const
{
    return text_cb ? text_cb(row, col, text_cb_user) : nullptr;
}

// Square icon flush with the cell's right edge, side length = cell height (clamped to the
// cell's width on a very narrow column). DrawTriangle pads its own glyph 3px inside this rect.
CRect TableBox::combo_arrow_rect(const CRect& cell_rc) const
{
    int side = cell_rc.Height();
    if (side > cell_rc.Width()) side = cell_rc.Width();
    return CRect(cell_rc.right - side, cell_rc.top, cell_rc.right, cell_rc.bottom);
}

void TableBox::draw_cell(CDC& dc, int row, int col, int x0, int y0, int x1, int y1)
{
    CRect rc(x0, y0, x1, y1);
    int status = cell_status(row, col);

    if (status == -1) {
        // Highlight the column/row header if the cursor or selection passes through this axis.
        // Column header (row < fixed_rows, col >= fixed_cols): highlight when col is inside the
        // current selection's col range [min(anchor_col,cur_col)..max(anchor_col,cur_col)].
        // Row header (col < fixed_cols, row >= fixed_rows): same for the row axis.
        // Corner cell (both < fixed): never highlighted.
        bool highlight = false;
        if (col >= fixed_cols && row < fixed_rows) {
            int c0 = anchor_col < cur_col ? anchor_col : cur_col;
            int c1 = anchor_col > cur_col ? anchor_col : cur_col;
            highlight = (col >= c0 && col <= c1);
        } else if (row >= fixed_rows && col < fixed_cols) {
            int r0 = anchor_row < cur_row ? anchor_row : cur_row;
            int r1 = anchor_row > cur_row ? anchor_row : cur_row;
            highlight = (row >= r0 && row <= r1);
        }

        COLORREF bg    = highlight ? RGB(224, 221, 200) : RGB(236, 233, 210);
        COLORREF inset = highlight ? RGB(128, 128, 128) : RGB(255, 255, 255);
        dc.FillSolidRect(&rc, bg);
        // 1px inward from the cell's left/top edge: draw_grid_lines runs AFTER draw_cell and
        // redraws its gray line exactly at rc.left/rc.top, which would overwrite a line drawn
        // there.
        CPen pen(PS_SOLID, 1, inset);
        CPen* old_pen = dc.SelectObject(&pen);
        dc.MoveTo(rc.left + 1, rc.bottom - 1);
        dc.LineTo(rc.left + 1, rc.top + 1);
        dc.LineTo(rc.right - 1, rc.top + 1);
        dc.SelectObject(old_pen);
    } else if (status == 1) {
        dc.FillSolidRect(&rc, RGB(204, 228, 247));   // Excel-like range selection tint (the
                                                      // focused cell gets this too when it is
                                                      // part of a real range, like any other
                                                      // selected cell -- see cell_status)
    } else {
        dc.FillSolidRect(&rc, RGB(255, 255, 255));
    }

    const char* raw_text = cell_text(row, col);

    int sel_index;
    std::vector<std::string> combo_items;
    if (ParseComboSpec(raw_text, sel_index, combo_items)) {
        // Combo (dropdown) cell: label clipped to the area left of the arrow column, then "▼"
        // (U+25BC) centered in arrow_rc using the same cell font. arrow_rc is kept full-size
        // as the hit-test zone (OnLButtonDown) regardless of the glyph's rendered width.
        CRect arrow_rc = combo_arrow_rect(rc);
        CFont* old_font = dc.SelectObject(&font);
        dc.SetBkMode(TRANSPARENT);
        dc.SetTextColor(RGB(0, 0, 0));

        const std::string& label = (sel_index >= 0) ? combo_items[sel_index] : std::string();
        if (!label.empty()) {
            wchar_t wbuf[256];
            int n = ::MultiByteToWideChar(CP_UTF8, 0, label.c_str(), -1, wbuf, 256);
            if (n > 0) {
                n--;
                dc.SaveDC();
                dc.IntersectClipRect(rc.left, rc.top, arrow_rc.left, rc.bottom);
                int pad = to_px(4);
                int ty = y0 + ((y1 - y0) - font_tm.tmHeight) / 2;
                ::TextOutW(dc.GetSafeHdc(), x0 + pad, ty, wbuf, n);
                dc.RestoreDC(-1);
            }
        }

        {
            const wchar_t tri[] = L"\x25BC";
            RECT ar = arrow_rc;
            ::DrawTextW(dc.GetSafeHdc(), tri, 1, &ar, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        dc.SelectObject(old_font);
        return;
    }

    if (raw_text != nullptr && *raw_text != '\0') {
        wchar_t wbuf[256];
        int n = ::MultiByteToWideChar(CP_UTF8, 0, raw_text, -1, wbuf, 256);
        if (n > 0) {
            n--;   // exclude the null terminator MultiByteToWideChar counted

            CFont* old_font = dc.SelectObject(&font);
            dc.SetBkMode(TRANSPARENT);
            dc.SetTextColor(RGB(0, 0, 0));

            int pad = to_px(4);
            int ty = y0 + ((y1 - y0) - font_tm.tmHeight) / 2;
            ::TextOutW(dc.GetSafeHdc(), x0 + pad, ty, wbuf, n);

            dc.SelectObject(old_font);
        }
    }
}

void TableBox::OnPaint()
{
    CPaintDC paintDC(this);
    CRect rc;
    GetClientRect(&rc);

    ensure_back_buffer(&paintDC, rc.Width(), rc.Height());
    CDC& dc = back_dc;

    // Background covers the whole client; any area not occupied by a drawn cell (grid smaller
    // than the client, or the overscroll-free clamp leaving a gap) simply stays this color, with
    // no grid lines, satisfying the "leftover area dark gray" layout rule automatically.
    dc.FillSolidRect(&rc, RGB(64, 64, 64));

    int total_rows = (int)row_heights.size();
    int total_cols = (int)col_widths.size();
    int fr1 = fixed_rows < total_rows ? fixed_rows : total_rows;
    int fc1 = fixed_cols < total_cols ? fixed_cols : total_cols;
    int cw  = corner_w();
    int ch  = corner_h();

    int r1 = last_visible_row(scroll_row, rc.Height() - ch);
    int c1 = last_visible_col(scroll_col, rc.Width()  - cw);

    if (fr1 > 0 && fc1 > 0) {
        draw_block(dc, 0, fr1, 0, fc1, 0, 0);
        draw_grid_lines(dc, 0, fr1, 0, fc1, 0, 0);
    }
    if (fr1 > 0 && c1 > scroll_col) {
        draw_block(dc, 0, fr1, scroll_col, c1, cw, 0);
        draw_grid_lines(dc, 0, fr1, scroll_col, c1, cw, 0);
    }
    if (fc1 > 0 && r1 > scroll_row) {
        draw_block(dc, scroll_row, r1, 0, fc1, 0, ch);
        draw_grid_lines(dc, scroll_row, r1, 0, fc1, 0, ch);
    }
    bool has_body = (r1 > scroll_row && c1 > scroll_col);
    if (has_body) {
        draw_block(dc, scroll_row, r1, scroll_col, c1, cw, ch);
        draw_grid_lines(dc, scroll_row, r1, scroll_col, c1, cw, ch);
    }

    // Drawn AFTER all grid lines so it can overwrite the focused cell's grid-line edges green.
    draw_focus_border(dc);

    // The scrollbar adapts its color to the body's actual background. draw_cell is virtual, so an
    // override may not paint white; sample the just-rendered back buffer instead of assuming a
    // color. A point 2px inside the first body cell avoids landing exactly on its grid line.
    // RGB(255,255,255) is only a fallback for the (rare) case the scrollbar shows with no body
    // cell currently visible to sample (e.g. the body column/row is fully clipped).
    COLORREF body_bg = RGB(255, 255, 255);
    if (has_body && cw + 2 < rc.Width() && ch + 2 < rc.Height())
        body_bg = dc.GetPixel(cw + 2, ch + 2);

    draw_overlay_scrollbars(dc, body_bg);

    paintDC.BitBlt(0, 0, rc.Width(), rc.Height(), &back_dc, 0, 0, SRCCOPY);
}

// ===== Overlay scrollbar (auto-hide/fade) =====

bool TableBox::vsbar_geometry(CRect& gutter, CRect& track, CRect& thumb, CRect& btn_dec, CRect& btn_inc) const
{
    int total = (int)row_heights.size();
    if (total <= fixed_rows)
        return false;

    CRect rc;
    GetClientRect(&rc);
    int top = corner_h();
    int avail = rc.bottom - top;
    if (avail <= 0)
        return false;

    int mr = max_scroll_row(avail);
    if (mr <= fixed_rows)
        return false;   // everything fits; nothing to scroll

    gutter.SetRect(rc.right - to_px(SBAR_W), top, rc.right, rc.bottom);
    int btn_h = to_px(SBAR_BTN_H);
    btn_dec.SetRect(gutter.left, gutter.top, gutter.right, gutter.top + btn_h);
    btn_inc.SetRect(gutter.left, gutter.bottom - btn_h, gutter.right, gutter.bottom);
    track.SetRect(gutter.left, gutter.top + btn_h, gutter.right, gutter.bottom - btn_h);

    int track_h = track.Height();
    if (track_h <= 0)
        return false;

    int visible = last_visible_row(scroll_row, avail) - scroll_row;
    int total_range = total - fixed_rows;
    int thumb_h = (int)((double)track_h * visible / total_range);
    int min_thumb = to_px(SBAR_MIN_THUMB);
    if (thumb_h < min_thumb) thumb_h = min_thumb;
    if (thumb_h > track_h)   thumb_h = track_h;

    int span = mr - fixed_rows;
    int thumb_y = track.top + (span > 0 ? (int)((double)(track_h - thumb_h) * (scroll_row - fixed_rows) / span) : 0);
    if (thumb_y < track.top)              thumb_y = track.top;
    if (thumb_y > track.bottom - thumb_h) thumb_y = track.bottom - thumb_h;

    int thumb_w = to_px(SBAR_THUMB_W);
    int tx = track.right - to_px(SBAR_INSET) - thumb_w;
    thumb.SetRect(tx, thumb_y, tx + thumb_w, thumb_y + thumb_h);
    return true;
}

bool TableBox::hsbar_geometry(CRect& gutter, CRect& track, CRect& thumb, CRect& btn_dec, CRect& btn_inc) const
{
    int total = (int)col_widths.size();
    if (total <= fixed_cols)
        return false;

    CRect rc;
    GetClientRect(&rc);
    int left = corner_w();
    int avail = rc.right - left;
    if (avail <= 0)
        return false;

    int mc = max_scroll_col(avail);
    if (mc <= fixed_cols)
        return false;

    gutter.SetRect(left, rc.bottom - to_px(SBAR_W), rc.right, rc.bottom);
    int btn_w = to_px(SBAR_BTN_H);
    btn_dec.SetRect(gutter.left, gutter.top, gutter.left + btn_w, gutter.bottom);
    btn_inc.SetRect(gutter.right - btn_w, gutter.top, gutter.right, gutter.bottom);
    track.SetRect(gutter.left + btn_w, gutter.top, gutter.right - btn_w, gutter.bottom);

    int track_w = track.Width();
    if (track_w <= 0)
        return false;

    int visible = last_visible_col(scroll_col, avail) - scroll_col;
    int total_range = total - fixed_cols;
    int thumb_w = (int)((double)track_w * visible / total_range);
    int min_thumb = to_px(SBAR_MIN_THUMB);
    if (thumb_w < min_thumb) thumb_w = min_thumb;
    if (thumb_w > track_w)   thumb_w = track_w;

    int span = mc - fixed_cols;
    int thumb_x = track.left + (span > 0 ? (int)((double)(track_w - thumb_w) * (scroll_col - fixed_cols) / span) : 0);
    if (thumb_x < track.left)             thumb_x = track.left;
    if (thumb_x > track.right - thumb_w)  thumb_x = track.right - thumb_w;

    int thumb_h = to_px(SBAR_THUMB_W);
    int ty = track.bottom - to_px(SBAR_INSET) - thumb_h;
    thumb.SetRect(thumb_x, ty, thumb_x + thumb_w, ty + thumb_h);
    return true;
}

// Resolve a point that hit-tests true for both bars' gutters (the shared bottom-right corner
// square) down to one bar, so the two never both react to the same point: keep whichever bar is
// already showing; if neither (or, degenerately, both) is, vertical wins.
void TableBox::resolve_sbar_corner(bool& want_v, bool& want_h) const
{
    if (want_v && want_h) {
        bool v_active = vsbar.alpha > 0;
        bool h_active = hsbar.alpha > 0;
        if (h_active && !v_active)
            want_v = false;
        else
            want_h = false;
    }
}

// Make the overlay fully opaque and (re)start the shared fade timer.
void TableBox::show_sbar(SbarState& s)
{
    if (!::IsWindow(m_hWnd))
        return;
    s.alpha = 255;
    s.hold_until = ::GetTickCount() + SBAR_HOLD_MS;
    SetTimer(SBAR_TIMER, SBAR_FADE_TICK_MS, NULL);
    Invalidate(FALSE);
}

// Draw one bar: gutter background, then the two arrow buttons, then the thumb on top.
void TableBox::draw_sbar_axis(CDC& dc, const CRect& gutter, const CRect& thumb,
                               const CRect& btn_dec, const CRect& btn_inc, const SbarState& s, bool vertical,
                               COLORREF body_bg)
{
    COLORREF thumb_color, gutter_color;
    SbarColors(body_bg, thumb_color, gutter_color);

    FillTranslucent(dc, gutter, gutter_color, (BYTE)(SBAR_GUTTER_OP * s.alpha / 255));

    bool active = s.hover || s.dragging;
    int op = active ? SBAR_OP_HOVER : SBAR_OP_IDLE;
    int radius = to_px(SBAR_RADIUS);
    DrawRoundedThumb(dc, thumb, thumb_color, op, radius, (BYTE)s.alpha);
    DrawTriangle(dc, btn_dec, thumb_color, op, (BYTE)s.alpha, vertical ? SbarDir::UP   : SbarDir::LEFT);
    DrawTriangle(dc, btn_inc, thumb_color, op, (BYTE)s.alpha, vertical ? SbarDir::DOWN : SbarDir::RIGHT);
}

void TableBox::draw_overlay_scrollbars(CDC& dc, COLORREF body_bg)
{
    CRect gutter, track, thumb, btn_dec, btn_inc;
    if (vsbar.alpha > 0 && vsbar_geometry(gutter, track, thumb, btn_dec, btn_inc))
        draw_sbar_axis(dc, gutter, thumb, btn_dec, btn_inc, vsbar, true, body_bg);
    if (hsbar.alpha > 0 && hsbar_geometry(gutter, track, thumb, btn_dec, btn_inc))
        draw_sbar_axis(dc, gutter, thumb, btn_dec, btn_inc, hsbar, false, body_bg);
}

void TableBox::OnSetFocus(CWnd* pOldWnd)
{
    CWnd::OnSetFocus(pOldWnd);
    has_focus = true;
    Invalidate();
}

void TableBox::OnKillFocus(CWnd* pNewWnd)
{
    CWnd::OnKillFocus(pNewWnd);
    has_focus = false;
    Invalidate();
}

UINT TableBox::OnGetDlgCode()
{
    return DLGC_WANTARROWS | DLGC_WANTALLKEYS;
}

void TableBox::OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags)
{
    bool shift = (::GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    bool ctrl  = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;

    CRect rc;
    GetClientRect(&rc);
    int page_rows = last_visible_row(scroll_row, rc.Height() - corner_h()) - scroll_row;
    if (page_rows < 1) page_rows = 1;

    int row = cur_row, col = cur_col;
    switch (nChar) {
    case VK_LEFT:  col -= 1; break;
    case VK_RIGHT: col += 1; break;
    case VK_UP:    row -= 1; break;
    case VK_DOWN:  row += 1; break;
    case VK_PRIOR: row -= page_rows; break;
    case VK_NEXT:  row += page_rows; break;
    case VK_HOME:  col = fixed_cols;       if (ctrl) row = fixed_rows;     break;
    case VK_END:   col = last_col_index(); if (ctrl) row = last_row_index(); break;
    case VK_RETURN:
        // Enters edit mode rather than moving the selection; only ever reached while TableBox
        // itself has focus, i.e. not already editing (the edit box/combo popup own focus then).
        edit_cell(cur_row, cur_col);
        return;
    default:
        CWnd::OnKeyDown(nChar, nRepCnt, nFlags);
        return;
    }
    move_current(row, col, shift);
}

// A printable character typed while a body cell is focused starts edit mode and replaces the
// cell's content with that character (Excel's type-to-overwrite). Combo cells are excluded --
// they have no free-text meaning, only a list pick (see Requirements.md/edit_cell). Only fires
// while TableBox itself has focus, i.e. not already editing.
void TableBox::OnChar(UINT nChar, UINT nRepCnt, UINT nFlags)
{
    if (nChar >= 0x20 && nChar != 0x7F &&
        cur_row >= fixed_rows && cur_col >= fixed_cols &&
        edit_cb != nullptr && edit_box == nullptr && combo_popup == nullptr) {
        int sel_idx;
        std::vector<std::string> combo_items;
        if (!ParseComboSpec(cell_text(cur_row, cur_col), sel_idx, combo_items)) {
            start_text_edit(cur_row, cur_col, (wchar_t)nChar);
            return;
        }
    }
    CWnd::OnChar(nChar, nRepCnt, nFlags);
}

BOOL TableBox::OnMouseWheel(UINT flags, short zDelta, CPoint pt)
{
    cancel_edit();
    int amount = (zDelta / 120) * 3;
    if (flags & MK_SHIFT) {
        scroll_col -= amount;
        clamp_scroll();
        show_sbar(hsbar);
    } else {
        scroll_row -= amount;
        clamp_scroll();
        show_sbar(vsbar);
    }
    Invalidate();
    return TRUE;
}

void TableBox::OnLButtonDown(UINT flags, CPoint pt)
{
    cancel_edit();
    SetFocus();

    int hc = hit_col_border(pt);
    if (hc >= 0) {
        resize_col = hc;
        resize_start_pt = pt.x;
        resize_start_sz = col_widths[hc];
        col_selection_span(hc, resize_group_c0, resize_group_c1);
        SetCapture();
        return;
    }
    int hr = hit_row_border(pt);
    if (hr >= 0) {
        resize_row = hr;
        resize_start_pt = pt.y;
        resize_start_sz = row_heights[hr];
        row_selection_span(hr, resize_group_r0, resize_group_r1);
        SetCapture();
        return;
    }

    CRect vg, vtrack, vthumb, vbd, vbi;
    CRect hg, htrack, hthumb, hbd, hbi;
    bool want_v = vsbar_geometry(vg, vtrack, vthumb, vbd, vbi) && vg.PtInRect(pt);
    bool want_h = hsbar_geometry(hg, htrack, hthumb, hbd, hbi) && hg.PtInRect(pt);
    resolve_sbar_corner(want_v, want_h);

    if (want_v) {
        if (vbd.PtInRect(pt)) {
            scroll_row -= 1;
            clamp_scroll();
            Invalidate();
        } else if (vbi.PtInRect(pt)) {
            scroll_row += 1;
            clamp_scroll();
            Invalidate();
        } else if (vthumb.PtInRect(pt)) {
            vsbar.dragging = true;
            vsbar.drag_off = pt.y - vthumb.top;
            SetCapture();
        } else if (vtrack.PtInRect(pt)) {
            CRect rc; GetClientRect(&rc);
            int page = last_visible_row(scroll_row, rc.Height() - corner_h()) - scroll_row;
            int amount = page * 3 / 4;
            if (amount < 1) amount = 1;
            scroll_row += (pt.y < vthumb.top) ? -amount : amount;
            clamp_scroll();
            Invalidate();
        }
        show_sbar(vsbar);
        return;
    }
    if (want_h) {
        if (hbd.PtInRect(pt)) {
            scroll_col -= 1;
            clamp_scroll();
            Invalidate();
        } else if (hbi.PtInRect(pt)) {
            scroll_col += 1;
            clamp_scroll();
            Invalidate();
        } else if (hthumb.PtInRect(pt)) {
            hsbar.dragging = true;
            hsbar.drag_off = pt.x - hthumb.left;
            SetCapture();
        } else if (htrack.PtInRect(pt)) {
            CRect rc; GetClientRect(&rc);
            int page = last_visible_col(scroll_col, rc.Width() - corner_w()) - scroll_col;
            int amount = page * 3 / 4;
            if (amount < 1) amount = 1;
            scroll_col += (pt.x < hthumb.left) ? -amount : amount;
            clamp_scroll();
            Invalidate();
        }
        show_sbar(hsbar);
        return;
    }

    int row, col;
    if (hit_test(pt, row, col)) {
        bool extend = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;

        if (row < fixed_rows && col < fixed_cols) {
            // Top-left corner cell: select the entire grid (Excel's Select All button). Focus
            // lands on the first body cell, same no-auto-scroll rule as the header click cases.
            select_range(last_row_index(), last_col_index(), fixed_rows, fixed_cols, false);
        } else if (row < fixed_rows) {
            // Column header: select the whole column. A fresh (non-Shift) click also starts a
            // drag-extendable multi-column selection (Excel-style header drag-select; see
            // header_drag_col in OnMouseMove) -- Shift-click stays one-shot (anchor unchanged).
            // Focus lands on the column's first body cell, and selecting does NOT auto-scroll --
            // jumping the viewport to wherever the last row happens to be would be jarring;
            // only an explicit arrow key afterward should move it.
            int last_row = last_row_index();
            if (extend) {
                select_range(anchor_row, anchor_col, last_row, col, false);
            } else {
                select_range(last_row, col, fixed_rows, col, false);
                header_drag_col = true;
                SetCapture();
            }
        } else if (col < fixed_cols) {
            // Row header: select the whole row. Same drag-extend rule as the column header case.
            int last_col = last_col_index();
            if (extend) {
                select_range(anchor_row, anchor_col, row, last_col, false);
            } else {
                select_range(row, last_col, row, fixed_cols, false);
                header_drag_row = true;
                SetCapture();
            }
        } else {
            // Body cell. A plain (non-extending) click on a combo cell's arrow icon opens its
            // dropdown immediately (matches a real ComboBox's drop-button click); otherwise
            // start a drag-extendable range selection as before.
            move_current(row, col, extend);
            CRect cell_rc;
            int sel_idx;
            std::vector<std::string> combo_items;
            if (!extend && cell_rect(row, col, cell_rc) &&
                ParseComboSpec(cell_text(row, col), sel_idx, combo_items) &&
                combo_arrow_rect(cell_rc).PtInRect(pt)) {
                edit_cell(row, col);
                return;
            }
            selecting = true;
            SetCapture();
        }
    }
}

void TableBox::OnMouseMove(UINT flags, CPoint pt)
{
    if (resize_col >= 0) {
        int delta = ::MulDiv(pt.x - resize_start_pt, 96, box_dpi > 0 ? box_dpi : 96);
        int w = resize_start_sz + delta;
        if (w < MIN_CELL_SIZE) w = MIN_CELL_SIZE;
        bool changed = false;
        for (int c = resize_group_c0; c <= resize_group_c1; ++c) {
            if (col_widths[c] != w) { col_widths[c] = w; changed = true; }
        }
        if (changed) { clamp_scroll(); Invalidate(); }
        return;
    }
    if (resize_row >= 0) {
        int delta = ::MulDiv(pt.y - resize_start_pt, 96, box_dpi > 0 ? box_dpi : 96);
        int h = resize_start_sz + delta;
        if (h < MIN_CELL_SIZE) h = MIN_CELL_SIZE;
        bool changed = false;
        for (int r = resize_group_r0; r <= resize_group_r1; ++r) {
            if (row_heights[r] != h) { row_heights[r] = h; changed = true; }
        }
        if (changed) { clamp_scroll(); Invalidate(); }
        return;
    }

    if (header_drag_col) {
        int row, col;
        hit_test(pt, row, col);
        select_range(anchor_row, anchor_col, fixed_rows, col, false);
        return;
    }
    if (header_drag_row) {
        int row, col;
        hit_test(pt, row, col);
        select_range(anchor_row, anchor_col, row, fixed_cols, false);
        return;
    }

    if (selecting) {
        int row, col;
        if (hit_test(pt, row, col))
            move_current(row, col, true);
    }

    CRect vg, vtrack, vthumb, vbd, vbi;
    CRect hg, htrack, hthumb, hbd, hbi;
    bool v_geo = vsbar_geometry(vg, vtrack, vthumb, vbd, vbi);
    bool h_geo = hsbar_geometry(hg, htrack, hthumb, hbd, hbi);

    // Hover ambiguity (the shared bottom-right corner) only matters when neither bar is being
    // dragged; an active drag already commits to its own axis regardless of where the cursor is.
    bool want_v_hover = v_geo && vg.PtInRect(pt);
    bool want_h_hover = h_geo && hg.PtInRect(pt);
    if (!vsbar.dragging && !hsbar.dragging)
        resolve_sbar_corner(want_v_hover, want_h_hover);

    if (vsbar.dragging) {
        CRect rc; GetClientRect(&rc);
        int mr = max_scroll_row(rc.Height() - corner_h());
        int span = mr - fixed_rows;
        int range = vtrack.Height() - vthumb.Height();
        int nr = fixed_rows;
        if (span > 0 && range > 0)
            nr = fixed_rows + (int)((double)(pt.y - vsbar.drag_off - vtrack.top) * span / range + 0.5);
        if (nr < fixed_rows) nr = fixed_rows;
        if (nr > mr)         nr = mr;
        if (nr != scroll_row) { scroll_row = nr; Invalidate(); }
        show_sbar(vsbar);
    } else if (v_geo) {
        if (want_v_hover != vsbar.hover) { vsbar.hover = want_v_hover; if (want_v_hover) show_sbar(vsbar); }
    }

    if (hsbar.dragging) {
        CRect rc; GetClientRect(&rc);
        int mc = max_scroll_col(rc.Width() - corner_w());
        int span = mc - fixed_cols;
        int range = htrack.Width() - hthumb.Width();
        int nc = fixed_cols;
        if (span > 0 && range > 0)
            nc = fixed_cols + (int)((double)(pt.x - hsbar.drag_off - htrack.left) * span / range + 0.5);
        if (nc < fixed_cols) nc = fixed_cols;
        if (nc > mc)         nc = mc;
        if (nc != scroll_col) { scroll_col = nc; Invalidate(); }
        show_sbar(hsbar);
    } else if (h_geo) {
        if (want_h_hover != hsbar.hover) { hsbar.hover = want_h_hover; if (want_h_hover) show_sbar(hsbar); }
    }

    TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hWnd, 0 };
    ::TrackMouseEvent(&tme);
}

void TableBox::OnLButtonUp(UINT flags, CPoint pt)
{
    if (resize_col >= 0) { resize_col = -1; ReleaseCapture(); return; }
    if (resize_row >= 0) { resize_row = -1; ReleaseCapture(); return; }
    if (header_drag_col) { header_drag_col = false; ReleaseCapture(); return; }
    if (header_drag_row) { header_drag_row = false; ReleaseCapture(); return; }
    if (vsbar.dragging) { vsbar.dragging = false; ReleaseCapture(); show_sbar(vsbar); }
    if (hsbar.dragging) { hsbar.dragging = false; ReleaseCapture(); show_sbar(hsbar); }
    if (selecting)      { selecting = false; ReleaseCapture(); }
}

void TableBox::OnLButtonDblClk(UINT flags, CPoint pt)
{
    int hc = hit_col_border(pt);
    if (hc >= 0) {
        int c0, c1;
        col_selection_span(hc, c0, c1);
        for (int c = c0; c <= c1; ++c) autofit_col(c);   // each column fitted to its own content
        return;
    }
    int hr = hit_row_border(pt);
    if (hr >= 0) {
        int r0, r1;
        row_selection_span(hr, r0, r1);
        for (int r = r0; r <= r1; ++r) autofit_row(r);   // each row fitted to its own content
        return;
    }

    int row, col;
    if (hit_test(pt, row, col) && row >= fixed_rows && col >= fixed_cols) {
        move_current(row, col, false);
        edit_cell(row, col);
        return;
    }
    CWnd::OnLButtonDblClk(flags, pt);
}

BOOL TableBox::OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message)
{
    if (nHitTest == HTCLIENT) {
        CPoint pt;
        ::GetCursorPos(&pt);
        ScreenToClient(&pt);
        if (resize_col >= 0 || hit_col_border(pt) >= 0) {
            ::SetCursor(::LoadCursorW(nullptr, (LPCWSTR)IDC_SIZEWE));
            return TRUE;
        }
        if (resize_row >= 0 || hit_row_border(pt) >= 0) {
            ::SetCursor(::LoadCursorW(nullptr, (LPCWSTR)IDC_SIZENS));
            return TRUE;
        }
    }
    return CWnd::OnSetCursor(pWnd, nHitTest, message);
}

void TableBox::OnMouseLeave()
{
    vsbar.hover = false;
    hsbar.hover = false;
}

void TableBox::OnTimer(UINT_PTR id)
{
    if (id == SBAR_TIMER) {
        bool any_visible = false;

        if (vsbar.dragging || vsbar.hover) {
            vsbar.hold_until = ::GetTickCount() + SBAR_HOLD_MS;
        } else if ((int)(::GetTickCount() - vsbar.hold_until) >= 0) {
            vsbar.alpha -= SBAR_FADE_STEP;
            if (vsbar.alpha < 0) vsbar.alpha = 0;
        }
        if (vsbar.alpha > 0) any_visible = true;

        if (hsbar.dragging || hsbar.hover) {
            hsbar.hold_until = ::GetTickCount() + SBAR_HOLD_MS;
        } else if ((int)(::GetTickCount() - hsbar.hold_until) >= 0) {
            hsbar.alpha -= SBAR_FADE_STEP;
            if (hsbar.alpha < 0) hsbar.alpha = 0;
        }
        if (hsbar.alpha > 0) any_visible = true;

        if (!any_visible)
            KillTimer(SBAR_TIMER);
        Invalidate(FALSE);
        return;
    }
    CWnd::OnTimer(id);
}

void TableBox::OnSize(UINT type, int cx, int cy)
{
    cancel_edit();
    CWnd::OnSize(type, cx, cy);

    int cur = (int)::GetDpiForWindow(m_hWnd);
    if (cur > 0 && cur != box_dpi) {
        box_dpi = cur;
        build_font();
    }

    clamp_scroll();
    Invalidate();
}

BOOL TableBox::OnEraseBkgnd(CDC* dc)
{
    // Background is painted in OnPaint to avoid flicker.
    return TRUE;
}
