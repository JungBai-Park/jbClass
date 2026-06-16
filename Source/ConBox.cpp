// ConBox.cpp
//
// Implementation of ConBox. To only USE the control, ConBox.h is enough; read this file when
// MODIFYING behavior. Use the table of contents below to Grep to one function instead of reading
// the whole file.
//
// === File layout (roughly this order) ===
//   helpers (static): IsWideChar, ParseFontOpts, DrawBlockElement, DrawBoxLine
//   message map     : BEGIN_MESSAGE_MAP ... END_MESSAGE_MAP
//   window/font     : open, make_font, apply_default_fonts, build_efont, build_kfont, calc_cell_size,
//                     set_efont, set_kfont, relayout_for_dpi, set_builtin_glyphs
//   color/cursor    : set_fg_color, set_bg_color, set_cursor_blend, set_cursor_blink, set_cursor, bump_cursor, remap_paper_color, OnTimer
//   margin          : set_margin, grid_size
//   config          : setup_from_ini, setup, CreateDefaultIni(static), ResolveIniPath(static), ParseIni(static), ParseColor(static)
//   cursor geometry : blend_cursor_color, cursor_screen_pos, get_cursor_rect, is_hangul_mode
//   grid/output     : update_metrics, recalc_origin, snap_to_grid, blank_row, reset_screen, line_at, clamp_cursor, erase_cells,
//                     scroll_lines_up/down, scroll_up_region, scroll_down_region, line_feed,
//                     insert_lines, delete_lines, insert_chars, delete_chars,
//                     enter_alt_screen, leave_alt_screen, put_char, print, update_scrollbar
//   VT parser       : Xterm256ToRgb(static), vt_feed, dispatch_csi
//   window messages : OnEraseBkgnd, OnSize (also detects DPI change), OnGetDlgCode, OnMouseWheel, OnDpiChanged
//   overlay scrollbar: sbar_geometry, sbar_show, draw_overlay_scrollbar, OnMouseLeave
//   IME (Korean)    : OnImeStart, OnImeComp, OnImeEnd, OnImeNotify
//   input           : set_input_sink, set_resize_sink, send_input_bytes, send_input_wide,
//                     terminal_keydown, paste_clipboard, finalize_composition, OnChar, OnKeyDown
//   focus           : OnSetFocus, OnKillFocus
//   mouse selection : hit_test, copy_selection, clear_selection, OnLButtonDown, OnLButtonDblClk,
//                     OnLButtonUp, OnMouseMove, OnRButtonDown, OnDropFiles
//   painting        : ensure_back_buffer, OnPaint
//   export          : save_emf, save_pdf, get_text_lines, save_log
//   child (ConPTY)  : Utf8ToWide/CreatePtyPipes(static), start(no-arg)/start(cmdline), write, resize, stop, is_running,
//                     set_exit_callback, pump, handle_child_exit, child_input_thunk/child_resize_thunk, OnDestroy
//
// ===== DESIGN OVERVIEW =====
// The big picture, data model and extension points live here (no separate design doc). When reading
// one function, its header note "(design: [TAG])" points to the matching [TAG] item below, so you
// need not read the whole file. Per-member detail is in ConBox.h; per-function detail is each
// function's own comment.
//
// [DM] Data model (cell grid)
//   screen: vector<Row>      current screen, always rows lines x cols cells. Row = vector<CharInfo>.
//   CharInfo{ wchar_t ch; uint8_t flags; COLORREF fg,bg; }  flags: CELL_WIDE|BOLD|ITALIC|UNDERLINE|STRIKE|BLINK|DOUBLE
//     CELL_WIDE = lead cell of double-width glyph (trail has ch=0).
//     CELL_DOUBLE (SGR 8/28) = glyph rendered at 2x size (always bold). English=2x2 cells, Korean=4x2 cells.
//       Cursor auto-advances 2x (put_char advance=w*2) so no manual spacing needed between big chars.
//       Bleed: upward (2x ascent) and rightward (2x width); OnPaint renders columns right-to-left so
//       a double glyph's rightward bleed overwrites already-drawn cells (not later ones). No 2nd pass.
//   scrollback: deque<Row>   lines pushed off the top (capped at max_scrollback, oldest dropped; deque for O(1) pop_front).
//   main_saved: vector<Row>  main screen backed up on alt-screen entry (swapped back on leave).
//   Key state: cur_row/cur_col (0-based screen cell cursor), view_top (top of view, unified index),
//     cursor_visible (?25), saved_cur (DECSC/RC), scroll_top/bot (DECSTBM region),
//     alt_active/saved_main_cur (alt screen), cols/rows, cell_w/h/base, margin_*, glyph_level,
//     vt_state/params/priv/gtlt/space (parser; persists across chunked print()), cur_fg/bg/bold/italic/underline/strike/blink/reverse (SGR),
//     app_cursor_keys/bracketed_paste (input encoding the child turned on via DEC modes),
//     back_dc/bmp (double buffer). ConPTY: h_pc/in_write/out_read/child_proc/child_running/exit_cb
//     (dormant when start() is unused).
//
// [COORD] Coordinate systems
//   Cell grid (cur_row,cur_col): screen cells. A wide glyph is lead+trail (2 cells = 2 visual columns).
//   Unified index idx: scrollback first, then screen. line_at(idx) returns the line. Total lines =
//     scrollback.size() + rows.
//   Screen/pixel: line relative to view_top, plus left/top margin.
//
// [VT] Output parser (print -> vt_feed -> dispatch_csi)
//   print: UTF-8 -> UTF-16, each char fed to the vt_feed state machine; then scroll-to-bottom,
//     bump cursor, repaint.
//   vt_feed: GROUND (glyph -> put_char; C0: CR/LF/BS/TAB(mult of 8)/BEL ignored) / ESC (7,8=DECSC/RC,
//     D=IND, E=NEL, M=RI, c=RIS, [=CSI, ]=OSC) / CSI (accumulate params, ?=priv, <=>=gtlt, ' '=space
//     (DECSCUSR), final byte -> dispatch_csi) / OSC (discard up to BEL/ST).
//   dispatch_csi: drops the whole sequence if gtlt (private <>= sequence; avoids final-byte misparse).
//     Otherwise cursor moves (CUU/CUD/CUF/CUB/CUP/HVP/CHA/VPA/CNL/CPL), erase ED/EL/ECH, SGR (m), modes h/l
//     (?25, ?1049/47, ?1048, ?1, ?2004), save/restore (s/u), full-screen (DECSTBM r, IL/DL, ICH/DCH,
//     SU/SD), queries (DSR n -> CPR/status to input sink, DA c -> VT102).
//   put_char: autowrap past width (advance=w*2 if cur_double), swap colors when reverse, wide glyph
//     sets the next cell to trail. double mode (CELL_DOUBLE) stores the glyph in 1 cell but advances
//     cur_col by w*2 so subsequent chars land past the 2x visual span without manual spacing.
//     line_feed: scroll_up_region at the region bottom (scroll_bot), else advance one line.
//   Scroll workers: scroll_lines_up/down (pure rotation within a range), scroll_up_region (preserves
//     displaced lines into scrollback only on the main screen with top==0), enter/leave_alt_screen
//     (swap with main_saved).
//
// [FONT] Font / cell size (calc_cell_size; full algorithm in that function's comment)
//   Fixed cell_w/h/base from font metrics. cell_w = ceil(max(2*w_e, w_k)/2) where w_e/w_k are the
//   width-ratio-adjusted English/Korean natural widths. English font is built to cell_w, Korean to
//   2*cell_w for final rendering. Match mode (set_kfont size<=0, default): Korean height matched to
//   English; off (size>0): Korean keeps its own height. adjust(l,t,r,b) then pads each cell side in px
//   (cell_w+=l+r, cell_h+=t+b; left/top also shift the glyph; built-in block/box glyphs stay full-cell).
//
// [PAINT] Drawing (OnPaint, double buffered)
//   Draw a whole frame into a memory DC then BitBlt (no flicker). Walk rows top-to-bottom, columns
//   RIGHT-TO-LEFT within each row. right-to-left order: CELL_DOUBLE glyphs bleed rightward; by the
//   time a double cell is drawn its right neighbors are already laid down, so the bleed overwrites
//   them (not erased by later renders). Wide=2 cells in kfont; trail/blank cells skip glyph. TA_BASELINE.
//   Block/box chars painted directly by DrawBlockElement/DrawBoxLine per glyph_level.
//   CELL_DOUBLE: drawn inline (not a second pass) using efont_double/kfont_double (2x, always bold);
//   underline/strike span the doubled width.
//   Cursor: when cursor_on && cursor_visible, fill get_cursor_rect with blend_cursor_color (2 cells in
//   Korean mode, 1 in English) and redraw the covered glyph(s) on top. child_caret (the child paints
//   the cursor cell as reverse = its bg differs from neighbors) suppresses the ConBox block (avoids a
//   double cursor).
//   Overlay scrollbar: last, draw_overlay_scrollbar AlphaBlends a thumb over the right edge (no native
//   WS_VSCROLL, so the client never shrinks). Auto-hide/fade via SBAR_TIMER; shown on user scroll/hover
//   (sbar_show), dragged via the mouse handlers. sbar_geometry derives the track/thumb from view_top.
//
// [IME] Korean composition (OnImeStart/Comp/End/Notify + comp_str in OnPaint)
//   Only the committed part (GCS_RESULTSTR) goes to the child via send_input_wide. The uncommitted
//   part (GCS_COMPSTR) is held in comp_str and drawn by ConBox (hollow 1px outline, pinned to the
//   cursor cell; OnImeComp returns 0 to suppress system inline + WM_CHAR duplication). Mid-line
//   composition shifts the line right via ScrollDC to preview insertion (for insert-mode children like
//   the Python REPL). Order guarantee: finalize_composition force-commits with ImmNotifyIME
//   (CPS_COMPLETE) right before a trigger key -> [completed][trigger]. Cursor width: is_hangul_mode
//   (ImmGetConversionStatus IME_CMODE_NATIVE); OnImeNotify reflects the Korean/English toggle at once.
//   A commit advances the child cursor one glyph right, so OnKeyDown corrects a plain Left/Right after
//   a commit (swallow Right / double Left), gated by the ime_committed flag (OnImeComp sets it; the MS
//   IME pre-commits before the arrow's WM_KEYDOWN so finalize's return value can't be used).
//
// [PTY] Child runner (start/write/resize/stop/pump/handle_child_exit; "child (ConPTY)" section below)
//   start: CreatePtyPipes (pipes + CreatePseudoConsole) -> STARTUPINFOEX+attr -> CreateProcessW ->
//     close PTY-side ends -> wire itself via set_input_sink/set_resize_sink -> SetTimer(PUMP_TIMER).
//     Input -> write -> child stdin; grid change -> resize -> ResizePseudoConsole. pump (PUMP_TIMER
//     polling): PeekNamedPipe -> ReadFile -> print. Exit detection: ConPTY's conhost keeps the output
//     pipe's write end open after the child exits, so EOF is never seen; poll the process handle
//     (WaitForSingleObject) instead -> handle_child_exit (stop then exit_cb). Lifetime: OnDestroy
//     (and the dtor) call stop(). With no start(), the ConPTY members stay dormant (pure view).
//
// [EXT] Extension points / limits
//   A new VT sequence = add a branch in dispatch_csi/vt_feed (cells are COLORREF, so 16/256/truecolor
//   are expressible). New helpers go in the static block atop this file (keep the portability unit two
//   files). On font/window/margin/width changes the order is calc_cell_size (font) -> update_metrics
//   (-> reset_screen). UTF-8 chunk-boundary split handled by utf8_tail carry-over in print(). Korean
//   IME order / cursor width cannot be auto-captured -> verify by real typing (Learned.md).
// ===== END DESIGN OVERVIEW =====

#include "ConBox.h"         // must come first: pulls in afxwin.h / windows.h before imm.h/winspool.h
#include <afxpriv.h>        // AfxHookWindowCreate / AfxUnhookWindowCreate (MFC window creation hook)
#include <string>
#include <sstream>
#include <map>
#include <cmath>            // std::fabs (overlay scrollbar antialiasing)
#include <imm.h>            // Korean IME (Input Method Manager)
#pragma comment(lib, "imm32.lib")
#include <winspool.h>       // EnumPrintersW, OpenPrinterW, GetPrinterW, CreateDCW (save_pdf)
#pragma comment(lib, "winspool.lib")
#pragma comment(lib, "msimg32.lib")   // AlphaBlend (overlay scrollbar fade)

// Some SDK configurations expose CreatePseudoConsole/HPCON but not this attribute macro, so define it
// if missing (ProcThreadAttributePseudoConsole=22 | PROC_THREAD_ATTRIBUTE_INPUT(0x00020000); a fixed
// value across SDKs).
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE_HANDLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE_HANDLE 0x00020016
#endif

static const UINT_PTR CURSOR_TIMER = 1;
// SGR blink (CELL_BLINK) visibility toggle. 2 is unused by other timers.
static const UINT_PTR BLINK_TIMER = 2;
// Child output polling timer (the one start() sets). 3 to stay distinct from CURSOR(1)/BLINK(2).
static const UINT_PTR PUMP_TIMER = 3;
static const UINT PUMP_INTERVAL_MS = 16;   // output poll interval; 16~30 keeps the view smooth
// Overlay scrollbar fade timer. 4 to stay distinct from CURSOR(1)/BLINK(2)/PUMP(3).
static const UINT_PTR SBAR_TIMER = 4;

// Overlay scrollbar geometry/fade tuning (px / ms / alpha). Tuned to read like wt.exe: a slim, rounded,
// translucent thumb hugging (but slightly inset from) the right edge, brighter on hover.
static const int   SBAR_W          = 14;   // gutter (hit-test) width at the right edge
static const int   SBAR_THUMB_W    = 6;    // visible thumb width (right-aligned inside the gutter)
static const int   SBAR_INSET      = 3;    // gap from the client's right edge to the thumb
static const int   SBAR_RADIUS     = 3;    // thumb corner radius (rounded ends)
static const int   SBAR_BTN_H      = SBAR_W;  // arrow button height (square; track runs between them)
static const int   SBAR_MIN_THUMB  = 24;   // minimum thumb height so it stays grabbable
static const DWORD SBAR_HOLD_MS    = 700;  // stay fully visible this long after the last activity
static const UINT  SBAR_FADE_TICK_MS = 33; // fade animation tick (~30 fps)
static const int   SBAR_FADE_STEP  = 24;   // alpha removed per tick once fading (gentle ~0.35s fade)
static const int   SBAR_OP_IDLE    = 130;  // max thumb opacity when shown (translucent, wt-like)
static const int   SBAR_OP_HOVER   = 215;  // brighter when hovering / dragging
static const int   SBAR_GUTTER_OP  = 70;   // gutter strip opacity (scaled by the fade alpha), full
                                            // SBAR_W width, always drawn while shown -- TableBox.cpp
                                            // uses the same values; keep both in sync (see its comment).
static const COLORREF SBAR_THUMB_COLOR = RGB(200, 200, 200);   // light gray, blended over content

// VT default colors: where SGR 0 (reset) and 39/49 return to (also the Requirements default). Tuned to
// look like wt.exe's Campbell scheme on 90% acrylic (light-gray text on dark-gray, not pure black).
static const COLORREF DEFAULT_FG = RGB(200, 200, 200);
static const COLORREF DEFAULT_BG = RGB(32, 32, 32);

enum { VT_GROUND = 0, VT_ESC = 1, VT_CSI = 2, VT_OSC = 3 };

// MAX_SCROLLBACK is now the instance member max_scrollback (configurable via config()).

BEGIN_MESSAGE_MAP(ConBox, CWnd)
    ON_WM_PAINT()
    ON_WM_ERASEBKGND()
    ON_WM_SIZE()
    ON_WM_CHAR()
    ON_WM_KEYDOWN()
    ON_WM_GETDLGCODE()
    ON_WM_MOUSEWHEEL()
    ON_WM_TIMER()
    ON_WM_DESTROY()
    ON_WM_LBUTTONDOWN()
    ON_WM_LBUTTONDBLCLK()
    ON_WM_LBUTTONUP()
    ON_WM_MOUSEMOVE()
    ON_WM_MOUSELEAVE()
    ON_WM_RBUTTONDOWN()
    ON_WM_DROPFILES()
    ON_WM_SETFOCUS()
    ON_WM_KILLFOCUS()
    ON_MESSAGE(WM_DPICHANGED, &ConBox::OnDpiChanged)
    ON_MESSAGE(WM_IME_STARTCOMPOSITION, &ConBox::OnImeStart)
    ON_MESSAGE(WM_IME_COMPOSITION, &ConBox::OnImeComp)
    ON_MESSAGE(WM_IME_ENDCOMPOSITION, &ConBox::OnImeEnd)
    ON_MESSAGE(WM_IME_NOTIFY, &ConBox::OnImeNotify)
END_MESSAGE_MAP()

// Whether a char is double-width (Korean, CJK, etc.): 1 cell for English, 2 cells for this range.
static bool IsWideChar(wchar_t ch)
{
    return (ch >= 0x1100 && ch <= 0x115F)   // Hangul Jamo
        || (ch >= 0x2E80 && ch <= 0x303E)   // CJK radicals, CJK symbols
        || (ch >= 0x3041 && ch <= 0x33FF)   // Hiragana/Katakana/Hangul compat jamo etc.
        || (ch >= 0x3400 && ch <= 0x4DBF)   // CJK Ext A
        || (ch >= 0x4E00 && ch <= 0x9FFF)   // CJK Unified
        || (ch >= 0xA000 && ch <= 0xA4CF)   // Yi
        || (ch >= 0xAC00 && ch <= 0xD7A3)   // Hangul syllables
        || (ch >= 0xF900 && ch <= 0xFAFF)   // CJK compat ideographs
        || (ch >= 0xFF00 && ch <= 0xFF60)   // fullwidth forms
        || (ch >= 0xFFE0 && ch <= 0xFFE6);  // fullwidth signs
}

// Convert a font name/size + option string to a LOGFONTW. (Helper for make_font.)
// option grammar: a number applies to the attribute letter that follows it.
//   B=Bold (with a number = lfWeight), I=Italic, U=Underline, S=Strikeout,
//   Q=Quality (0..6), W=Width ratio percent (100=default).
// dpi_y converts points to pixel height (usually 96). width_pct returns the width ratio.
static LOGFONTW ParseFontOpts(const char* name, float size_pt, const char* option, int dpi_y, int& width_pct)
{
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));

    // points -> pixel height, rounded to nearest. A negative height means it is the cell (em) height.
    lf.lfHeight = -(LONG)(size_pt * dpi_y / 72.0f + 0.5f);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = DEFAULT_QUALITY; // 0

    if (name != nullptr)
        ::MultiByteToWideChar(CP_UTF8, 0, name, -1, lf.lfFaceName, LF_FACESIZE);

    width_pct = 100;

    // Parse option left to right; a run of digits becomes the value of the attribute letter that follows.
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
        case 'B': case 'b':
            lf.lfWeight = has_num ? num : FW_BOLD;
            break;
        case 'I': case 'i':
            lf.lfItalic = 1;
            break;
        case 'U': case 'u':
            lf.lfUnderline = 1;
            break;
        case 'S': case 's':
            lf.lfStrikeOut = 1;
            break;
        case 'Q': case 'q':
            lf.lfQuality = (has_num && num >= 0 && num <= 6) ? (BYTE)num : DEFAULT_QUALITY;
            break;
        case 'W': case 'w':
            width_pct = has_num ? num : 100;
            break;
        default:
            break;   // unknown letters are ignored
        }

        num = 0;
        has_num = false;
    }

    return lf;
}

// Paint a block-element char (U+2580..259F) as a shape instead of via the font. These glyphs are
// integer subdivisions of the cell, so a 4-quadrant fill reproduces them exactly and, unlike font
// glyphs, leaves no gap between adjacent cells. Returns true if ch was handled.
// (1/8 blocks 2581..2587/2589..258F and shades 2591..2593 are not quadrant-expressible -> return
//  false so the font draws them.)
static bool DrawBlockElement(CDC& dc, wchar_t ch, int px, int py, int cw, int chh, COLORREF fg)
{
    // 4-bit quadrant mask: UL=1, UR=2, LL=4, LR=8.
    int mask;
    switch (ch) {
    case 0x2580: mask = 1 | 2;         break;  // UPPER HALF
    case 0x2584: mask = 4 | 8;         break;  // LOWER HALF
    case 0x2588: mask = 1 | 2 | 4 | 8; break;  // FULL BLOCK
    case 0x258C: mask = 1 | 4;         break;  // LEFT HALF
    case 0x2590: mask = 2 | 8;         break;  // RIGHT HALF
    case 0x2596: mask = 4;             break;  // QUADRANT LOWER LEFT
    case 0x2597: mask = 8;             break;  // QUADRANT LOWER RIGHT
    case 0x2598: mask = 1;             break;  // QUADRANT UPPER LEFT
    case 0x2599: mask = 1 | 4 | 8;     break;  // UL + LL + LR
    case 0x259A: mask = 1 | 8;         break;  // UL + LR
    case 0x259B: mask = 1 | 2 | 4;     break;  // UL + UR + LL
    case 0x259C: mask = 1 | 2 | 8;     break;  // UL + UR + LR
    case 0x259D: mask = 2;             break;  // QUADRANT UPPER RIGHT
    case 0x259E: mask = 2 | 4;         break;  // UR + LL
    case 0x259F: mask = 2 | 4 | 8;     break;  // UR + LL + LR
    default:     return false;
    }

    // Split the cell at its center. Adjacent cells use the same formula, so boundaries meet exactly.
    int midx = px + cw / 2;
    int midy = py + chh / 2;
    int rx = px + cw;
    int by = py + chh;

    if (mask & 1) dc.FillSolidRect(px,   py,   midx - px, midy - py, fg);  // UL
    if (mask & 2) dc.FillSolidRect(midx, py,   rx - midx, midy - py, fg);  // UR
    if (mask & 4) dc.FillSolidRect(px,   midy, midx - px, by - midy, fg);  // LL
    if (mask & 8) dc.FillSolidRect(midx, midy, rx - midx, by - midy, fg);  // LR
    return true;
}

// Draw line/junction/diagonal/double-line box-drawing chars as shapes instead of via the font. Lines
// are centered on the cell, so adjacent cells (same center formula) connect seamlessly. Returns true
// if ch was handled.
// Supports: single orthogonal/junctions, rounded corners (drawn square), diagonals, pure double lines.
// (Mixed single/double, dashed, heavy lines are not handled -> left to the font.)
static bool DrawBoxLine(CDC& dc, wchar_t ch, int px, int py, int cw, int chh, COLORREF fg)
{
    // Line thickness approximates cell height (min 1px).
    int t = chh / 12;
    if (t < 1) t = 1;

    int midx = px + cw / 2;
    int midy = py + chh / 2;
    int rx = px + cw;
    int by = py + chh;

    // Diagonals cannot be filled rects; stroke from corner to corner so adjacent cells join at corners.
    if (ch == 0x2571 || ch == 0x2572 || ch == 0x2573) {
        CPen pen(PS_SOLID, t, fg);
        CPen* oldpen = dc.SelectObject(&pen);
        if (ch != 0x2572) { dc.MoveTo(px, by); dc.LineTo(rx, py); }  // forward slash (incl. cross)
        if (ch != 0x2571) { dc.MoveTo(px, py); dc.LineTo(rx, by); }  // back slash (incl. cross)
        dc.SelectObject(oldpen);
        return true;
    }

    int top = midy - t / 2;   // y of the horizontal single-line band
    int lft = midx - t / 2;   // x of the vertical single-line band

    // Single lines/junctions + rounded corners (drawn square). 4-dir bits: N=1, E=2, S=4, W=8.
    int dir = -1;
    switch (ch) {
    case 0x2500: dir = 2 | 8;         break;  // horizontal
    case 0x2502: dir = 1 | 4;         break;  // vertical
    case 0x250C: dir = 2 | 4;         break;  // down+right
    case 0x2510: dir = 4 | 8;         break;  // down+left
    case 0x2514: dir = 1 | 2;         break;  // up+right
    case 0x2518: dir = 1 | 8;         break;  // up+left
    case 0x251C: dir = 1 | 2 | 4;     break;  // vertical+right
    case 0x2524: dir = 1 | 4 | 8;     break;  // vertical+left
    case 0x252C: dir = 2 | 4 | 8;     break;  // horizontal+down
    case 0x2534: dir = 1 | 2 | 8;     break;  // horizontal+up
    case 0x253C: dir = 1 | 2 | 4 | 8; break;  // cross
    case 0x256D: dir = 2 | 4;         break;  // rounded down+right -> square
    case 0x256E: dir = 4 | 8;         break;  // rounded down+left
    case 0x256F: dir = 1 | 8;         break;  // rounded up+left
    case 0x2570: dir = 1 | 2;         break;  // rounded up+right
    default:     break;
    }
    if (dir >= 0) {
        if (dir & 8) dc.FillSolidRect(px,   top,  midx - px, t,         fg);  // west
        if (dir & 2) dc.FillSolidRect(midx, top,  rx - midx, t,         fg);  // east
        if (dir & 1) dc.FillSolidRect(lft,  py,   t,         midy - py, fg);  // north
        if (dir & 4) dc.FillSolidRect(lft,  midy, t,         by - midy, fg);  // south
        return true;
    }

    // Pure double lines: two parallel strokes offset +-g from center. At corners/junctions the strokes
    // close in an L (outer stroke to the outer intersection, inner to the inner). Horizontal strokes at
    // top0(upper)/top1(lower); vertical at lft0(left)/lft1(right).
    int g = t + 1;
    int top0 = midy - g - t / 2;
    int top1 = midy + g - t / 2;
    int lft0 = midx - g - t / 2;
    int lft1 = midx + g - t / 2;

    switch (ch) {
    case 0x2550:  // horizontal double
        dc.FillSolidRect(px, top0, cw, t, fg);
        dc.FillSolidRect(px, top1, cw, t, fg);
        return true;
    case 0x2551:  // vertical double
        dc.FillSolidRect(lft0, py, t, chh, fg);
        dc.FillSolidRect(lft1, py, t, chh, fg);
        return true;
    case 0x2554:  // E+S (top-left corner)
        dc.FillSolidRect(midx - g, top0, rx - (midx - g), t, fg);
        dc.FillSolidRect(midx + g, top1, rx - (midx + g), t, fg);
        dc.FillSolidRect(lft0, midy - g, t, by - (midy - g), fg);
        dc.FillSolidRect(lft1, midy + g, t, by - (midy + g), fg);
        return true;
    case 0x2557:  // S+W (top-right corner)
        dc.FillSolidRect(px, top0, (midx + g) - px, t, fg);
        dc.FillSolidRect(px, top1, (midx - g) - px, t, fg);
        dc.FillSolidRect(lft1, midy - g, t, by - (midy - g), fg);
        dc.FillSolidRect(lft0, midy + g, t, by - (midy + g), fg);
        return true;
    case 0x255A:  // N+E (bottom-left corner)
        dc.FillSolidRect(midx + g, top0, rx - (midx + g), t, fg);
        dc.FillSolidRect(midx - g, top1, rx - (midx - g), t, fg);
        dc.FillSolidRect(lft0, py, t, (midy + g) - py, fg);
        dc.FillSolidRect(lft1, py, t, (midy - g) - py, fg);
        return true;
    case 0x255D:  // N+W (bottom-right corner)
        dc.FillSolidRect(px, top0, (midx - g) - px, t, fg);
        dc.FillSolidRect(px, top1, (midx + g) - px, t, fg);
        dc.FillSolidRect(lft0, py, t, (midy - g) - py, fg);
        dc.FillSolidRect(lft1, py, t, (midy + g) - py, fg);
        return true;
    case 0x2560:  // N+E+S (left T)
        dc.FillSolidRect(lft0, py, t, chh, fg);
        dc.FillSolidRect(lft1, py, t, chh, fg);
        dc.FillSolidRect(midx + g, top0, rx - (midx + g), t, fg);
        dc.FillSolidRect(midx + g, top1, rx - (midx + g), t, fg);
        return true;
    case 0x2563:  // N+S+W (right T)
        dc.FillSolidRect(lft0, py, t, chh, fg);
        dc.FillSolidRect(lft1, py, t, chh, fg);
        dc.FillSolidRect(px, top0, (midx - g) - px, t, fg);
        dc.FillSolidRect(px, top1, (midx - g) - px, t, fg);
        return true;
    case 0x2566:  // E+S+W (top T)
        dc.FillSolidRect(px, top0, cw, t, fg);
        dc.FillSolidRect(px, top1, cw, t, fg);
        dc.FillSolidRect(lft0, midy + g, t, by - (midy + g), fg);
        dc.FillSolidRect(lft1, midy + g, t, by - (midy + g), fg);
        return true;
    case 0x2569:  // N+E+W (bottom T)
        dc.FillSolidRect(px, top0, cw, t, fg);
        dc.FillSolidRect(px, top1, cw, t, fg);
        dc.FillSolidRect(lft0, py, t, (midy - g) - py, fg);
        dc.FillSolidRect(lft1, py, t, (midy - g) - py, fg);
        return true;
    case 0x256C:  // N+E+S+W (cross, center open)
        dc.FillSolidRect(px, top0, cw, t, fg);
        dc.FillSolidRect(px, top1, cw, t, fg);
        dc.FillSolidRect(lft0, py, t, chh, fg);
        dc.FillSolidRect(lft1, py, t, chh, fg);
        return true;
    default:
        return false;
    }
}

ConBox::ConBox()
{
    default_fg = DEFAULT_FG;
    default_bg = DEFAULT_BG;
    cur_fg = DEFAULT_FG;
    cur_bg = DEFAULT_BG;

    // Safe placeholders until calc_cell_size measures the font.
    cell_w = 8;
    cell_h = 16;
    cell_base = 12;

    kfont_match_efont = true;

    cols = 1;
    rows = 1;

    margin_top = 10;
    margin_bottom = 10;
    margin_left = 10;
    margin_right = 10;

    origin_x = 0;
    origin_y = 0;

    snap_mode = 2;

    view_top = 0;
    cur_row = 0;
    cur_col = 0;
    cursor_visible = true;
    saved_cur = { 0, 0 };

    // Scroll region starts as the whole screen; scroll_bot is set to rows-1 in reset_screen/update_metrics.
    scroll_top = 0;
    scroll_bot = 0;

    alt_active = false;
    saved_main_cur = { 0, 0 };

    utf8_tail_len = 0;
    vt_state = VT_GROUND;
    vt_nparam = 0;
    vt_priv = false;
    vt_gtlt = false;
    vt_space = false;
    ime_committed = false;
    cur_bold = false;
    cur_italic = false;
    cur_underline = false;
    cur_strike = false;
    cur_blink = false;
    cur_reverse = false;
    cur_double = false;
    blink_on = true;

    // Input modes default off; the child turns them on.
    app_cursor_keys = false;
    bracketed_paste = false;

    // No sinks by default (read-only viewer). start(), or the host's set_input_sink, installs them.
    input_sink = nullptr;
    input_sink_user = nullptr;
    resize_sink = nullptr;
    resize_sink_user = nullptr;

    // ConPTY child state stays empty until start() (pure terminal view).
    h_pc = nullptr;
    in_write = nullptr;
    out_read = nullptr;
    ZeroMemory(&child_proc, sizeof(child_proc));
    child_running = false;
    exit_cb = nullptr;
    exit_cb_user = nullptr;
    log_file = INVALID_HANDLE_VALUE;

    cursor_bg_weight = 4;
    cursor_fg_weight = 6;

    efont_width_pct = 100;
    kfont_width_pct = 100;

    adjust_left = 0;
    adjust_top = 0;
    adjust_right = 0;
    adjust_bottom = 0;

    glyph_level = 2;

    // Cursor blink: start visible; interval follows the system caret rate. If GetCaretBlinkTime returns
    // INFINITE (blink disabled system-wide), use 0 = no blink, always on.
    has_focus = false;
    cursor_on = true;
    UINT caret = ::GetCaretBlinkTime();
    cursor_blink_ms = (caret == INFINITE) ? 0 : (int)caret;
    cursor_type = 1;   // blink block (default until set_cursor is called)

    ZeroMemory(&efont_lf, sizeof(efont_lf));
    ZeroMemory(&kfont_lf, sizeof(kfont_lf));

    // Font specs (name/option default empty = "unset"; apply_default_fonts fills them). box_dpi 0 until
    // open() sets it from the parent monitor; make_font then falls back to the primary monitor DPI.
    efont_size = 0.0f;
    kfont_size = 0.0f;
    box_dpi = 0;

    sel_active = false;
    selecting = false;
    sel_block = false;
    sel_anchor_row = 0;
    sel_anchor_col = 0;
    sel_end_row = 0;
    sel_end_col = 0;

    max_scrollback = 5000;
    cfg_cols = 96;
    cfg_rows = 32;
    cfg_cmdline = "";
    cfg_lines_per_paper = 50;

    // Default 16-color ANSI palette (matches the hardcoded base16[] in Xterm256ToRgb).
    static const COLORREF def16[16] = {
        RGB(0, 0, 0),       RGB(205, 0, 0),     RGB(0, 205, 0),     RGB(205, 205, 0),
        RGB(0, 0, 238),     RGB(205, 0, 205),   RGB(0, 205, 205),   RGB(229, 229, 229),
        RGB(127, 127, 127), RGB(255, 0, 0),     RGB(0, 255, 0),     RGB(255, 255, 0),
        RGB(92, 92, 255),   RGB(255, 0, 255),   RGB(0, 255, 255),   RGB(255, 255, 255)
    };
    memcpy(ansi_colors, def16, sizeof(def16));

    // Paper (export) colors: black text on white background by default.
    paper_default_fg = RGB(0x00, 0x00, 0x00);
    paper_default_bg = RGB(0xFF, 0xFF, 0xFF);
    // Semantic inversion of screen palette for white-bg export:
    //   index 0 (screen bg black) -> white (#FFFFFF); index 7 (screen fg white) -> black (#000000).
    //   index 8 (bright bg) -> near-white (#EEEEEE); index 15 (bright fg) -> black (#000000).
    //   chromatic pairs (1-6 / 9-14) collapsed to the same dark Tango color so bright
    //   variants (yellow, cyan, green) don't become invisible on white paper.
    static const COLORREF paper16[16] = {
        RGB(0xFF,0xFF,0xFF), RGB(0xCC,0x00,0x00), RGB(0x4E,0x9A,0x06), RGB(0xC4,0xA0,0x00),
        RGB(0x34,0x65,0xA4), RGB(0x75,0x50,0x7B), RGB(0x06,0x98,0x9A), RGB(0x00,0x00,0x00),
        RGB(0xEE,0xEE,0xEE), RGB(0xCC,0x00,0x00), RGB(0x4E,0x9A,0x06), RGB(0xC4,0xA0,0x00),
        RGB(0x34,0x65,0xA4), RGB(0x75,0x50,0x7B), RGB(0x06,0x98,0x9A), RGB(0x00,0x00,0x00)
    };
    memcpy(paper_ansi_colors, paper16, sizeof(paper16));

    // Overlay scrollbar starts hidden (alpha 0); shown on user scroll / hover.
    sbar_alpha = 0;
    sbar_hold_until = 0;
    sbar_hover = false;
    sbar_dragging = false;
    sbar_drag_off = 0;

    // Double-buffer cache is created on the first OnPaint.
    back_bmp_saved = nullptr;
    back_w = 0;
    back_h = 0;

    // Pre-allocate reusable print() buffers to avoid per-call heap allocation on the pump() hot path.
    print_buf.reserve(8192);
    print_ws.reserve(8192);
}

ConBox::~ConBox()
{
    // Tear down child/PTY/polling if still up (OnDestroy usually handles it first; this is a safety net).
    stop();

    // Restore the original bitmap so back_bmp is not destroyed while still selected into back_dc.
    if (back_bmp_saved)
        back_dc.SelectObject(back_bmp_saved);
}

// ===== Config file loader =====
// Resolve a UTF-8 INI path to a wide absolute path. Relative paths are anchored at the EXE
// directory (not the working directory) so the config file travels with the executable.
static std::wstring ResolveIniPath(const char* utf8)
{
    int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w(n, 0);
    ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), n);
    if (!w.empty() && w.back() == 0) w.pop_back();   // strip the null terminator

    // Absolute path: has a drive letter (x:\...) or starts with a UNC slash.
    bool absolute = (w.size() >= 2 && w[1] == L':') || (!w.empty() && w[0] == L'\\');
    if (absolute) return w;

    // Relative: prepend the directory that contains the EXE.
    wchar_t exe[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    size_t slash = dir.rfind(L'\\');
    if (slash != std::wstring::npos) dir.resize(slash + 1); else dir.clear();
    return dir + w;
}

// Parse an INI file into a key->value map. Section headers are silently ignored so the caller
// can match keys regardless of which section they appear in.  key names are lower-cased.
// Parse INI-format contents (key=value, one per line) into a key->value map.
// contents: UTF-8 string with \n line endings. If nullptr, returns empty map.
// Section headers and comments are silently ignored; key names are lowercased.
static std::map<std::string, std::string> ParseIni(const char* contents)
{
    std::map<std::string, std::string> m;
    if (!contents) return m;

    std::istringstream ss(contents);
    std::string line;
    while (std::getline(ss, line)) {
        // Trim leading whitespace.
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
        // Skip empty lines, comments, and section headers.
        if (start >= line.size() || line[start] == ';' || line[start] == '#' || line[start] == '[') continue;

        line = line.substr(start);
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;

        // Key: everything before '=', trailing-trimmed.
        std::string key = line.substr(0, eq_pos);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        for (char& c : key) c = (char)tolower((unsigned char)c);

        // Value: everything after '=', leading-trimmed, inline comment and trailing whitespace stripped.
        std::string val = line.substr(eq_pos + 1);
        size_t val_start = 0;
        while (val_start < val.size() && (val[val_start] == ' ' || val[val_start] == '\t')) ++val_start;
        val = val.substr(val_start);
        // Strip inline comment (ConBox INI values never contain ';').
        size_t semi_pos = val.find(';');
        if (semi_pos != std::string::npos) val = val.substr(0, semi_pos);
        // Trim trailing whitespace.
        while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' ')) val.pop_back();

        if (!key.empty()) m[key] = val;
    }
    return m;
}

// Parse "#RRGGBB" hex color string to COLORREF.  Returns def on failure.
static COLORREF ParseColor(const std::string& s, COLORREF def)
{
    if (s.size() == 7 && s[0] == '#') {
        unsigned int v = 0;
        if (sscanf_s(s.c_str() + 1, "%x", &v) == 1)
            return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    }
    return def;
}

// Write a default INI file to path (called on first run when no config exists).
// The generated INI text contains Korean UTF-8 comments directly in these literals.
// Keep this source file saved as UTF-8 with BOM so MSVC reads them correctly.
static void CreateDefaultIni(const wchar_t* path)
{
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"w");
    if (!f) return;
    fputs(
        "; ConBox 설정 파일\n"
        "; 섹션 이름은 무시됩니다 -- 키 이름으로만 인식합니다.\n"
        "\n"
        "[font]\n"
        "; 폰트 크기는 포인트(pt) 단위. option: B=굵게, I=기울임, 90W=너비 90%\n"
        "efont_name    = Cascadia Mono   ; 영문 폰트 이름\n"
        "efont_size    = 12              ; 영문 폰트 크기 (포인트)\n"
        "efont_opts    =                 ; 영문 폰트 옵션\n"
        "kfont_name    = Malgun Gothic   ; 한글 폰트 이름\n"
        "kfont_size    = 0               ; 0 이하: 영문 높이에 맞춤 / 양수: 크기 직접 지정\n"
        "kfont_opts    = B               ; 한글 폰트 옵션\n"
        "\n"
        "[layout]\n"
        "; 마진: 콘텐츠와 창 가장자리 사이 여백(px). adjust: 셀 크기 미세조정(px, 음수=줄임)\n"
        "margin_top    = 10              ; 위쪽 여백\n"
        "margin_left   = 10              ; 왼쪽 여백\n"
        "margin_bottom = 10              ; 아래쪽 여백\n"
        "margin_right  = 10              ; 오른쪽 여백\n"
        "adjust_left   = 0               ; 셀 왼쪽 패딩\n"
        "adjust_top    = -2              ; 셀 위쪽 패딩\n"
        "adjust_right  = 0               ; 셀 오른쪽 패딩\n"
        "adjust_bottom = 0               ; 셀 아래쪽 패딩\n"
        "grid_cols     = 96              ; 가로 칸 수\n"
        "grid_rows     = 32              ; 세로 줄 수\n"
        "snap_mode     = 2               ; DPI 변경시 창 스냅: 0=센터링만(작으면 우/하단 짤림), 1=작을때만 확대, 2=margin 확보후 항상 스냅\n"
        "\n"
        "[screen]\n"
        "; 색상 형식: #RRGGBB\n"
        "screen_text       = #C8C8C8     ; 기본 글자색\n"
        "screen_back       = #202020     ; 기본 배경색\n"
        "; ANSI 16색 팔레트 (SGR 30~37/90~97 색상, 0-indexed)\n"
        "screen_palette00  = #000000     ; 검정\n"
        "screen_palette01  = #CD0000     ; 빨강\n"
        "screen_palette02  = #00CD00     ; 초록\n"
        "screen_palette03  = #CDCD00     ; 노랑\n"
        "screen_palette04  = #0000EE     ; 파랑\n"
        "screen_palette05  = #CD00CD     ; 자홍\n"
        "screen_palette06  = #00CDCD     ; 청록\n"
        "screen_palette07  = #E5E5E5     ; 흰색(밝은)\n"
        "screen_palette08  = #7F7F7F     ; 회색(밝은 검정)\n"
        "screen_palette09  = #FF0000     ; 밝은 빨강\n"
        "screen_palette10  = #00FF00     ; 밝은 초록\n"
        "screen_palette11  = #FFFF00     ; 밝은 노랑\n"
        "screen_palette12  = #5C5CFF     ; 밝은 파랑\n"
        "screen_palette13  = #FF00FF     ; 밝은 자홍\n"
        "screen_palette14  = #00FFFF     ; 밝은 청록\n"
        "screen_palette15  = #FFFFFF     ; 흰색\n"
        "\n"
        "[paper]\n"
        "; EMF/PDF 출력용 색상\n"
        "paper_text        = #000000\n"
        "paper_back        = #FFFFFF\n"
        "paper_palette00   = #FFFFFF\n"
        "paper_palette01   = #CC0000\n"
        "paper_palette02   = #4E9A06\n"
        "paper_palette03   = #C4A000\n"
        "paper_palette04   = #3465A4\n"
        "paper_palette05   = #75507B\n"
        "paper_palette06   = #06989A\n"
        "paper_palette07   = #000000\n"
        "paper_palette08   = #EEEEEE\n"
        "paper_palette09   = #CC0000\n"
        "paper_palette10   = #4E9A06\n"
        "paper_palette11   = #C4A000\n"
        "paper_palette12   = #3465A4\n"
        "paper_palette13   = #75507B\n"
        "paper_palette14   = #06989A\n"
        "paper_palette15   = #000000\n"
        "lines_per_paper   = 50         ; EMF 그림 한장에 표시되는 줄 수\n"
        "\n"
        "[cursor]\n"
        "; 커서 모양: 홀수=깜빡임, 짝수=고정\n"
        "cursor_type     = 0             ; 0=기본(3), 1=깜빡블록, 2=고정블록, 3=깜빡밑줄, 4=고정밑줄, 5=깜빡I빔, 6=고정I빔\n"
        "cursor_blend_bg = 4             ; 커서 색상 - 배경색 비중\n"
        "cursor_blend_fg = 6             ; 커서 색상 - 글자색 비중\n"
        "cursor_blink_ms = 0             ; 깜빡임 간격(ms). 0 = 시스템 설정 따름\n"
        "\n"
        "[rendering]\n"
        "builtin_glyphs = 2              ; 0=폰트, 1=블록문자 직접그림, 2=박스선까지 직접그림(기본)\n"
        "scrollback_cap = 5000           ; 스크롤백 최대 줄 수\n"
        "\n"
        "[child]\n"
        "cmdline =                       ; 실행할 자식 프로세스 명령줄 (비워두면 자동 실행 안함)\n",
        f);
    fclose(f);
}

// Apply INI-format settings from a string. contents: UTF-8 with \n line endings.
void ConBox::setup(const char* contents)
{
    if (!contents) return;

    std::map<std::string, std::string> m = ParseIni(contents);
    auto get = [&](const char* k) -> const std::string* {
        auto it = m.find(k);
        return (it != m.end()) ? &it->second : nullptr;
    };
    auto geti = [&](const char* k, int def) -> int {
        const std::string* s = get(k);
        if (!s || s->empty()) return def;
        try { return std::stoi(*s); } catch (...) { return def; }
    };

    // Font
    std::string efont_name = "Cascadia Mono", efont_opts = "";
    float efont_size = 12.0f;
    std::string kfont_name = "Malgun Gothic", kfont_opts = "B";
    float kfont_size = 0.0f;
    if (const std::string* s = get("efont_name")) efont_name = *s;
    if (const std::string* s = get("efont_size")) { try { efont_size = std::stof(*s); } catch (...) {} }
    if (const std::string* s = get("efont_opts")) efont_opts = *s;
    if (const std::string* s = get("kfont_name")) kfont_name = *s;
    if (const std::string* s = get("kfont_size")) { try { kfont_size = std::stof(*s); } catch (...) {} }
    if (const std::string* s = get("kfont_opts")) kfont_opts = *s;
    set_efont(efont_name.c_str(), efont_size, efont_opts.c_str());
    set_kfont(kfont_name.c_str(), kfont_size, kfont_opts.c_str());

    // Layout
    int mt = geti("margin_top", 10), ml = geti("margin_left", 10);
    int mb = geti("margin_bottom", 10), mr = geti("margin_right", 10);
    set_margin(mt, ml, mb, mr);
    int al = geti("adjust_left", 0), at = geti("adjust_top", -2);
    int ar = geti("adjust_right", 0), ab = geti("adjust_bottom", 0);
    adjust(al, at, ar, ab);
    snap_mode = geti("snap_mode", 2);

    // Screen colors: screen_text / screen_back / screen_palette00..15 (0-indexed, matching ANSI indices 0..15).
    if (const std::string* s = get("screen_text")) set_fg_color(ParseColor(*s, default_fg));
    if (const std::string* s = get("screen_back")) set_bg_color(ParseColor(*s, default_bg));

    char pal_key[24];
    for (int i = 0; i < 16; i++) {
        sprintf_s(pal_key, sizeof(pal_key), "screen_palette%02d", i);
        if (const std::string* s = get(pal_key))
            ansi_colors[i] = ParseColor(*s, ansi_colors[i]);
    }

    // Paper colors for EMF/PDF export. Defaults are Tango Light (set in ctor).
    // Only entries present in the INI override the defaults.
    if (const std::string* s = get("paper_text")) paper_default_fg = ParseColor(*s, paper_default_fg);
    if (const std::string* s = get("paper_back")) paper_default_bg = ParseColor(*s, paper_default_bg);
    for (int i = 0; i < 16; i++) {
        sprintf_s(pal_key, sizeof(pal_key), "paper_palette%02d", i);
        if (const std::string* s = get(pal_key))
            paper_ansi_colors[i] = ParseColor(*s, paper_ansi_colors[i]);
    }

    // Cursor
    set_cursor(geti("cursor_type", 0));
    set_cursor_blend(geti("cursor_blend_bg", 4), geti("cursor_blend_fg", 6));
    set_cursor_blink(geti("cursor_blink_ms", 0));

    // Rendering
    set_builtin_glyphs(geti("builtin_glyphs", 2));
    int cap = geti("scrollback_cap", 5000);
    if (cap > 0) max_scrollback = cap;

    // Host-consumed values (stored; accessed via config_cols/rows/cmdline)
    cfg_cols = geti("grid_cols", 96);
    cfg_rows = geti("grid_rows", 32);
    if (const std::string* s = get("cmdline")) { if (!s->empty()) cfg_cmdline = *s; }
    cfg_lines_per_paper = geti("lines_per_paper", 50);
    if (cfg_lines_per_paper < 1) cfg_lines_per_paper = 1;
}

// Load settings from an INI file.  Sections are ignored; keys are matched by name only.
// Relative paths are resolved against the EXE directory.
// - File not found: auto-created with compiled-in defaults at the resolved path; a status message is
//   stored in ini_msg and printed by open() once the window exists. Settings stay at constructor defaults.
// - File found but cannot be opened: same deferred print() path, no settings changed.
void ConBox::setup_from_ini(const char* path)
{
    bool explicit_path = (path && *path);
    const char* src = explicit_path ? path : "ConBox.ini";
    std::wstring wide_path = ResolveIniPath(src);

    // Helper: wstring -> UTF-8 string (for the deferred print() message).
    auto w2u = [](const std::wstring& ws) -> std::string {
        int n = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n - 1 : 0, '\0');
        if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], n, nullptr, nullptr);
        return s;
    };

    if (::GetFileAttributesW(wide_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CreateDefaultIni(wide_path.c_str());
        ini_msg = "ConBox: INI file not found. Created with defaults:\r\n" + w2u(wide_path) + "\r\n";
        return;
    }

    // Read entire file into a string and pass to setup().
    FILE* f = nullptr;
    _wfopen_s(&f, wide_path.c_str(), L"r");
    if (!f) {
        ini_msg = "ConBox: Cannot open INI file:\r\n" + w2u(wide_path) + "\r\n";
        return;
    }

    std::string content;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        content.append(buf, n);
    }
    fclose(f);

    setup(content.c_str());
}

void ConBox::open(CWnd* parent, int left, int top)
{
    // Register the "ConBox" window class on first call; subsequent calls (multiple instances) skip.
    // Fixed name allows FindWindowEx(parent, NULL, L"ConBox", NULL) from host/test scripts.
    // No background brush (we paint it). Arrow cursor (like wt.exe).
    // CS_HREDRAW|CS_VREDRAW so a resize repaints the whole control.
    WNDCLASSEXW wc = {};
    if (!::GetClassInfoExW(AfxGetInstanceHandle(), L"ConBox", &wc))
    {
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc   = ::DefWindowProcW;
        wc.hInstance     = AfxGetInstanceHandle();
        wc.hCursor       = ::LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = L"ConBox";
        ::RegisterClassExW(&wc);
    }

    // Compute cell metrics before creating the window so the pixel size can be derived from the
    // configured grid. The window does not exist yet, so seed box_dpi from the parent's monitor DPI
    // (a WS_CHILD inherits its parent's DPI under Per-Monitor V2); make_font uses it for the pt->px
    // conversion. Then ensure specs and build the fonts at that DPI. (Fonts the host set earlier via
    // setup_from_ini were built at the primary-monitor DPI; building again here corrects them.)
    box_dpi = (int)::GetDpiForWindow(parent->GetSafeHwnd());
    apply_default_fonts();
    build_efont();
    build_kfont();
    calc_cell_size();

    // Window size = configured grid (cfg_rows x cfg_cols cells) plus the margins. Margins are 96 DPI
    // logical, so scale them to physical pixels at box_dpi (cell_w/cell_h are already physical).
    int pw = cfg_cols * cell_w + ::MulDiv(margin_left, box_dpi, 96) + ::MulDiv(margin_right, box_dpi, 96);
    int ph = cfg_rows * cell_h + ::MulDiv(margin_top,  box_dpi, 96) + ::MulDiv(margin_bottom, box_dpi, 96);
    CRect rc(left, top, left + pw, top + ph);

    // No WS_VSCROLL: a native vertical scrollbar would shrink the client area the first time scrollback
    // appears, reflowing the grid and resizing the child PTY (a visible flash). Instead the overlay
    // scrollbar (draw_overlay_scrollbar) is painted over the right edge and never reserves client space.
    // CreateWindowExW keeps the class name as wchar_t in both MBCS and Unicode builds.
    // AfxHookWindowCreate installs MFC's CBT hook so the HWND-to-CWnd mapping and message map
    // are set up exactly as CWnd::CreateEx would — no SubclassWindow side-effects.
    AfxHookWindowCreate(this);
    HWND hwnd = ::CreateWindowExW(0, L"ConBox", L"",
        WS_CHILD | WS_VISIBLE,
        rc.left, rc.top, rc.Width(), rc.Height(),
        parent->GetSafeHwnd(), nullptr, AfxGetInstanceHandle(), nullptr);
    if (!AfxUnhookWindowCreate())
        PostNcDestroy();

    update_metrics();
    update_scrollbar();

    // Accept drag-and-drop files so OnDropFiles can type their paths to the child.
    DragAcceptFiles(TRUE);

    // The window exists now, so start the blink timers.
    // cursor_blink_ms: set by set_cursor_blink (0 = system blink disabled -> always on).
    cursor_on = true;
    if (cursor_blink_ms > 0)
        SetTimer(CURSOR_TIMER, cursor_blink_ms, NULL);
    blink_on = true;
    SetTimer(BLINK_TIMER, 500, NULL);   // SGR blink: 500ms toggle (always running)

    // Print any deferred INI status message (stored by setup_from_ini before the window existed).
    if (!ini_msg.empty()) {
        print(ini_msg.c_str());
        ini_msg.clear();
    }

    // Auto-start child if a cmdline was configured.
    if (!cfg_cmdline.empty())
        start(cfg_cmdline.c_str());
}

void ConBox::make_font(CFont& font, LOGFONTW& lf_out, int& width_pct, const char* name, float size, const char* option)
{
    // Per-monitor DPI for pt->pixel conversion. Prefer the window's own DPI
    // (GetDpiForWindow) so the font matches the monitor the control lives on.
    // make_font also runs BEFORE the window exists (set_efont/set_kfont via
    // setup_from_ini, and build_efont/build_kfont inside open() before CreateWindowExW):
    // there GetDpiForWindow(NULL) returns 0. Fall back to box_dpi (set by open() from the
    // parent monitor), then to the primary-monitor DPI (GetDeviceCaps) if even that is unknown.
    int dpi = m_hWnd ? (int)::GetDpiForWindow(m_hWnd) : 0;
    if (dpi <= 0) dpi = box_dpi;
    if (dpi <= 0) {
        HDC hdc = ::GetDC(NULL);
        dpi = ::GetDeviceCaps(hdc, LOGPIXELSY);
        ::ReleaseDC(NULL, hdc);
    }

    lf_out = ParseFontOpts(name, size, option, dpi, width_pct);
    font.DeleteObject();
    font.Attach(::CreateFontIndirectW(&lf_out));
}

// Create a bold/italic variant of base_lf. If base lfWeight >= 600, bold uses FW_EXTRABOLD (800)
// to get visually bolder; otherwise FW_BOLD (700). This allows SGR bold on already-bold fonts
// (e.g. Malgun Gothic B) to become noticeably thicker.
static void MakeFontVariant(CFont& font, LOGFONTW lf, bool bold, bool italic)
{
    if (bold)   lf.lfWeight = (lf.lfWeight >= 600) ? FW_EXTRABOLD : FW_BOLD;
    if (italic) lf.lfItalic = 1;
    font.DeleteObject();
    font.Attach(::CreateFontIndirectW(&lf));
}

// Build the double-size (2x) variant of src into dst. Used for SGR 8 (double-size) cells. The glyph
// is always rendered bold (titles), so bold/italic SGR is ignored on double cells -- no italic
// variant is created. Height and width are doubled from src's current LOGFONT.
static void MakeDoubleFont(CFont& dst, CFont& src)
{
    LOGFONTW lf;
    ::GetObjectW(src.m_hObject, sizeof(LOGFONTW), &lf);
    lf.lfHeight = lf.lfHeight < 0 ? lf.lfHeight * 2 : -((-lf.lfHeight) * 2);
    if (lf.lfWidth != 0) lf.lfWidth *= 2;
    lf.lfWeight = (lf.lfWeight >= 600) ? FW_EXTRABOLD : FW_BOLD;   // always bold
    dst.DeleteObject();
    dst.Attach(::CreateFontIndirectW(&lf));
}

void ConBox::apply_default_fonts()
{
    // Defaults: English Cascadia Mono 12pt normal, Korean Malgun Gothic normal (wt.exe Claude profile).
    // Korean size 0 = match-English-height mode (kfont_match_efont=true in the ctor), consistent with
    // the "size<=0 means match" rule. Specs only -- the GDI build happens in build_efont/build_kfont,
    // so the empty-name test marks a font the host never set (set_efont/set_kfont fill the name).
    if (efont_name.empty()) { efont_name = "Cascadia Mono"; efont_size = 12.0f; efont_opts.clear(); }
    if (kfont_name.empty()) { kfont_name = "Malgun Gothic"; kfont_size = 0.0f;  kfont_opts.clear(); }
}

// (design: [FONT])
void ConBox::calc_cell_size()
{
    BOOL has_wnd = ::IsWindow(m_hWnd);
    HWND meas_wnd = has_wnd ? m_hWnd : NULL;
    HDC hdc = ::GetDC(meas_wnd);

    // 1. Measure the English font in its natural state (lfWidth = 0).
    LOGFONTW elf = efont_lf;
    elf.lfWidth = 0;
    efont.DeleteObject();
    efont.Attach(::CreateFontIndirectW(&elf));

    HGDIOBJ old = ::SelectObject(hdc, efont.GetSafeHandle());
    TEXTMETRICW tm;
    ::GetTextMetricsW(hdc, &tm);
    SIZE sz;
    ::GetTextExtentPoint32W(hdc, L"W", 1, &sz);
    int natw_e = sz.cx;                 // English natural char width
    int eh = tm.tmHeight + tm.tmExternalLeading;
    int ea = tm.tmAscent;

    // 2. Measure the Korean font's size and natural width.
    int kh = 0, ka = 0, natw_k = 0;
    LOGFONTW klf = kfont_lf;
    klf.lfWidth = 0;

    if (kfont_match_efont) {
        // Height-match: build Korean at the English em, measure its real height, then scale the em so
        // its height equals English (eh). Width stays natural (lfWidth=0).
        klf.lfHeight = efont_lf.lfHeight;
        kfont.DeleteObject();
        kfont.Attach(::CreateFontIndirectW(&klf));
        ::SelectObject(hdc, kfont.GetSafeHandle());
        ::GetTextMetricsW(hdc, &tm);
        int kh0 = tm.tmHeight + tm.tmExternalLeading;
        if (kh0 > 0 && kh0 != eh) {
            LONG newh = (LONG)((double)klf.lfHeight * (double)eh / (double)kh0);
            if (newh == 0) newh = klf.lfHeight;
            klf.lfHeight = newh;
            ::SelectObject(hdc, old);
            kfont.DeleteObject();
            kfont.Attach(::CreateFontIndirectW(&klf));
            ::SelectObject(hdc, kfont.GetSafeHandle());
            ::GetTextMetricsW(hdc, &tm);
        }
        SIZE ksz;
        ::GetTextExtentPoint32W(hdc, L"\xac00", 1, &ksz);   // U+AC00 hangul syllable
        natw_k = ksz.cx;
        kh = tm.tmHeight + tm.tmExternalLeading;
        ka = tm.tmAscent;
    }
    else {
        kfont.DeleteObject();
        kfont.Attach(::CreateFontIndirectW(&klf));
        ::SelectObject(hdc, kfont.GetSafeHandle());
        ::GetTextMetricsW(hdc, &tm);
        SIZE ksz;
        ::GetTextExtentPoint32W(hdc, L"\xac00", 1, &ksz);   // U+AC00 hangul syllable
        natw_k = ksz.cx;
        kh = tm.tmHeight + tm.tmExternalLeading;
        ka = tm.tmAscent;
    }

    // 3. Target widths after applying each width ratio.
    int w_e = (int)(natw_e * efont_width_pct / 100.0f + 0.5f);
    int w_k = (int)(natw_k * kfont_width_pct / 100.0f + 0.5f);

    // 4. Cell width = ceil( max(2*w_e, w_k) / 2 ).
    int max_w = (2 * w_e > w_k) ? (2 * w_e) : w_k;
    cell_w = (max_w + 1) / 2;
    if (cell_w < 1) cell_w = 1;

    // 5. Set lfWidth and rebuild only when the ratio is not 100%. At 100% (default) the natural-width
    //    (lfWidth=0) fonts built above are kept (avoids scaling artifacts).
    if (efont_width_pct != 100) {
        elf.lfWidth = w_e;
        efont.DeleteObject();
        efont.Attach(::CreateFontIndirectW(&elf));
    }

    if (kfont_width_pct != 100) {
        klf.lfWidth = (int)(w_k / 2.0f + 0.5f);
        kfont.DeleteObject();
        kfont.Attach(::CreateFontIndirectW(&klf));
    }

    // Line height/baseline take the larger of the two fonts to avoid clipping.
    cell_h = (eh > kh) ? eh : kh;
    cell_base = (ea > ka) ? ea : ka;

    // Apply the adjust() per-side pixel padding. left/right grow the cell width, top/bottom the height.
    // top also pushes the glyph baseline down; left pushes the glyph right (added at TextOutW, since the
    // glyph horizontal origin is the cell's left edge). bottom/right only enlarge the cell (no glyph
    // shift). Negative values trim; clamp to keep the cell at least 1px.
    cell_w += adjust_left + adjust_right;
    cell_h += adjust_top + adjust_bottom;
    cell_base += adjust_top;
    if (cell_w < 1) cell_w = 1;
    if (cell_h < 1) cell_h = 1;

    ::SelectObject(hdc, old);
    ::ReleaseDC(meas_wnd, hdc);

    // Build the double-size (2x, always-bold) variants from the now-finalized fonts. This must run
    // here (not in set_efont/set_kfont) because in Korean match mode kfont's final height is only
    // settled above; making kfont_double earlier would capture the pre-match (size 0) LOGFONT and
    // render Korean double-size glyphs at the wrong (1x-looking) size.
    MakeDoubleFont(efont_double, efont);
    MakeDoubleFont(kfont_double, kfont);

    // In match mode kfont_lf.lfHeight was 0 at set_kfont time, so the bold/italic variants built
    // there also got lfHeight=0 (GDI default, too large). Sync kfont_lf from the now-finalized
    // kfont GDI handle and rebuild the variants with the correct matched height.
    if (kfont_match_efont) {
        ::GetObjectW(kfont.GetSafeHandle(), sizeof(LOGFONTW), &kfont_lf);
        MakeFontVariant(kfont_bold,        kfont_lf, true,  false);
        MakeFontVariant(kfont_italic,      kfont_lf, false, true);
        MakeFontVariant(kfont_bold_italic, kfont_lf, true,  true);
    }
}

// Build efont + variants from the stored efont_* spec at the current DPI (make_font).
void ConBox::build_efont()
{
    make_font(efont, efont_lf, efont_width_pct, efont_name.c_str(), efont_size, efont_opts.c_str());
    MakeFontVariant(efont_bold,        efont_lf, true,  false);
    MakeFontVariant(efont_italic,      efont_lf, false, true);
    MakeFontVariant(efont_bold_italic, efont_lf, true,  true);
    // efont_double is (re)built in calc_cell_size.
}

// Build kfont + variants from the stored kfont_* spec; size<=0 keeps match mode.
void ConBox::build_kfont()
{
    make_font(kfont, kfont_lf, kfont_width_pct, kfont_name.c_str(), kfont_size, kfont_opts.c_str());
    MakeFontVariant(kfont_bold,        kfont_lf, true,  false);
    MakeFontVariant(kfont_italic,      kfont_lf, false, true);
    MakeFontVariant(kfont_bold_italic, kfont_lf, true,  true);
    // kfont_double is (re)built in calc_cell_size.

    // size<=0 turns on match mode (size ignored); positive turns it off. Match mode changes line
    // height (cell_h); the caller recomputes the grid.
    kfont_match_efont = (kfont_size <= 0.0f);
}

void ConBox::set_efont(const char* name, float size, const char* option)
{
    efont_name = name ? name : "";
    efont_size = size;
    efont_opts = option ? option : "";
    build_efont();

    // Cell size changed; recompute the grid if the window is up.
    if (::IsWindow(m_hWnd)) {
        calc_cell_size();
        update_metrics();
    }
}

void ConBox::set_kfont(const char* name, float size, const char* option)
{
    kfont_name = name ? name : "";
    kfont_size = size;
    kfont_opts = option ? option : "";
    build_kfont();

    if (::IsWindow(m_hWnd)) {
        calc_cell_size();
        update_metrics();

        int maxtop = (int)scrollback.size();
        if (maxtop < 0) maxtop = 0;
        if (view_top > maxtop) view_top = maxtop;

        update_scrollbar();
        Invalidate();
    }
}

// Rebuild both fonts at the window's current DPI and repaint, PRESERVING the logical grid (cols/rows).
// Does NOT call update_metrics(): a DPI change only rescales cell pixels, so the child PTY must not be
// resized (that makes the shell re-emit -> duplicated/lost content). Called from OnSize when it detects
// a DPI change (box_dpi != GetDpiForWindow), which is the single, order-independent DPI entry point for
// the WS_CHILD case.
void ConBox::relayout_for_dpi()
{
    box_dpi = (int)::GetDpiForWindow(m_hWnd);
    if (box_dpi <= 0) box_dpi = 96;
    build_efont();
    build_kfont();
    calc_cell_size();
    recalc_origin();   // grid (cols/rows) preserved, but cell pixels changed -> re-center

    int maxtop = (int)scrollback.size();
    if (maxtop < 0) maxtop = 0;
    if (view_top > maxtop) view_top = maxtop;

    update_scrollbar();
    Invalidate();
}

LRESULT ConBox::OnDpiChanged(WPARAM, LPARAM l)
{
    // Top-level / popup ConBox only (a WS_CHILD ConBox gets no WM_DPICHANGED; OnSize handles it).
    // Apply the OS-suggested rect; the resulting OnSize rebuilds fonts + preserves the grid.
    const RECT* r = reinterpret_cast<const RECT*>(l);
    if (r)
        ::SetWindowPos(m_hWnd, nullptr, r->left, r->top,
                       r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
    return 0;
}

void ConBox::adjust(int left, int top, int right, int bottom)
{
    // Stored per-side pixel padding persists until the next font change.
    adjust_left = left;
    adjust_top = top;
    adjust_right = right;
    adjust_bottom = bottom;

    if (::IsWindow(m_hWnd)) {
        calc_cell_size();
        update_metrics();

        int maxtop = (int)scrollback.size();
        if (maxtop < 0) maxtop = 0;
        if (view_top > maxtop) view_top = maxtop;

        update_scrollbar();
        Invalidate();
    }
}

void ConBox::set_builtin_glyphs(int level)
{
    if (level < 0) level = 0;
    if (level > 2) level = 2;
    glyph_level = level;

    // Only the draw method changed; a repaint is enough (no grid recompute).
    if (::IsWindow(m_hWnd))
        Invalidate();
}

void ConBox::set_fg_color(COLORREF fg)
{
    default_fg = fg;
    cur_fg = fg;
}

void ConBox::set_bg_color(COLORREF bg)
{
    default_bg = bg;
    cur_bg = bg;
}

void ConBox::set_cursor_blend(int bg_weight, int fg_weight)
{
    // Clamp negatives to 0; ignore a zero sum (no meaning).
    if (bg_weight < 0) bg_weight = 0;
    if (fg_weight < 0) fg_weight = 0;
    if (bg_weight + fg_weight <= 0)
        return;

    cursor_bg_weight = bg_weight;
    cursor_fg_weight = fg_weight;

    if (::IsWindow(m_hWnd)) {
        CRect rc;
        if (get_cursor_rect(rc))
            InvalidateRect(&rc, FALSE);
    }
}

void ConBox::set_cursor_blink(int interval_ms)
{
    // interval_ms <= 0: follow the system caret rate (GetCaretBlinkTime). If the system has blink
    // disabled (INFINITE), cursor_blink_ms stays 0 = always on (no timer). This matches the INI
    // default comment "0 = system rate" and the constructor's own initialization logic.
    if (interval_ms <= 0) {
        UINT caret = ::GetCaretBlinkTime();
        cursor_blink_ms = (caret == INFINITE) ? 0 : (int)caret;
    } else {
        cursor_blink_ms = interval_ms;
    }

    if (::IsWindow(m_hWnd)) {
        KillTimer(CURSOR_TIMER);
        cursor_on = true;
        if (cursor_blink_ms > 0)
            SetTimer(CURSOR_TIMER, cursor_blink_ms, NULL);
        Invalidate();
    }
}

void ConBox::set_cursor(int type)
{
    // type 0 = default. Default = blinking underline (3), matching the classic conhost.exe cursor.
    if (type == 0)
        type = 3;
    cursor_type = type;

    // Even types are fixed (no blink) -> kill the timer and pin visible. Odd types blink at the
    // configured rate. The actual shape (block/underline/I-beam) is chosen in OnPaint by cursor_type.
    if (::IsWindow(m_hWnd)) {
        KillTimer(CURSOR_TIMER);
        cursor_on = true;
        if ((cursor_type & 1) == 1 && cursor_blink_ms > 0)
            SetTimer(CURSOR_TIMER, cursor_blink_ms, NULL);
        Invalidate();
    }
}

void ConBox::bump_cursor()
{
    // Make the cursor visible now and restart the timer so it stays solid for a beat after input/move
    // (so no off-frame lands right then). Fixed (even) types never blink, so leave their timer alone.
    cursor_on = true;
    if (::IsWindow(m_hWnd) && cursor_blink_ms > 0 && (cursor_type & 1) == 1) {
        KillTimer(CURSOR_TIMER);
        SetTimer(CURSOR_TIMER, cursor_blink_ms, NULL);
    }
}

void ConBox::OnTimer(UINT_PTR id)
{
    if (id == CURSOR_TIMER) {
        if ((cursor_type & 1) == 0)   // fixed (even) type: ignore the blink tick
            return;
        cursor_on = !cursor_on;
        CRect rc;
        if (get_cursor_rect(rc))
            InvalidateRect(&rc, FALSE);
        return;
    }
    if (id == BLINK_TIMER) {
        blink_on = !blink_on;
        // Skip Invalidate when no CELL_BLINK cells are visible -- avoids a full repaint every 500ms
        // on typical screens. Scan the visible viewport via line_at(view_top+row) so scrollback rows
        // that are currently scrolled into view are also checked (screen[] alone misses them).
        bool has_blink = false;
        int total = (int)scrollback.size() + rows;
        for (int row = 0; row < rows && !has_blink; ++row) {
            int idx = view_top + row;
            if (idx >= total) break;
            for (const CharInfo& c : line_at(idx))
                if (c.flags & CELL_BLINK) { has_blink = true; break; }
        }
        if (has_blink) Invalidate(FALSE);
        return;
    }
    if (id == PUMP_TIMER) {
        // Child (ConPTY) output polling. start() set this; no child means no timer.
        pump();
        return;
    }
    if (id == SBAR_TIMER) {
        // Overlay scrollbar fade. Held fully visible while hovering/dragging or within the hold window;
        // otherwise alpha decays each tick until 0, then the timer stops.
        if (sbar_dragging || sbar_hover) {
            sbar_hold_until = ::GetTickCount() + SBAR_HOLD_MS;   // keep it up
            return;
        }
        if ((int)(::GetTickCount() - sbar_hold_until) < 0)
            return;   // still inside the hold window: stay opaque
        sbar_alpha -= SBAR_FADE_STEP;
        if (sbar_alpha <= 0) {
            sbar_alpha = 0;
            KillTimer(SBAR_TIMER);
        }
        CRect track, thumb;
        if (sbar_geometry(track, thumb)) {
            CRect full;
            GetClientRect(&full);
            CRect gutter(full.right - sbar_px(SBAR_W), full.top, full.right, full.bottom);
            InvalidateRect(&gutter, FALSE);
        }
        return;
    }
    CWnd::OnTimer(id);
}

void ConBox::set_margin(int top, int left, int bottom, int right)
{
    // CSS-shorthand omission: resolve left before right (right can follow left).
    if (left < 0) left = top;
    if (bottom < 0) bottom = top;
    if (right < 0) right = left;

    margin_top = top;
    margin_left = left;
    margin_bottom = bottom;
    margin_right = right;

    // After open(), recompute the grid like a resize.
    if (::IsWindow(m_hWnd)) {
        update_metrics();

        int maxtop = (int)scrollback.size();
        if (maxtop < 0) maxtop = 0;
        if (view_top > maxtop) view_top = maxtop;

        update_scrollbar();
        Invalidate();
    }
}



// Cursor block color: mix default bg and fg per channel at cursor_bg_weight:cursor_fg_weight. The
// default 6:4 is near-bg so a glyph drawn over the block in fg stays legible. Independent of SGR colors.
COLORREF ConBox::blend_cursor_color() const
{
    int bw = cursor_bg_weight;
    int fw = cursor_fg_weight;
    int sum = bw + fw;
    if (sum <= 0)
        return default_bg;

    int r = (GetRValue(default_bg) * bw + GetRValue(default_fg) * fw) / sum;
    int g = (GetGValue(default_bg) * bw + GetGValue(default_fg) * fw) / sum;
    int b = (GetBValue(default_bg) * bw + GetBValue(default_fg) * fw) / sum;
    return RGB(r, g, b);
}

// Map a screen COLORREF to its paper (export) equivalent.
// Matches default_fg/bg and ansi_colors[0..15]; truecolor / 256-color values pass through.
COLORREF ConBox::remap_paper_color(COLORREF c) const
{
    if (c == default_fg) return paper_default_fg;
    if (c == default_bg) return paper_default_bg;
    for (int i = 0; i < 16; i++)
        if (c == ansi_colors[i]) return paper_ansi_colors[i];
    return c;
}

bool ConBox::cursor_screen_pos(int& row_out, int& vx_out) const
{
    if (screen.empty())
        return false;

    // Subtract view_top from the cursor's unified index (scrollback then screen) to get the screen row.
    // Column is the cell coord directly. (Off-screen is judged by the caller via row.)
    int abs_idx = (int)scrollback.size() + cur_row;
    row_out = abs_idx - view_top;
    vx_out = cur_col;
    return true;
}

bool ConBox::get_cursor_rect(CRect& rc) const
{
    int row, vx;
    if (!cursor_screen_pos(row, vx))
        return false;
    if (row < 0 || row >= rows)
        return false;

    // Block cursor filling the cell. In Korean input mode (even before composing) it is 2 cells wide;
    // English mode 1 cell.
    int cw = is_hangul_mode() ? cell_w * 2 : cell_w;

    int px = origin_x + vx * cell_w;
    int py = origin_y + row * cell_h;
    rc.SetRect(px, py, px + cw, py + cell_h);
    return true;
}

// Whether the IME is in Korean (jamo) mode: ImmGetConversionStatus's IME_CMODE_NATIVE flag. Correct
// right after a Korean/English toggle (before composition starts).
bool ConBox::is_hangul_mode() const
{
    HIMC himc = ImmGetContext(m_hWnd);
    if (!himc)
        return false;

    DWORD conv = 0, sentence = 0;
    bool native = false;
    if (ImmGetConversionStatus(himc, &conv, &sentence))
        native = (conv & IME_CMODE_NATIVE) != 0;

    ImmReleaseContext(m_hWnd, himc);
    return native;
}

void ConBox::update_metrics()
{
    int old_cols = cols, old_rows = rows;

    CRect rc;
    GetClientRect(&rc);

    // Margins are 96 DPI logical; scale to physical at box_dpi before subtracting from the client.
    int dpi = box_dpi > 0 ? box_dpi : 96;
    int avail_w = rc.Width()  - ::MulDiv(margin_left, dpi, 96) - ::MulDiv(margin_right, dpi, 96);
    int avail_h = rc.Height() - ::MulDiv(margin_top,  dpi, 96) - ::MulDiv(margin_bottom, dpi, 96);
    cols = (cell_w > 0) ? (avail_w / cell_w) : 1;
    rows = (cell_h > 0) ? (avail_h / cell_h) : 1;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    recalc_origin();
    reset_screen();

    // If the grid actually changed and a resize sink is set, notify it (to sync the child PTY size).
    if ((cols != old_cols || rows != old_rows) && resize_sink != nullptr)
        resize_sink(rows, cols, resize_sink_user);
}

// Center the grid (cols*cell_w x rows*cell_h) in the client area. The configured margins do NOT
// position the grid for drawing -- they only sized the window / derived cols,rows -- so the realized
// margin is whatever centering leaves and may exceed or fall short of the configured value. When the
// grid is larger than the client (integer rounding at some DPI), the origin clamps to 0 so drawing
// starts at the top-left corner and the bottom/right is clipped. hit_test, the caret, and the IME
// windows all read origin_x/origin_y so they stay aligned with what OnPaint draws.
void ConBox::recalc_origin()
{
    CRect rc;
    GetClientRect(&rc);
    origin_x = (rc.Width()  - cols * cell_w) / 2;
    origin_y = (rc.Height() - rows * cell_h) / 2;
    if (origin_x < 0) origin_x = 0;
    if (origin_y < 0) origin_y = 0;
}

// Resize the window so the preserved grid fits, per snap_mode (ini). On a DPI change the host moves us
// to a linearly-scaled rect, but cell pixels do not scale linearly (integer font rounding), so the same
// cols/rows may need a few more (or fewer) pixels than the linear rect provides -> the last column(s)/row
// get clipped (deficit) or extra slack appears (surplus). The whole-grid size is cols*cell_w / rows*cell_h
// plus the DPI-scaled margins.
//   snap_mode 0: do nothing (recalc_origin centers the slack; a deficit is clipped at the bottom/right).
//   snap_mode 1: grow only -- snap an axis up to the grid size when it would be clipped; leave a surplus
//                axis alone (centered slack).
//   snap_mode 2: always snap each axis to the exact grid+margin size (grow or shrink).
// SWP_NOMOVE keeps the upper-left corner fixed; only the right/bottom edges move. The SetWindowPos
// triggers a follow-up OnSize with cur == box_dpi, which recomputes the IDENTICAL grid (same size ->
// no resize_sink -> no PTY resize). The non-client difference is added so a bordered popup ConBox sizes
// its CLIENT (not its frame) to the grid.
void ConBox::snap_to_grid()
{
    if (snap_mode <= 0)
        return;

    int dpi = box_dpi > 0 ? box_dpi : 96;
    int need_w = cols * cell_w + ::MulDiv(margin_left, dpi, 96) + ::MulDiv(margin_right, dpi, 96);
    int need_h = rows * cell_h + ::MulDiv(margin_top,  dpi, 96) + ::MulDiv(margin_bottom, dpi, 96);

    CRect cr, wr;
    GetClientRect(&cr);

    int tw, th;
    if (snap_mode == 1) {
        if (cr.Width() >= need_w && cr.Height() >= need_h)
            return;   // both axes already fit -> leave surplus to centering
        tw = max(cr.Width(),  need_w);
        th = max(cr.Height(), need_h);
    } else {          // snap_mode 2: exact grid+margin on both axes
        if (cr.Width() == need_w && cr.Height() == need_h)
            return;
        tw = need_w;
        th = need_h;
    }

    GetWindowRect(&wr);
    int nc_w = wr.Width()  - cr.Width();    // non-client width  (0 for a borderless WS_CHILD)
    int nc_h = wr.Height() - cr.Height();
    SetWindowPos(nullptr, 0, 0, tw + nc_w, th + nc_h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

Row ConBox::blank_row() const
{
    // Filled with the current SGR background so erase/scroll follow the background color.
    CharInfo c;
    c.ch = L' ';
    c.flags = 0;
    c.fg = cur_fg;
    c.bg = cur_bg;
    return Row(cols < 1 ? 1 : cols, c);
}

void ConBox::reset_screen()
{
    // Fit screen to the current rows x cols. Existing lines are kept but padded/truncated to cols and
    // the row count adjusted to rows (cell-level pad/truncate only; no fine reflow).
    CharInfo blank;
    blank.ch = L' ';
    blank.flags = 0;
    blank.fg = cur_fg;
    blank.bg = cur_bg;

    for (size_t r = 0; r < screen.size(); ++r) {
        Row& line = screen[r];
        if ((int)line.size() > cols)
            line.resize(cols);
        else if ((int)line.size() < cols)
            line.resize(cols, blank);
    }
    if ((int)screen.size() < rows) {
        while ((int)screen.size() < rows)
            screen.push_back(blank_row());
    }
    else if ((int)screen.size() > rows) {
        screen.resize(rows);
    }
    clamp_cursor();

    // Keep the scroll region within the new screen; reset to full screen if it became invalid. (On a
    // window resize a full-screen app re-sets the region via SIGWINCH, so clamping here is safe.)
    if (scroll_top < 0) scroll_top = 0;
    if (scroll_bot > rows - 1 || scroll_bot < 0) scroll_bot = rows - 1;
    if (scroll_top >= scroll_bot) { scroll_top = 0; scroll_bot = rows - 1; }
}

// (design: [COORD]/[DM])
const Row& ConBox::line_at(int idx) const
{
    // Unified index: scrollback first, then the current screen.
    int sb = (int)scrollback.size();
    if (idx < sb)
        return scrollback[idx];
    return screen[idx - sb];
}

void ConBox::clamp_cursor()
{
    if (cur_row < 0) cur_row = 0;
    if (cur_row > rows - 1) cur_row = rows - 1;
    if (cur_col < 0) cur_col = 0;
    if (cur_col > cols) cur_col = cols;   // cols = "just past line end" (wraps on the next glyph)
}

void ConBox::erase_cells(int row, int c0, int c1)
{
    if (row < 0 || row >= (int)screen.size())
        return;
    Row& line = screen[row];
    if (c0 < 0) c0 = 0;
    if (c1 > (int)line.size()) c1 = (int)line.size();
    for (int c = c0; c < c1; ++c) {
        line[c].ch = L' ';
        line[c].flags = 0;
        line[c].fg = cur_fg;
        line[c].bg = cur_bg;
    }
}

void ConBox::scroll_lines_up(int top, int bot, int n)
{
    // Rotate [top, bot] up by n (drop n top lines, add n blanks at the bottom). Pure line move, does
    // not touch scrollback (the caller pushes first if preservation is needed).
    if (top < 0) top = 0;
    if (bot > (int)screen.size() - 1) bot = (int)screen.size() - 1;
    if (top > bot) return;
    int region = bot - top + 1;
    if (n < 1) return;
    if (n > region) n = region;

    for (int i = 0; i < n; ++i) {
        screen.erase(screen.begin() + top);
        screen.insert(screen.begin() + bot, blank_row());
    }
}

void ConBox::scroll_lines_down(int top, int bot, int n)
{
    // Rotate [top, bot] down by n (drop n bottom lines, add n blanks at the top).
    if (top < 0) top = 0;
    if (bot > (int)screen.size() - 1) bot = (int)screen.size() - 1;
    if (top > bot) return;
    int region = bot - top + 1;
    if (n < 1) return;
    if (n > region) n = region;

    for (int i = 0; i < n; ++i) {
        screen.erase(screen.begin() + bot);
        screen.insert(screen.begin() + top, blank_row());
    }
}

void ConBox::scroll_up_region(int n)
{
    // Scroll [scroll_top, scroll_bot] up by n. Only on the main screen with the region top at the
    // screen top (scroll_top==0) are the displaced lines preserved into scrollback. On the alt screen
    // or a partial region they are dropped.
    if (n < 1) return;
    int region = scroll_bot - scroll_top + 1;
    if (n > region) n = region;

    if (!alt_active && scroll_top == 0) {
        for (int i = 0; i < n; ++i)
            scrollback.push_back(screen[i]);

        while ((int)scrollback.size() > max_scrollback)
            scrollback.pop_front();  // O(1) with deque
    }

    scroll_lines_up(scroll_top, scroll_bot, n);
}

void ConBox::scroll_down_region(int n)
{
    // RI/SD. Does not touch scrollback.
    scroll_lines_down(scroll_top, scroll_bot, n);
}

void ConBox::line_feed()
{
    // At the region bottom, scroll the region up (cursor stays on that line); otherwise advance one
    // line without leaving the screen.
    if (cur_row == scroll_bot) {
        scroll_up_region(1);
    } else {
        if (cur_row < rows - 1) cur_row++;
    }
}

void ConBox::insert_lines(int n)
{
    // IL: only when the cursor is inside the scroll region. Insert n blank lines at the cursor line,
    // pushing lines below toward scroll_bot. Cursor moves to column 1.
    if (cur_row < scroll_top || cur_row > scroll_bot)
        return;
    if (n < 1) n = 1;
    scroll_lines_down(cur_row, scroll_bot, n);
    cur_col = 0;
}

void ConBox::delete_lines(int n)
{
    // DL: only inside the scroll region. Delete n lines from the cursor line, pulling lines up and
    // filling blanks at the region bottom. Cursor moves to column 1.
    if (cur_row < scroll_top || cur_row > scroll_bot)
        return;
    if (n < 1) n = 1;
    scroll_lines_up(cur_row, scroll_bot, n);
    cur_col = 0;
}

void ConBox::insert_chars(int n)
{
    // ICH: insert n blanks at the cursor within the line; right cells shift and overflow past cols is
    // dropped. Cursor stays put.
    if (cur_row < 0 || cur_row >= (int)screen.size())
        return;
    Row& line = screen[cur_row];
    int c = cur_col;
    if (c < 0) c = 0;
    if (c >= (int)line.size())
        return;
    if (n < 1) n = 1;
    if (n > (int)line.size() - c) n = (int)line.size() - c;

    CharInfo blank;
    blank.ch = L' '; blank.flags = 0; blank.fg = cur_fg; blank.bg = cur_bg;
    line.insert(line.begin() + c, n, blank);
    line.resize(cols, blank);   // drop overflow to keep line width = cols
}

void ConBox::delete_chars(int n)
{
    // DCH: delete n cells at the cursor within the line, pull right cells in, fill blanks at the end.
    if (cur_row < 0 || cur_row >= (int)screen.size())
        return;
    Row& line = screen[cur_row];
    int c = cur_col;
    if (c < 0) c = 0;
    if (c >= (int)line.size())
        return;
    if (n < 1) n = 1;
    if (n > (int)line.size() - c) n = (int)line.size() - c;

    CharInfo blank;
    blank.ch = L' '; blank.flags = 0; blank.fg = cur_fg; blank.bg = cur_bg;
    line.erase(line.begin() + c, line.begin() + c + n);
    line.resize(cols, blank);   // refill the end to keep line width = cols
}

void ConBox::enter_alt_screen()
{
    // Back up the main screen and cursor, switch to a blank alt screen. Scrollback remains but is
    // frozen (hidden) while alt_active.
    if (alt_active)
        return;
    main_saved.swap(screen);          // back up main (screen left empty)
    saved_main_cur = { cur_row, cur_col };

    screen.assign(rows, blank_row());
    cur_row = 0;
    cur_col = 0;
    scroll_top = 0;
    scroll_bot = rows - 1;
    alt_active = true;
}

void ConBox::leave_alt_screen()
{
    // Restore the backed-up main screen and cursor.
    if (!alt_active)
        return;
    screen.swap(main_saved);
    main_saved.clear();
    alt_active = false;

    // Fit the restored main screen to the current cols/rows (a resize may have happened); restore cursor/region.
    cur_row = saved_main_cur.row;
    cur_col = saved_main_cur.col;
    scroll_top = 0;
    scroll_bot = rows - 1;
    reset_screen();
}

// (design: [VT])
void ConBox::put_char(wchar_t wc)
{
    // Write one glyph at the cursor, autowrapping first if it would exceed cols. A wide glyph goes in
    // the lead cell and sets the next cell to trail (ch=0).
    if (cur_row < 0 || cur_row >= (int)screen.size())
        return;

    int w = IsWideChar(wc) ? 2 : 1;
    int advance = cur_double ? w * 2 : w;   // double mode: cursor skips 2x to leave room for bleed
    if (cur_col + advance > cols) {
        cur_col = 0;
        line_feed();
    }

    // reverse swaps fg/bg into the cell.
    COLORREF fg = cur_reverse ? cur_bg : cur_fg;
    COLORREF bg = cur_reverse ? cur_fg : cur_bg;
    uint8_t attr = (cur_bold      ? CELL_BOLD      : 0)
                 | (cur_italic    ? CELL_ITALIC    : 0)
                 | (cur_underline ? CELL_UNDERLINE : 0)
                 | (cur_strike    ? CELL_STRIKE    : 0)
                 | (cur_blink     ? CELL_BLINK     : 0)
                 | (cur_double    ? CELL_DOUBLE    : 0);

    Row& line = screen[cur_row];
    if (cur_col >= 0 && cur_col < (int)line.size()) {
        line[cur_col].ch = wc;
        line[cur_col].flags = attr | (w == 2 ? CELL_WIDE : 0);
        line[cur_col].fg = fg;
        line[cur_col].bg = bg;
        if (w == 2 && cur_col + 1 < (int)line.size()) {
            line[cur_col + 1].ch = 0;   // trail cell (the lead draws both)
            line[cur_col + 1].flags = attr;   // no CELL_WIDE on trail
            line[cur_col + 1].fg = fg;
            line[cur_col + 1].bg = bg;
        }
    }
    cur_col += advance;
}

void ConBox::print(const char* text)
{
    if (text == nullptr)
        return;

    // Prepend any incomplete UTF-8 lead bytes carried over from the previous call, then detect and
    // save any new trailing incomplete sequence before converting.  This prevents MultiByteToWideChar
    // from substituting U+FFFD when a multi-byte sequence is split across two pump() chunks.
    int raw_len = (int)strlen(text);
    if (raw_len == 0 && utf8_tail_len == 0)
        return;

    // Build working buffer (member reuse): tail bytes + new bytes. No per-call allocation.
    print_buf.clear();
    if (utf8_tail_len > 0)
        print_buf.append(utf8_tail, utf8_tail_len);
    print_buf.append(text, raw_len);
    utf8_tail_len = 0;

    // Detect trailing incomplete UTF-8 sequence.
    // Walk backward up to 3 bytes to find a lead byte whose required sequence length exceeds what
    // remains in the buffer; if found, move those bytes into utf8_tail.
    int blen = (int)print_buf.size();
    for (int back = 1; back <= 3 && back <= blen; ++back) {
        unsigned char b = (unsigned char)print_buf[blen - back];
        if (b < 0x80) break;             // plain ASCII byte; no incomplete sequence possible
        if (b >= 0xC0) {                 // lead byte found
            int need = (b >= 0xF0) ? 4 : (b >= 0xE0) ? 3 : 2;
            if (back < need) {           // fewer bytes present than the sequence requires
                utf8_tail_len = back;
                memcpy(utf8_tail, print_buf.data() + blen - back, back);
                print_buf.resize(blen - back);
            }
            break;
        }
        // continuation byte (0x80-0xBF): keep walking back toward the lead
    }

    // Pre-allocate worst-case (blen wchar_t: UTF-8 byte count >= wchar_t count for BMP) then
    // convert in one call, avoiding a separate size-query call.
    int bufsz = (int)print_buf.size();
    print_ws.resize(bufsz);
    int wlen = ::MultiByteToWideChar(CP_UTF8, 0, print_buf.data(), bufsz, print_ws.data(), bufsz);
    if (wlen <= 0)
        return;

    // Create the screen if it does not exist yet (before font/window is fixed).
    if (screen.empty())
        reset_screen();

    // Feed each wchar_t to the VT parser.  No null terminator in output (explicit byte length was
    // passed to MultiByteToWideChar), so iterate over all wlen chars.
    for (int i = 0; i < wlen; ++i)
        vt_feed(print_ws[i]);

    // New output invalidates any selection (cell positions moved); clear it, scroll to bottom, repaint.
    sel_active = false;
    int maxtop = (int)scrollback.size();   // total lines - rows = scrollback.size() (screen always = rows)
    view_top = (maxtop > 0) ? maxtop : 0;
    update_scrollbar();
    bump_cursor();
    Invalidate();
}

// Convert an xterm 256-color index to COLORREF: 0-15 base, 16-231 6x6x6 cube, 232-255 grayscale.
static COLORREF Xterm256ToRgb(int n, const COLORREF* pal)
{
    // pal points to ansi_colors[16] (caller-owned; runtime-configurable via config color0..color15).
    if (n < 0) n = 0;
    if (n > 255) n = 255;
    if (n < 16)
        return pal[n];
    if (n < 232) {
        int i = n - 16;
        int r = i / 36, g = (i / 6) % 6, b = i % 6;
        // Cube step: 0 -> 0, 1..5 -> 55 + 40*v.
        int rr = r ? 55 + 40 * r : 0;
        int gg = g ? 55 + 40 * g : 0;
        int bb = b ? 55 + 40 * b : 0;
        return RGB(rr, gg, bb);
    }
    int v = 8 + 10 * (n - 232);   // 232..255 -> 8, 18, ..., 238
    return RGB(v, v, v);
}

// (design: [VT])
void ConBox::vt_feed(wchar_t wc)
{
    // Parser state machine. State (vt_state etc.) is a member, so a sequence survives print() chunks.
    switch (vt_state) {
    case VT_GROUND:
        if (wc == 0x1B) { vt_state = VT_ESC; return; }   // ESC
        if (wc == L'\r') { cur_col = 0; return; }          // CR
        if (wc == L'\n') { line_feed(); return; }          // LF
        if (wc == 0x08) { if (cur_col > 0) cur_col--; return; }  // BS
        if (wc == L'\t') {
            // Advance to the next 8-column tab stop (not past the last cell).
            int next = (cur_col / 8 + 1) * 8;
            if (next > cols - 1) next = cols - 1;
            cur_col = next;
            return;
        }
        if (wc == 0x07) return;   // BEL ignored
        if (wc < 0x20) return;    // other C0 ignored
        put_char(wc);
        return;

    case VT_ESC:
        if (wc == L'[') {
            vt_state = VT_CSI;
            vt_nparam = 0;
            vt_priv = false;
            vt_gtlt = false;
            vt_space = false;
            for (int k = 0; k < 16; ++k) vt_params[k] = 0;
            return;
        }
        if (wc == L']') { vt_state = VT_OSC; return; }   // OSC: discard (window title etc.)
        if (wc == L'7') { saved_cur = { cur_row, cur_col }; vt_state = VT_GROUND; return; }   // DECSC
        if (wc == L'8') { cur_row = saved_cur.row; cur_col = saved_cur.col; clamp_cursor(); vt_state = VT_GROUND; return; }  // DECRC
        if (wc == L'M') {   // RI: at the region top scroll down, else move up one line
            if (cur_row == scroll_top) scroll_down_region(1);
            else if (cur_row > 0) cur_row--;
            vt_state = VT_GROUND; return;
        }
        if (wc == L'D') {   // IND: index -- move down (opposite of RI)
            if (cur_row == scroll_bot) scroll_up_region(1);
            else if (cur_row < rows - 1) cur_row++;
            vt_state = VT_GROUND; return;
        }
        if (wc == L'E') {   // NEL: next line (CR + LF)
            cur_col = 0;
            if (cur_row == scroll_bot)
                scroll_up_region(1);
            else if (cur_row < rows - 1)
                cur_row++;
            vt_state = VT_GROUND; return;
        }
        if (wc == L'c') {   // RIS: reset to initial state
            cur_row = 0; cur_col = 0;
            saved_cur = { 0, 0 };
            scroll_top = 0; scroll_bot = rows - 1;
            app_cursor_keys = false;
            bracketed_paste = false;
            cursor_visible = true;
            cur_fg = default_fg; cur_bg = default_bg;
            cur_bold = cur_italic = cur_underline = cur_strike = cur_blink = cur_reverse = false;
            for (int r = 0; r < rows; ++r) erase_cells(r, 0, cols);
            if (alt_active) leave_alt_screen();
            vt_state = VT_GROUND; return;
        }
        // Other ESC sequences (standalone / 2-byte) ignored.
        vt_state = VT_GROUND;
        return;

    case VT_CSI:
        if (wc >= L'0' && wc <= L'9') {
            if (vt_nparam == 0) vt_nparam = 1;
            int idx = vt_nparam - 1;
            if (idx < 16)
                vt_params[idx] = vt_params[idx] * 10 + (int)(wc - L'0');
            return;
        }
        if (wc == L';') {
            if (vt_nparam == 0) vt_nparam = 1;   // an empty first param is 0
            if (vt_nparam < 16) { vt_params[vt_nparam] = 0; vt_nparam++; }
            return;
        }
        if (wc == L'?') { vt_priv = true; return; }
        // '<' '=' '>' prefix 2nd DA / kitty keyboard / XTMODKEYS etc. Mark it (dispatch_csi drops the
        // whole sequence) so the final byte (u/m/q...) is not misparsed as a standard command.
        if (wc == L'<' || wc == L'=' || wc == L'>') { vt_gtlt = true; return; }
        if (wc >= 0x40 && wc <= 0x7E) {
            dispatch_csi(wc);
            vt_state = VT_GROUND;
            return;
        }
        // Intermediate bytes (0x20-0x2F). Only ' ' (0x20) is tracked, to tell DECSCUSR (CSI Ps SP q)
        // apart from a bare CSI q; the rest are ignored. The sequence continues either way.
        if (wc == 0x20) vt_space = true;
        return;

    case VT_OSC:
        // OSC string (set window title etc.): discard up to the terminator. Terminator: BEL (0x07) or
        // ST (ESC '\'). On ESC, go to ESC state to finish on the next char.
        if (wc == 0x07) { vt_state = VT_GROUND; return; }
        if (wc == 0x1B) { vt_state = VT_ESC; return; }
        return;

    default:
        vt_state = VT_GROUND;
        return;
    }
}

// (design: [VT])
void ConBox::dispatch_csi(wchar_t fin)
{
    // Sequences prefixed with '<' '=' '>' (2nd DA, kitty keyboard protocol, XTMODKEYS...) are
    // unsupported. Their final byte (u/m/q...) must not be misparsed as a standard command (e.g.
    // ESC[<u being treated as restore-cursor once jumped the cursor to 0,0), so drop the whole sequence.
    if (vt_gtlt)
        return;

    // First param (0 if none). Each command applies its own default (usually 1) when absent.
    int n = (vt_nparam >= 1) ? vt_params[0] : 0;

    switch (fin) {
    case L'A': { int d = n ? n : 1; cur_row -= d; clamp_cursor(); break; }   // CUU
    case L'B': { int d = n ? n : 1; cur_row += d; clamp_cursor(); break; }   // CUD
    case L'C': { int d = n ? n : 1; cur_col += d; if (cur_col > cols - 1) cur_col = cols - 1; break; }  // CUF
    case L'D': { int d = n ? n : 1; cur_col -= d; if (cur_col < 0) cur_col = 0; break; }   // CUB
    case L'E': { int d = n ? n : 1; cur_row += d; cur_col = 0; clamp_cursor(); break; }   // CNL
    case L'F': { int d = n ? n : 1; cur_row -= d; cur_col = 0; clamp_cursor(); break; }   // CPL

    case L'H':
    case L'f': {   // CUP / HVP (1-based -> 0-based)
        int r = (vt_nparam >= 1 && vt_params[0]) ? vt_params[0] : 1;
        int c = (vt_nparam >= 2 && vt_params[1]) ? vt_params[1] : 1;
        cur_row = r - 1;
        cur_col = c - 1;
        clamp_cursor();
        break;
    }
    case L'G':
    case L'`': { int c = n ? n : 1; cur_col = c - 1; clamp_cursor(); break; }   // CHA
    case L'd': { int r = n ? n : 1; cur_row = r - 1; clamp_cursor(); break; }   // VPA

    case L'J':   // ED (erase in display)
        if (n == 0) {
            erase_cells(cur_row, cur_col, cols);
            for (int r = cur_row + 1; r < rows; ++r) erase_cells(r, 0, cols);
        } else if (n == 1) {
            for (int r = 0; r < cur_row; ++r) erase_cells(r, 0, cols);
            erase_cells(cur_row, 0, cur_col + 1);
        } else {   // 2, 3: whole screen
            for (int r = 0; r < rows; ++r) erase_cells(r, 0, cols);
        }
        break;

    case L'K':   // EL (erase in line)
        if (n == 0) erase_cells(cur_row, cur_col, cols);
        else if (n == 1) erase_cells(cur_row, 0, cur_col + 1);
        else erase_cells(cur_row, 0, cols);
        break;

    case L'X': {   // ECH (blank n cells from the cursor, no shift)
        int d = n ? n : 1;
        erase_cells(cur_row, cur_col, cur_col + d);
        break;
    }
    case L'@': insert_chars(n ? n : 1); break;   // ICH
    case L'P': delete_chars(n ? n : 1); break;   // DCH
    case L'L': insert_lines(n ? n : 1); break;   // IL
    case L'M': delete_lines(n ? n : 1); break;   // DL
    case L'S': scroll_up_region(n ? n : 1); break;     // SU
    case L'T': scroll_down_region(n ? n : 1); break;   // SD

    case L'r': {   // DECSTBM (set scroll region). The private form (?r = DEC mode restore) is ignored.
        if (vt_priv) break;
        int t = (vt_nparam >= 1 && vt_params[0]) ? vt_params[0] : 1;
        int b = (vt_nparam >= 2 && vt_params[1]) ? vt_params[1] : rows;
        t--; b--;   // 1-based -> 0-based
        if (t < 0) t = 0;
        if (b > rows - 1) b = rows - 1;
        if (t < b) {
            scroll_top = t;
            scroll_bot = b;
            // After DECSTBM the cursor goes home.
            cur_row = 0;
            cur_col = 0;
        }
        break;
    }

    case L'q':   // DECSCUSR (CSI Ps SP q): set cursor style. The SP intermediate is required (vt_space);
        // a bare CSI q (e.g. DECLL) is a different command and is ignored here. VT style numbers map
        // 1:1 to set_cursor types (0 = default = blinking block).
        if (vt_space) {
            int s = n ? n : 1;       // 0/absent -> 1 (blinking block)
            if (s >= 1 && s <= 6)
                set_cursor(s);
        }
        break;

    case L'm': {   // SGR (color/attributes)
        if (vt_nparam == 0) {
            // No params = reset (SGR 0).
            cur_fg = default_fg; cur_bg = default_bg;
            cur_bold = cur_italic = cur_underline = cur_strike = cur_blink = cur_reverse = cur_double = false;
            break;
        }
        for (int i = 0; i < vt_nparam; ++i) {
            int p = vt_params[i];
            if (p == 0)  { cur_fg = default_fg; cur_bg = default_bg;
                           cur_bold = cur_italic = cur_underline = cur_strike = cur_blink = cur_reverse = cur_double = false; }
            else if (p == 1)  cur_bold = true;
            else if (p == 3)  cur_italic = true;
            else if (p == 4)  cur_underline = true;
            else if (p == 5 || p == 6) cur_blink = true;
            else if (p == 7)  cur_reverse = true;
            else if (p == 9)  cur_strike = true;
            else if (p == 22) cur_bold = false;
            else if (p == 23) cur_italic = false;
            else if (p == 24) cur_underline = false;
            else if (p == 25) cur_blink = false;
            else if (p == 27) cur_reverse = false;
            else if (p == 29) cur_strike = false;
            else if (p == 8)  cur_double = true;    // SGR 8 (conceal) repurposed: double-size ON
            else if (p == 28) cur_double = false;   // SGR 28 (reveal) repurposed: double-size OFF
            else if (p >= 30 && p <= 37) cur_fg = Xterm256ToRgb(p - 30, ansi_colors);
            else if (p >= 40 && p <= 47) cur_bg = Xterm256ToRgb(p - 40, ansi_colors);
            else if (p >= 90 && p <= 97) cur_fg = Xterm256ToRgb(8 + (p - 90), ansi_colors);
            else if (p >= 100 && p <= 107) cur_bg = Xterm256ToRgb(8 + (p - 100), ansi_colors);
            else if (p == 39) cur_fg = default_fg;
            else if (p == 49) cur_bg = default_bg;
            else if (p == 38 || p == 48) {
                // Extended color: 38;5;n (256) or 38;2;r;g;b (truecolor). 48 = background.
                COLORREF col = (p == 38) ? cur_fg : cur_bg;
                if (i + 1 < vt_nparam && vt_params[i + 1] == 5) {
                    if (i + 2 < vt_nparam) col = Xterm256ToRgb(vt_params[i + 2], ansi_colors);
                    i += 2;
                }
                else if (i + 1 < vt_nparam && vt_params[i + 1] == 2) {
                    if (i + 4 < vt_nparam) col = RGB(vt_params[i + 2], vt_params[i + 3], vt_params[i + 4]);
                    i += 4;
                }
                if (p == 38) cur_fg = col; else cur_bg = col;
            }
        }
        break;
    }

    case L'h':
    case L'l': {   // set (h) / reset (l) mode
        bool set = (fin == L'h');
        if (vt_priv) {
            if (n == 25) {
                cursor_visible = set;   // DECTCEM: cursor shown
            }
            else if (n == 1) {
                app_cursor_keys = set;  // DECCKM: app cursor keys (arrows as ESC O x)
            }
            else if (n == 2004) {
                bracketed_paste = set;  // bracketed paste
            }
            else if (n == 1049 || n == 1047 || n == 47) {
                // Alt screen (vim/htop etc.). (?1049 also bundles cursor save/restore; we handle the
                // cursor in enter/leave_alt_screen.)
                if (set) enter_alt_screen();
                else leave_alt_screen();
            }
            else if (n == 1048) {
                // Cursor save (h) / restore (l), same as DECSC/DECRC.
                if (set) { saved_cur = { cur_row, cur_col }; }
                else { cur_row = saved_cur.row; cur_col = saved_cur.col; clamp_cursor(); }
            }
            // Other private modes (?7 autowrap is always on, etc.) ignored.
        }
        break;
    }
    case L's': saved_cur = { cur_row, cur_col }; break;                              // save cursor
    case L'u': cur_row = saved_cur.row; cur_col = saved_cur.col; clamp_cursor(); break; // restore cursor

    case L'n':   // DSR (device status report): answer the child's query via the input sink.
        if (!vt_priv && input_sink != nullptr) {
            if (n == 6) {
                // CPR: report cursor position as ESC[{row};{col}R (1-based, screen-relative).
                char buf[32];
                int len = wsprintfA(buf, "\x1b[%d;%dR", cur_row + 1, cur_col + 1);
                send_input_bytes(buf, len);
            }
            else if (n == 5) {
                // Terminal status OK.
                send_input_bytes("\x1b[0n", 4);
            }
        }
        break;

    case L'c':   // DA (device attributes): answer Primary DA as VT102-compatible.
        if (input_sink != nullptr && (n == 0)) {
            // ESC[?6c = VT102. Lets the child identify terminal capabilities.
            send_input_bytes("\x1b[?6c", 5);
        }
        break;

    default:
        break;   // other unsupported CSI ignored
    }
}

void ConBox::update_scrollbar()
{
    // No native scrollbar (the overlay bar is what the user sees). This now only clamps/pins view_top;
    // the overlay's visibility is driven by sbar_show()/SBAR_TIMER, its geometry by sbar_geometry().
    if (!::IsWindow(m_hWnd))
        return;

    // On the alt screen, scrollback is frozen/hidden: pin the view to the screen start.
    if (alt_active) {
        view_top = (int)scrollback.size();
        return;
    }

    int total = (int)scrollback.size() + rows;
    if (total <= rows) {
        // No scrollback (screen only): pin to top.
        view_top = 0;
        return;
    }

    // Content overflows: keep view_top within [0, scrollback size].
    int maxtop = (int)scrollback.size();
    if (view_top < 0) view_top = 0;
    if (view_top > maxtop) view_top = maxtop;
}

BOOL ConBox::OnEraseBkgnd(CDC* dc)
{
    // Background is erased in OnPaint to avoid flicker; return TRUE to block the default erase.
    return TRUE;
}

void ConBox::OnSize(UINT type, int cx, int cy)
{
    CWnd::OnSize(type, cx, cy);

    // DPI change detection. The host resizes us to a new-DPI rect on a monitor change; the window's
    // own DPI is now authoritative. If it differs from box_dpi this is a DPI change (not a user
    // resize): rebuild fonts and PRESERVE the logical grid (no update_metrics -> no child PTY resize
    // -> the shell does not re-emit, so content is not duplicated/lost). Order-independent: it does
    // not depend on WM_DPICHANGED_*PARENT timing. A genuine user resize has cur == box_dpi and falls
    // through to the normal grid recompute.
    int cur = (int)::GetDpiForWindow(m_hWnd);
    if (cur > 0 && cur != box_dpi) {
        relayout_for_dpi();
        snap_to_grid();   // grow (never shrink) so the preserved grid is not clipped at the new DPI
        return;
    }

    update_metrics();

    int maxtop = (int)scrollback.size();
    if (maxtop < 0) maxtop = 0;
    if (view_top > maxtop) view_top = maxtop;

    update_scrollbar();
    Invalidate();
}

UINT ConBox::OnGetDlgCode()
{
    // Take arrows/Tab/Enter/chars directly so a parent dialog does not intercept them.
    return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB;
}

// ===== Overlay scrollbar (auto-hide/fade) =====

// Compute the gutter (track) and thumb rects for the current view. Returns false when there is nothing
// to scroll (no scrollback, or the alt screen owns the surface), in which case the bar is not drawn.
bool ConBox::sbar_geometry(CRect& track, CRect& thumb) const
{
    if (alt_active)
        return false;
    int total = (int)scrollback.size() + rows;
    int maxtop = total - rows;            // == scrollback size
    if (maxtop <= 0)
        return false;

    CRect rc;
    GetClientRect(&rc);
    if (rc.Height() <= 0 || rc.Width() <= 0)
        return false;

    // Gutter is a thin strip at the right edge (mostly over the right margin, so it barely covers text).
    // Track runs between the two arrow buttons (each SBAR_BTN_H px tall at the gutter top/bottom).
    track.SetRect(rc.right - sbar_px(SBAR_W), rc.top + sbar_px(SBAR_BTN_H), rc.right, rc.bottom - sbar_px(SBAR_BTN_H));

    int track_h = track.Height();
    if (track_h <= 0)
        return false;
    int thumb_h = (int)((double)track_h * rows / total);
    int min_thumb = sbar_px(SBAR_MIN_THUMB);
    if (thumb_h < min_thumb) thumb_h = min_thumb;
    if (thumb_h > track_h)        thumb_h = track_h;

    int thumb_y = track.top + (int)((double)(track_h - thumb_h) * view_top / maxtop);
    if (thumb_y < track.top)               thumb_y = track.top;
    if (thumb_y > track.bottom - thumb_h)  thumb_y = track.bottom - thumb_h;

    // Thumb is a slim bar near the edge, inset by SBAR_INSET (does not touch the very right edge).
    int thumb_w = sbar_px(SBAR_THUMB_W);
    int tx = track.right - sbar_px(SBAR_INSET) - thumb_w;
    thumb.SetRect(tx, thumb_y, tx + thumb_w, thumb_y + thumb_h);
    return true;
}

// Make the overlay fully opaque and (re)start the fade timer. Called on user scroll and gutter hover.
void ConBox::sbar_show()
{
    if (!::IsWindow(m_hWnd))
        return;
    CRect track, thumb;
    if (!sbar_geometry(track, thumb))
        return;   // nothing to scroll -> nothing to show
    sbar_alpha = 255;
    sbar_hold_until = ::GetTickCount() + SBAR_HOLD_MS;
    SetTimer(SBAR_TIMER, SBAR_FADE_TICK_MS, NULL);
    CRect full;
    GetClientRect(&full);
    CRect gutter(full.right - sbar_px(SBAR_W), full.top, full.right, full.bottom);
    InvalidateRect(&gutter, FALSE);
}

// Render an antialiased shape into rc via 4x4 supersampling: inside(x,y) tests a point in LOCAL
// pixel coordinates (0,0 = rc's top-left, sub-pixel positions allowed). Used for the rounded
// thumb and arrow triangles so their curved/diagonal edges are smooth, not a hard pixel staircase.
// baseAlpha = per-pixel opacity when fully shown; fadeAlpha = the current fade level (AlphaBlend's
// SourceConstantAlpha). Same technique as TableBox.cpp's DrawAA -- keep both in sync.
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
    bi.bmiHeader.biHeight = -h;   // top-down
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

// Antialiased filled triangle (arrow button), tip up or down, padded 3px from rc's edges.
static void DrawTriangle(CDC& dc, const CRect& rc, COLORREF color, int baseAlpha, BYTE fadeAlpha, bool up)
{
    int w = rc.Width(), h = rc.Height();
    const double pad = 3.0;
    double v0x = w / 2.0,     v0y = up ? pad        : h - 1 - pad;   // tip
    double v1x = pad,         v1y = up ? h - 1 - pad : pad;          // base left
    double v2x = w - 1 - pad, v2y = v1y;                             // base right
    DrawAA(dc, rc, color, baseAlpha, fadeAlpha, [=](double x, double y) {
        double d0 = (v1x-v0x)*(y-v0y) - (v1y-v0y)*(x-v0x);
        double d1 = (v2x-v1x)*(y-v1y) - (v2y-v1y)*(x-v1x);
        double d2 = (v0x-v2x)*(y-v2y) - (v0y-v2y)*(x-v2x);
        return (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
    });
}

// Flat translucent fill (the gutter strip background) -- a 1x1 source stretched over rc via
// AlphaBlend. No AA needed: the gutter is always an axis-aligned rect, so its edges are already
// pixel-exact. Same technique as TableBox.cpp's FillTranslucent -- keep both in sync.
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

// Composite the overlay scrollbar into the back buffer (called last in OnPaint, on top of glyphs/cursor).
// A full-width gutter fill (always drawn while shown, not just while active) makes the bar read as a
// SBAR_W-wide strip rather than just the thin thumb; the rounded thumb (brighter on hover/drag) and the
// two arrow buttons (scroll 1 line on click) are drawn on top, all antialiased. All fade via sbar_alpha.
void ConBox::draw_overlay_scrollbar(CDC& dc)
{
    if (sbar_alpha <= 0)
        return;
    CRect track, thumb;
    if (!sbar_geometry(track, thumb))
        return;

    bool active = (sbar_hover || sbar_dragging);

    // Pick thumb/gutter shades that contrast with the current background so the bar stays visible on
    // both dark and light backgrounds (the fixed dark grays were invisible on white). Perceived
    // luminance of default_bg decides: bright bg -> dark thumb + light gutter, dark bg -> the original
    // light thumb + dark gutter.
    int bg_lum = (GetRValue(default_bg) * 299 + GetGValue(default_bg) * 587 + GetBValue(default_bg) * 114) / 1000;
    bool light_bg = (bg_lum > 128);
    COLORREF thumb_color  = light_bg ? RGB(96, 96, 96)    : SBAR_THUMB_COLOR;
    COLORREF gutter_color = light_bg ? RGB(176, 176, 176) : RGB(80, 80, 80);

    CRect rc_full;
    GetClientRect(&rc_full);
    CRect gutter(rc_full.right - sbar_px(SBAR_W), rc_full.top, rc_full.right, rc_full.bottom);
    FillTranslucent(dc, gutter, gutter_color, (BYTE)(SBAR_GUTTER_OP * sbar_alpha / 255));

    // The thumb: translucent (idle) or brighter (hover/drag), rounded, fading by sbar_alpha.
    int opcap = active ? SBAR_OP_HOVER : SBAR_OP_IDLE;
    DrawRoundedThumb(dc, thumb, thumb_color, opcap, sbar_px(SBAR_RADIUS), (BYTE)sbar_alpha);

    // Arrow buttons at gutter top/bottom (same opacity as thumb).
    int sw = sbar_px(SBAR_W), bh = sbar_px(SBAR_BTN_H);
    CRect btn_up(rc_full.right - sw, rc_full.top, rc_full.right, rc_full.top + bh);
    CRect btn_dn(rc_full.right - sw, rc_full.bottom - bh, rc_full.right, rc_full.bottom);
    DrawTriangle(dc, btn_up, thumb_color, opcap, (BYTE)sbar_alpha, true);
    DrawTriangle(dc, btn_dn, thumb_color, opcap, (BYTE)sbar_alpha, false);
}

BOOL ConBox::OnMouseWheel(UINT flags, short zDelta, CPoint pt)
{
    // Scrollback view is frozen on the alt screen (the child owns the screen); ignore the wheel.
    if (alt_active)
        return TRUE;

    // Three lines per wheel notch (120). Wheel up goes to earlier content.
    int maxtop = (int)scrollback.size();
    if (maxtop < 0) maxtop = 0;

    int nt = view_top - (zDelta / 120) * 3;
    if (nt < 0) nt = 0;
    if (nt > maxtop) nt = maxtop;
    if (nt != view_top) {
        view_top = nt;
        update_scrollbar();
        sbar_show();   // user scroll -> flash the overlay bar
        Invalidate();
    }
    return TRUE;
}

// ===== Mouse drag selection / clipboard =====
// hit_test: client pixel coords -> unified (scrollback+screen) row index + cell column. Coords outside
// the margins clamp to the edge cell.
void ConBox::hit_test(CPoint pt, int& abs_row, int& col) const
{
    int total = (int)scrollback.size() + rows;

    int r = (pt.y - origin_y) / cell_h;  // screen row (relative to view_top)
    if (r < 0) r = 0;
    if (r >= rows) r = rows - 1;
    abs_row = view_top + r;
    if (abs_row < 0) abs_row = 0;
    if (abs_row >= total) abs_row = total - 1;

    int c = (pt.x - origin_x) / cell_w;
    if (c < 0) c = 0;
    if (c >= cols) c = cols - 1;

    // If a wide glyph's trail cell (ch=0) was hit, snap to its lead cell.
    int idx = abs_row;
    if (idx >= 0 && idx < total) {
        const Row& line = line_at(idx);
        if (c > 0 && c < (int)line.size() && line[c].ch == 0)
            --c;
    }
    col = c;
}

// copy_selection: extract the selected text as Unicode to the clipboard. Sort the row range, collect
// each line's selected cells, trim trailing spaces, join lines with CR+LF. Trail cells (ch=0) skipped.
void ConBox::copy_selection()
{
    if (!sel_active) return;

    int total = (int)scrollback.size() + rows;
    std::wstring text;

    if (sel_block) {
        // Rectangular block: sort rows and cols independently.
        int rmin = sel_anchor_row, rmax = sel_end_row;
        int cmin = sel_anchor_col, cmax = sel_end_col;
        if (rmin > rmax) std::swap(rmin, rmax);
        if (cmin > cmax) std::swap(cmin, cmax);

        for (int r = rmin; r <= rmax; ++r) {
            if (r < 0 || r >= total) continue;
            const Row& line = line_at(r);
            int ncell = (int)line.size();

            std::wstring row_text;
            for (int i = cmin; i <= cmax; ++i) {
                wchar_t ch = (i < ncell) ? line[i].ch : L' ';
                if (ch == 0) continue;   // trail cell of a wide glyph
                row_text += ch;
            }
            while (!row_text.empty() && row_text.back() == L' ')
                row_text.pop_back();

            if (r > rmin) text += L"\r\n";
            text += row_text;
        }
    } else {
        // Linear selection: order anchor..end into (r0,c0)..(r1,c1).
        int r0 = sel_anchor_row, c0 = sel_anchor_col;
        int r1 = sel_end_row, c1 = sel_end_col;
        if (r0 > r1 || (r0 == r1 && c0 > c1)) {
            std::swap(r0, r1); std::swap(c0, c1);
        }

        for (int r = r0; r <= r1; ++r) {
            if (r < 0 || r >= total) continue;
            const Row& line = line_at(r);
            int ncell = (int)line.size();

            int cs = (r == r0) ? c0 : 0;
            int ce = (r == r1) ? c1 : ncell - 1;
            if (cs < 0) cs = 0;
            if (ce >= ncell) ce = ncell - 1;

            std::wstring row_text;
            for (int i = cs; i <= ce; ++i) {
                wchar_t ch = line[i].ch;
                if (ch == 0) continue;   // trail cell of a wide glyph
                row_text += ch;
            }
            while (!row_text.empty() && row_text.back() == L' ')
                row_text.pop_back();

            if (r > r0) text += L"\r\n";
            text += row_text;
        }
    }

    if (text.empty()) return;

    if (!::OpenClipboard(m_hWnd)) return;
    ::EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hg = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hg) {
        wchar_t* p = (wchar_t*)::GlobalLock(hg);
        if (p) {
            memcpy(p, text.c_str(), bytes);
            ::GlobalUnlock(hg);
            ::SetClipboardData(CF_UNICODETEXT, hg);
        }
    }
    ::CloseClipboard();
}

void ConBox::clear_selection()
{
    if (sel_active) {
        sel_active = false;
        Invalidate();
    }
}

void ConBox::OnSetFocus(CWnd*)
{
    has_focus = true;
    cursor_on = true;
    if (cursor_blink_ms > 0 && (cursor_type & 1) == 1) {
        KillTimer(CURSOR_TIMER);
        SetTimer(CURSOR_TIMER, cursor_blink_ms, NULL);
    }
    CRect rc;
    if (get_cursor_rect(rc)) InvalidateRect(&rc, FALSE);
}

void ConBox::OnKillFocus(CWnd*)
{
    has_focus = false;
    CRect rc;
    if (get_cursor_rect(rc)) InvalidateRect(&rc, FALSE);
}

void ConBox::OnLButtonDown(UINT flags, CPoint pt)
{
    SetFocus();   // claim keyboard focus on click (CWnd does not auto-focus on click unlike standard controls)
    // Finalize any IME composition first (a click is also a composition-ending trigger).
    finalize_composition();
    ime_committed = false;   // a click is not an arrow; don't let it enable the arrow correction

    // Overlay scrollbar: a press anywhere in the gutter drives the bar, not text selection.
    // Buttons scroll 1 line; thumb drag; track click pages. Done before sel state so a scroll
    // gesture never starts/clears a selection.
    CRect track, thumb;
    if (sbar_geometry(track, thumb)) {
        CRect full;
        GetClientRect(&full);
        CRect gutter(full.right - sbar_px(SBAR_W), full.top, full.right, full.bottom);
        if (gutter.PtInRect(pt)) {
            int maxtop = (int)scrollback.size();
            int bh = sbar_px(SBAR_BTN_H);
            CRect btn_up(gutter.left, gutter.top, gutter.right, gutter.top + bh);
            CRect btn_dn(gutter.left, gutter.bottom - bh, gutter.right, gutter.bottom);
            if (btn_up.PtInRect(pt)) {
                // Arrow up: scroll one line toward older content.
                int nt = view_top - 1;
                if (nt < 0) nt = 0;
                if (nt != view_top) { view_top = nt; update_scrollbar(); Invalidate(); }
            } else if (btn_dn.PtInRect(pt)) {
                // Arrow down: scroll one line toward newer content.
                int nt = view_top + 1;
                if (nt > maxtop) nt = maxtop;
                if (nt != view_top) { view_top = nt; update_scrollbar(); Invalidate(); }
            } else if (thumb.PtInRect(pt)) {
                // Grab the thumb; keep the grab offset so it does not jump under the cursor.
                sbar_dragging = true;
                sbar_drag_off = pt.y - thumb.top;
                SetCapture();
            } else {
                // Press in the empty track: page toward the click.
                int nt = view_top + ((pt.y < thumb.top) ? -rows : rows);
                if (nt < 0) nt = 0;
                if (nt > maxtop) nt = maxtop;
                if (nt != view_top) { view_top = nt; update_scrollbar(); Invalidate(); }
            }
            sbar_show();
            return;
        }
    }

    sel_active = false;

    // Alt+drag = rectangular (block) selection; plain drag = linear selection.
    sel_block = (::GetKeyState(VK_MENU) & 0x8000) != 0;

    // Start drag: anchor and end at the same cell.
    hit_test(pt, sel_anchor_row, sel_anchor_col);
    sel_end_row = sel_anchor_row;
    sel_end_col = sel_anchor_col;
    selecting = true;

    SetCapture();   // keep tracking outside the window
    Invalidate();
}

void ConBox::OnLButtonDblClk(UINT flags, CPoint pt)
{
    // Double-click: select the "word" under the cursor (a maximal run of non-space cells).
    // Whitespace class (ch==' ' or ch==0) expands over adjacent spaces; word class over non-spaces.
    // The first click of the double-click already ran OnLButtonDown (which may have started a
    // one-cell selection + SetCapture); we supersede it here by clearing selecting/capture first.
    if (selecting) { ReleaseCapture(); selecting = false; }

    // Gutter click: ignore (same gate as OnLButtonDown).
    CRect track, thumb;
    if (sbar_geometry(track, thumb)) {
        CRect full; GetClientRect(&full);
        if (CRect(full.right - sbar_px(SBAR_W), full.top, full.right, full.bottom).PtInRect(pt))
            return;
    }

    int abs_row, col;
    hit_test(pt, abs_row, col);

    int total = (int)scrollback.size() + rows;
    if (abs_row < 0 || abs_row >= total)
        return;
    const Row& row_data = line_at(abs_row);

    // eff_ch: returns the effective char at col c (trail cell -> its lead's char; OOB -> space).
    auto eff_ch = [&](int c) -> wchar_t {
        if (c < 0 || c >= cols) return L' ';
        wchar_t ch = (c < (int)row_data.size()) ? row_data[c].ch : L' ';
        if (ch == 0 && c > 0)  // trail: return lead's char
            ch = ((c-1) < (int)row_data.size()) ? row_data[c-1].ch : L' ';
        return ch;
    };

    // Two classes: whitespace (space/empty) or word (everything else).
    auto is_word = [](wchar_t ch) -> bool { return ch != L' ' && ch != 0; };
    bool cls = is_word(eff_ch(col));

    int c0 = col, c1 = col;
    while (c0 > 0 && is_word(eff_ch(c0 - 1)) == cls) --c0;
    while (c1 < cols - 1 && is_word(eff_ch(c1 + 1)) == cls) ++c1;

    sel_anchor_row = abs_row; sel_anchor_col = c0;
    sel_end_row    = abs_row; sel_end_col    = c1;
    sel_block  = false;
    sel_active = true;
    copy_selection();
    Invalidate();
}

void ConBox::OnLButtonUp(UINT flags, CPoint pt)
{
    // End a thumb drag (if any) before the selection path.
    if (sbar_dragging) {
        sbar_dragging = false;
        ReleaseCapture();
        sbar_show();   // restart the hold/fade now that dragging stopped
        return;
    }

    if (!selecting) return;

    hit_test(pt, sel_end_row, sel_end_col);
    selecting = false;
    ReleaseCapture();

    // Same cell as the anchor = a click, not a selection.
    if (sel_anchor_row == sel_end_row && sel_anchor_col == sel_end_col) {
        sel_active = false;
        Invalidate();
        return;
    }

    // Selection confirmed: auto-copy to the clipboard.
    sel_active = true;
    copy_selection();
    Invalidate();
}

void ConBox::OnMouseMove(UINT flags, CPoint pt)
{
    // Dragging the overlay thumb: map the cursor y to view_top (inverse of sbar_geometry's thumb_y).
    if (sbar_dragging) {
        CRect track, thumb;
        if (sbar_geometry(track, thumb)) {
            int maxtop = (int)scrollback.size();
            int span = track.Height() - thumb.Height();   // travel range of the thumb top
            int ty = pt.y - sbar_drag_off - track.top;
            int nt = (span > 0) ? (int)((double)ty * maxtop / span + 0.5) : 0;
            if (nt < 0) nt = 0;
            if (nt > maxtop) nt = maxtop;
            if (nt != view_top) { view_top = nt; update_scrollbar(); Invalidate(); }
        }
        sbar_show();
        return;
    }

    // Hover anywhere in the gutter (buttons + track; when not selecting): show the bar and arm
    // WM_MOUSELEAVE so it fades on exit.
    if (!selecting) {
        CRect track, thumb;
        bool has = sbar_geometry(track, thumb);
        CRect gutter_rc;
        if (has) {
            CRect full;
            GetClientRect(&full);
            gutter_rc.SetRect(full.right - sbar_px(SBAR_W), full.top, full.right, full.bottom);
        }
        bool in = has && gutter_rc.PtInRect(pt);
        if (in && !sbar_hover) {
            sbar_hover = true;
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = m_hWnd;
            tme.dwHoverTime = 0;
            ::TrackMouseEvent(&tme);
            sbar_show();
        }
        else if (!in && sbar_hover) {
            sbar_hover = false;   // off the gutter (still in the window): let the hold/fade run
            if (has) InvalidateRect(&gutter_rc, FALSE);
        }
    }

    if (!selecting) return;

    hit_test(pt, sel_end_row, sel_end_col);
    sel_active = true;
    Invalidate();
}

void ConBox::OnMouseLeave()
{
    // Left the window: drop gutter hover so the overlay can fade. The fade timer is already running
    // (started by sbar_show on hover); just refresh the hold window so it lingers briefly, then fades.
    if (sbar_hover) {
        sbar_hover = false;
        sbar_hold_until = ::GetTickCount() + SBAR_HOLD_MS;
        if (::IsWindow(m_hWnd))
            SetTimer(SBAR_TIMER, SBAR_FADE_TICK_MS, NULL);
        CRect track, thumb;
        if (sbar_geometry(track, thumb)) {
            CRect full;
            GetClientRect(&full);
            CRect gutter(full.right - sbar_px(SBAR_W), full.top, full.right, full.bottom);
            InvalidateRect(&gutter, FALSE);
        }
    }
}

void ConBox::OnRButtonDown(UINT flags, CPoint pt)
{
    finalize_composition();
    ime_committed = false;   // a click is not an arrow; don't let it enable the arrow correction
    clear_selection();
    // Paste the clipboard to the child stdin.
    paste_clipboard();
}

void ConBox::OnDropFiles(HDROP hdrop)
{
    // Type dropped file paths to the child stdin (wt.exe behavior).
    // Paths with spaces are wrapped in double quotes; multiple files are space-separated.
    if (input_sink == nullptr) {
        DragFinish(hdrop);
        return;
    }
    finalize_composition();
    clear_selection();
    UINT n = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
        UINT len = DragQueryFileW(hdrop, i, nullptr, 0);
        if (len == 0)
            continue;
        std::vector<wchar_t> buf(len + 1);
        DragQueryFileW(hdrop, i, buf.data(), len + 1);
        bool has_space = (wcschr(buf.data(), L' ') != nullptr);
        if (i > 0)
            send_input_bytes(" ", 1);
        if (has_space)
            send_input_bytes("\"", 1);
        send_input_wide(buf.data(), (int)len);
        if (has_space)
            send_input_bytes("\"", 1);
    }
    DragFinish(hdrop);
}

LRESULT ConBox::OnImeStart(WPARAM w, LPARAM l)
{
    // Move the composition/candidate windows to the cursor and set the composing font to Korean. We
    // draw the composing glyph ourselves, so no default composition window (return below).
    HIMC himc = ImmGetContext(m_hWnd);
    if (himc) {
        int row, vx;
        if (cursor_screen_pos(row, vx) && row >= 0 && row < rows) {
            COMPOSITIONFORM cf;
            ZeroMemory(&cf, sizeof(cf));
            cf.dwStyle = CFS_POINT;
            cf.ptCurrentPos.x = origin_x + vx * cell_w;
            cf.ptCurrentPos.y = origin_y + row * cell_h;
            ImmSetCompositionWindow(himc, &cf);

            // Candidate list just below the cursor.
            CANDIDATEFORM caf;
            ZeroMemory(&caf, sizeof(caf));
            caf.dwIndex = 0;
            caf.dwStyle = CFS_CANDIDATEPOS;
            caf.ptCurrentPos.x = origin_x + vx * cell_w;
            caf.ptCurrentPos.y = origin_y + (row + 1) * cell_h;
            ImmSetCandidateWindow(himc, &caf);

            ImmSetCompositionFontW(himc, &kfont_lf);
        }
        ImmReleaseContext(m_hWnd, himc);
    }
    bump_cursor();
    // ConBox draws the composing glyph (OnImeComp stores comp_str, OnPaint draws it in the cursor cell).
    // Above only positions the candidate list; system inline is not used.
    return Default();
}

// (design: [IME])
LRESULT ConBox::OnImeComp(WPARAM w, LPARAM l)
{
    // Committed text (GCS_RESULTSTR) goes to the child as UTF-8; the uncommitted part (GCS_COMPSTR) is
    // stored in comp_str and drawn by ConBox in the cursor cell (OnPaint). System inline display is
    // killed by consuming the message (return 0); this avoids the composing glyph overlapping the first
    // cell when the composition window cannot be pinned to the cursor.
    // (The "completed in the moved cell" ordering issue under mid-composition arrow keys is handled by
    //  force-committing right before the trigger key - see Requirements.md sec 10.)
    HIMC himc = ImmGetContext(m_hWnd);
    if (himc == NULL)
        return Default();

    // Committed part: extract and send to the child (terminal mode), then clear the composing display.
    if (l & GCS_RESULTSTR) {
        LONG bytes = ImmGetCompositionStringW(himc, GCS_RESULTSTR, NULL, 0);
        int n = bytes / (int)sizeof(wchar_t);
        if (n > 0 && input_sink != nullptr) {
            std::wstring r;
            r.resize(n);
            ImmGetCompositionStringW(himc, GCS_RESULTSTR, &r[0], bytes);
            send_input_wide(r.c_str(), n);
            ime_committed = true;   // a glyph was just committed; OnKeyDown uses this to fix arrow moves
        }
        comp_str.clear();
    }

    // Uncommitted part: store in comp_str (clear if length 0). We only hold it; we draw it ourselves.
    if (l & GCS_COMPSTR) {
        LONG bytes = ImmGetCompositionStringW(himc, GCS_COMPSTR, NULL, 0);
        int n = bytes / (int)sizeof(wchar_t);
        if (n > 0) {
            comp_str.resize(n);
            ImmGetCompositionStringW(himc, GCS_COMPSTR, &comp_str[0], bytes);
        }
        else {
            comp_str.clear();
        }
    }

    ImmReleaseContext(m_hWnd, himc);

    // The composing/committed display changed: invalidate from the cursor cell to the line's right edge
    // (OnPaint shifts that strip right to mimic insertion, so invalidate the strip, not just 2 cells).
    CRect rc;
    if (get_cursor_rect(rc)) {
        rc.right = origin_x + cols * cell_w + cell_w;
        InvalidateRect(rc);
    }
    bump_cursor();
    // Consume the message to suppress system inline display and the committed part's WM_CHAR/WM_IME_CHAR dup.
    return 0;
}

LRESULT ConBox::OnImeEnd(WPARAM w, LPARAM l)
{
    // Composition ended: clear our composing display (comp_str) and repaint. The committed part was
    // already sent in OnImeComp (GCS_RESULTSTR) and is drawn by the child's echo.
    comp_str.clear();
    // Invalidate to the line end to undo the rightward shift drawn during composition.
    CRect rc;
    if (get_cursor_rect(rc)) {
        rc.right = origin_x + cols * cell_w + cell_w;
        InvalidateRect(rc);
    }
    bump_cursor();
    return Default();
}

LRESULT ConBox::OnImeNotify(WPARAM w, LPARAM l)
{
    // A conversion-mode change (e.g. the Korean/English key) notifies here. Repaint the cursor area to
    // reflect the matching width (long/short) at once. Other notifications (candidate window etc.) go
    // to default handling.
    LRESULT res = Default();
    if (w == IMN_SETCONVERSIONMODE) {
        CRect rc;
        if (get_cursor_rect(rc)) {
            // Width toggles between 1 and 2 cells, so refresh the 2-cell area.
            rc.right = rc.left + cell_w * 2;
            InvalidateRect(rc);
        }
    }
    return res;
}

void ConBox::set_input_sink(void (*sink)(const char* bytes, int len, void* user), void* user)
{
    input_sink = sink;
    input_sink_user = user;
}

void ConBox::set_resize_sink(void (*sink)(int rows, int cols, void* user), void* user)
{
    resize_sink = sink;
    resize_sink_user = user;
}

void ConBox::send_input_bytes(const char* bytes, int len)
{
    if (input_sink != nullptr && bytes != nullptr && len > 0)
        input_sink(bytes, len, input_sink_user);
}

void ConBox::send_input_wide(const wchar_t* ws, int n)
{
    if (input_sink == nullptr || ws == nullptr || n <= 0)
        return;
    int bytes = ::WideCharToMultiByte(CP_UTF8, 0, ws, n, NULL, 0, NULL, NULL);
    if (bytes <= 0)
        return;
    std::vector<char> buf(bytes);
    ::WideCharToMultiByte(CP_UTF8, 0, ws, n, buf.data(), bytes, NULL, NULL);
    send_input_bytes(buf.data(), bytes);
}

// Encode non-char keys (arrows/edit keys) as VT sequences to the child. Printable chars / Enter / Tab /
// Backspace / Esc / Ctrl+letter go through the WM_CHAR path (OnChar), not here (avoids duplication).
// Returns true if it sent something.
bool ConBox::terminal_keydown(UINT vk, bool ctrl, bool shift)
{
    // Ctrl+C: with a selection, copy + clear it; otherwise send interrupt (0x03).
    if (ctrl && (vk == 'C')) {
        if (sel_active) {
            copy_selection();
            clear_selection();
        } else {
            send_input_bytes("\x03", 1);
        }
        return true;
    }
    // Ctrl+V: paste the clipboard to the child stdin (wrapped if bracketed paste).
    if (ctrl && (vk == 'V')) {
        paste_clipboard();
        return true;
    }
    // Shift+Tab: backtab.
    if (vk == VK_TAB && shift) {
        send_input_bytes("\x1b[Z", 3);
        return true;
    }

    // Modifier code (xterm convention): 1 + Shift(1) + Alt(2) + Ctrl(4). Here only Shift/Ctrl. With a
    // modifier, use ESC[1;{mod}{final}.
    int mod = (shift ? 1 : 0) + (ctrl ? 4 : 0);

    // Arrows/Home/End by final byte (A/B/C/D/H/F). With no modifier the prefix is ESC[ or ESC O per
    // app-cursor-keys (DECCKM). With a modifier it is ESC[1;{mod}.
    char fin = 0;
    switch (vk) {
    case VK_UP:    fin = 'A'; break;
    case VK_DOWN:  fin = 'B'; break;
    case VK_RIGHT: fin = 'C'; break;
    case VK_LEFT:  fin = 'D'; break;
    case VK_HOME:  fin = 'H'; break;
    case VK_END:   fin = 'F'; break;
    default: break;
    }
    if (fin != 0) {
        char buf[16];
        int len;
        if (mod != 0)
            len = wsprintfA(buf, "\x1b[1;%d%c", mod + 1, fin);
        else if (app_cursor_keys)
            len = wsprintfA(buf, "\x1bO%c", fin);
        else
            len = wsprintfA(buf, "\x1b[%c", fin);
        send_input_bytes(buf, len);
        return true;
    }

    // Other edit/function keys -> fixed VT sequences. (Modifier combos are rarely used here, so simplified.)
    const char* seq = nullptr;
    switch (vk) {
    case VK_DELETE: seq = "\x1b[3~"; break;
    case VK_INSERT: seq = "\x1b[2~"; break;
    case VK_PRIOR:  seq = "\x1b[5~"; break;   // PageUp
    case VK_NEXT:   seq = "\x1b[6~"; break;   // PageDown
    case VK_F1:  seq = "\x1bOP";   break;
    case VK_F2:  seq = "\x1bOQ";   break;
    case VK_F3:  seq = "\x1bOR";   break;
    case VK_F4:  seq = "\x1bOS";   break;
    case VK_F5:  seq = "\x1b[15~"; break;
    case VK_F6:  seq = "\x1b[17~"; break;
    case VK_F7:  seq = "\x1b[18~"; break;
    case VK_F8:  seq = "\x1b[19~"; break;
    case VK_F9:  seq = "\x1b[20~"; break;
    case VK_F10: seq = "\x1b[21~"; break;
    case VK_F11: seq = "\x1b[23~"; break;
    case VK_F12: seq = "\x1b[24~"; break;
    default: break;
    }
    if (seq != nullptr) {
        send_input_bytes(seq, (int)strlen(seq));
        return true;
    }
    return false;
}

void ConBox::paste_clipboard()
{
    // Send clipboard Unicode text to the child stdin. In bracketed paste mode wrap it in
    // ESC[200~ / ESC[201~ so the child handles the paste as one chunk.
    if (input_sink == nullptr)
        return;
    if (!::OpenClipboard(m_hWnd))
        return;
    HANDLE h = ::GetClipboardData(CF_UNICODETEXT);
    if (h != NULL) {
        const wchar_t* p = (const wchar_t*)::GlobalLock(h);
        if (p != NULL) {
            if (bracketed_paste)
                send_input_bytes("\x1b[200~", 6);
            send_input_wide(p, (int)wcslen(p));
            if (bracketed_paste)
                send_input_bytes("\x1b[201~", 6);
            ::GlobalUnlock(h);
        }
    }
    ::CloseClipboard();
}

bool ConBox::finalize_composition()
{
    // Force-commit an in-progress Korean composition. CPS_COMPLETE dispatches WM_IME_COMPOSITION
    // (GCS_RESULTSTR) synchronously, so before this returns OnImeComp has already sent the completed
    // glyph's UTF-8 to the child. Calling it right before sending a trigger key thus guarantees the
    // "complete in place, then trigger" order (Requirements.md sec 10).
    //
    // Some Korean IMEs commit on their own first when an arrow etc. arrives (WM_IME_COMPOSITION before
    // WM_KEYDOWN); then no composition remains here and this is a safe no-op. Either way the completed
    // part is sent before the trigger. A duplicate commit message is filtered by OnImeComp's empty
    // RESULTSTR (n==0) guard, so nothing is double-sent.
    HIMC himc = ImmGetContext(m_hWnd);
    if (himc == NULL)
        return false;

    bool had = false;
    LONG bytes = ImmGetCompositionStringW(himc, GCS_COMPSTR, NULL, 0);
    if (bytes > 0) {
        ImmNotifyIME(himc, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);   // synchronously processes WM_IME_COMPOSITION
        had = true;
    }
    ImmReleaseContext(m_hWnd, himc);
    return had;
}

void ConBox::OnChar(UINT ch, UINT rep, UINT flags)
{
    // Terminal (raw) mode: no local edit/echo; send chars/control bytes to the child. (WM_CHAR is the
    // post-layout/IME result, so it suits printable/control byte sending.)
    if (input_sink != nullptr) {
        ime_committed = false;   // a real keystroke breaks the "commit then arrow" sequence
        for (UINT k = 0; k < rep; ++k) {
            if (ch == 0x08) {
                // Backspace -> DEL (0x7F) per readline/Unix convention.
                send_input_bytes("\x7f", 1);
            }
            else if (ch == L'\r' || ch == L'\n') {
                // Enter -> CR (0x0D).
                send_input_bytes("\r", 1);
            }
            else if (ch == 0x03 || ch == 0x16) {
                // Ctrl+C (0x03) and Ctrl+V (0x16) are special-cased in terminal_keydown (copy/interrupt
                // and clipboard paste). WM_KEYDOWN and WM_CHAR both fire, so dropping the WM_CHAR byte
                // here avoids sending it twice -- otherwise the child re-pastes (Ctrl+V shows twice as
                // PSReadLine treats 0x16 as its own paste) / double-interrupts.
            }
            else if (ch < 0x80) {
                // Printable ASCII + other control bytes (Tab=0x09, Esc=0x1B, Ctrl+letter...) as one byte.
                char b = (char)ch;
                send_input_bytes(&b, 1);
            }
            else {
                // Non-ASCII (Korean syllables, IME-committed text) -> UTF-8.
                wchar_t wc = (wchar_t)ch;
                send_input_wide(&wc, 1);
            }
        }
        bump_cursor();
        return;
    }

    // Read-only viewer (no input sink): no local edit/echo.
}

void ConBox::OnKeyDown(UINT vk, UINT rep, UINT flags)
{
    // Terminal (raw) mode: arrows/edit keys as VT sequences; printable / Enter / Backspace / Tab / Esc
    // go through WM_CHAR (OnChar). No local line editing.
    if (input_sink != nullptr) {
        bool tctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool tshift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;

        // For a composition-ending trigger key, force-commit the IME first so the completed glyph is
        // sent before the trigger (Requirements.md sec 10). The MS Korean IME usually pre-commits on the
        // arrow's WM_IME_COMPOSITION (OnImeComp sets ime_committed) *before* this WM_KEYDOWN, so
        // finalize_composition() here is then a no-op; capture ime_committed (set by either path) to know
        // a commit just happened, then consume it.
        bool committed = ime_committed;
        switch (vk) {
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_HOME: case VK_END: case VK_DELETE: case VK_INSERT:
        case VK_PRIOR: case VK_NEXT: case VK_RETURN: case VK_TAB: case VK_ESCAPE:
            if (finalize_composition())
                committed = true;
            break;
        default:
            break;
        }
        ime_committed = false;   // consumed: only a commit immediately before this key counts

        // IME compose-finalize horizontal-arrow fix: committing the glyph inserts it and advances the
        // child cursor one glyph to the RIGHT, so a plain Left/Right then lands one glyph off from the
        // glyph's visible (composing) cell. Make the arrow act relative to that cell:
        //   Right: the commit already moved right one glyph (== the intended right move) -> swallow it.
        //   Left : send Left once more so the extra Left cancels the commit's advance and the normal
        //          Left below then moves one glyph left (net -1).
        // Only for unmodified arrows; Ctrl/Shift+arrow (word move / selection) keep their raw behavior.
        bool plain = !tctrl && !tshift;
        if (committed && plain && vk == VK_RIGHT) {
            bump_cursor();
            return;
        }
        if (committed && plain && vk == VK_LEFT)
            terminal_keydown(VK_LEFT, false, false);    // extra Left offsets the commit's right advance

        if (terminal_keydown(vk, tctrl, tshift))
            bump_cursor();
        return;
    }

    // Read-only viewer (no input sink): no local line editing, only scroll shortcuts (PageUp/Down,
    // Ctrl+Home/End). On the alt screen the scrollback view is frozen, so leave them to default too.
    if (alt_active) {
        CWnd::OnKeyDown(vk, rep, flags);
        return;
    }
    bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    int maxtop = (int)scrollback.size();
    if (maxtop < 0) maxtop = 0;

    int nt = view_top;
    switch (vk) {
    case VK_PRIOR: nt -= rows; break;   // PageUp
    case VK_NEXT:  nt += rows; break;   // PageDown
    case VK_HOME:  if (!ctrl) { CWnd::OnKeyDown(vk, rep, flags); return; } nt = 0; break;
    case VK_END:   if (!ctrl) { CWnd::OnKeyDown(vk, rep, flags); return; } nt = maxtop; break;
    default:
        CWnd::OnKeyDown(vk, rep, flags);
        return;
    }

    if (nt < 0) nt = 0;
    if (nt > maxtop) nt = maxtop;
    if (nt != view_top) {
        view_top = nt;
        update_scrollbar();
        sbar_show();   // keyboard scroll -> flash the overlay bar
        Invalidate();
    }
}

// Lazily prepare the double-buffer memory DC/bitmap. The DC is created once and reused; the bitmap is
// rebuilt only when the client size changes (avoids a CreateCompatibleDC/Bitmap per frequent repaint
// such as cursor blink).
void ConBox::ensure_back_buffer(CDC* ref, int w, int h)
{
    if (back_dc.GetSafeHdc() == nullptr)
        back_dc.CreateCompatibleDC(ref);

    // Rebuild the bitmap if missing or the size changed.
    if (back_bmp.GetSafeHandle() == nullptr || w != back_w || h != back_h) {
        // Detach the old bitmap (restore the original) before discarding it.
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

// (design: [PAINT]/[IME])
void ConBox::OnPaint()
{
    CPaintDC paintDC(this);

    CRect rc;
    GetClientRect(&rc);

    // Double buffering: drawing straight to the screen would briefly show the "fill background -> draw
    // glyphs" intermediate, flickering on fast full repaints. So draw a whole frame into a memory DC
    // and BitBlt it once. The DC/bitmap are cached as members and rebuilt only on size change.
    ensure_back_buffer(&paintDC, rc.Width(), rc.Height());

    // All drawing below targets the memory DC.
    CDC& dc = back_dc;

    dc.FillSolidRect(rc, cur_bg);

    // Glyphs: fill the cell background, then draw transparent. TA_BASELINE aligns Korean and English baselines.
    dc.SetBkMode(TRANSPARENT);
    UINT old_align = dc.SetTextAlign(TA_LEFT | TA_BASELINE);
    CFont* old_font = dc.SelectObject(&efont);

    // GDI call cache: only call SelectObject/SetTextColor when the value actually changes.
    CFont*   last_font = nullptr;
    COLORREF last_fg   = CLR_INVALID;

    // Draw visible lines top to bottom (unified index view_top+row).
    int total = (int)scrollback.size() + rows;

    // CELL_DOUBLE glyphs (2x size) bleed upward (into the row above) and rightward (into cells to the
    // right). Rendering columns right-to-left means cells to the right are already laid down before a
    // double glyph draws over them, so the bleed is never erased. Upward bleed is safe because rows
    // are rendered top-to-bottom (the row above is already complete). No second pass needed.

    // Pre-sort the selection range if active. Block mode sorts rows and cols independently;
    // linear mode sorts so (sr0,sc0) precedes (sr1,sc1) in reading order.
    int sr0 = 0, sc0 = 0, sr1 = -1, sc1 = -1;   // sr1 < sr0 means no selection
    if (sel_active) {
        sr0 = sel_anchor_row; sc0 = sel_anchor_col;
        sr1 = sel_end_row;    sc1 = sel_end_col;
        if (sel_block) {
            if (sr0 > sr1) std::swap(sr0, sr1);
            if (sc0 > sc1) std::swap(sc0, sc1);
        } else {
            if (sr0 > sr1 || (sr0 == sr1 && sc0 > sc1)) {
                std::swap(sr0, sr1); std::swap(sc0, sc1);
            }
        }
    }

    for (int row = 0; row < rows; ++row) {
        int idx = view_top + row;
        if (idx < 0 || idx >= total)
            break;

        const Row& line = line_at(idx);

        int ncell = (int)line.size();
        if (ncell > cols) ncell = cols;

        // Render columns right-to-left so CELL_DOUBLE right-bleed overwrites already-drawn cells
        // (not cells drawn later). Wide trail cells (ch=0) are naturally encountered before their
        // lead cell; no explicit skip needed.
        for (int i = ncell - 1; i >= 0; --i) {
            const CharInfo& c = line[i];
            int w = (c.flags & CELL_WIDE) ? 2 : 1;

            int px = origin_x + i * cell_w;
            int py = origin_y + row * cell_h;

            COLORREF fg = c.fg;
            COLORREF bg = c.bg;

            // Inside the selection, swap fg/bg for the inverted look.
            if (sel_active && idx >= sr0 && idx <= sr1) {
                bool in_sel = false;
                if (sel_block) {
                    in_sel = (i >= sc0 && i <= sc1);        // rectangle: col range every row
                } else if (sr0 == sr1) {
                    in_sel = (i >= sc0 && i <= sc1);        // same line
                } else if (idx == sr0) {
                    in_sel = (i >= sc0);                    // first line: from start col
                } else if (idx == sr1) {
                    in_sel = (i <= sc1);                    // last line: to end col
                } else {
                    in_sel = true;                          // middle lines: all cells
                }
                if (in_sel) std::swap(fg, bg);
            }

            // Fill the cell background. CELL_DOUBLE bleeds right (2x width) and upward (1 row):
            // extend the bg rect to cover the full bleed area so the glyph interior shows
            // the correct bg color. The row above is expected blank per spec; overpainting it
            // is intentional. GDI clips automatically if py-cell_h < 0 (first row).
            CRect cell = (c.flags & CELL_DOUBLE)
                ? CRect(px, py - cell_h, px + 2 * w * cell_w, py + cell_h)
                : CRect(px, py, px + w * cell_w, py + cell_h);
            dc.FillSolidRect(cell, bg);

            // Blank cells and wide trail cells (ch=0) draw no glyph. Others draw as a shape (block/box)
            // or via the font (wide -> Korean font; bold/italic -> variant font).
            // CELL_DOUBLE: drawn inline (right-to-left column order makes the bleed safe; see above).
            if (c.flags & CELL_DOUBLE) {
                int dw = 2 * w * cell_w;
                int dbl_base = cell_base + adjust_top;
                // Glyph skipped for blank/trail (ch=0/' ') and blink-off; decorations always drawn.
                if (c.ch != 0 && c.ch != L' ' && !(c.flags & CELL_BLINK && !blink_on)) {
                    bool cw = (c.flags & CELL_WIDE) != 0;
                    CFont* df = cw ? &kfont_double : &efont_double;
                    if (df != last_font) { dc.SelectObject(df); last_font = df; }
                    if (fg != last_fg)   { dc.SetTextColor(fg); last_fg = fg; }
                    // adjust is scaled 2x for double-size glyphs (left/top position; right/bottom
                    // are already 2x via dw = 2*w*cell_w which uses the adjusted cell_w).
                    ::TextOutW(dc.GetSafeHdc(),px + 2 * adjust_left, py + dbl_base, &c.ch, 1);
                }
                if (c.flags & CELL_UNDERLINE)
                    dc.FillSolidRect(CRect(px, py + cell_h - 2, px + dw, py + cell_h), fg);
                if (c.flags & CELL_STRIKE) {
                    int my = py + dbl_base - cell_h / 2;
                    dc.FillSolidRect(CRect(px, my, px + dw, my + 2), fg);
                }
            } else {
                if (c.ch != 0 && c.ch != L' ' && !(c.flags & CELL_BLINK && !blink_on)) {
                    bool drawn = false;
                    if (glyph_level >= 1)
                        drawn = DrawBlockElement(dc, c.ch, px, py, cell_w, cell_h, fg);
                    if (!drawn && glyph_level >= 2)
                        drawn = DrawBoxLine(dc, c.ch, px, py, cell_w, cell_h, fg);
                    if (!drawn) {
                        bool cb = (c.flags & CELL_BOLD)   != 0;
                        bool ci = (c.flags & CELL_ITALIC) != 0;
                        bool cw = (c.flags & CELL_WIDE)   != 0;
                        CFont* f = cw ? (cb && ci ? &kfont_bold_italic : cb ? &kfont_bold : ci ? &kfont_italic : &kfont)
                                      : (cb && ci ? &efont_bold_italic : cb ? &efont_bold : ci ? &efont_italic : &efont);
                        if (f  != last_font) { dc.SelectObject(f);  last_font = f; }
                        if (fg != last_fg)   { dc.SetTextColor(fg); last_fg = fg; }
                        ::TextOutW(dc.GetSafeHdc(),px + adjust_left, py + cell_base, &c.ch, 1);
                    }
                }
                // Underline and strikethrough decoration lines (drawn even on space/trail cells).
                if (c.flags & CELL_UNDERLINE)
                    dc.FillSolidRect(CRect(px, py + cell_h - 1, px + w * cell_w, py + cell_h), fg);
                if (c.flags & CELL_STRIKE)
                    dc.FillSolidRect(CRect(px, py + cell_h / 2, px + w * cell_w, py + cell_h / 2 + 1), fg);
            }

        }
    }

    dc.SelectObject(old_font);
    dc.SetTextAlign(old_align);

    // While composing (uncommitted), draw the glyph in the block cursor cell (instead of system inline).
    // Pinning it to the ConBox block cursor (cur_row/cur_col, kept exact by the child's echo) avoids the
    // composing glyph overlapping the input-start cell. A hollow (bg) rect with a 1px outline marks
    // composing, with the fg glyph on top; always visible (no blink) so the glyph does not flash away.
    if (!comp_str.empty()) {
        CRect cur;
        if (get_cursor_rect(cur)) {
            // Composition display width in cells (one Korean syllable is usually 2).
            int cells = 0;
            for (size_t i = 0; i < comp_str.size(); ++i)
                cells += IsWideChar(comp_str[i]) ? 2 : 1;
            if (cells < 1) cells = 1;
            int comp_w_px = cells * cell_w;
            cur.right = cur.left + comp_w_px;

            // Mid-line composition (over existing text) would hide the underlying glyph: the composing
            // glyph is not yet sent to the child (only committed text is), so the child cannot push the
            // following text right by inserting. For an insert-mode line editor (Python REPL/readline),
            // mimic the insertion the child's echo will do: shift the pixels from the cursor to the
            // line's right edge right by the composition width (ScrollDC is safe for overlapping moves
            // in the same DC). The composing block is drawn into the exposed gap below. The trailing
            // glyph is clipped, but this is a temporary composing display, restored on composition end
            // (OnImeComp/OnImeEnd invalidate).
            int row_right = origin_x + cols * cell_w;
            if (cur.left < row_right) {
                CRect scroll_rc(cur.left, cur.top, row_right, cur.bottom);
                dc.ScrollDC(comp_w_px, 0, &scroll_rc, &scroll_rc, NULL, NULL);
            }

            // Hollow (bg) rect with a 1px outline: fill the whole rect with the outline color, then
            // cover the inside (1px in) with bg, leaving a thin border. Outline = the cursor blend color.
            const int bw = 1;
            COLORREF cblk = blend_cursor_color();
            dc.FillSolidRect(cur, cblk);
            CRect inner(cur.left + bw, cur.top + bw, cur.right - bw, cur.bottom - bw);
            if (inner.right > inner.left && inner.bottom > inner.top)
                dc.FillSolidRect(inner, cur_bg);

            int saved = dc.SaveDC();
            dc.IntersectClipRect(&cur);
            dc.SetTextAlign(TA_LEFT | TA_BASELINE);
            dc.SetBkMode(TRANSPARENT);
            dc.SetTextColor(cur_fg);

            int x = cur.left;
            for (size_t i = 0; i < comp_str.size(); ++i) {
                wchar_t wc = comp_str[i];
                bool wide = IsWideChar(wc);
                dc.SelectObject(wide ? &kfont : &efont);
                bool cdrawn = (glyph_level >= 1 && DrawBlockElement(dc, wc, x, cur.top, cell_w, cell_h, cur_fg))
                           || (glyph_level >= 2 && DrawBoxLine(dc, wc, x, cur.top, cell_w, cell_h, cur_fg));
                if (!cdrawn)
                    ::TextOutW(dc.GetSafeHdc(),x + adjust_left, cur.top + cell_base, &wc, 1);
                x += (wide ? 2 : 1) * cell_w;
            }

            dc.RestoreDC(saved);

            // I-beam shows during composition too (block/underline only show the hollow box). Draw it
            // last so it sits on top: hollow box -> composing glyph -> I-beam (so the cursor is never
            // hidden behind the glyph). During composition ONLY, the I-beam goes to the RIGHT of the
            // hollow box (the composing syllable stays inside the box, the cursor sits just past it).
            // 3px fg, always visible while composing (no blink), like the hollow box.
            if (cursor_type >= 5 && cursor_visible)
                dc.FillSolidRect(CRect(cur.right - 3, cur.top, cur.right, cur.bottom), default_fg);
        }
    }
    // Draw the cursor: a cell-filling block, only when cursor_on && cursor_visible (so it blinks). Not
    // drawn off-screen. If the cursor cell already has a glyph, redraw it in fg over the block so it is
    // not hidden (including the 2nd cell a Korean-mode 2-cell block covers). Skipped while composing
    // (comp_str non-empty) since the composing glyph was drawn above.
    else if (cursor_on && cursor_visible && has_focus) {
        CRect cur;
        if (get_cursor_rect(cur)) {
            // Check whether the child itself highlighted the cursor cell (reverse caret). Some TUIs
            // (claude) paint the input caret as a reverse cell AND keep the hardware cursor there (?25h);
            // drawing the ConBox block too would reveal the child's bright cell when blink is off, so it
            // looks like two cursors. If the cursor cell's bg differs from its neighbors (a local
            // highlight = the child painted a caret), skip our block and use the child's. A full-surface
            // theme bg (all cells the same non-default bg) matches neighbors, so it is not treated as a
            // caret and the cursor does not vanish.
            bool child_caret = false;
            if (cur_row >= 0 && cur_row < (int)screen.size()) {
                const Row& cl = screen[cur_row];
                if (cur_col >= 0 && cur_col < (int)cl.size()) {
                    COLORREF ref_bg = default_bg;
                    if (cur_col > 0)                       ref_bg = cl[cur_col - 1].bg;
                    else if (cur_col + 1 < (int)cl.size()) ref_bg = cl[cur_col + 1].bg;
                    child_caret = (cl[cur_col].bg != ref_bg);
                }
            }
            if (!child_caret) {
            COLORREF cblk = blend_cursor_color();
            if (cursor_type <= 2) {
            // Block cursor: fill the whole cell with the blend color.
            dc.FillSolidRect(cur, cblk);

            if (cur_row >= 0 && cur_row < (int)screen.size()) {
                const Row& line = screen[cur_row];

                // Redraw the covered glyphs from cur_col so they are not hidden. Parts outside the block
                // were already drawn in their own color, so clip to the block rect and only cover the
                // inside in fg.
                int saved = dc.SaveDC();
                dc.IntersectClipRect(&cur);
                dc.SetTextAlign(TA_LEFT | TA_BASELINE);
                dc.SetBkMode(TRANSPARENT);
                dc.SetTextColor(cur_fg);

                int x = cur.left;
                for (int i = cur_col; i < (int)line.size() && x < cur.right; ++i) {
                    const CharInfo& cc = line[i];
                    int w = (cc.flags & CELL_WIDE) ? 2 : 1;
                    if (cc.ch != 0 && cc.ch != L' ') {
                        bool cb = (cc.flags & CELL_BOLD)   != 0;
                        bool ci = (cc.flags & CELL_ITALIC) != 0;
                        bool cw = (cc.flags & CELL_WIDE)   != 0;
                        CFont* f = cw ? (cb && ci ? &kfont_bold_italic : cb ? &kfont_bold : ci ? &kfont_italic : &kfont)
                                      : (cb && ci ? &efont_bold_italic : cb ? &efont_bold : ci ? &efont_italic : &efont);
                        dc.SelectObject(f);
                        bool cdrawn = (glyph_level >= 1 && DrawBlockElement(dc, cc.ch, x, cur.top, cell_w, cell_h, cur_fg))
                                   || (glyph_level >= 2 && DrawBoxLine(dc, cc.ch, x, cur.top, cell_w, cell_h, cur_fg));
                        if (!cdrawn)
                            ::TextOutW(dc.GetSafeHdc(),x + adjust_left, cur.top + cell_base, &cc.ch, 1);
                    }
                    x += w * cell_w;
                }

                dc.RestoreDC(saved);
            }
            }   // block cursor (cursor_type <= 2)
            else if (cursor_type <= 4) {
            // Underline cursor: a 3px-high bar at the cell bottom in the cursor blend color. The
            // glyph was drawn earlier in OnPaint and stays visible above the bar (no redraw needed).
            dc.FillSolidRect(CRect(cur.left, cur.bottom - 3, cur.right, cur.bottom), cblk);
            }
            else {
            // I-beam cursor: a vertical bar at the cell's LEFT edge -- the insertion point, since a
            // char typed here pushes existing text right. English (1-cell): a single 1px fg line.
            // Korean (2-cell): a 3px fg line (left edge to +3px, kept inside the cell). All fg so the
            // Korean bar reads clearly distinct from text.
            if (is_hangul_mode())
                dc.FillSolidRect(CRect(cur.left, cur.top, cur.left + 3, cur.bottom), default_fg);
            else
                dc.FillSolidRect(CRect(cur.left, cur.top, cur.left + 1, cur.bottom), default_fg);
            }
            }   // if (!child_caret)
        }
    }

    // Overlay scrollbar on top of everything (only when alpha > 0; auto-hidden otherwise).
    draw_overlay_scrollbar(dc);

    // Blit the finished frame to the screen at once (no flicker). The bitmap stays a live member (freed
    // in the dtor), so it is not detached here.
    paintDC.BitBlt(0, 0, rc.Width(), rc.Height(), &back_dc, 0, 0, SRCCOPY);
}


// ===== EMF export =====
// save_emf: renders the full scrollback+screen buffer to a series of EMF vector files.
// Each page is a chunk of cfg_lines_per_paper rows. If the first row of a page has CELL_DOUBLE
// glyphs (which bleed upward by cell_h), an extra blank row is prepended (y_offset=cell_h) so the
// bleed is not clipped. Rendering order per row: right-to-left, matching OnPaint (so CELL_DOUBLE
// rightward bleed overwrites already-drawn cells, not later ones). Glyphs are drawn via
// SetTextColor + TextOutW (EMF text records -- vector, not bitmap). Block/box chars go through
// DrawBlockElement/DrawBoxLine (shape-based). Note: BeginPath/FillPath does NOT work on EMF DCs
// as on screen DCs; TextOutW on an EMF DC renders immediately regardless of the path bracket.

// (design: [PAINT]) -- same coordinate/cell logic as OnPaint but targets an EMF DC.
// Returns true if at least one EMF file was successfully written.
bool ConBox::save_emf(const char* dir)
{
    if (!dir || !*dir) return false;

    int total = (int)scrollback.size() + rows;
    if (total <= 0) return false;

    // Convert dir (UTF-8) to wide string for Win32 path APIs.
    int wlen = ::MultiByteToWideChar(CP_UTF8, 0, dir, -1, NULL, 0);
    if (wlen <= 0) return false;
    std::vector<wchar_t> wdir(wlen);
    ::MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir.data(), wlen);

    // Page width is fixed (same for all pages); height varies only when extra_top is added.
    int page_w = margin_left + cols * cell_w + margin_right;

    bool wrote_any = false;
    int page_idx = 0;
    for (int row_start = 0; row_start < total; row_start += cfg_lines_per_paper, ++page_idx) {
        int row_end = min(row_start + cfg_lines_per_paper, total);
        int n_lines = row_end - row_start;

        // If the first row of this page has any CELL_DOUBLE glyph, prepend a blank row so the
        // 2x upward bleed (which extends into the row above) is not clipped at the page top.
        bool extra_top = false;
        {
            const Row& first = line_at(row_start);
            for (const CharInfo& c : first) {
                if (c.flags & CELL_DOUBLE) { extra_top = true; break; }
            }
        }
        int y_offset = extra_top ? cell_h : 0;
        int page_h   = margin_top + y_offset + n_lines * cell_h + margin_bottom;

        // Build output filename: <dir>\ConBox000.emf
        wchar_t filename[MAX_PATH];
        swprintf_s(filename, MAX_PATH, L"%s\\ConBox%03d.emf", wdir.data(), page_idx);

        // Pass NULL for lpRect so GDI auto-computes the frame from actual drawing operations,
        // giving a tight canvas that matches exactly the drawn content (no extra whitespace).
        HDC hMeta = ::CreateEnhMetaFileW(NULL, filename, NULL, NULL);
        if (!hMeta) continue;

        // Wrap in CDC for DrawBlockElement/DrawBoxLine compatibility. Must Detach() before
        // CloseEnhMetaFile (CDC destructor would DeleteDC if we let it go out of scope owned).
        CDC cdc;
        cdc.Attach(hMeta);
        cdc.SetBkMode(TRANSPARENT);
        cdc.SetTextAlign(TA_LEFT | TA_BASELINE);
        CFont* old_font = cdc.SelectObject(&efont);

        // Fill the page background with the paper bg color.
        cdc.FillSolidRect(CRect(0, 0, page_w, page_h), paper_default_bg);

        // Render rows. Right-to-left column order (same as OnPaint): CELL_DOUBLE glyphs bleed
        // rightward; rendering right-to-left means the bleed overwrites already-drawn cells
        // (not cells rendered after the glyph). Upward bleed is safe because rows go top-to-bottom.
        for (int ri = 0; ri < n_lines; ++ri) {
            const Row& line = line_at(row_start + ri);
            int ncell = min((int)line.size(), cols);
            int py = margin_top + y_offset + ri * cell_h;

            for (int i = ncell - 1; i >= 0; --i) {
                const CharInfo& c = line[i];
                int w  = (c.flags & CELL_WIDE) ? 2 : 1;
                int px = margin_left + i * cell_w;
                COLORREF fg = remap_paper_color(c.fg);
                COLORREF bg = remap_paper_color(c.bg);

                // --- Background ---
                // CELL_DOUBLE: background covers the full 2x bleed area (upward by cell_h,
                // rightward to 2*w*cell_w); same formula as OnPaint.
                if (c.flags & CELL_DOUBLE) {
                    cdc.FillSolidRect(
                        CRect(px, py - cell_h, px + 2 * w * cell_w, py + cell_h), bg);
                } else {
                    cdc.FillSolidRect(
                        CRect(px, py, px + w * cell_w, py + cell_h), bg);
                }

                // --- Glyph + decorations ---
                // Structure mirrors OnPaint: CELL_DOUBLE handled first (glyph skipped for ch=0/' '
                // but decorations always drawn), non-double second (same skip rule for glyph only).
                if (c.flags & CELL_DOUBLE) {
                    int dw       = 2 * w * cell_w;
                    int dbl_base = cell_base + adjust_top;
                    // Glyph: skipped for trail cells (ch=0) and spaces; blink always visible in EMF.
                    if (c.ch != 0 && c.ch != L' ') {
                        bool cw = (c.flags & CELL_WIDE) != 0;
                        cdc.SelectObject(cw ? &kfont_double : &efont_double);
                        cdc.SetTextColor(fg);
                        ::TextOutW(cdc.GetSafeHdc(),px + 2 * adjust_left, py + dbl_base, &c.ch, 1);
                    }
                    // Decorations always drawn (even for trail ch=0 and double-size spaces).
                    if (c.flags & CELL_UNDERLINE)
                        cdc.FillSolidRect(
                            CRect(px, py + cell_h - 2, px + dw, py + cell_h), fg);
                    if (c.flags & CELL_STRIKE) {
                        int my = py + dbl_base - cell_h / 2;
                        cdc.FillSolidRect(CRect(px, my, px + dw, my + 2), fg);
                    }
                } else {
                    // Normal glyph: block/box chars via DrawBlockElement/DrawBoxLine;
                    // other chars via SetTextColor + TextOutW (vector text record in EMF).
                    if (c.ch != 0 && c.ch != L' ') {
                        bool drawn = false;
                        if (glyph_level >= 1)
                            drawn = DrawBlockElement(cdc, c.ch, px, py, cell_w, cell_h, fg);
                        if (!drawn && glyph_level >= 2)
                            drawn = DrawBoxLine(cdc, c.ch, px, py, cell_w, cell_h, fg);
                        if (!drawn) {
                            bool cb = (c.flags & CELL_BOLD)   != 0;
                            bool ci = (c.flags & CELL_ITALIC) != 0;
                            bool cw = (c.flags & CELL_WIDE)   != 0;
                            CFont* f = cw
                                ? (cb&&ci ? &kfont_bold_italic : cb ? &kfont_bold : ci ? &kfont_italic : &kfont)
                                : (cb&&ci ? &efont_bold_italic : cb ? &efont_bold : ci ? &efont_italic : &efont);
                            cdc.SelectObject(f);
                            cdc.SetTextColor(fg);
                            ::TextOutW(cdc.GetSafeHdc(),px + adjust_left, py + cell_base, &c.ch, 1);
                        }
                    }
                    // Decorations drawn even on space / trail ch=0 cells.
                    if (c.flags & CELL_UNDERLINE)
                        cdc.FillSolidRect(
                            CRect(px, py + cell_h - 1, px + w * cell_w, py + cell_h), fg);
                    if (c.flags & CELL_STRIKE)
                        cdc.FillSolidRect(
                            CRect(px, py + cell_h / 2, px + w * cell_w, py + cell_h / 2 + 1), fg);
                }
            }
        }

        cdc.SelectObject(old_font);
        cdc.Detach();   // CDC must not own hMeta when CloseEnhMetaFile is called

        HENHMETAFILE hmf = ::CloseEnhMetaFile(hMeta);   // writes the .emf file to disk
        if (hmf) { ::DeleteEnhMetaFile(hmf); wrote_any = true; }
    }
    return wrote_any;
}


// ===== PDF export =====
// save_pdf: renders scrollback+screen to a PDF file via the system PDF printer.
// Finds the first printer whose name contains "PDF" (EnumPrintersW level 1), then retrieves
// its DEVMODE via GetPrinterW level 2, sets a custom page size (dmPaperSize=DMPAPER_USER,
// dmPaperWidth/Length in 0.1mm units), creates a printer DC, and drives GDI printing with
// DOCINFO.lpszOutput pointing at the output file (no Save dialog shown).
// Coordinate scaling: MM_ANISOTROPIC maps logical units (= screen pixels) to printer dots so
// all existing cell_w/cell_h coordinates and the pre-built screen-DPI fonts work directly.
// StartPage resets DC state, so mapping mode, bk mode, text align, and font are re-applied
// after every StartPage.  The cell rendering loop is a verbatim copy of save_emf's loop.
// (design: [PAINT])

bool ConBox::save_pdf(const char* path)
{
    if (!path || !*path) return false;

    int total = (int)scrollback.size() + rows;
    if (total <= 0) return false;

    // Convert output path to wide string.
    int wlen = ::MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) return false;
    std::vector<wchar_t> wpath(wlen);
    ::MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), wlen);

    // --- Find a PDF-capable printer (name contains "PDF", case-insensitive) ---
    DWORD needed = 0, count = 0;
    ::EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS,
                    NULL, 1, NULL, 0, &needed, &count);
    if (needed == 0) return false;
    std::vector<BYTE> enum_buf(needed);
    if (!::EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS,
                         NULL, 1, enum_buf.data(), needed, &needed, &count))
        return false;

    PRINTER_INFO_1W* pi1   = (PRINTER_INFO_1W*)enum_buf.data();
    WCHAR*           pname = nullptr;
    for (DWORD i = 0; i < count; ++i) {
        if (!pi1[i].pName) continue;
        WCHAR tmp[256];
        wcsncpy_s(tmp, pi1[i].pName, _TRUNCATE);
        _wcsupr_s(tmp, _countof(tmp));
        if (wcsstr(tmp, L"PDF")) { pname = pi1[i].pName; break; }
    }
    if (!pname) return false;

    // --- Content width in screen pixels (height is determined per-page below) ---
    int page_w_px = margin_left + cols * cell_w + margin_right;

    // --- Open printer; get driver name for CreateDCW ---
    // Custom paper size (DMPAPER_USER) is silently ignored by the Microsoft Print to PDF
    // driver -- it always reverts to its stored default (A4).  Instead we create the DC
    // with the default DEVMODE, query the actual printable area (HORZRES/VERTRES), and
    // derive how many terminal rows fit per page from that area.
    HANDLE hPrinter = NULL;
    if (!::OpenPrinterW(pname, &hPrinter, NULL)) return false;
    DWORD pi2_needed = 0;
    ::GetPrinterW(hPrinter, 2, NULL, 0, &pi2_needed);
    std::vector<BYTE> pi2_buf(pi2_needed);
    if (!::GetPrinterW(hPrinter, 2, pi2_buf.data(), pi2_needed, &pi2_needed)) {
        ::ClosePrinter(hPrinter); return false;
    }
    ::ClosePrinter(hPrinter);
    PRINTER_INFO_2W* pi2 = (PRINTER_INFO_2W*)pi2_buf.data();

    // --- Create printer DC (default paper, no DEVMODE override) ---
    HDC hdc = ::CreateDCW(pi2->pDriverName, pname, pi2->pPortName, NULL);
    if (!hdc) return false;

    // --- Derive rows per page from the actual printable area ---
    // Scale: fit content width exactly to the printable width (horzres device dots).
    // At that scale, compute how many logical pixels are available vertically, then
    // divide by cell_h to get complete rows.  The page height is then exactly
    // margin_top + rows_per_page * cell_h + margin_bottom with no wasted space.
    int horzres = ::GetDeviceCaps(hdc, HORZRES);
    int vertres = ::GetDeviceCaps(hdc, VERTRES);

    // avail_h: how many logical (screen-pixel) units fit in the printable height at the
    // width-fit scale.  MulDiv avoids 32-bit overflow: (vertres * page_w_px) / horzres.
    int avail_h      = ::MulDiv(vertres, page_w_px, horzres);
    int rows_per_page = max(1, (avail_h - margin_top - margin_bottom) / cell_h);
    int page_h_px    = margin_top + rows_per_page * cell_h + margin_bottom;

    // Viewport height for this page_h_px (clamped to avoid off-by-one overflow).
    int vp_x = horzres;
    int vp_y = min(vertres, ::MulDiv(horzres, page_h_px, page_w_px));

    // --- Start print job; lpszOutput routes output to file without a Save dialog ---
    DOCINFOW di = {};
    di.cbSize      = sizeof(di);
    di.lpszDocName = L"ConBox";
    di.lpszOutput  = wpath.data();
    if (::StartDocW(hdc, &di) <= 0) {
        ::DeleteDC(hdc);
        return false;
    }

    // Wrap in CDC for MFC drawing helpers (FillSolidRect, DrawBlockElement, etc.).
    // Must Detach() before DeleteDC; never let CDC destructor own a printer HDC that was
    // already passed to EndDoc/DeleteDC.
    CDC cdc;
    cdc.Attach(hdc);

    // --- Page loop ---
    for (int row_start = 0; row_start < total; row_start += rows_per_page) {
        int row_end = min(row_start + rows_per_page, total);
        int n_lines = row_end - row_start;

        // Check for CELL_DOUBLE in the first row (needs an extra blank row above for bleed).
        bool extra_top = false;
        {
            const Row& first = line_at(row_start);
            for (const CharInfo& c : first) {
                if (c.flags & CELL_DOUBLE) { extra_top = true; break; }
            }
        }
        int y_offset = extra_top ? cell_h : 0;

        ::StartPage(hdc);

        // StartPage resets DC state (mapping mode, selected objects, bk mode, text align).
        // Re-apply after every StartPage so drawing calls use the correct coordinate system.
        ::SetMapMode(hdc, MM_ANISOTROPIC);
        ::SetWindowExtEx(hdc,  page_w_px, page_h_px, NULL);
        ::SetViewportExtEx(hdc, vp_x,     vp_y,      NULL);
        cdc.SetBkMode(TRANSPARENT);
        cdc.SetTextAlign(TA_LEFT | TA_BASELINE);
        CFont* old_font = cdc.SelectObject(&efont);

        // Page background.
        cdc.FillSolidRect(CRect(0, 0, page_w_px, page_h_px), paper_default_bg);

        // Cell rendering loop -- verbatim copy of save_emf's loop.
        // Right-to-left column order so CELL_DOUBLE rightward bleed overwrites already-drawn cells.
        for (int ri = 0; ri < n_lines; ++ri) {
            const Row& line = line_at(row_start + ri);
            int ncell = min((int)line.size(), cols);
            int py = margin_top + y_offset + ri * cell_h;

            for (int i = ncell - 1; i >= 0; --i) {
                const CharInfo& c = line[i];
                int w  = (c.flags & CELL_WIDE) ? 2 : 1;
                int px = margin_left + i * cell_w;
                COLORREF fg = remap_paper_color(c.fg);
                COLORREF bg = remap_paper_color(c.bg);

                // --- Background ---
                if (c.flags & CELL_DOUBLE) {
                    cdc.FillSolidRect(
                        CRect(px, py - cell_h, px + 2 * w * cell_w, py + cell_h), bg);
                } else {
                    cdc.FillSolidRect(
                        CRect(px, py, px + w * cell_w, py + cell_h), bg);
                }

                // --- Glyph + decorations (mirrors save_emf / OnPaint structure) ---
                if (c.flags & CELL_DOUBLE) {
                    int dw       = 2 * w * cell_w;
                    int dbl_base = cell_base + adjust_top;
                    if (c.ch != 0 && c.ch != L' ') {
                        bool cw = (c.flags & CELL_WIDE) != 0;
                        cdc.SelectObject(cw ? &kfont_double : &efont_double);
                        cdc.SetTextColor(fg);
                        ::TextOutW(cdc.GetSafeHdc(),px + 2 * adjust_left, py + dbl_base, &c.ch, 1);
                    }
                    // Decorations always drawn (even for trail ch=0 and double-size spaces).
                    if (c.flags & CELL_UNDERLINE)
                        cdc.FillSolidRect(
                            CRect(px, py + cell_h - 2, px + dw, py + cell_h), fg);
                    if (c.flags & CELL_STRIKE) {
                        int my = py + dbl_base - cell_h / 2;
                        cdc.FillSolidRect(CRect(px, my, px + dw, my + 2), fg);
                    }
                } else {
                    if (c.ch != 0 && c.ch != L' ') {
                        bool drawn = false;
                        if (glyph_level >= 1)
                            drawn = DrawBlockElement(cdc, c.ch, px, py, cell_w, cell_h, fg);
                        if (!drawn && glyph_level >= 2)
                            drawn = DrawBoxLine(cdc, c.ch, px, py, cell_w, cell_h, fg);
                        if (!drawn) {
                            bool cb = (c.flags & CELL_BOLD)   != 0;
                            bool ci = (c.flags & CELL_ITALIC) != 0;
                            bool cw = (c.flags & CELL_WIDE)   != 0;
                            CFont* f = cw
                                ? (cb&&ci ? &kfont_bold_italic : cb ? &kfont_bold : ci ? &kfont_italic : &kfont)
                                : (cb&&ci ? &efont_bold_italic : cb ? &efont_bold : ci ? &efont_italic : &efont);
                            cdc.SelectObject(f);
                            cdc.SetTextColor(fg);
                            ::TextOutW(cdc.GetSafeHdc(),px + adjust_left, py + cell_base, &c.ch, 1);
                        }
                    }
                    // Decorations drawn even on space / trail ch=0 cells.
                    if (c.flags & CELL_UNDERLINE)
                        cdc.FillSolidRect(
                            CRect(px, py + cell_h - 1, px + w * cell_w, py + cell_h), fg);
                    if (c.flags & CELL_STRIKE)
                        cdc.FillSolidRect(
                            CRect(px, py + cell_h / 2, px + w * cell_w, py + cell_h / 2 + 1), fg);
                }
            }
        }

        cdc.SelectObject(old_font);
        ::EndPage(hdc);
    }

    cdc.Detach();
    ::EndDoc(hdc);
    ::DeleteDC(hdc);
    return true;
}


// ===== Text extract =====
// get_text_lines: returns the full scrollback+screen buffer as UTF-8 text lines.
// Each element is a null-terminated std::string with no trailing newline. Trailing spaces trimmed.
// Trail cell detection: put_char sets ch=0 on the trail but does NOT set CELL_WIDE there (only
// the lead has CELL_WIDE). Trails are skipped by position (lead+1), not by flag check.
// CELL_DOUBLE extras: Korean (CELL_WIDE+CELL_DOUBLE) = 4-cell advance -> skip trail + 2 blanks.
//                     English/etc (CELL_DOUBLE only)  = 2-cell advance -> skip 1 blank.

std::vector<std::string> ConBox::get_text_lines() const
{
    int total = (int)scrollback.size() + rows;
    std::vector<std::string> result;
    result.reserve(total);

    std::vector<wchar_t> wline;

    for (int r = 0; r < total; ++r) {
        const Row& row = line_at(r);
        int ncell = min((int)row.size(), cols);
        wline.clear();

        int i = 0;
        while (i < ncell) {
            const CharInfo& c = row[i];
            bool is_wide   = (c.flags & CELL_WIDE)   != 0;
            bool is_double = (c.flags & CELL_DOUBLE) != 0;

            wline.push_back(c.ch ? c.ch : L' ');
            ++i;

            if (is_wide) {
                // Skip trail cell. Trail has ch=0 but no CELL_WIDE (put_char sets flags=attr on trail).
                // Detect by lead position: trail is always at lead+1.
                if (i < ncell) ++i;
                if (is_double) {
                    // Korean double (4-cell advance): skip 2 extra blank cells.
                    if (i < ncell && (row[i].ch == 0 || row[i].ch == L' ')) ++i;
                    if (i < ncell && (row[i].ch == 0 || row[i].ch == L' ')) ++i;
                }
            } else if (is_double) {
                // English/special/blank double (2-cell advance): skip 1 extra blank cell.
                if (i < ncell && (row[i].ch == 0 || row[i].ch == L' ')) ++i;
            }
        }

        // Trim trailing spaces.
        while (!wline.empty() && (wline.back() == L' ' || wline.back() == 0))
            wline.pop_back();

        // Convert to UTF-8; no newline appended (caller decides line ending).
        if (wline.empty()) {
            result.emplace_back();
        } else {
            int u8len = ::WideCharToMultiByte(CP_UTF8, 0, wline.data(), (int)wline.size(),
                                              NULL, 0, NULL, NULL);
            std::string s(u8len, '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, wline.data(), (int)wline.size(),
                                  &s[0], u8len, NULL, NULL);
            result.push_back(std::move(s));
        }
    }

    return result;
}


// Open or close the raw child-output log file. Bytes are written in pump() before VT parsing so
// VT codes are preserved exactly as the child emitted them (no conversion, no CR/LF translation).
// Calling with nullptr/empty closes any open log; calling with a path creates/replaces the file.
// No window is needed -- safe to call before open() so the file is ready before the child starts.
int ConBox::save_log(const char* file_name)
{
    // Close any currently open log (idempotent; also the close path when file_name is null).
    if (log_file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(log_file);
        log_file = INVALID_HANDLE_VALUE;
    }
    if (!file_name || !*file_name)
        return 0;

    // Convert UTF-8 path to wide for CreateFileW (same pattern as save_emf).
    int wlen = ::MultiByteToWideChar(CP_UTF8, 0, file_name, -1, NULL, 0);
    if (wlen <= 0)
        return (int)::GetLastError();
    std::vector<wchar_t> wpath(wlen);
    ::MultiByteToWideChar(CP_UTF8, 0, file_name, -1, wpath.data(), wlen);

    // OPEN_ALWAYS: create if missing, otherwise append to the existing file.
    log_file = ::CreateFileW(wpath.data(), GENERIC_WRITE, FILE_SHARE_READ,
                             NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log_file == INVALID_HANDLE_VALUE)
        return (int)::GetLastError();

    // Seek to end so new output is appended.
    LARGE_INTEGER zero = {};
    ::SetFilePointerEx(log_file, zero, NULL, FILE_END);

    // Write 4 blank lines, then a 100-column separator with the local start time.
    // Layout: 5 CRLFs (ends previous content + 4 blank lines) then
    //   "--- YYYY-MM-DD HH:MM:SS " (24 chars) + 76 dashes = 100 cols.
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    char hdr[128];
    int hlen = sprintf_s(hdr, sizeof(hdr),
                         "\r\n\r\n\r\n\r\n\r\n--- %04d-%02d-%02d %02d:%02d:%02d "
                         "----------------------------------------------------------------------------\r\n",
                         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    DWORD written;
    ::WriteFile(log_file, hdr, hlen, &written, NULL);
    return 0;
}


// ===== Child runner (ConPTY) =====
// Once start() spawns a child, this group handles ConPTY I/O. Child output: PUMP_TIMER calls pump()
// periodically -> print(). Key input / resize: the input/resize sinks start() wired to itself
// (child_input_thunk/child_resize_thunk) deliver to write()/resize().

// UTF-8 -> UTF-16 (null-terminated). Empty vector on failure/empty input.
static std::vector<wchar_t> Utf8ToWide(const char* s)
{
    std::vector<wchar_t> out;
    if (s == nullptr)
        return out;
    int wlen = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (wlen <= 0)
        return out;
    out.resize(wlen);
    ::MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), wlen);
    return out;
}

// Make the input/output pipe pair and a pseudo console (CreatePseudoConsole). The PTY-side ends (the
// child's input read end, output write end) are closed by the caller after spawn. The ends we keep
// (in_write, out_read) are returned for the members.
static bool CreatePtyPipes(int cols, int rows,
    HPCON& h_pc_out, HANDLE& in_write_out, HANDLE& out_read_out,
    HANDLE& pty_in_read_out, HANDLE& pty_out_write_out)
{
    HANDLE in_read = nullptr, in_write = nullptr;
    HANDLE out_read = nullptr, out_write = nullptr;

    if (!::CreatePipe(&in_read, &in_write, NULL, 0))
        return false;
    if (!::CreatePipe(&out_read, &out_write, NULL, 0)) {
        ::CloseHandle(in_read);
        ::CloseHandle(in_write);
        return false;
    }

    COORD size;
    size.X = (SHORT)cols;
    size.Y = (SHORT)rows;
    HPCON pc = nullptr;
    HRESULT hr = ::CreatePseudoConsole(size, in_read, out_write, 0, &pc);
    if (FAILED(hr)) {
        ::CloseHandle(in_read);
        ::CloseHandle(in_write);
        ::CloseHandle(out_read);
        ::CloseHandle(out_write);
        return false;
    }

    h_pc_out = pc;
    in_write_out = in_write;
    out_read_out = out_read;
    // The caller closes these after spawning the child (so out_read sees EOF on child exit).
    pty_in_read_out = in_read;
    pty_out_write_out = out_write;
    return true;
}

bool ConBox::start()
{
    return start(cfg_cmdline.c_str());
}

// (design: [PTY])
bool ConBox::start(const char* cmdline)
{
    // Restart if already running.
    stop();

    // Size the child console to the current grid (always matched).
    int c = this->cols;
    int r = this->rows;
    if (c < 1) c = 1;
    if (r < 1) r = 1;

    // Pipes + pseudo console. The PTY-side ends are closed after spawn.
    HANDLE pty_in_read = nullptr, pty_out_write = nullptr;
    if (!CreatePtyPipes(c, r, h_pc, in_write, out_read, pty_in_read, pty_out_write)) {
        stop();
        return false;
    }

    // Attribute list so the child attaches to the pseudo console.
    STARTUPINFOEXW si;
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(STARTUPINFOEXW);

    // Size the list for one attribute (PSEUDOCONSOLE_HANDLE), then allocate it.
    SIZE_T attr_bytes = 0;
    ::InitializeProcThreadAttributeList(NULL, 1, 0, &attr_bytes);
    si.lpAttributeList =
        (LPPROC_THREAD_ATTRIBUTE_LIST)::HeapAlloc(::GetProcessHeap(), 0, attr_bytes);
    if (si.lpAttributeList == nullptr) {
        ::CloseHandle(pty_in_read);
        ::CloseHandle(pty_out_write);
        stop();
        return false;
    }
    if (!::InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_bytes) ||
        !::UpdateProcThreadAttribute(si.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE_HANDLE,
            h_pc, sizeof(h_pc), NULL, NULL)) {
        ::HeapFree(::GetProcessHeap(), 0, si.lpAttributeList);
        ::CloseHandle(pty_in_read);
        ::CloseHandle(pty_out_write);
        stop();
        return false;
    }

    // CreateProcessW needs a writable command-line buffer, so use a mutable vector.
    std::vector<wchar_t> cmd = Utf8ToWide(cmdline);
    if (cmd.empty()) {
        ::DeleteProcThreadAttributeList(si.lpAttributeList);
        ::HeapFree(::GetProcessHeap(), 0, si.lpAttributeList);
        ::CloseHandle(pty_in_read);
        ::CloseHandle(pty_out_write);
        stop();
        return false;
    }

    BOOL ok = ::CreateProcessW(
        NULL,            // find the module from the command line
        cmd.data(),      // mutable command-line buffer
        NULL, NULL,
        FALSE,           // do not inherit handles (only the console via the pseudoconsole attribute)
        EXTENDED_STARTUPINFO_PRESENT,
        NULL, NULL,
        &si.StartupInfo,
        &child_proc);

    ::DeleteProcThreadAttributeList(si.lpAttributeList);
    ::HeapFree(::GetProcessHeap(), 0, si.lpAttributeList);

    // Close the PTY-side ends now. The pseudo console holds its own copies, and closing the output
    // write end is what lets out_read see EOF / broken pipe when the child exits.
    ::CloseHandle(pty_in_read);
    ::CloseHandle(pty_out_write);

    if (!ok) {
        ZeroMemory(&child_proc, sizeof(child_proc));
        stop();
        return false;
    }

    // Wire input (ConBox keys -> child stdin) and resize (grid -> ResizePseudoConsole) to itself, reusing
    // the generic sink mechanism (same path as a pure-view host's set_input_sink).
    set_input_sink(&ConBox::child_input_thunk, this);
    set_resize_sink(&ConBox::child_resize_thunk, this);

    // Output polling timer on ConBox's own window. start() must be called after open() (the timer needs
    // the window).
    SetTimer(PUMP_TIMER, PUMP_INTERVAL_MS, NULL);

    child_running = true;
    return true;
}

void ConBox::write(const char* data, int len)
{
    if (in_write == nullptr || data == nullptr || len <= 0)
        return;
    DWORD written = 0;
    ::WriteFile(in_write, data, (DWORD)len, &written, NULL);
}

void ConBox::child_input_thunk(const char* bytes, int len, void* user)
{
    ConBox* self = (ConBox*)user;
    if (self != nullptr)
        self->write(bytes, len);
}

void ConBox::resize(int rows, int cols)
{
    // Only resize while the pseudo console is alive. The child detects the size change and repaints.
    if (h_pc == nullptr || !child_running)
        return;
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    COORD size;
    size.X = (SHORT)cols;
    size.Y = (SHORT)rows;
    ::ResizePseudoConsole(h_pc, size);
}

void ConBox::child_resize_thunk(int rows, int cols, void* user)
{
    ConBox* self = (ConBox*)user;
    if (self != nullptr)
        self->resize(rows, cols);
}

void ConBox::set_exit_callback(void (*cb)(void* user), void* user)
{
    exit_cb = cb;
    exit_cb_user = user;
}

void ConBox::stop()
{
    // Kill the polling timer first (so pump is no longer called). KillTimer only while the window lives.
    if (GetSafeHwnd() != nullptr)
        KillTimer(PUMP_TIMER);

    // Release the input/resize sinks start() wired to itself (with no child, input is dropped).
    if (input_sink == &ConBox::child_input_thunk)
        set_input_sink(nullptr, nullptr);
    if (resize_sink == &ConBox::child_resize_thunk)
        set_resize_sink(nullptr, nullptr);

    // Closing the pseudo console ends the child console session.
    if (h_pc != nullptr) {
        ::ClosePseudoConsole(h_pc);
        h_pc = nullptr;
    }
    if (in_write != nullptr) {
        ::CloseHandle(in_write);
        in_write = nullptr;
    }
    if (out_read != nullptr) {
        ::CloseHandle(out_read);
        out_read = nullptr;
    }
    if (child_proc.hThread != nullptr) {
        ::CloseHandle(child_proc.hThread);
        child_proc.hThread = nullptr;
    }
    if (child_proc.hProcess != nullptr) {
        ::CloseHandle(child_proc.hProcess);
        child_proc.hProcess = nullptr;
    }
    ZeroMemory(&child_proc, sizeof(child_proc));
    child_running = false;
}

bool ConBox::is_running() const
{
    return child_running;
}

void ConBox::pump()
{
    if (!child_running || out_read == nullptr)
        return;

    // Check how many bytes are queued first (avoid a blocking ReadFile). A PeekNamedPipe failure means
    // the pipe is closed (child gone), so clean up.
    DWORD avail = 0;
    if (!::PeekNamedPipe(out_read, NULL, 0, NULL, &avail, NULL)) {
        handle_child_exit();
        return;
    }

    // Read only what is queued and feed it to print() (UTF-8, parsed as VT). One pump may read in
    // several passes.
    while (avail > 0) {
        char buf[4096];
        DWORD want = (avail < sizeof(buf) - 1) ? avail : (DWORD)(sizeof(buf) - 1);
        DWORD got = 0;
        if (!::ReadFile(out_read, buf, want, &got, NULL) || got == 0) {
            handle_child_exit();
            return;
        }
        // Write raw bytes to log before VT parsing (VT codes intact, no conversion).
        if (log_file != INVALID_HANDLE_VALUE) {
            DWORD written;
            ::WriteFile(log_file, buf, got, &written, NULL);
        }
        buf[got] = '\0';   // print() expects null-terminated UTF-8
        print(buf);
        avail -= got;
    }

    // Child-exit detection. Under ConPTY the conhost keeps the output pipe's write end open after the
    // child exits, so PeekNamedPipe never sees EOF. Poll the process handle directly. On confirmed exit,
    // drain remaining output first, then clean up / callback.
    if (child_proc.hProcess != nullptr &&
        ::WaitForSingleObject(child_proc.hProcess, 0) == WAIT_OBJECT_0) {
        DWORD more = 0;
        if (::PeekNamedPipe(out_read, NULL, 0, NULL, &more, NULL) && more > 0)
            return;   // output still pending; read it on the next pump
        handle_child_exit();
    }
}

void ConBox::handle_child_exit()
{
    // The child exited naturally. Grab the callback info first, then clean up, then call the callback
    // with cleanup done (is_running()==false) so it may immediately start() again without conflict.
    void (*cb)(void*) = exit_cb;
    void* user = exit_cb_user;
    stop();
    if (cb != nullptr)
        cb(user);
}

void ConBox::OnDestroy()
{
    // Clean up child/polling timer before the window is destroyed (so polling never touches a dying window).
    stop();
    // Auto-close the log file so no bytes are lost if the window closes while logging.
    if (log_file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(log_file);
        log_file = INVALID_HANDLE_VALUE;
    }
    KillTimer(BLINK_TIMER);
    KillTimer(SBAR_TIMER);
    CWnd::OnDestroy();
}
