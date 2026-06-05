// ConBox.cpp
//
// Implementation of CConBox. To only USE the control, ConBox.h is enough; read this file when
// MODIFYING behavior. Use the table of contents below to Grep to one function instead of reading
// the whole file.
//
// === File layout (roughly this order) ===
//   helpers (static): IsWideChar, ParseFontOpts, DrawBlockElement, DrawBoxLine
//   message map     : BEGIN_MESSAGE_MAP ... END_MESSAGE_MAP
//   window/font     : open, make_font, apply_default_fonts, calc_cell_size,
//                     set_efont, set_kfont, set_builtin_glyphs
//   color/cursor    : set_color, set_bg_color, set_cursor_blend, set_cursor_blink, set_cursor, bump_cursor, OnTimer
//   margin          : set_margin, client_size_for_grid
//   cursor geometry : blend_cursor_color, cursor_screen_pos, get_cursor_rect, is_hangul_mode
//   grid/output     : update_metrics, blank_row, reset_screen, line_at, clamp_cursor, erase_cells,
//                     scroll_lines_up/down, scroll_up_region, scroll_down_region, line_feed,
//                     insert_lines, delete_lines, insert_chars, delete_chars,
//                     enter_alt_screen, leave_alt_screen, put_char, print, scroll_to_bottom, update_scrollbar
//   VT parser       : Xterm256ToRgb(static), vt_feed, dispatch_csi
//   window messages : OnEraseBkgnd, OnSize, OnGetDlgCode, OnVScroll, OnMouseWheel
//   IME (Korean)    : OnImeStart, OnImeComp, OnImeEnd, OnImeNotify
//   input           : set_input_sink, set_resize_sink, send_input_bytes, send_input_wide,
//                     terminal_keydown, paste_clipboard, finalize_composition, OnChar, OnKeyDown
//   mouse selection : hit_test, copy_selection, clear_selection, OnLButtonDown, OnLButtonUp,
//                     OnMouseMove, OnRButtonDown
//   painting        : ensure_back_buffer, OnPaint
//   child (ConPTY)  : Utf8ToWide/CreatePtyPipes(static), start, write, resize, stop, is_running,
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
//   CharInfo{ wchar_t ch; COLORREF fg,bg; bool wide; }  wide = lead cell of a double-width glyph
//     (the next cell is its trail, ch=0).
//   scrollback: vector<Row>  lines pushed off the top (capped at MAX_SCROLLBACK, oldest dropped).
//   main_saved: vector<Row>  main screen backed up on alt-screen entry (swapped back on leave).
//   Key state: cur_row/cur_col (0-based screen cell cursor), view_top (top of view, unified index),
//     cursor_visible (?25), saved_row/col (DECSC/RC), scroll_top/bot (DECSTBM region),
//     alt_active/saved_main_* (alt screen), cols/rows, cell_w/h/base, margin_*, glyph_level,
//     vt_state/params/priv/gtlt/space (parser; persists across chunked print()), cur_fg/bg/bold/reverse (SGR),
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
//     M=RI, [=CSI, ]=OSC) / CSI (accumulate params, ?=priv, <=>=gtlt, ' '=space (DECSCUSR), final byte
//     -> dispatch_csi) /
//     OSC (discard up to BEL/ST).
//   dispatch_csi: drops the whole sequence if gtlt (private <>= sequence; avoids final-byte misparse).
//     Otherwise cursor moves (CUU/CUD/CUF/CUB/CUP/HVP/CHA/VPA), erase ED/EL/ECH, SGR (m), modes h/l
//     (?25, ?1049/47, ?1048, ?1, ?2004), save/restore (s/u), full-screen (DECSTBM r, IL/DL, ICH/DCH,
//     SU/SD), queries (DSR n -> CPR/status to input sink, DA c -> VT102).
//   put_char: autowrap past width, swap colors when reverse, wide glyph sets the next cell to trail.
//     line_feed: scroll_up_region at the region bottom (scroll_bot), else advance one line.
//   Scroll workers: scroll_lines_up/down (pure rotation within a range), scroll_up_region (preserves
//     displaced lines into scrollback only on the main screen with top==0), enter/leave_alt_screen
//     (swap with main_saved).
//
// [FONT] Font / cell size (calc_cell_size; full algorithm in that function's comment)
//   Fixed cell_w/h/base from font metrics. cell_w = ceil(max(2*w_e, w_k)/2) where w_e/w_k are the
//   width-ratio-adjusted English/Korean natural widths. English font is built to cell_w, Korean to
//   2*cell_w for final rendering. Match mode (set_kfont size<=0, default): Korean height matched to
//   English; off (size>0): Korean keeps its own height.
//
// [PAINT] Drawing (OnPaint, double buffered)
//   Draw a whole frame into a memory DC then BitBlt (no flicker). Walk rows lines from view_top via
//   line_at, cell by cell (wide = 2 cells in kfont; trail/blank cells skip the glyph), TA_BASELINE
//   aligned. Block/box chars are painted directly by DrawBlockElement/DrawBoxLine per glyph_level.
//   Cursor: when cursor_on && cursor_visible, fill get_cursor_rect with blend_cursor_color (2 cells in
//   Korean mode, 1 in English) and redraw the covered glyph(s) on top. child_caret (the child paints
//   the cursor cell as reverse = its bg differs from neighbors) suppresses the ConBox block (avoids a
//   double cursor).
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
//   (-> reset_screen). UTF-8 split across an output chunk boundary can corrupt a glyph (rare). Korean
//   IME order / cursor width cannot be auto-captured -> verify by real typing (Learned.md).
// ===== END DESIGN OVERVIEW =====

#include "ConBox.h"
#include <string>
#include <imm.h>            // Korean IME (Input Method Manager)
#pragma comment(lib, "imm32.lib")

// Some SDK configurations expose CreatePseudoConsole/HPCON but not this attribute macro, so define it
// if missing (ProcThreadAttributePseudoConsole=22 | PROC_THREAD_ATTRIBUTE_INPUT(0x00020000); a fixed
// value across SDKs).
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE_HANDLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE_HANDLE 0x00020016
#endif

static const UINT_PTR CURSOR_TIMER = 1;
// Child output polling timer (the one start() sets). 3 to stay distinct from CURSOR(1)/DBG(2).
static const UINT_PTR PUMP_TIMER = 3;
static const UINT PUMP_INTERVAL_MS = 16;   // output poll interval; 16~30 keeps the view smooth

// VT default colors: where SGR 0 (reset) and 39/49 return to (also the Requirements default). Tuned to
// look like wt.exe's Campbell scheme on 90% acrylic (light-gray text on dark-gray, not pure black).
static const COLORREF DEFAULT_FG = RGB(200, 200, 200);
static const COLORREF DEFAULT_BG = RGB(32, 32, 32);

enum { VT_GROUND = 0, VT_ESC = 1, VT_CSI = 2, VT_OSC = 3 };

static const int MAX_SCROLLBACK = 5000;

// ===== [DEBUG] Korean/cursor render log+capture (on removal delete this block and the // [DEBUG] callers) =====
// Harness enable switch: 0 = off (default), 1 = on (capture + state log on input/output). Set to 1 and
// rebuild to enable. (Child I/O dump is a separate switch, DBG_IO, below.)
#define DBG_HARNESS 0
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
// One-shot timer for the deferred (echo-settle) capture. 2 to avoid clashing with CURSOR_TIMER(1).
static const UINT_PTR DBG_TIMER = 2;
static void DbgLog(const char* fmt, ...)
{
	FILE* fp = nullptr;
	if (fopen_s(&fp, "D:\\Work\\Study\\ConBox\\Temp\\condbg.log", "ab") != 0 || fp == nullptr)
		return;
	SYSTEMTIME st; GetLocalTime(&st);
	fprintf(fp, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	va_list ap; va_start(ap, fmt); vfprintf(fp, fmt, ap); va_end(ap);
	fputc('\n', fp);
	fclose(fp);
}
// Find the PNG encoder CLSID (standard GDI+ pattern).
static int DbgGetEncoderClsid(const WCHAR* mime, CLSID* clsid)
{
	UINT num = 0, size = 0;
	Gdiplus::GetImageEncodersSize(&num, &size);
	if (size == 0) return -1;
	std::vector<BYTE> buf(size);
	Gdiplus::ImageCodecInfo* info = (Gdiplus::ImageCodecInfo*)buf.data();
	Gdiplus::GetImageEncoders(num, size, info);
	for (UINT i = 0; i < num; ++i)
		if (wcscmp(info[i].MimeType, mime) == 0) { *clsid = info[i].Clsid; return (int)i; }
	return -1;
}
// Save an HBITMAP (the back buffer) straight to PNG. GDI+ is lazily started once.
static void DbgWritePng(HBITMAP hbmp, const char* path)
{
	if (hbmp == nullptr) return;
	static bool inited = false;
	static ULONG_PTR token = 0;
	if (!inited) {
		Gdiplus::GdiplusStartupInput input;
		if (Gdiplus::GdiplusStartup(&token, &input, NULL) != Gdiplus::Ok) return;
		inited = true;
	}
	WCHAR wpath[300];
	MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, 300);
	Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromHBITMAP(hbmp, NULL);
	if (bmp != nullptr) {
		CLSID clsid;
		if (DbgGetEncoderClsid(L"image/png", &clsid) >= 0)
			bmp->Save(wpath, &clsid, NULL);
		delete bmp;
	}
}

// [DEBUG] I/O dump enable switch: 0 = off (default), 1 = on (dump child IN/OUT bytes to conexe_io.log).
// Set to 1 and rebuild to enable. (Screen capture is the DBG_HARNESS switch above.)
#define DBG_IO 0

// [DEBUG] Dump child I/O bytes in human-readable form to the Temp log (ESC/CR/LF/control as markers),
// to eyeball the cursor control sequences the child sends. With DBG_IO 0 the body is not compiled, so
// calls are no-ops.
static void DbgDumpIo(const char* tag, const char* data, int len)
{
#if DBG_IO
	FILE* fp = nullptr;
	if (fopen_s(&fp, "D:\\Work\\Study\\ConBox\\Temp\\conexe_io.log", "ab") != 0 || fp == nullptr)
		return;
	fprintf(fp, "[%s %d] ", tag, len);
	for (int i = 0; i < len; ++i) {
		unsigned char c = (unsigned char)data[i];
		if (c == 0x1B)      fputs("<ESC>", fp);
		else if (c == 0x0D) fputs("<CR>", fp);
		else if (c == 0x0A) fputs("<LF>\n", fp);   // also emit a real newline for readability
		else if (c == 0x7F) fputs("<DEL>", fp);
		else if (c < 0x20)  fprintf(fp, "<%02X>", c);
		else                fputc(c, fp);
	}
	fputs("\n----\n", fp);
	fclose(fp);
#else
	(void)tag; (void)data; (void)len;
#endif
}
// ===== [DEBUG] end =====

BEGIN_MESSAGE_MAP(CConBox, CWnd)
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
	ON_WM_SIZE()
	ON_WM_CHAR()
	ON_WM_KEYDOWN()
	ON_WM_GETDLGCODE()
	ON_WM_VSCROLL()
	ON_WM_MOUSEWHEEL()
	ON_WM_TIMER()
	ON_WM_DESTROY()
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONUP()
	ON_WM_MOUSEMOVE()
	ON_WM_RBUTTONDOWN()
	ON_MESSAGE(WM_IME_STARTCOMPOSITION, &CConBox::OnImeStart)
	ON_MESSAGE(WM_IME_COMPOSITION, &CConBox::OnImeComp)
	ON_MESSAGE(WM_IME_ENDCOMPOSITION, &CConBox::OnImeEnd)
	ON_MESSAGE(WM_IME_NOTIFY, &CConBox::OnImeNotify)
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

// Convert a font name/size + opts string to a LOGFONTW. (Helper for make_font.)
// opts grammar: a number applies to the attribute letter that follows it.
//   B=Bold (with a number = lfWeight), I=Italic, U=Underline, S=Strikeout,
//   Q=Quality (0..6), W=Width ratio percent (100=default).
// dpi_y converts points to pixel height (usually 96). width_pct returns the width ratio.
static LOGFONTW ParseFontOpts(const char* name, float size_pt, const char* opts, int dpi_y, int& width_pct)
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

	// Parse opts left to right; a run of digits becomes the value of the attribute letter that follows.
	int num = 0;
	bool has_num = false;
	for (const char* p = opts; p != nullptr && *p != '\0'; ++p) {
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

CConBox::CConBox()
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

	view_top = 0;
	cur_row = 0;
	cur_col = 0;
	cursor_visible = true;
	saved_row = 0;
	saved_col = 0;

	// Scroll region starts as the whole screen; scroll_bot is set to rows-1 in reset_screen/update_metrics.
	scroll_top = 0;
	scroll_bot = 0;

	alt_active = false;
	saved_main_row = 0;
	saved_main_col = 0;

	vt_state = VT_GROUND;
	vt_nparam = 0;
	vt_priv = false;
	vt_gtlt = false;
	vt_space = false;
	ime_committed = false;
	cur_bold = false;
	cur_reverse = false;

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

	cursor_bg_weight = 4;
	cursor_fg_weight = 6;

	efont_width_pct = 100;
	kfont_width_pct = 100;

	adjust_h = 1.0f;
	adjust_w = 1.0f;

	glyph_level = 1;

	// Cursor blink: start visible; interval follows the system caret rate. If GetCaretBlinkTime returns
	// INFINITE (blink disabled system-wide), use 0 = no blink, always on.
	cursor_on = true;
	UINT caret = ::GetCaretBlinkTime();
	cursor_blink_ms = (caret == INFINITE) ? 0 : (int)caret;
	cursor_type = 1;   // blink block (default until set_cursor is called)

	ZeroMemory(&efont_lf, sizeof(efont_lf));
	ZeroMemory(&kfont_lf, sizeof(kfont_lf));

	sel_active = false;
	selecting = false;
	sel_anchor_row = 0;
	sel_anchor_col = 0;
	sel_end_row = 0;
	sel_end_col = 0;

	// Double-buffer cache is created on the first OnPaint.
	back_old_bmp = nullptr;
	back_w = 0;
	back_h = 0;

	// [DEBUG] Recording follows the DBG_HARNESS switch (default off). When dbg_record is false,
	// dbg_dump and the settle timer (DBG_TIMER) do nothing.
	dbg_record = (DBG_HARNESS != 0);
	dbg_seq = 0;
	dbg_input_seen = false;
}

CConBox::~CConBox()
{
	// Tear down child/PTY/polling if still up (OnDestroy usually handles it first; this is a safety net).
	stop();

	// Restore the original bitmap so back_bmp is not destroyed while still selected into back_dc.
	if (back_old_bmp)
		back_dc.SelectObject(back_old_bmp);
}

void CConBox::open(CWnd* parent, int x0, int y0, int x1, int y1)
{
	// Register a dedicated window class. No background brush (we paint it). I-Beam cursor for text.
	// CS_HREDRAW|CS_VREDRAW so a resize repaints the whole control.
	LPCTSTR cls = AfxRegisterWndClass(
		CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS,
		::LoadCursor(nullptr, IDC_IBEAM),
		nullptr,
		nullptr);

	CRect rc(x0, y0, x1, y1);
	CreateEx(0, cls, _T(""),
		WS_CHILD | WS_VISIBLE | WS_VSCROLL,
		rc, parent, 0);

	// Fonts are fixed now (defaults if the host set none), so compute cell size and grid.
	apply_default_fonts();
	calc_cell_size();
	update_metrics();
	update_scrollbar();

	// The window exists now, so start the blink timer. Interval 0 (system blink off) = always on.
	cursor_on = true;
	if (cursor_blink_ms > 0)
		SetTimer(CURSOR_TIMER, cursor_blink_ms, NULL);
}

void CConBox::make_font(CFont& font, LOGFONTW& lf_out, int& width_pct, const char* name, float size, const char* opts)
{
	HDC hdc = ::GetDC(NULL);
	int dpi = ::GetDeviceCaps(hdc, LOGPIXELSY);
	::ReleaseDC(NULL, hdc);

	lf_out = ParseFontOpts(name, size, opts, dpi, width_pct);
	font.DeleteObject();
	font.CreateFontIndirectW(&lf_out);
}

void CConBox::apply_default_fonts()
{
	// Defaults: English Cascadia Mono 12pt normal, Korean Malgun Gothic normal (wt.exe Claude profile).
	// Korean size 0 = match-English-height mode (kfont_match_efont=true in the ctor), consistent with
	// the "size<=0 means match" rule.
	if (efont.GetSafeHandle() == NULL)
		make_font(efont, efont_lf, efont_width_pct, "Cascadia Mono", 12, "");
	if (kfont.GetSafeHandle() == NULL)
		make_font(kfont, kfont_lf, kfont_width_pct, "Malgun Gothic", 0, "");
}

// (design: [FONT])
void CConBox::calc_cell_size()
{
	BOOL has_wnd = ::IsWindow(m_hWnd);
	HWND meas_wnd = has_wnd ? m_hWnd : NULL;
	HDC hdc = ::GetDC(meas_wnd);

	// 1. Measure the English font in its natural state (lfWidth = 0).
	LOGFONTW elf = efont_lf;
	elf.lfWidth = 0;
	efont.DeleteObject();
	efont.CreateFontIndirectW(&elf);

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
		kfont.CreateFontIndirectW(&klf);
		::SelectObject(hdc, kfont.GetSafeHandle());
		::GetTextMetricsW(hdc, &tm);
		int kh0 = tm.tmHeight + tm.tmExternalLeading;
		if (kh0 > 0 && kh0 != eh) {
			LONG newh = (LONG)((double)klf.lfHeight * (double)eh / (double)kh0);
			if (newh == 0) newh = klf.lfHeight;
			klf.lfHeight = newh;
			::SelectObject(hdc, old);
			kfont.DeleteObject();
			kfont.CreateFontIndirectW(&klf);
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
		kfont.CreateFontIndirectW(&klf);
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
		efont.CreateFontIndirectW(&elf);
	}

	if (kfont_width_pct != 100) {
		klf.lfWidth = (int)(w_k / 2.0f + 0.5f);
		kfont.DeleteObject();
		kfont.CreateFontIndirectW(&klf);
	}

	// Line height/baseline take the larger of the two fonts to avoid clipping.
	cell_h = (eh > kh) ? eh : kh;
	cell_base = (ea > ka) ? ea : ka;

	// Apply the adjust_* scale ratios.
	cell_w = (int)(cell_w * adjust_w + 0.5f);
	cell_h = (int)(cell_h * adjust_h + 0.5f);
	if (cell_w < 1) cell_w = 1;
	if (cell_h < 1) cell_h = 1;

	::SelectObject(hdc, old);
	::ReleaseDC(meas_wnd, hdc);
}

void CConBox::set_efont(const char* name, float size, const char* opts)
{
	make_font(efont, efont_lf, efont_width_pct, name, size, opts);

	// Cell size changed; recompute the grid if the window is up.
	if (::IsWindow(m_hWnd)) {
		calc_cell_size();
		update_metrics();
	}
}

void CConBox::set_kfont(const char* name, float size, const char* opts)
{
	make_font(kfont, kfont_lf, kfont_width_pct, name, size, opts);

	// size<=0 turns on match mode (size ignored); positive turns it off. Match mode changes line
	// height (cell_h), so the grid is recomputed below.
	kfont_match_efont = (size <= 0.0f);

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

void CConBox::adjust(float h_ratio, float w_ratio)
{
	// Stored ratios persist until the next font change.
	adjust_h = h_ratio;
	adjust_w = w_ratio;

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

void CConBox::set_builtin_glyphs(int level)
{
	if (level < 0) level = 0;
	if (level > 2) level = 2;
	glyph_level = level;

	// Only the draw method changed; a repaint is enough (no grid recompute).
	if (::IsWindow(m_hWnd))
		Invalidate();
}

void CConBox::set_color(COLORREF fg)
{
	default_fg = fg;
	cur_fg = fg;
}

void CConBox::set_bg_color(COLORREF bg)
{
	default_bg = bg;
	cur_bg = bg;
}

void CConBox::set_cursor_blend(int bg_weight, int fg_weight)
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

void CConBox::set_cursor_blink(int interval_ms)
{
	if (interval_ms < 0)
		interval_ms = 0;
	cursor_blink_ms = interval_ms;

	// Restart the timer. Interval 0 = no blink, always on.
	if (::IsWindow(m_hWnd)) {
		KillTimer(CURSOR_TIMER);
		cursor_on = true;
		if (cursor_blink_ms > 0)
			SetTimer(CURSOR_TIMER, cursor_blink_ms, NULL);
		Invalidate();
	}
}

void CConBox::set_cursor(int type)
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

void CConBox::bump_cursor()
{
	// Make the cursor visible now and restart the timer so it stays solid for a beat after input/move
	// (so no off-frame lands right then). Fixed (even) types never blink, so leave their timer alone.
	cursor_on = true;
	if (::IsWindow(m_hWnd) && cursor_blink_ms > 0 && (cursor_type & 1) == 1) {
		KillTimer(CURSOR_TIMER);
		SetTimer(CURSOR_TIMER, cursor_blink_ms, NULL);
	}
}

void CConBox::OnTimer(UINT_PTR id)
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
	if (id == PUMP_TIMER) {
		// Child (ConPTY) output polling. start() set this; no child means no timer.
		pump();
		return;
	}
	if (id == DBG_TIMER) {   // [DEBUG] capture the settled screen once ~80ms after output stops
		KillTimer(DBG_TIMER);
		dbg_dump("out");
		return;
	}
	CWnd::OnTimer(id);
}

// [DEBUG] Save the current back buffer (exactly what is shown) to PNG and log cursor/composition state.
void CConBox::dbg_dump(const char* tag)
{
	if (!dbg_record) return;
	// A non-"out" capture is an input event; enable output-settle captures only after the first input
	// (skip the child's boot output as noise).
	if (strcmp(tag, "out") != 0)
		dbg_input_seen = true;
	if (GetSafeHwnd() != nullptr)
		UpdateWindow();   // flush any pending paint into the back buffer before capturing
	++dbg_seq;
	char path[300];
	sprintf_s(path, sizeof(path), "D:\\Work\\Study\\ConBox\\Temp\\cap_%03d_%s.png", dbg_seq, tag);

	// Crop to the cursor row (full width, +-1 row) so the PNG stays small (easier to read). Width stays
	// full so long Korean input fits; only height is narrowed. Fall back to the whole buffer if the
	// cursor position is unavailable (rare).
	CRect cur;
	if (get_cursor_rect(cur) && back_w > 0 && back_h > 0) {
		int left   = 0;
		int right  = back_w;
		int top    = cur.top    - cell_h;
		int bottom = cur.bottom + cell_h;
		if (top < 0) top = 0;
		if (bottom > back_h) bottom = back_h;
		int cw = right - left, ch = bottom - top;
		if (cw > 0 && ch > 0) {
			CDC crop_dc;
			crop_dc.CreateCompatibleDC(&back_dc);
			CBitmap crop_bmp;
			crop_bmp.CreateCompatibleBitmap(&back_dc, cw, ch);
			CBitmap* old = crop_dc.SelectObject(&crop_bmp);
			crop_dc.BitBlt(0, 0, cw, ch, &back_dc, left, top, SRCCOPY);
			crop_dc.SelectObject(old);
			DbgWritePng((HBITMAP)crop_bmp.GetSafeHandle(), path);
		}
	}
	else {
		DbgWritePng((HBITMAP)back_bmp.GetSafeHandle(), path);
	}

	// Also log the composing string (comp_str) as UTF-8 and codepoints.
	char comp_utf8[128] = { 0 };
	if (!comp_str.empty())
		WideCharToMultiByte(CP_UTF8, 0, comp_str.c_str(), (int)comp_str.size(),
			comp_utf8, sizeof(comp_utf8) - 1, NULL, NULL);
	char cps[160] = { 0 }; int off = 0;
	for (size_t i = 0; i < comp_str.size() && off < (int)sizeof(cps) - 8; ++i)
		off += sprintf_s(cps + off, sizeof(cps) - off, "U+%04X ", (unsigned)comp_str[i]);

	DbgLog("[seq=%03d %s] cur=(%d,%d) on=%d vis=%d hangul=%d comp=\"%s\" %s",
		dbg_seq, tag, cur_row, cur_col, cursor_on ? 1 : 0, cursor_visible ? 1 : 0,
		is_hangul_mode() ? 1 : 0, comp_utf8, cps);
}

void CConBox::set_margin(int top, int left, int bottom, int right)
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

void CConBox::client_size_for_grid(int cols, int rows, int& w, int& h) const
{
	// Client area = glyph area (cols x cell_w, rows x cell_h) plus the margins. cell_w/cell_h are
	// fixed in open() from font/DPI.
	if (cols < 0) cols = 0;
	if (rows < 0) rows = 0;
	w = cols * cell_w + margin_left + margin_right;
	h = rows * cell_h + margin_top + margin_bottom;
}

// Cursor block color: mix default bg and fg per channel at cursor_bg_weight:cursor_fg_weight. The
// default 6:4 is near-bg so a glyph drawn over the block in fg stays legible. Independent of SGR colors.
COLORREF CConBox::blend_cursor_color() const
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

bool CConBox::cursor_screen_pos(int& row_out, int& vx_out) const
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

bool CConBox::get_cursor_rect(CRect& rc) const
{
	int row, vx;
	if (!cursor_screen_pos(row, vx))
		return false;
	if (row < 0 || row >= rows)
		return false;

	// Block cursor filling the cell. In Korean input mode (even before composing) it is 2 cells wide;
	// English mode 1 cell.
	int cw = is_hangul_mode() ? cell_w * 2 : cell_w;

	int px = margin_left + vx * cell_w;
	int py = margin_top + row * cell_h;
	rc.SetRect(px, py, px + cw, py + cell_h);
	return true;
}

// Whether the IME is in Korean (jamo) mode: ImmGetConversionStatus's IME_CMODE_NATIVE flag. Correct
// right after a Korean/English toggle (before composition starts).
bool CConBox::is_hangul_mode() const
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

void CConBox::update_metrics()
{
	int old_cols = cols, old_rows = rows;

	CRect rc;
	GetClientRect(&rc);

	int avail_w = rc.Width() - margin_left - margin_right;
	int avail_h = rc.Height() - margin_top - margin_bottom;
	cols = (cell_w > 0) ? (avail_w / cell_w) : 1;
	rows = (cell_h > 0) ? (avail_h / cell_h) : 1;
	if (cols < 1) cols = 1;
	if (rows < 1) rows = 1;

	reset_screen();

	// If the grid actually changed and a resize sink is set, notify it (to sync the child PTY size).
	if ((cols != old_cols || rows != old_rows) && resize_sink != nullptr)
		resize_sink(cols, rows, resize_sink_user);
}

Row CConBox::blank_row() const
{
	// Filled with the current SGR background so erase/scroll follow the background color.
	CharInfo c;
	c.ch = L' ';
	c.fg = cur_fg;
	c.bg = cur_bg;
	c.wide = false;
	return Row(cols < 1 ? 1 : cols, c);
}

void CConBox::reset_screen()
{
	// Fit screen to the current rows x cols. Existing lines are kept but padded/truncated to cols and
	// the row count adjusted to rows (cell-level pad/truncate only; no fine reflow).
	CharInfo blank;
	blank.ch = L' ';
	blank.fg = cur_fg;
	blank.bg = cur_bg;
	blank.wide = false;

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
const Row& CConBox::line_at(int idx) const
{
	// Unified index: scrollback first, then the current screen.
	int sb = (int)scrollback.size();
	if (idx < sb)
		return scrollback[idx];
	return screen[idx - sb];
}

void CConBox::clamp_cursor()
{
	if (cur_row < 0) cur_row = 0;
	if (cur_row > rows - 1) cur_row = rows - 1;
	if (cur_col < 0) cur_col = 0;
	if (cur_col > cols) cur_col = cols;   // cols = "just past line end" (wraps on the next glyph)
}

void CConBox::erase_cells(int row, int c0, int c1)
{
	if (row < 0 || row >= (int)screen.size())
		return;
	Row& line = screen[row];
	if (c0 < 0) c0 = 0;
	if (c1 > (int)line.size()) c1 = (int)line.size();
	for (int c = c0; c < c1; ++c) {
		line[c].ch = L' ';
		line[c].fg = cur_fg;
		line[c].bg = cur_bg;
		line[c].wide = false;
	}
}

void CConBox::scroll_lines_up(int top, int bot, int n)
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

void CConBox::scroll_lines_down(int top, int bot, int n)
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

void CConBox::scroll_up_region(int n)
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

		int over = (int)scrollback.size() - MAX_SCROLLBACK;
		if (over > 0)
			scrollback.erase(scrollback.begin(), scrollback.begin() + over);
	}

	scroll_lines_up(scroll_top, scroll_bot, n);
}

void CConBox::scroll_down_region(int n)
{
	// RI/SD. Does not touch scrollback.
	scroll_lines_down(scroll_top, scroll_bot, n);
}

void CConBox::line_feed()
{
	// At the region bottom, scroll the region up (cursor stays on that line); otherwise advance one
	// line without leaving the screen.
	if (cur_row == scroll_bot)
		scroll_up_region(1);
	else if (cur_row < rows - 1)
		cur_row++;
}

void CConBox::insert_lines(int n)
{
	// IL: only when the cursor is inside the scroll region. Insert n blank lines at the cursor line,
	// pushing lines below toward scroll_bot. Cursor moves to column 1.
	if (cur_row < scroll_top || cur_row > scroll_bot)
		return;
	if (n < 1) n = 1;
	scroll_lines_down(cur_row, scroll_bot, n);
	cur_col = 0;
}

void CConBox::delete_lines(int n)
{
	// DL: only inside the scroll region. Delete n lines from the cursor line, pulling lines up and
	// filling blanks at the region bottom. Cursor moves to column 1.
	if (cur_row < scroll_top || cur_row > scroll_bot)
		return;
	if (n < 1) n = 1;
	scroll_lines_up(cur_row, scroll_bot, n);
	cur_col = 0;
}

void CConBox::insert_chars(int n)
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
	blank.ch = L' '; blank.fg = cur_fg; blank.bg = cur_bg; blank.wide = false;
	line.insert(line.begin() + c, n, blank);
	line.resize(cols, blank);   // drop overflow to keep line width = cols
}

void CConBox::delete_chars(int n)
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
	blank.ch = L' '; blank.fg = cur_fg; blank.bg = cur_bg; blank.wide = false;
	line.erase(line.begin() + c, line.begin() + c + n);
	line.resize(cols, blank);   // refill the end to keep line width = cols
}

void CConBox::enter_alt_screen()
{
	// Back up the main screen and cursor, switch to a blank alt screen. Scrollback remains but is
	// frozen (hidden) while alt_active.
	if (alt_active)
		return;
	main_saved.swap(screen);          // back up main (screen left empty)
	saved_main_row = cur_row;
	saved_main_col = cur_col;

	screen.assign(rows, blank_row());
	cur_row = 0;
	cur_col = 0;
	scroll_top = 0;
	scroll_bot = rows - 1;
	alt_active = true;
}

void CConBox::leave_alt_screen()
{
	// Restore the backed-up main screen and cursor.
	if (!alt_active)
		return;
	screen.swap(main_saved);
	main_saved.clear();
	alt_active = false;

	// Fit the restored main screen to the current cols/rows (a resize may have happened); restore cursor/region.
	cur_row = saved_main_row;
	cur_col = saved_main_col;
	scroll_top = 0;
	scroll_bot = rows - 1;
	reset_screen();
}

// (design: [VT])
void CConBox::put_char(wchar_t wc)
{
	// Write one glyph at the cursor, autowrapping first if it would exceed cols. A wide glyph goes in
	// the lead cell and sets the next cell to trail (ch=0).
	if (cur_row < 0 || cur_row >= (int)screen.size())
		return;

	int w = IsWideChar(wc) ? 2 : 1;
	if (cur_col + w > cols) {
		cur_col = 0;
		line_feed();
	}

	// reverse swaps fg/bg into the cell.
	COLORREF fg = cur_reverse ? cur_bg : cur_fg;
	COLORREF bg = cur_reverse ? cur_fg : cur_bg;

	Row& line = screen[cur_row];
	if (cur_col >= 0 && cur_col < (int)line.size()) {
		line[cur_col].ch = wc;
		line[cur_col].fg = fg;
		line[cur_col].bg = bg;
		line[cur_col].wide = (w == 2);
		if (w == 2 && cur_col + 1 < (int)line.size()) {
			line[cur_col + 1].ch = 0;   // trail cell (the lead draws both)
			line[cur_col + 1].fg = fg;
			line[cur_col + 1].bg = bg;
			line[cur_col + 1].wide = false;
		}
	}
	cur_col += w;
}

void CConBox::print(const char* text)
{
	if (text == nullptr)
		return;

	int wlen = ::MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
	if (wlen <= 1)
		return;   // empty or conversion failure

	std::vector<wchar_t> ws(wlen);
	::MultiByteToWideChar(CP_UTF8, 0, text, -1, ws.data(), wlen);

	// Create the screen if it does not exist yet (before font/window is fixed).
	if (screen.empty())
		reset_screen();

	// Feed each char to the VT parser (skip the trailing null terminator).
	for (int i = 0; i < wlen - 1; ++i)
		vt_feed(ws[i]);

	// New output invalidates any selection (cell positions moved); clear it, scroll to bottom, repaint.
	sel_active = false;
	scroll_to_bottom();
	update_scrollbar();
	bump_cursor();
	Invalidate();

	// [DEBUG] Restart the 80ms settle timer on each output; if no more output arrives OnTimer leaves
	// one "out" capture (coalesces consecutive chunks). Nothing before the first input (boot).
	if (dbg_record && dbg_input_seen && GetSafeHwnd() != nullptr)
		SetTimer(DBG_TIMER, 80, NULL);
}

void CConBox::scroll_to_bottom()
{
	int total = (int)scrollback.size() + rows;
	int maxtop = total - rows;
	view_top = (maxtop > 0) ? maxtop : 0;
}

// Convert an xterm 256-color index to COLORREF: 0-15 base, 16-231 6x6x6 cube, 232-255 grayscale.
static COLORREF Xterm256ToRgb(int n)
{
	static const COLORREF base16[16] = {
		RGB(0, 0, 0),       RGB(205, 0, 0),     RGB(0, 205, 0),     RGB(205, 205, 0),
		RGB(0, 0, 238),     RGB(205, 0, 205),   RGB(0, 205, 205),   RGB(229, 229, 229),
		RGB(127, 127, 127), RGB(255, 0, 0),     RGB(0, 255, 0),     RGB(255, 255, 0),
		RGB(92, 92, 255),   RGB(255, 0, 255),   RGB(0, 255, 255),   RGB(255, 255, 255)
	};
	if (n < 0) n = 0;
	if (n > 255) n = 255;
	if (n < 16)
		return base16[n];
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
void CConBox::vt_feed(wchar_t wc)
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
		if (wc == L'7') { saved_row = cur_row; saved_col = cur_col; vt_state = VT_GROUND; return; }   // DECSC
		if (wc == L'8') { cur_row = saved_row; cur_col = saved_col; clamp_cursor(); vt_state = VT_GROUND; return; }  // DECRC
		if (wc == L'M') {   // RI: at the region top scroll down, else move up one line
			if (cur_row == scroll_top) scroll_down_region(1);
			else if (cur_row > 0) cur_row--;
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
void CConBox::dispatch_csi(wchar_t fin)
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
			cur_bold = false; cur_reverse = false;
			break;
		}
		for (int i = 0; i < vt_nparam; ++i) {
			int p = vt_params[i];
			if (p == 0) { cur_fg = default_fg; cur_bg = default_bg; cur_bold = false; cur_reverse = false; }
			else if (p == 1) cur_bold = true;
			else if (p == 22) cur_bold = false;
			else if (p == 7) cur_reverse = true;
			else if (p == 27) cur_reverse = false;
			else if (p >= 30 && p <= 37) cur_fg = Xterm256ToRgb((cur_bold ? 8 : 0) + (p - 30));
			else if (p >= 40 && p <= 47) cur_bg = Xterm256ToRgb(p - 40);
			else if (p >= 90 && p <= 97) cur_fg = Xterm256ToRgb(8 + (p - 90));
			else if (p >= 100 && p <= 107) cur_bg = Xterm256ToRgb(8 + (p - 100));
			else if (p == 39) cur_fg = default_fg;
			else if (p == 49) cur_bg = default_bg;
			else if (p == 38 || p == 48) {
				// Extended color: 38;5;n (256) or 38;2;r;g;b (truecolor). 48 = background.
				COLORREF col = (p == 38) ? cur_fg : cur_bg;
				if (i + 1 < vt_nparam && vt_params[i + 1] == 5) {
					if (i + 2 < vt_nparam) col = Xterm256ToRgb(vt_params[i + 2]);
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
				if (set) { saved_row = cur_row; saved_col = cur_col; }
				else { cur_row = saved_row; cur_col = saved_col; clamp_cursor(); }
			}
			// Other private modes (?7 autowrap is always on, etc.) ignored.
		}
		break;
	}
	case L's': saved_row = cur_row; saved_col = cur_col; break;                 // save cursor
	case L'u': cur_row = saved_row; cur_col = saved_col; clamp_cursor(); break; // restore cursor

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

void CConBox::update_scrollbar()
{
	if (!::IsWindow(m_hWnd))
		return;

	// On the alt screen, scrollback is frozen/hidden: hide the bar and pin the view to the screen start.
	if (alt_active) {
		ShowScrollBar(SB_VERT, FALSE);
		view_top = (int)scrollback.size();
		return;
	}

	int total = (int)scrollback.size() + rows;
	if (total <= rows) {
		// No scrollback (screen only): hide the bar, pin to top.
		ShowScrollBar(SB_VERT, FALSE);
		view_top = 0;
		return;
	}

	// Content overflows: show the bar and set range/page/pos.
	ShowScrollBar(SB_VERT, TRUE);

	SCROLLINFO si;
	ZeroMemory(&si, sizeof(si));
	si.cbSize = sizeof(si);
	si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
	si.nMin = 0;
	si.nMax = total - 1;       // last screen-line index
	si.nPage = rows;
	si.nPos = view_top;
	SetScrollInfo(SB_VERT, &si, TRUE);
}

BOOL CConBox::OnEraseBkgnd(CDC* dc)
{
	// Background is erased in OnPaint to avoid flicker; return TRUE to block the default erase.
	return TRUE;
}

void CConBox::OnSize(UINT type, int cx, int cy)
{
	CWnd::OnSize(type, cx, cy);

	update_metrics();

	int maxtop = (int)scrollback.size();
	if (maxtop < 0) maxtop = 0;
	if (view_top > maxtop) view_top = maxtop;

	update_scrollbar();
	Invalidate();
}

UINT CConBox::OnGetDlgCode()
{
	// Take arrows/Tab/Enter/chars directly so a parent dialog does not intercept them.
	return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB;
}

void CConBox::OnVScroll(UINT code, UINT pos, CScrollBar* sb)
{
	// Scrollback view is frozen on the alt screen; ignore scroll input.
	if (alt_active)
		return;

	int maxtop = (int)scrollback.size();
	if (maxtop < 0) maxtop = 0;

	int nt = view_top;
	switch (code) {
	case SB_LINEUP:   nt -= 1;    break;
	case SB_LINEDOWN: nt += 1;    break;
	case SB_PAGEUP:   nt -= rows; break;
	case SB_PAGEDOWN: nt += rows; break;
	case SB_TOP:      nt = 0;      break;
	case SB_BOTTOM:   nt = maxtop; break;
	case SB_THUMBTRACK:
	case SB_THUMBPOSITION: {
		// Read the actual drag position.
		SCROLLINFO si;
		ZeroMemory(&si, sizeof(si));
		si.cbSize = sizeof(si);
		si.fMask = SIF_TRACKPOS;
		GetScrollInfo(SB_VERT, &si);
		nt = si.nTrackPos;
		break;
	}
	default: break;
	}

	if (nt < 0) nt = 0;
	if (nt > maxtop) nt = maxtop;
	if (nt != view_top) {
		view_top = nt;
		update_scrollbar();
		Invalidate();
	}
}

BOOL CConBox::OnMouseWheel(UINT flags, short zDelta, CPoint pt)
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
		Invalidate();
	}
	return TRUE;
}

// ===== Mouse drag selection / clipboard =====
// hit_test: client pixel coords -> unified (scrollback+screen) row index + cell column. Coords outside
// the margins clamp to the edge cell.
void CConBox::hit_test(CPoint pt, int& abs_row, int& col) const
{
	int total = (int)scrollback.size() + rows;

	int r = (pt.y - margin_top) / cell_h;  // screen row (relative to view_top)
	if (r < 0) r = 0;
	if (r >= rows) r = rows - 1;
	abs_row = view_top + r;
	if (abs_row < 0) abs_row = 0;
	if (abs_row >= total) abs_row = total - 1;

	int c = (pt.x - margin_left) / cell_w;
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
void CConBox::copy_selection()
{
	if (!sel_active) return;

	int total = (int)scrollback.size() + rows;

	// Order anchor..end into (r0,c0)..(r1,c1) (anchor may be later).
	int r0 = sel_anchor_row, c0 = sel_anchor_col;
	int r1 = sel_end_row, c1 = sel_end_col;
	if (r0 > r1 || (r0 == r1 && c0 > c1)) {
		std::swap(r0, r1); std::swap(c0, c1);
	}

	std::wstring text;
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

void CConBox::clear_selection()
{
	if (sel_active) {
		sel_active = false;
		Invalidate();
	}
}

void CConBox::OnLButtonDown(UINT flags, CPoint pt)
{
	// Finalize any IME composition first (a click is also a composition-ending trigger).
	finalize_composition();
	ime_committed = false;   // a click is not an arrow; don't let it enable the arrow correction

	sel_active = false;

	// Start drag: anchor and end at the same cell.
	hit_test(pt, sel_anchor_row, sel_anchor_col);
	sel_end_row = sel_anchor_row;
	sel_end_col = sel_anchor_col;
	selecting = true;

	SetCapture();   // keep tracking outside the window
	Invalidate();
}

void CConBox::OnLButtonUp(UINT flags, CPoint pt)
{
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

void CConBox::OnMouseMove(UINT flags, CPoint pt)
{
	if (!selecting) return;

	hit_test(pt, sel_end_row, sel_end_col);
	sel_active = true;
	Invalidate();
}

void CConBox::OnRButtonDown(UINT flags, CPoint pt)
{
	finalize_composition();
	ime_committed = false;   // a click is not an arrow; don't let it enable the arrow correction
	clear_selection();
	// Paste the clipboard to the child stdin.
	paste_clipboard();
}

LRESULT CConBox::OnImeStart(WPARAM w, LPARAM l)
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
			cf.ptCurrentPos.x = margin_left + vx * cell_w;
			cf.ptCurrentPos.y = margin_top + row * cell_h;
			ImmSetCompositionWindow(himc, &cf);

			// Candidate list just below the cursor.
			CANDIDATEFORM caf;
			ZeroMemory(&caf, sizeof(caf));
			caf.dwIndex = 0;
			caf.dwStyle = CFS_CANDIDATEPOS;
			caf.ptCurrentPos.x = margin_left + vx * cell_w;
			caf.ptCurrentPos.y = margin_top + (row + 1) * cell_h;
			ImmSetCandidateWindow(himc, &caf);

			ImmSetCompositionFontW(himc, &kfont_lf);
		}
		ImmReleaseContext(m_hWnd, himc);
	}
	bump_cursor();
	dbg_dump("imestart");   // [DEBUG]
	// ConBox draws the composing glyph (OnImeComp stores comp_str, OnPaint draws it in the cursor cell).
	// Above only positions the candidate list; system inline is not used.
	return Default();
}

// (design: [IME])
LRESULT CConBox::OnImeComp(WPARAM w, LPARAM l)
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
		rc.right = margin_left + cols * cell_w + cell_w;
		InvalidateRect(rc);
	}
	bump_cursor();
	dbg_dump("ime");   // [DEBUG]
	// Consume the message to suppress system inline display and the committed part's WM_CHAR/WM_IME_CHAR dup.
	return 0;
}

LRESULT CConBox::OnImeEnd(WPARAM w, LPARAM l)
{
	// Composition ended: clear our composing display (comp_str) and repaint. The committed part was
	// already sent in OnImeComp (GCS_RESULTSTR) and is drawn by the child's echo.
	comp_str.clear();
	// Invalidate to the line end to undo the rightward shift drawn during composition.
	CRect rc;
	if (get_cursor_rect(rc)) {
		rc.right = margin_left + cols * cell_w + cell_w;
		InvalidateRect(rc);
	}
	bump_cursor();
	dbg_dump("imeend");   // [DEBUG]
	return Default();
}

LRESULT CConBox::OnImeNotify(WPARAM w, LPARAM l)
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

void CConBox::set_input_sink(void (*sink)(const char* bytes, int len, void* user), void* user)
{
	input_sink = sink;
	input_sink_user = user;
}

void CConBox::set_resize_sink(void (*sink)(int cols, int rows, void* user), void* user)
{
	resize_sink = sink;
	resize_sink_user = user;
}

void CConBox::send_input_bytes(const char* bytes, int len)
{
	if (input_sink != nullptr && bytes != nullptr && len > 0)
		input_sink(bytes, len, input_sink_user);
}

void CConBox::send_input_wide(const wchar_t* ws, int n)
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
bool CConBox::terminal_keydown(UINT vk, bool ctrl, bool shift)
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

void CConBox::paste_clipboard()
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

bool CConBox::finalize_composition()
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

void CConBox::OnChar(UINT ch, UINT rep, UINT flags)
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
		dbg_dump("char");   // [DEBUG]
		return;
	}

	// Read-only viewer (no input sink): no local edit/echo.
}

void CConBox::OnKeyDown(UINT vk, UINT rep, UINT flags)
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
			if (vk != VK_PROCESSKEY) dbg_dump("key");   // [DEBUG]
			return;
		}
		if (committed && plain && vk == VK_LEFT)
			terminal_keydown(VK_LEFT, false, false);    // extra Left offsets the commit's right advance

		if (terminal_keydown(vk, tctrl, tshift))
			bump_cursor();
		// [DEBUG] A key handled by the IME (VK_PROCESSKEY) is already captured in OnImeComp; skip to
		// avoid a duplicate.
		if (vk != VK_PROCESSKEY) dbg_dump("key");
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
		Invalidate();
	}
}

// Lazily prepare the double-buffer memory DC/bitmap. The DC is created once and reused; the bitmap is
// rebuilt only when the client size changes (avoids a CreateCompatibleDC/Bitmap per frequent repaint
// such as cursor blink).
void CConBox::ensure_back_buffer(CDC* ref, int w, int h)
{
	if (back_dc.GetSafeHdc() == nullptr)
		back_dc.CreateCompatibleDC(ref);

	// Rebuild the bitmap if missing or the size changed.
	if (back_bmp.GetSafeHandle() == nullptr || w != back_w || h != back_h) {
		// Detach the old bitmap (restore the original) before discarding it.
		if (back_old_bmp) {
			back_dc.SelectObject(back_old_bmp);
			back_old_bmp = nullptr;
		}
		back_bmp.DeleteObject();

		back_bmp.CreateCompatibleBitmap(ref, w, h);
		back_old_bmp = back_dc.SelectObject(&back_bmp);
		back_w = w;
		back_h = h;
	}
}

// (design: [PAINT]/[IME])
void CConBox::OnPaint()
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

	// Draw visible lines top to bottom (unified index view_top+row).
	int total = (int)scrollback.size() + rows;

	// Pre-sort the selection range if active (anchor/end are stored unordered).
	int sr0 = 0, sc0 = 0, sr1 = -1, sc1 = -1;   // sr1 < sr0 means no selection
	if (sel_active) {
		sr0 = sel_anchor_row; sc0 = sel_anchor_col;
		sr1 = sel_end_row;    sc1 = sel_end_col;
		if (sr0 > sr1 || (sr0 == sr1 && sc0 > sc1)) {
			std::swap(sr0, sr1); std::swap(sc0, sc1);
		}
	}

	for (int row = 0; row < rows; ++row) {
		int idx = view_top + row;
		if (idx < 0 || idx >= total)
			break;

		const Row& line = line_at(idx);

		int ncell = (int)line.size();
		if (ncell > cols) ncell = cols;

		// The cell index (i) is the visual column. A wide glyph fills its trail cell and skips it.
		for (int i = 0; i < ncell; ++i) {
			const CharInfo& c = line[i];
			int w = c.wide ? 2 : 1;

			int px = margin_left + i * cell_w;
			int py = margin_top + row * cell_h;

			COLORREF fg = c.fg;
			COLORREF bg = c.bg;

			// Inside the selection, swap fg/bg for the inverted look.
			if (sel_active && idx >= sr0 && idx <= sr1) {
				bool in_sel = false;
				if (sr0 == sr1)
					in_sel = (i >= sc0 && i <= sc1);       // same line
				else if (idx == sr0)
					in_sel = (i >= sc0);                    // first line: from start
				else if (idx == sr1)
					in_sel = (i <= sc1);                    // last line: to end
				else
					in_sel = true;                          // middle line: all
				if (in_sel) std::swap(fg, bg);
			}

			// Fill the cell background (2 cells together if wide).
			CRect cell(px, py, px + w * cell_w, py + cell_h);
			dc.FillSolidRect(cell, bg);

			// Blank cells and wide trail cells (ch=0) draw no glyph. Others draw as a shape (block/box)
			// or via the font (wide uses the Korean font).
			if (c.ch != 0 && c.ch != L' ') {
				bool drawn = false;
				if (glyph_level >= 1)
					drawn = DrawBlockElement(dc, c.ch, px, py, cell_w, cell_h, fg);
				if (!drawn && glyph_level >= 2)
					drawn = DrawBoxLine(dc, c.ch, px, py, cell_w, cell_h, fg);
				if (!drawn) {
					dc.SelectObject(c.wide ? &kfont : &efont);
					dc.SetTextColor(fg);
					dc.TextOutW(px, py + cell_base, &c.ch, 1);
				}
			}

			if (w == 2) ++i;   // skip the trail cell
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
			int row_right = margin_left + cols * cell_w;
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
					dc.TextOutW(x, cur.top + cell_base, &wc, 1);
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
	else if (cursor_on && cursor_visible) {
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
					int w = cc.wide ? 2 : 1;
					if (cc.ch != 0 && cc.ch != L' ') {
						dc.SelectObject(cc.wide ? &kfont : &efont);
						bool cdrawn = (glyph_level >= 1 && DrawBlockElement(dc, cc.ch, x, cur.top, cell_w, cell_h, cur_fg))
						           || (glyph_level >= 2 && DrawBoxLine(dc, cc.ch, x, cur.top, cell_w, cell_h, cur_fg));
						if (!cdrawn)
							dc.TextOutW(x, cur.top + cell_base, &cc.ch, 1);
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

	// Blit the finished frame to the screen at once (no flicker). The bitmap stays a live member (freed
	// in the dtor), so it is not detached here.
	paintDC.BitBlt(0, 0, rc.Width(), rc.Height(), &back_dc, 0, 0, SRCCOPY);
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

// (design: [PTY])
bool CConBox::start(const char* cmdline)
{
	// Restart if already running.
	stop();

	// Size the child console to the current grid (always matched).
	int cols = grid_cols();
	int rows = grid_rows();
	if (cols < 1) cols = 1;
	if (rows < 1) rows = 1;

	// Pipes + pseudo console. The PTY-side ends are closed after spawn.
	HANDLE pty_in_read = nullptr, pty_out_write = nullptr;
	if (!CreatePtyPipes(cols, rows, h_pc, in_write, out_read, pty_in_read, pty_out_write)) {
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
	set_input_sink(&CConBox::child_input_thunk, this);
	set_resize_sink(&CConBox::child_resize_thunk, this);

	// Output polling timer on ConBox's own window. start() must be called after open() (the timer needs
	// the window).
	SetTimer(PUMP_TIMER, PUMP_INTERVAL_MS, NULL);

	child_running = true;
	return true;
}

void CConBox::write(const char* data, int len)
{
	if (in_write == nullptr || data == nullptr || len <= 0)
		return;
	DbgDumpIo("IN", data, len);   // [DEBUG] logged only when DBG_IO is 1
	DWORD written = 0;
	::WriteFile(in_write, data, (DWORD)len, &written, NULL);
}

void CConBox::child_input_thunk(const char* bytes, int len, void* user)
{
	CConBox* self = (CConBox*)user;
	if (self != nullptr)
		self->write(bytes, len);
}

void CConBox::resize(int cols, int rows)
{
	// Only resize while the pseudo console is alive. The child detects the size change and repaints.
	if (h_pc == nullptr || !child_running)
		return;
	if (cols < 1) cols = 1;
	if (rows < 1) rows = 1;
	COORD size;
	size.X = (SHORT)cols;
	size.Y = (SHORT)rows;
	::ResizePseudoConsole(h_pc, size);
}

void CConBox::child_resize_thunk(int cols, int rows, void* user)
{
	CConBox* self = (CConBox*)user;
	if (self != nullptr)
		self->resize(cols, rows);
}

void CConBox::set_exit_callback(void (*cb)(void* user), void* user)
{
	exit_cb = cb;
	exit_cb_user = user;
}

void CConBox::stop()
{
	// Kill the polling timer first (so pump is no longer called). KillTimer only while the window lives.
	if (GetSafeHwnd() != nullptr)
		KillTimer(PUMP_TIMER);

	// Release the input/resize sinks start() wired to itself (with no child, input is dropped).
	if (input_sink == &CConBox::child_input_thunk)
		set_input_sink(nullptr, nullptr);
	if (resize_sink == &CConBox::child_resize_thunk)
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

bool CConBox::is_running() const
{
	return child_running;
}

void CConBox::pump()
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
		buf[got] = '\0';   // print() expects null-terminated UTF-8
		DbgDumpIo("OUT", buf, (int)got);   // [DEBUG] logged only when DBG_IO is 1
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

void CConBox::handle_child_exit()
{
	// The child exited naturally. Grab the callback info first, then clean up, then call the callback
	// with cleanup done (is_running()==false) so it may immediately start() again without conflict.
	void (*cb)(void*) = exit_cb;
	void* user = exit_cb_user;
	stop();
	if (cb != nullptr)
		cb(user);
}

void CConBox::OnDestroy()
{
	// Clean up child/polling timer before the window is destroyed (so polling never touches a dying window).
	stop();
	CWnd::OnDestroy();
}
