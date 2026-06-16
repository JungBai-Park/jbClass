// TableBox.cpp
//
// Implementation of TableBox. See TableBox.h for the public contract; read this file only
// when modifying behavior.
//
// === File layout ===
//   helpers (static) : ParseFontOpts, SbarColors, DrawAA, DrawRoundedThumb, DrawTriangle,
//                       FillTranslucent
//   message map       : BEGIN_MESSAGE_MAP ... END_MESSAGE_MAP
//   ctor/dtor         : TableBox, ~TableBox
//   window/font       : open, build_font
//   grid definition   : set_cols x2, set_rows x2, set_fixed, set_callback, set_font
//   layout            : to_px, corner_w, corner_h, last_visible_row/col, max_scroll_row/col,
//                        clamp_scroll
//   painting          : ensure_back_buffer, draw_block, draw_grid_lines, draw_cell, cell_text, OnPaint
//   overlay scrollbar : vsbar_geometry, hsbar_geometry, resolve_sbar_corner, show_sbar,
//                        draw_sbar_axis, draw_overlay_scrollbars
//   input             : OnMouseWheel, OnLButtonDown, OnMouseMove, OnLButtonUp, OnMouseLeave, OnTimer
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

BEGIN_MESSAGE_MAP(TableBox, CWnd)
    ON_WM_PAINT()
    ON_WM_SIZE()
    ON_WM_ERASEBKGND()
    ON_WM_MOUSEWHEEL()
    ON_WM_LBUTTONDOWN()
    ON_WM_MOUSEMOVE()
    ON_WM_LBUTTONUP()
    ON_WM_MOUSELEAVE()
    ON_WM_TIMER()
END_MESSAGE_MAP()

TableBox::TableBox()
{
    col_widths.assign(10000, 75);
    row_heights.assign(10000, 20);

    fixed_rows = 1;
    fixed_cols = 1;
    scroll_row = fixed_rows;
    scroll_col = fixed_cols;

    text_cb = nullptr;
    text_cb_user = nullptr;

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
    if (::IsWindow(m_hWnd)) { clamp_scroll(); Invalidate(); }
}

void TableBox::set_cols(const std::vector<int>& widths)
{
    col_widths = widths;
    if (::IsWindow(m_hWnd)) { clamp_scroll(); Invalidate(); }
}

void TableBox::set_rows(int height, int limit)
{
    row_heights.assign(limit, height);
    if (::IsWindow(m_hWnd)) { clamp_scroll(); Invalidate(); }
}

void TableBox::set_rows(const std::vector<int>& heights)
{
    row_heights = heights;
    if (::IsWindow(m_hWnd)) { clamp_scroll(); Invalidate(); }
}

void TableBox::set_fixed(int rows, int cols)
{
    fixed_rows = rows;
    fixed_cols = cols;
    if (scroll_row < fixed_rows) scroll_row = fixed_rows;
    if (scroll_col < fixed_cols) scroll_col = fixed_cols;
    if (::IsWindow(m_hWnd)) { clamp_scroll(); Invalidate(); }
}

void TableBox::set_callback(const char* (*cb)(int row, int col, void* user), void* user)
{
    text_cb = cb;
    text_cb_user = user;
    if (::IsWindow(m_hWnd)) Invalidate();
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

void TableBox::draw_cell(CDC& dc, int row, int col, int x0, int y0, int x1, int y1)
{
    CRect rc(x0, y0, x1, y1);
    bool fixed = (row < fixed_rows) || (col < fixed_cols);

    if (fixed) {
        dc.FillSolidRect(&rc, ::GetSysColor(COLOR_BTNFACE));
        dc.Draw3dRect(&rc, ::GetSysColor(COLOR_3DHILIGHT), ::GetSysColor(COLOR_3DSHADOW));
    } else {
        dc.FillSolidRect(&rc, RGB(255, 255, 255));
    }

    const char* text = cell_text(row, col);
    if (text == nullptr || *text == '\0')
        return;

    wchar_t wbuf[256];
    int n = ::MultiByteToWideChar(CP_UTF8, 0, text, -1, wbuf, 256);
    if (n <= 0)
        return;
    n--;   // exclude the null terminator MultiByteToWideChar counted

    CFont* old_font = dc.SelectObject(&font);
    dc.SetBkMode(TRANSPARENT);
    dc.SetTextColor(RGB(0, 0, 0));

    int pad = to_px(4);
    int ty = y0 + ((y1 - y0) - font_tm.tmHeight) / 2;
    ::TextOutW(dc.GetSafeHdc(), x0 + pad, ty, wbuf, n);

    dc.SelectObject(old_font);
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

BOOL TableBox::OnMouseWheel(UINT flags, short zDelta, CPoint pt)
{
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
    SetFocus();

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
}

void TableBox::OnMouseMove(UINT flags, CPoint pt)
{
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
    if (vsbar.dragging) { vsbar.dragging = false; ReleaseCapture(); show_sbar(vsbar); }
    if (hsbar.dragging) { hsbar.dragging = false; ReleaseCapture(); show_sbar(hsbar); }
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
