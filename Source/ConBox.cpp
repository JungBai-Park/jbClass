// ConBox.cpp
//
// CConBox 의 구현. 사용법만 알면 되는 경우 ConBox.h 만 읽으면 충분하다.
// 이 파일은 동작을 "수정"할 때만 본다. 아래 목차로 필요한 함수만 찾아(Grep) 부분만 읽는다.
//
// === 파일 구성 (대략 이 순서) ===
//   보조(static)   : IsWideChar, ParseFontOpts(폰트 opts 파서),
//                    DrawBlockElement(블록 요소 직접 칠), DrawBoxLine(직선/정션/대각선/이중선 직접 칠)
//   메시지 맵      : BEGIN_MESSAGE_MAP ... END_MESSAGE_MAP
//   창/폰트        : open, make_font, apply_default_fonts, calc_cell_size,
//                    set_efont, set_kfont, set_kfont_fill, set_builtin_glyphs
//   색상/커서 설정 : set_color, set_bg_color, set_cursor_blend, set_cursor_blink,
//                    bump_cursor, OnTimer
//   여백           : set_margin, client_size_for_grid
//   커서 기하      : blend_cursor_color, cursor_screen_pos, get_cursor_rect, is_hangul_mode
//   그리드/출력    : update_metrics, blank_row, reset_screen, line_at, clamp_cursor, erase_cells,
//                    scroll_lines_up/down, scroll_up_region, scroll_down_region, line_feed,
//                    insert_lines, delete_lines, insert_chars, delete_chars,
//                    enter_alt_screen, leave_alt_screen, put_char, print, scroll_to_bottom, update_scrollbar
//   VT 파서        : Xterm256ToRgb(static), vt_feed, dispatch_csi
//   창 메시지      : OnEraseBkgnd, OnSize, OnGetDlgCode, OnVScroll, OnMouseWheel
//   IME(한글)      : OnImeStart, OnImeComp, OnImeEnd, OnImeNotify
//   입력           : set_input_sink, set_resize_sink, send_input_bytes, send_input_wide,
//                    terminal_keydown, paste_clipboard, finalize_composition, OnChar, OnKeyDown
//   그리기         : ensure_back_buffer, OnPaint

#include "ConBox.h"
#include <string>
#include <imm.h>            // 한글 IME (Input Method Manager)
#pragma comment(lib, "imm32.lib")

// 커서 깜빡임 타이머 ID.
static const UINT_PTR CURSOR_TIMER = 1;

// VT 기본 색. SGR 0(리셋)과 39/49 가 복귀하는 색이며, 요구사양 기본값(흰 글자/검은 바탕)과 같다.
static const COLORREF DEFAULT_FG = RGB(255, 255, 255);
static const COLORREF DEFAULT_BG = RGB(0, 0, 0);

// VT 파서 상태값. (vt_state)
enum { VT_GROUND = 0, VT_ESC = 1, VT_CSI = 2, VT_OSC = 3 };

// 스크롤백 줄 수 상한.
static const int MAX_SCROLLBACK = 5000;

// 메시지 맵: 그리기/배경/크기/키보드/스크롤/IME 를 처리한다.
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
	ON_MESSAGE(WM_IME_STARTCOMPOSITION, &CConBox::OnImeStart)
	ON_MESSAGE(WM_IME_COMPOSITION, &CConBox::OnImeComp)
	ON_MESSAGE(WM_IME_ENDCOMPOSITION, &CConBox::OnImeEnd)
	ON_MESSAGE(WM_IME_NOTIFY, &CConBox::OnImeNotify)
END_MESSAGE_MAP()

// 한 글자가 2배 폭(한글, CJK 등)인지 판정한다.
// 화면 폭 계산과 렌더링에서 영문은 1칸, 이 범위의 글자는 2칸으로 다룬다.
static bool IsWideChar(wchar_t ch)
{
	return (ch >= 0x1100 && ch <= 0x115F)   // 한글 자모
		|| (ch >= 0x2E80 && ch <= 0x303E)   // CJK 부수, 한중일 기호
		|| (ch >= 0x3041 && ch <= 0x33FF)   // 히라가나/가타카나/한글 호환자모 등
		|| (ch >= 0x3400 && ch <= 0x4DBF)   // CJK 확장 A
		|| (ch >= 0x4E00 && ch <= 0x9FFF)   // CJK 통합 한자
		|| (ch >= 0xA000 && ch <= 0xA4CF)   // 이 등
		|| (ch >= 0xAC00 && ch <= 0xD7A3)   // 한글 완성형 음절
		|| (ch >= 0xF900 && ch <= 0xFAFF)   // CJK 호환 한자
		|| (ch >= 0xFF00 && ch <= 0xFF60)   // 전각 영숫자/기호
		|| (ch >= 0xFFE0 && ch <= 0xFFE6);  // 전각 통화기호 등
}

// 폰트 추가설정 문자열(opts)과 이름/크기를 LOGFONTW 로 변환한다. (make_font 전용 보조)
//
// name    : 폰트 이름 (UTF-8)
// size_pt : 글자 크기 (포인트)
// opts    : 추가설정 문자열. B=Bold, I=Italic, U=Underline, S=Strikeout, 숫자+W=Weight.
//           숫자는 바로 뒤에 오는 속성의 값(정도)으로 쓰인다.
// dpi_y   : 세로 방향 DPI (포인트를 픽셀 높이로 환산할 때 사용, 보통 96)
static LOGFONTW ParseFontOpts(const char* name, float size_pt, const char* opts, int dpi_y)
{
	LOGFONTW lf;
	ZeroMemory(&lf, sizeof(lf));

	// 포인트 크기를 픽셀 높이로 환산한다. (픽셀 = 포인트 x DPI / 72, 가장 가까운 정수로 반올림)
	// 소수점 포인트(예: 10.5)도 받지만 최종 높이는 정수 픽셀로 양자화된다.
	// 음수 높이는 글자 셀(em) 높이를 기준으로 한다는 의미이다.
	lf.lfHeight = -(LONG)(size_pt * dpi_y / 72.0f + 0.5f);
	lf.lfWeight = FW_NORMAL;
	lf.lfCharSet = DEFAULT_CHARSET;

	// 폰트 이름(UTF-8)을 와이드 문자로 변환해 채운다.
	if (name != nullptr)
		::MultiByteToWideChar(CP_UTF8, 0, name, -1, lf.lfFaceName, LF_FACESIZE);

	// 추가설정 문자열을 왼쪽부터 한 글자씩 해석한다.
	// 숫자가 나오면 누적해 두었다가 바로 뒤 속성의 값으로 사용한다.
	// 현재 값을 사용하는 속성은 W(Weight) 뿐이다.
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
		case 'B': case 'b': lf.lfWeight = FW_BOLD; break;
		case 'I': case 'i': lf.lfItalic = 1; break;
		case 'U': case 'u': lf.lfUnderline = 1; break;
		case 'S': case 's': lf.lfStrikeOut = 1; break;
		case 'W': case 'w': if (has_num) lf.lfWeight = num; break;
		default: break;   // 알 수 없는 기호는 무시한다.
		}

		// 한 속성을 처리했으면 누적 숫자를 비운다.
		num = 0;
		has_num = false;
	}

	return lf;
}

// 블록 요소 문자(U+2580~259F)를 폰트 글리프 대신 도형으로 칸 안에 직접 칠한다.
// 이 글자들은 칸을 정수분할한 사각형이라 4사분면(좌상/우상/좌하/우하) 채움 조합으로
// 정확히 표현된다. 폰트 글리프는 칸을 가로/세로로 꽉 채우지 않아 인접 칸 사이에 틈이
// 생기지만, 사분면을 직접 칠하면 경계가 맞물려 틈이 없다.
// ch 가 대상이면 fg 로 해당 사분면을 칠하고 true, 아니면 false 를 돌려준다.
// (부분 1/8 블록 2581~2587/2589~258F 와 음영 2591~2593 은 사분면으로 표현되지 않으므로
//  false 로 두어 폰트가 그리게 한다.)
static bool DrawBlockElement(CDC& dc, wchar_t ch, int px, int py, int cw, int chh, COLORREF fg)
{
	// 각 글자를 4비트 사분면 마스크로 바꾼다. (UL=1, UR=2, LL=4, LR=8)
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
	default:     return false;                 // 사분면으로 표현 못 하면 폰트에 맡긴다.
	}

	// 칸을 가로/세로 중앙에서 나눈다. 인접 칸도 같은 공식을 쓰므로 경계가 정확히 맞물린다.
	int midx = px + cw / 2;
	int midy = py + chh / 2;
	int rx = px + cw;    // 칸 오른쪽 끝
	int by = py + chh;   // 칸 아래쪽 끝

	if (mask & 1) dc.FillSolidRect(px,   py,   midx - px, midy - py, fg);  // 좌상
	if (mask & 2) dc.FillSolidRect(midx, py,   rx - midx, midy - py, fg);  // 우상
	if (mask & 4) dc.FillSolidRect(px,   midy, midx - px, by - midy, fg);  // 좌하
	if (mask & 8) dc.FillSolidRect(midx, midy, rx - midx, by - midy, fg);  // 우하
	return true;
}

// 직선/정션/대각선/이중선 박스 드로잉 문자를 폰트 대신 직접 그린다.
// 칸 중앙을 기준으로 선을 칠하며, 인접 칸이 같은 중앙 공식을 쓰므로 칸 경계에서 끊김 없이
// 이어진다. ch 가 대상이면 fg 로 칠하고 true, 아니면 false 를 돌려준다.
// 지원: 단선 직교/정션(─│┌┐└┘├┤┬┴┼), 둥근 모서리(╭╮╰╯, 직각으로 대체),
//       대각선(╱╲╳), 순수 이중선(═║╔╗╚╝╠╣╦╩╬).
// (단/이중 혼합선, 점선, 굵은선은 대상이 아니며 폰트에 맡긴다.)
static bool DrawBoxLine(CDC& dc, wchar_t ch, int px, int py, int cw, int chh, COLORREF fg)
{
	// 선 두께는 칸 높이에 비례한 근사값(최소 1px)이다.
	int t = chh / 12;
	if (t < 1) t = 1;

	int midx = px + cw / 2;
	int midy = py + chh / 2;
	int rx = px + cw;   // 칸 오른쪽 끝
	int by = py + chh;  // 칸 아래쪽 끝

	// 대각선은 사선이라 사각형으로 못 그리므로 펜으로 칸 모서리끼리 긋는다.
	// 칸 모서리 좌표를 끝점으로 써 인접 칸과 모서리에서 이어진다.
	if (ch == 0x2571 || ch == 0x2572 || ch == 0x2573) {
		CPen pen(PS_SOLID, t, fg);
		CPen* oldpen = dc.SelectObject(&pen);
		if (ch != 0x2572) { dc.MoveTo(px, by); dc.LineTo(rx, py); }  // ╱ (╳ 포함)
		if (ch != 0x2571) { dc.MoveTo(px, py); dc.LineTo(rx, by); }  // ╲ (╳ 포함)
		dc.SelectObject(oldpen);
		return true;
	}

	int top = midy - t / 2;   // 단선 가로띠 위쪽 y
	int lft = midx - t / 2;   // 단선 세로띠 왼쪽 x

	// 단선 직교/정션 + 둥근 모서리(직각으로 대체). 4방향 비트(N=1 위, E=2 오른쪽, S=4 아래, W=8 왼쪽).
	int dir = -1;
	switch (ch) {
	case 0x2500: dir = 2 | 8;         break;  // ─
	case 0x2502: dir = 1 | 4;         break;  // │
	case 0x250C: dir = 2 | 4;         break;  // ┌
	case 0x2510: dir = 4 | 8;         break;  // ┐
	case 0x2514: dir = 1 | 2;         break;  // └
	case 0x2518: dir = 1 | 8;         break;  // ┘
	case 0x251C: dir = 1 | 2 | 4;     break;  // ├
	case 0x2524: dir = 1 | 4 | 8;     break;  // ┤
	case 0x252C: dir = 2 | 4 | 8;     break;  // ┬
	case 0x2534: dir = 1 | 2 | 8;     break;  // ┴
	case 0x253C: dir = 1 | 2 | 4 | 8; break;  // ┼
	case 0x256D: dir = 2 | 4;         break;  // ╭ 좌상 -> ┌ (둥근 모서리를 직각으로)
	case 0x256E: dir = 4 | 8;         break;  // ╮ 우상 -> ┐
	case 0x256F: dir = 1 | 8;         break;  // ╯ 우하 -> ┘
	case 0x2570: dir = 1 | 2;         break;  // ╰ 좌하 -> └
	default:     break;
	}
	if (dir >= 0) {
		if (dir & 8) dc.FillSolidRect(px,   top,  midx - px, t,         fg);  // 왼쪽 가로
		if (dir & 2) dc.FillSolidRect(midx, top,  rx - midx, t,         fg);  // 오른쪽 가로
		if (dir & 1) dc.FillSolidRect(lft,  py,   t,         midy - py, fg);  // 위 세로
		if (dir & 4) dc.FillSolidRect(lft,  midy, t,         by - midy, fg);  // 아래 세로
		return true;
	}

	// 순수 이중선. 두 평행선을 중심에서 ±g 떨어뜨려 그린다. 코너/정션에서는 두 평행선이
	// ㄱ자로 닫히도록(바깥 평행선은 바깥 교차점, 안쪽 평행선은 안쪽 교차점까지) 각 획의
	// 중심측 끝을 맞춘다. 가로획은 top0(위)/top1(아래), 세로획은 lft0(좌)/lft1(우) 위치.
	int g = t + 1;
	int top0 = midy - g - t / 2;   // 가로 위획 y
	int top1 = midy + g - t / 2;   // 가로 아래획 y
	int lft0 = midx - g - t / 2;   // 세로 좌획 x
	int lft1 = midx + g - t / 2;   // 세로 우획 x

	switch (ch) {
	case 0x2550:  // ═  가로 이중선
		dc.FillSolidRect(px, top0, cw, t, fg);
		dc.FillSolidRect(px, top1, cw, t, fg);
		return true;
	case 0x2551:  // ║  세로 이중선
		dc.FillSolidRect(lft0, py, t, chh, fg);
		dc.FillSolidRect(lft1, py, t, chh, fg);
		return true;
	case 0x2554:  // ╔  E+S (좌상 코너)
		dc.FillSolidRect(midx - g, top0, rx - (midx - g), t, fg);  // 위가로 [midx-g, rx]
		dc.FillSolidRect(midx + g, top1, rx - (midx + g), t, fg);  // 아래가로 [midx+g, rx]
		dc.FillSolidRect(lft0, midy - g, t, by - (midy - g), fg);  // 좌세로 [midy-g, by]
		dc.FillSolidRect(lft1, midy + g, t, by - (midy + g), fg);  // 우세로 [midy+g, by]
		return true;
	case 0x2557:  // ╗  S+W (우상 코너)
		dc.FillSolidRect(px, top0, (midx + g) - px, t, fg);        // 위가로 [px, midx+g]
		dc.FillSolidRect(px, top1, (midx - g) - px, t, fg);        // 아래가로 [px, midx-g]
		dc.FillSolidRect(lft1, midy - g, t, by - (midy - g), fg);  // 우세로 [midy-g, by]
		dc.FillSolidRect(lft0, midy + g, t, by - (midy + g), fg);  // 좌세로 [midy+g, by]
		return true;
	case 0x255A:  // ╚  N+E (좌하 코너)
		dc.FillSolidRect(midx + g, top0, rx - (midx + g), t, fg);  // 위가로 [midx+g, rx]
		dc.FillSolidRect(midx - g, top1, rx - (midx - g), t, fg);  // 아래가로 [midx-g, rx]
		dc.FillSolidRect(lft0, py, t, (midy + g) - py, fg);        // 좌세로 [py, midy+g]
		dc.FillSolidRect(lft1, py, t, (midy - g) - py, fg);        // 우세로 [py, midy-g]
		return true;
	case 0x255D:  // ╝  N+W (우하 코너)
		dc.FillSolidRect(px, top0, (midx - g) - px, t, fg);        // 위가로 [px, midx-g]
		dc.FillSolidRect(px, top1, (midx + g) - px, t, fg);        // 아래가로 [px, midx+g]
		dc.FillSolidRect(lft0, py, t, (midy - g) - py, fg);        // 좌세로 [py, midy-g]
		dc.FillSolidRect(lft1, py, t, (midy + g) - py, fg);        // 우세로 [py, midy+g]
		return true;
	case 0x2560:  // ╠  N+E+S (좌측 T)
		dc.FillSolidRect(lft0, py, t, chh, fg);                    // 좌세로 full
		dc.FillSolidRect(lft1, py, t, chh, fg);                    // 우세로 full
		dc.FillSolidRect(midx + g, top0, rx - (midx + g), t, fg);  // 위가로 [midx+g, rx]
		dc.FillSolidRect(midx + g, top1, rx - (midx + g), t, fg);  // 아래가로 [midx+g, rx]
		return true;
	case 0x2563:  // ╣  N+S+W (우측 T)
		dc.FillSolidRect(lft0, py, t, chh, fg);                    // 좌세로 full
		dc.FillSolidRect(lft1, py, t, chh, fg);                    // 우세로 full
		dc.FillSolidRect(px, top0, (midx - g) - px, t, fg);        // 위가로 [px, midx-g]
		dc.FillSolidRect(px, top1, (midx - g) - px, t, fg);        // 아래가로 [px, midx-g]
		return true;
	case 0x2566:  // ╦  E+S+W (상단 T)
		dc.FillSolidRect(px, top0, cw, t, fg);                     // 위가로 full
		dc.FillSolidRect(px, top1, cw, t, fg);                     // 아래가로 full
		dc.FillSolidRect(lft0, midy + g, t, by - (midy + g), fg);  // 좌세로 [midy+g, by]
		dc.FillSolidRect(lft1, midy + g, t, by - (midy + g), fg);  // 우세로 [midy+g, by]
		return true;
	case 0x2569:  // ╩  N+E+W (하단 T)
		dc.FillSolidRect(px, top0, cw, t, fg);                     // 위가로 full
		dc.FillSolidRect(px, top1, cw, t, fg);                     // 아래가로 full
		dc.FillSolidRect(lft0, py, t, (midy - g) - py, fg);        // 좌세로 [py, midy-g]
		dc.FillSolidRect(lft1, py, t, (midy - g) - py, fg);        // 우세로 [py, midy-g]
		return true;
	case 0x256C:  // ╬  N+E+S+W (십자, 가운데 비움)
		dc.FillSolidRect(px, top0, cw, t, fg);                     // 위가로 full
		dc.FillSolidRect(px, top1, cw, t, fg);                     // 아래가로 full
		dc.FillSolidRect(lft0, py, t, chh, fg);                    // 좌세로 full
		dc.FillSolidRect(lft1, py, t, chh, fg);                    // 우세로 full
		return true;
	default:
		return false;
	}
}

CConBox::CConBox()
{
	// 색상 기본값: 검은 바탕에 흰 글씨.
	cur_fg = RGB(255, 255, 255);
	cur_bg = RGB(0, 0, 0);

	// 셀 크기는 폰트 측정 전까지 쓰일 안전한 임시값으로 둔다.
	cell_w = 8;
	cell_h = 16;
	cell_base = 12;

	// 기본적으로 줄 높이를 영문 폰트 기준으로 잡고 한글 높이를 거기에 맞춘다.
	kfont_match_efont = true;

	cols = 1;
	rows = 1;

	// 안쪽 여백 기본값은 사방 10 픽셀.
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

	// 스크롤 영역은 화면 전체로 시작한다. scroll_bot 은 reset_screen/update_metrics 에서 rows-1 로 맞춰진다.
	scroll_top = 0;
	scroll_bot = 0;

	// 대체 화면은 처음엔 비활성(주 화면).
	alt_active = false;
	saved_main_row = 0;
	saved_main_col = 0;

	// VT 파서 초기 상태(GROUND)와 SGR 속성.
	vt_state = VT_GROUND;
	vt_nparam = 0;
	vt_priv = false;
	vt_gtlt = false;
	cur_bold = false;
	cur_reverse = false;

	// 입력 모드 기본값: 일반 커서 키(앱 모드 꺼짐), 붙여넣기 감싸기 꺼짐. 자식이 켠다.
	app_cursor_keys = false;
	bracketed_paste = false;

	// 입력 싱크는 기본적으로 없다(읽기 전용 뷰어). ConExe 등이 set_input_sink 로 등록한다.
	input_sink = nullptr;
	input_sink_user = nullptr;

	// 리사이즈 싱크도 기본적으로 없다. ConExe 가 attach() 에서 set_resize_sink 로 등록한다.
	resize_sink = nullptr;
	resize_sink_user = nullptr;

	// 블록 커서 색 혼합 비율 기본값: 배경:전경 = 6:4 (배경에 가까운 중간색).
	cursor_bg_weight = 6;
	cursor_fg_weight = 4;

	// match 모드에서 한 칸 폭(장평)을 정하는 비율 기본값.
	kfill_ratio = 0.92f;
	emin_ratio = 0.7f;

	// 박스/블록 문자 직접 렌더는 기본적으로 블록 요소만 켠다(로고 등의 칸 틈 방지).
	glyph_level = 1;

	// 커서 깜빡임. 표시 상태는 일단 켜 두고, 간격은 시스템 캐럿 깜빡임 속도를 따른다.
	// GetCaretBlinkTime 이 INFINITE 를 돌려주면(시스템에서 깜빡임을 꺼 둔 경우) 0 으로
	// 두어 깜빡이지 않고 항상 켜 둔다.
	cursor_on = true;
	UINT caret = ::GetCaretBlinkTime();
	cursor_blink_ms = (caret == INFINITE) ? 0 : (int)caret;

	ZeroMemory(&efont_lf, sizeof(efont_lf));
	ZeroMemory(&kfont_lf, sizeof(kfont_lf));

	// 더블 버퍼링 캐시는 첫 OnPaint 에서 만든다.
	back_old_bmp = nullptr;
	back_w = 0;
	back_h = 0;
}

CConBox::~CConBox()
{
	// 메모리 DC 에 백버퍼 비트맵이 선택된 채로 CBitmap 이 파괴되지 않도록,
	// 원래 비트맵을 되돌려 놓는다. (CDC/CBitmap 멤버 자체는 자동 정리된다.)
	if (back_old_bmp)
		back_dc.SelectObject(back_old_bmp);
}

void CConBox::open(CWnd* parent, int x0, int y0, int x1, int y1)
{
	// 전용 윈도우 클래스를 등록한다.
	// 배경 브러시는 직접 그리므로 지정하지 않고(NULL),
	// 마우스 커서는 텍스트 편집에 어울리는 I-Beam 으로 설정한다.
	// 크기가 바뀌면 전체를 다시 그리도록 CS_HREDRAW | CS_VREDRAW 를 준다.
	LPCTSTR cls = AfxRegisterWndClass(
		CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS,
		::LoadCursor(nullptr, IDC_IBEAM),
		nullptr,
		nullptr);

	// 세로 스크롤바를 가진 자식 창으로 생성한다.
	CRect rc(x0, y0, x1, y1);
	CreateEx(0, cls, _T(""),
		WS_CHILD | WS_VISIBLE | WS_VSCROLL,
		rc, parent, 0);

	// 사용자가 폰트를 지정하지 않았다면 기본 폰트를 채우고,
	// 폰트가 정해졌으니 고정 그리드 한 칸의 크기와 화면 칸 수를 계산한다.
	apply_default_fonts();
	calc_cell_size();
	update_metrics();
	update_scrollbar();

	// 윈도우 핸들이 만들어졌으니 커서 깜빡임 타이머를 시작한다.
	// 간격이 0 이면(시스템에서 깜빡임을 끈 경우 등) 타이머 없이 항상 켜 둔다.
	cursor_on = true;
	if (cursor_blink_ms > 0)
		SetTimer(CURSOR_TIMER, cursor_blink_ms, NULL);
}

void CConBox::make_font(CFont& font, LOGFONTW& lf_out, const char* name, float size, const char* opts)
{
	// 포인트->픽셀 환산에 쓸 화면 DPI 를 구한다.
	HDC hdc = ::GetDC(NULL);
	int dpi = ::GetDeviceCaps(hdc, LOGPIXELSY);
	::ReleaseDC(NULL, hdc);

	lf_out = ParseFontOpts(name, size, opts, dpi);
	font.DeleteObject();
	font.CreateFontIndirectW(&lf_out);
}

void CConBox::apply_default_fonts()
{
	// 기본값: 영문 Consolas 13pt Bold, 한글 맑은 고딕 Bold.
	// 한글 크기는 0(=영문 높이에 맞추는 match 모드, 생성자에서 kfont_match_efont=true)으로
	// 두어 "size<=0 이면 match" 규칙과 일관되게 한다.
	if (efont.GetSafeHandle() == NULL)
		make_font(efont, efont_lf, "Consolas", 13, "B");
	if (kfont.GetSafeHandle() == NULL)
		make_font(kfont, kfont_lf, "Malgun Gothic", 0, "B");
}

void CConBox::calc_cell_size()
{
	// 창이 있으면 창 DC, 없으면 화면 DC 로 폰트 크기를 측정한다.
	BOOL has_wnd = ::IsWindow(m_hWnd);
	HWND meas_wnd = has_wnd ? m_hWnd : NULL;
	HDC hdc = ::GetDC(meas_wnd);

	// 먼저 영문 폰트를 사용자 원본(efont_lf, 자연 폭)으로 되돌려 자연 크기를 잰다.
	// (이전 호출에서 장평이 적용돼 있을 수 있으므로 매번 원본부터 측정한다.)
	efont.DeleteObject();
	efont.CreateFontIndirectW(&efont_lf);

	HGDIOBJ old = ::SelectObject(hdc, efont.GetSafeHandle());
	TEXTMETRICW tm;
	::GetTextMetricsW(hdc, &tm);
	SIZE sz;
	::GetTextExtentPoint32W(hdc, L"W", 1, &sz);
	int natw = sz.cx;                 // 영문 자연 글자폭
	cell_w = natw;
	int eh = tm.tmHeight + tm.tmExternalLeading;
	int ea = tm.tmAscent;

	if (kfont_match_efont) {
		// 한글 자간이 넓어 보이는 문제를 줄인다.
		// 핵심: 한글은 가로로 찌그러뜨리지 않고(자연 폭 유지, 압축하면 획이 깨진다)
		// 높이만 영문에 맞춘다. 그런 다음 그 한글 폭에 맞춰 한 칸 폭(cell_w)을 좁혀,
		// 한글이 두 칸을 거의 채우게 한다. 영문은 그 cell_w 로 장평(약간 좁아짐)한다.

		// 한글 높이 맞춤: 영문과 같은 em 으로 만들어 실제 높이를 재고, 영문 높이(eh)와
		// 같아지도록 em 을 비례 보정한다. 폭은 자연 폭(lfWidth=0) 그대로 둔다.
		LOGFONTW lf = kfont_lf;
		lf.lfWidth = 0;
		lf.lfHeight = efont_lf.lfHeight;
		kfont.DeleteObject();
		kfont.CreateFontIndirectW(&lf);
		::SelectObject(hdc, kfont.GetSafeHandle());
		::GetTextMetricsW(hdc, &tm);
		int kh0 = tm.tmHeight + tm.tmExternalLeading;
		if (kh0 > 0 && kh0 != eh) {
			LONG newh = (LONG)((double)lf.lfHeight * (double)eh / (double)kh0);
			if (newh == 0) newh = lf.lfHeight;
			lf.lfHeight = newh;
			::SelectObject(hdc, old);
			kfont.DeleteObject();
			kfont.CreateFontIndirectW(&lf);
			::SelectObject(hdc, kfont.GetSafeHandle());
			::GetTextMetricsW(hdc, &tm);
		}

		// 높이를 맞춘 한글 한 글자의 자연 폭을 잰다.
		SIZE ksz;
		::GetTextExtentPoint32W(hdc, L"가", 1, &ksz);
		int kwid = ksz.cx;
		int kh = tm.tmHeight + tm.tmExternalLeading;
		int ka = tm.tmAscent;

		// 한 칸 폭을 한글 자연 폭 기준으로 정한다(두 칸이 한글의 kfill_ratio 만큼이 되도록).
		// 영문은 원본보다 넓히지 않고(<=natw), 가독성 하한(emin_ratio) 밑으로 좁히지도 않는다.
		int cw = (int)(kwid / (2.0 * kfill_ratio) + 0.5);
		int emin = (int)(natw * emin_ratio + 0.5);
		if (cw > natw) cw = natw;
		if (cw < emin) cw = emin;
		if (cw < 1) cw = 1;

		// 영문 장평: 위에서 정한 cw 로 폭만 좁혀 다시 만든다. (지금 hdc 에는 kfont 가 선택돼 있어
		// efont 는 선택돼 있지 않으므로 바로 삭제·재생성해도 된다.)
		LOGFONTW elf = efont_lf;
		elf.lfWidth = cw;
		efont.DeleteObject();
		efont.CreateFontIndirectW(&elf);
		::SelectObject(hdc, efont.GetSafeHandle());
		::GetTextExtentPoint32W(hdc, L"W", 1, &sz);
		cell_w = sz.cx;

		// 줄 높이/기준선은 두 폰트 중 큰 쪽을 택해 글자 잘림을 막는다(거의 영문과 같다).
		cell_h = (eh > kh) ? eh : kh;
		cell_base = (ea > ka) ? ea : ka;
	}
	else {
		// 옵션을 끈 경우: 영문은 위에서 원본으로 복원됨(cell_w = natw). 한글도 원본 크기로 만든다.
		LOGFONTW lf = kfont_lf;
		kfont.DeleteObject();
		kfont.CreateFontIndirectW(&lf);

		// 한글 폰트 높이와 기준선도 재서 두 폰트 중 큰 쪽(기준선은 큰 ascent)에 맞춘다.
		// 이렇게 하면 한글과 영문이 같은 기준선 위에 놓여 글자 바닥이 가지런해진다.
		::SelectObject(hdc, kfont.GetSafeHandle());
		::GetTextMetricsW(hdc, &tm);
		int kh = tm.tmHeight + tm.tmExternalLeading;
		int ka = tm.tmAscent;

		cell_h = (eh > kh) ? eh : kh;
		cell_base = (ea > ka) ? ea : ka;
	}

	::SelectObject(hdc, old);
	::ReleaseDC(meas_wnd, hdc);
}

void CConBox::set_efont(const char* name, float size, const char* opts)
{
	make_font(efont, efont_lf, name, size, opts);

	// 창이 이미 떠 있으면 칸 크기를 다시 재고 화면을 갱신한다.
	if (::IsWindow(m_hWnd)) {
		calc_cell_size();
		Invalidate();
	}
}

void CConBox::set_kfont(const char* name, float size, const char* opts)
{
	make_font(kfont, kfont_lf, name, size, opts);

	// size 가 0 이하이면 한글을 영문 높이에 맞추는 match 모드를 켜고(이때 size 는 무시된다),
	// 양수이면 끈다. match 모드는 줄 높이(cell_h)를 바꾸므로 아래에서 그리드를 다시 잡는다.
	kfont_match_efont = (size <= 0.0f);

	// 창이 떠 있으면 칸 크기(줄 높이)가 바뀌므로 그리드/줄바꿈을 다시 계산한다.
	if (::IsWindow(m_hWnd)) {
		calc_cell_size();
		update_metrics();

		// 스크롤 위치가 범위를 벗어나면 맞춰 준다.
		int maxtop = (int)scrollback.size();
		if (maxtop < 0) maxtop = 0;
		if (view_top > maxtop) view_top = maxtop;

		update_scrollbar();
		Invalidate();
	}
}

void CConBox::set_kfont_fill(float fill_ratio, float emin_floor)
{
	// 채움 비율은 양수여야 의미가 있고, 가독성 하한은 0~1 범위여야 한다. 벗어난 값은 무시한다.
	if (fill_ratio > 0.0f)
		kfill_ratio = fill_ratio;
	if (emin_floor >= 0.0f && emin_floor <= 1.0f)
		emin_ratio = emin_floor;

	// 창이 떠 있으면 칸 폭/줄 높이가 바뀔 수 있으므로 그리드/줄바꿈을 다시 계산한다.
	if (::IsWindow(m_hWnd)) {
		calc_cell_size();
		update_metrics();

		// 스크롤 위치가 범위를 벗어나면 맞춰 준다.
		int maxtop = (int)scrollback.size();
		if (maxtop < 0) maxtop = 0;
		if (view_top > maxtop) view_top = maxtop;

		update_scrollbar();
		Invalidate();
	}
}

void CConBox::set_builtin_glyphs(int level)
{
	// 범위를 벗어난 값은 가까운 끝으로 맞춘다. (0=끔, 1=블록요소, 2=+직선박스선)
	if (level < 0) level = 0;
	if (level > 2) level = 2;
	glyph_level = level;

	// 그리는 방식만 바뀌므로 칸 수/줄바꿈 재계산 없이 다시 그리기만 하면 된다.
	if (::IsWindow(m_hWnd))
		Invalidate();
}

void CConBox::set_color(COLORREF fg)
{
	cur_fg = fg;
}

void CConBox::set_bg_color(COLORREF bg)
{
	cur_bg = bg;
}

void CConBox::set_cursor_blend(int bg_weight, int fg_weight)
{
	// 음수는 0 으로 막고, 합이 0 이면(둘 다 0) 의미가 없으므로 무시한다.
	if (bg_weight < 0) bg_weight = 0;
	if (fg_weight < 0) fg_weight = 0;
	if (bg_weight + fg_weight <= 0)
		return;

	cursor_bg_weight = bg_weight;
	cursor_fg_weight = fg_weight;

	// 창이 있으면 커서 칸을 다시 그려 바뀐 색을 즉시 반영한다.
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

	// 창이 있으면 기존 타이머를 끄고 다시 건다. 간격이 0 이면 깜빡임 없이 항상 켜 둔다.
	if (::IsWindow(m_hWnd)) {
		KillTimer(CURSOR_TIMER);
		cursor_on = true;
		if (cursor_blink_ms > 0)
			SetTimer(CURSOR_TIMER, cursor_blink_ms, NULL);
		Invalidate();
	}
}

void CConBox::bump_cursor()
{
	// 커서를 즉시 보이게 하고, 타이머가 있으면 재시작해 입력/이동 직후 한 박자 동안은
	// 커서가 확실히 보이게 한다(이 시점에 마침 꺼지는 프레임이 걸리지 않도록).
	cursor_on = true;
	if (::IsWindow(m_hWnd) && cursor_blink_ms > 0) {
		KillTimer(CURSOR_TIMER);
		SetTimer(CURSOR_TIMER, cursor_blink_ms, NULL);
	}
}

void CConBox::OnTimer(UINT_PTR id)
{
	if (id == CURSOR_TIMER) {
		// 표시 상태를 뒤집고 커서 자리만 다시 그린다.
		cursor_on = !cursor_on;
		CRect rc;
		if (get_cursor_rect(rc))
			InvalidateRect(&rc, FALSE);
		return;
	}
	CWnd::OnTimer(id);
}

void CConBox::set_margin(int top, int left, int bottom, int right)
{
	// CSS 단축 스타일로 생략한 변을 채운다. left 를 먼저 결정해야 right 가 그 값을 따를 수 있다.
	if (left < 0) left = top;
	if (bottom < 0) bottom = top;
	if (right < 0) right = left;

	margin_top = top;
	margin_left = left;
	margin_bottom = bottom;
	margin_right = right;

	// open 이후라면 resize 와 마찬가지로 칸 수/줄 수와 줄바꿈을 다시 계산한다.
	if (::IsWindow(m_hWnd)) {
		update_metrics();

		// 스크롤 위치가 범위를 벗어나면 맞춰 준다.
		int maxtop = (int)scrollback.size();
		if (maxtop < 0) maxtop = 0;
		if (view_top > maxtop) view_top = maxtop;

		update_scrollbar();
		Invalidate();
	}
}

void CConBox::client_size_for_grid(int cols, int rows, int& w, int& h) const
{
	// 글자 영역은 (칸 수 x 한 칸 픽셀)이고, 그 둘레로 안쪽 여백이 더해진 것이
	// 클라이언트 영역이다. 한 칸 픽셀(cell_w/cell_h)은 open 에서 폰트/DPI 로 확정된다.
	if (cols < 0) cols = 0;
	if (rows < 0) rows = 0;
	w = cols * cell_w + margin_left + margin_right;
	h = rows * cell_h + margin_top + margin_bottom;
}

// 블록 커서 색을 계산한다.
// 배경색과 전경색을 채널별로 cursor_bg_weight : cursor_fg_weight 비율로 섞는다.
// 기본값(6:4)은 배경에 가까운 중간색이라, 커서가 칸을 채워도 그 위에 전경색으로
// 그린 글자가 잘 보인다. 비율은 set_cursor_blend 로 바꿀 수 있다.
COLORREF CConBox::blend_cursor_color() const
{
	int bw = cursor_bg_weight;
	int fw = cursor_fg_weight;
	int sum = bw + fw;
	if (sum <= 0)
		return cur_bg;

	int r = (GetRValue(cur_bg) * bw + GetRValue(cur_fg) * fw) / sum;
	int g = (GetGValue(cur_bg) * bw + GetGValue(cur_fg) * fw) / sum;
	int b = (GetBValue(cur_bg) * bw + GetBValue(cur_fg) * fw) / sum;
	return RGB(r, g, b);
}

bool CConBox::cursor_screen_pos(int& row_out, int& vx_out) const
{
	if (screen.empty())
		return false;

	// 커서의 통합 인덱스(scrollback 다음이 화면)에서 보기 맨 위(view_top)를 빼 화면 줄 번호를
	// 얻는다. 가로는 칸 좌표(cur_col)라 그대로 쓴다. (화면 밖 여부는 호출 측에서 row 로 판정)
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

	// 커서는 칸을 꽉 채우는 블록이다.
	// 한글 입력 모드이면 (아직 조합 전이라도) 한글 한 글자 폭에 맞춰
	// 두 칸 폭으로, 영문 모드면 한 칸 폭으로 채운다.
	int cw = is_hangul_mode() ? cell_w * 2 : cell_w;

	int px = margin_left + vx * cell_w;
	int py = margin_top + row * cell_h;
	rc.SetRect(px, py, px + cw, py + cell_h);
	return true;
}

// 지금 IME 가 한글 자모 입력 모드인지 조회한다.
// ImmGetConversionStatus 의 IME_CMODE_NATIVE 플래그가 켜져 있으면 한글 모드다.
// 한/영 키로 모드만 바꾼 직후(조합 시작 전)에도 올바른 값을 돌려준다.
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
	// 클라이언트 크기에서 안쪽 여백을 뺀 영역을 한 칸 크기로 나누어
	// 가로 칸 수와 세로 줄 수를 구한다.
	int old_cols = cols, old_rows = rows;

	CRect rc;
	GetClientRect(&rc);

	int avail_w = rc.Width() - margin_left - margin_right;
	int avail_h = rc.Height() - margin_top - margin_bottom;
	cols = (cell_w > 0) ? (avail_w / cell_w) : 1;
	rows = (cell_h > 0) ? (avail_h / cell_h) : 1;
	if (cols < 1) cols = 1;
	if (rows < 1) rows = 1;

	// 새 cols/rows 에 맞춰 화면 그리드를 재구성한다(행 수/줄 폭 맞춤, 커서 clamp).
	reset_screen();

	// 그리드 크기가 실제로 바뀌었고 리사이즈 싱크가 있으면 새 크기를 통지한다(자식 PTY 크기 동기화).
	if ((cols != old_cols || rows != old_rows) && resize_sink != nullptr)
		resize_sink(cols, rows, resize_sink_user);
}

Row CConBox::blank_row() const
{
	// cols 칸의 빈 줄(공백 셀, 현재 배경색). 현재 SGR 배경(cur_bg)으로 채워 지우기/스크롤이
	// 배경색을 따르게 한다.
	CharInfo c;
	c.ch = L' ';
	c.fg = cur_fg;
	c.bg = cur_bg;
	c.wide = false;
	return Row(cols < 1 ? 1 : cols, c);
}

void CConBox::reset_screen()
{
	// 화면(screen)을 현재 rows x cols 크기로 맞춘다. 기존 줄은 보존하되 cols 로 패딩/자르고,
	// 행 수를 rows 로 맞춘다. (정교한 reflow/PTY 동기화는 M3c)
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

	// 스크롤 영역이 새 화면 크기를 벗어나지 않게 맞춘다. 범위가 깨지면 화면 전체로 되돌린다.
	// (창 리사이즈 시 풀스크린 앱이 SIGWINCH 로 영역을 다시 설정하므로 여기선 안전하게 clamp 만 한다.)
	if (scroll_top < 0) scroll_top = 0;
	if (scroll_bot > rows - 1 || scroll_bot < 0) scroll_bot = rows - 1;
	if (scroll_top >= scroll_bot) { scroll_top = 0; scroll_bot = rows - 1; }
}

const Row& CConBox::line_at(int idx) const
{
	// (scrollback + screen) 통합 인덱스. 앞부분은 scrollback, 그 다음이 현재 화면이다.
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
	if (cur_col > cols) cur_col = cols;   // cols 는 "줄 끝 다음"(다음 글자에서 줄바꿈) 위치 허용
}

void CConBox::erase_cells(int row, int c0, int c1)
{
	// screen[row] 의 [c0, c1) 칸을 공백 셀(현재 배경색)로 지운다.
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
	// 줄 범위 [top, bot] 안에서 위로 n줄 회전한다(윗줄 n개 제거, 아래에 빈 줄 n개).
	// scrollback 에는 관여하지 않는 순수 줄 이동이다(보존이 필요하면 호출 측이 미리 push 한다).
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
	// 줄 범위 [top, bot] 안에서 아래로 n줄 회전한다(아래줄 n개 제거, 위에 빈 줄 n개).
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
	// 스크롤 영역 [scroll_top, scroll_bot] 을 위로 n줄 스크롤한다.
	// 주 화면이고 영역 상단이 화면 맨 위(scroll_top==0)일 때만, 밀려나는 윗줄을 scrollback 으로
	// 보존한다(과거 출력 보기용). 대체 화면이거나 부분 스크롤 영역이면 보존하지 않고 버린다.
	if (n < 1) return;
	int region = scroll_bot - scroll_top + 1;
	if (n > region) n = region;

	if (!alt_active && scroll_top == 0) {
		for (int i = 0; i < n; ++i)
			scrollback.push_back(screen[i]);

		// 스크롤백 상한 트림.
		int over = (int)scrollback.size() - MAX_SCROLLBACK;
		if (over > 0)
			scrollback.erase(scrollback.begin(), scrollback.begin() + over);
	}

	scroll_lines_up(scroll_top, scroll_bot, n);
}

void CConBox::scroll_down_region(int n)
{
	// 스크롤 영역을 아래로 n줄 스크롤한다(RI/SD). scrollback 에는 관여하지 않는다.
	scroll_lines_down(scroll_top, scroll_bot, n);
}

void CConBox::line_feed()
{
	// 커서가 스크롤 영역 하단에 있으면 영역을 한 줄 위로 스크롤한다(커서는 그 줄에 머문다).
	// 그 밖에는 화면 끝을 넘지 않는 선에서 한 줄 내린다.
	if (cur_row == scroll_bot)
		scroll_up_region(1);
	else if (cur_row < rows - 1)
		cur_row++;
}

void CConBox::insert_lines(int n)
{
	// IL: 커서가 스크롤 영역 안에 있을 때만 동작한다. 커서 줄부터 빈 줄을 n개 삽입하고
	// 아래 줄들을 영역 하단(scroll_bot)으로 밀어낸다. 커서는 1열로 이동한다.
	if (cur_row < scroll_top || cur_row > scroll_bot)
		return;
	if (n < 1) n = 1;
	scroll_lines_down(cur_row, scroll_bot, n);
	cur_col = 0;
}

void CConBox::delete_lines(int n)
{
	// DL: 커서가 스크롤 영역 안에 있을 때만 동작한다. 커서 줄부터 n줄 삭제하고 아래 줄들을
	// 끌어올리며 영역 하단에 빈 줄을 채운다. 커서는 1열로 이동한다.
	if (cur_row < scroll_top || cur_row > scroll_bot)
		return;
	if (n < 1) n = 1;
	scroll_lines_up(cur_row, scroll_bot, n);
	cur_col = 0;
}

void CConBox::insert_chars(int n)
{
	// ICH: 커서 칸부터 같은 줄에서 빈 칸을 n개 삽입한다. 오른쪽 칸들은 밀려나며 줄 폭(cols)을
	// 넘친 칸은 버린다. 커서 위치는 그대로 둔다.
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
	line.resize(cols, blank);   // 오른쪽으로 밀려 넘친 칸은 버려 줄 폭을 cols 로 유지한다.
}

void CConBox::delete_chars(int n)
{
	// DCH: 커서 칸부터 같은 줄에서 n칸을 삭제하고 오른쪽 칸들을 끌어당긴 뒤 끝에 빈 칸을 채운다.
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
	line.resize(cols, blank);   // 끌어당겨 줄어든 만큼 끝에 빈 칸을 채워 줄 폭을 cols 로 유지한다.
}

void CConBox::enter_alt_screen()
{
	// 대체 화면 진입: 주 화면과 커서를 백업하고 빈 대체 화면으로 바꾼다.
	// 스크롤백은 그대로 남지만 alt_active 동안은 보이지 않게(동결) 한다.
	if (alt_active)
		return;
	main_saved.swap(screen);          // 주 화면을 백업(screen 은 비워짐)
	saved_main_row = cur_row;
	saved_main_col = cur_col;

	screen.assign(rows, blank_row()); // 빈 대체 화면
	cur_row = 0;
	cur_col = 0;
	scroll_top = 0;
	scroll_bot = rows - 1;
	alt_active = true;
}

void CConBox::leave_alt_screen()
{
	// 대체 화면 복귀: 백업한 주 화면과 커서를 되돌린다.
	if (!alt_active)
		return;
	screen.swap(main_saved);
	main_saved.clear();
	alt_active = false;

	// 복귀한 주 화면을 현재 cols/rows 에 맞추고(리사이즈가 있었을 수 있다) 커서/영역을 되돌린다.
	cur_row = saved_main_row;
	cur_col = saved_main_col;
	scroll_top = 0;
	scroll_bot = rows - 1;
	reset_screen();
}

void CConBox::put_char(wchar_t wc)
{
	// 현재 커서 칸에 글자 하나를 쓴다. 이 글자를 넣으면 칸이 화면 폭(cols)을 넘는 경우 먼저
	// 자동 줄바꿈(autowrap)한다. 2배 폭 글자는 lead 칸에 쓰고 다음 칸을 trail(ch=0)로 둔다.
	if (cur_row < 0 || cur_row >= (int)screen.size())
		return;

	int w = IsWideChar(wc) ? 2 : 1;
	if (cur_col + w > cols) {
		cur_col = 0;
		line_feed();
	}

	// reverse 속성이면 전경/배경을 스왑해 셀에 저장한다.
	COLORREF fg = cur_reverse ? cur_bg : cur_fg;
	COLORREF bg = cur_reverse ? cur_fg : cur_bg;

	Row& line = screen[cur_row];
	if (cur_col >= 0 && cur_col < (int)line.size()) {
		line[cur_col].ch = wc;
		line[cur_col].fg = fg;
		line[cur_col].bg = bg;
		line[cur_col].wide = (w == 2);
		// 2배 폭 글자의 trail 칸(다음 칸)을 비워 둔다(렌더는 lead 가 두 칸을 그린다).
		if (w == 2 && cur_col + 1 < (int)line.size()) {
			line[cur_col + 1].ch = 0;
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

	// UTF-8 입력을 UTF-16 으로 변환한다.
	int wlen = ::MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
	if (wlen <= 1)
		return;   // 빈 문자열이거나 변환 실패

	std::vector<wchar_t> ws(wlen);
	::MultiByteToWideChar(CP_UTF8, 0, text, -1, ws.data(), wlen);

	// 화면이 아직 없으면(폰트/창 확정 전) 만들어 둔다.
	if (screen.empty())
		reset_screen();

	// 변환된 문자열을 한 글자씩 VT 파서에 넣는다. (마지막 원소는 널 종료라 제외)
	for (int i = 0; i < wlen - 1; ++i)
		vt_feed(ws[i]);

	// 강제로 맨 아래까지 스크롤한 뒤 화면을 갱신한다.
	scroll_to_bottom();
	update_scrollbar();
	bump_cursor();
	Invalidate();
}

void CConBox::scroll_to_bottom()
{
	int total = (int)scrollback.size() + rows;
	int maxtop = total - rows;
	view_top = (maxtop > 0) ? maxtop : 0;
}

// xterm 256색 인덱스를 COLORREF 로 변환한다.
// 0-15 기본 16색, 16-231 6x6x6 컬러 큐브, 232-255 24단계 그레이스케일.
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
		// 큐브 단계: 0 -> 0, 1..5 -> 55 + 40*v
		int rr = r ? 55 + 40 * r : 0;
		int gg = g ? 55 + 40 * g : 0;
		int bb = b ? 55 + 40 * b : 0;
		return RGB(rr, gg, bb);
	}
	int v = 8 + 10 * (n - 232);   // 232..255 -> 8, 18, ..., 238
	return RGB(v, v, v);
}

void CConBox::vt_feed(wchar_t wc)
{
	// VT 파서 상태기계. 한 글자(또는 제어문자)를 받아 현재 상태에 따라 처리한다.
	// 파서 상태(vt_state 등)는 멤버라 print 가 청크로 여러 번 불려도 시퀀스가 이어진다.
	switch (vt_state) {
	case VT_GROUND:
		if (wc == 0x1B) { vt_state = VT_ESC; return; }   // ESC
		if (wc == L'\r') { cur_col = 0; return; }          // CR
		if (wc == L'\n') { line_feed(); return; }          // LF
		if (wc == 0x08) { if (cur_col > 0) cur_col--; return; }  // BS
		if (wc == L'\t') {
			// 다음 8칸 탭 정지 위치로 이동한다(마지막 칸을 넘지 않는다).
			int next = (cur_col / 8 + 1) * 8;
			if (next > cols - 1) next = cols - 1;
			cur_col = next;
			return;
		}
		if (wc == 0x07) return;   // BEL 무시
		if (wc < 0x20) return;    // 그 밖의 C0 제어문자 무시
		put_char(wc);
		return;

	case VT_ESC:
		if (wc == L'[') {
			// CSI 진입: 파라미터 초기화.
			vt_state = VT_CSI;
			vt_nparam = 0;
			vt_priv = false;
			vt_gtlt = false;
			for (int k = 0; k < 16; ++k) vt_params[k] = 0;
			return;
		}
		if (wc == L']') { vt_state = VT_OSC; return; }   // OSC: 내용 버림(창 제목 등)
		if (wc == L'7') { saved_row = cur_row; saved_col = cur_col; vt_state = VT_GROUND; return; }   // DECSC
		if (wc == L'8') { cur_row = saved_row; cur_col = saved_col; clamp_cursor(); vt_state = VT_GROUND; return; }  // DECRC
		if (wc == L'M') {   // RI(역 인덱스): 커서가 영역 상단이면 영역을 아래로 스크롤, 아니면 한 줄 위로
			if (cur_row == scroll_top) scroll_down_region(1);
			else if (cur_row > 0) cur_row--;
			vt_state = VT_GROUND; return;
		}
		// 그 밖의 ESC 시퀀스(독립/2바이트)는 무시한다.
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
			if (vt_nparam == 0) vt_nparam = 1;   // 비어 있던 첫 파라미터는 0
			if (vt_nparam < 16) { vt_params[vt_nparam] = 0; vt_nparam++; }
			return;
		}
		if (wc == L'?') { vt_priv = true; return; }
		// '<' '=' '>' 는 2차 DA/kitty keyboard/XTMODKEYS 등 프라이빗 시퀀스의 접두다. 표시만 해 두고
		// (dispatch_csi 가 통째로 무시) 최종 바이트(u/m/q 등)를 표준 명령으로 오인하지 않게 한다.
		if (wc == L'<' || wc == L'=' || wc == L'>') { vt_gtlt = true; return; }
		if (wc >= 0x40 && wc <= 0x7E) {
			dispatch_csi(wc);
			vt_state = VT_GROUND;
			return;
		}
		// 중간 바이트(0x20-0x2F 등)는 무시하되 시퀀스는 계속 이어진다.
		return;

	case VT_OSC:
		// OSC 문자열(창 제목 설정 등)은 표시하지 않고 종료자까지 버린다.
		// 종료자: BEL(0x07) 또는 ST(ESC '\'). ESC 가 오면 ESC 상태로 보내 다음 글자에서 끝낸다.
		if (wc == 0x07) { vt_state = VT_GROUND; return; }
		if (wc == 0x1B) { vt_state = VT_ESC; return; }
		return;

	default:
		vt_state = VT_GROUND;
		return;
	}
}

void CConBox::dispatch_csi(wchar_t fin)
{
	// '<' '=' '>' 접두로 시작한 시퀀스(2차 DA, kitty keyboard 프로토콜, XTMODKEYS 등)는
	// 우리가 지원하지 않는다. 최종 바이트(u/m/q 등)를 표준 명령(커서 복원/SGR 등)으로 오인하면
	// 안 되므로(예: ESC[<u 를 커서 복원으로 처리해 커서가 0,0 으로 튀던 버그) 통째로 무시한다.
	if (vt_gtlt)
		return;

	// 첫 파라미터(없으면 0). 각 명령이 기본값 규칙(보통 없으면 1)을 따로 적용한다.
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

	case L'J':   // ED (화면 지우기)
		if (n == 0) {
			erase_cells(cur_row, cur_col, cols);
			for (int r = cur_row + 1; r < rows; ++r) erase_cells(r, 0, cols);
		} else if (n == 1) {
			for (int r = 0; r < cur_row; ++r) erase_cells(r, 0, cols);
			erase_cells(cur_row, 0, cur_col + 1);
		} else {   // 2, 3: 화면 전체
			for (int r = 0; r < rows; ++r) erase_cells(r, 0, cols);
		}
		break;

	case L'K':   // EL (줄 지우기)
		if (n == 0) erase_cells(cur_row, cur_col, cols);
		else if (n == 1) erase_cells(cur_row, 0, cur_col + 1);
		else erase_cells(cur_row, 0, cols);
		break;

	case L'X': {   // ECH (커서 칸부터 n칸을 빈 칸으로, 밀지 않음)
		int d = n ? n : 1;
		erase_cells(cur_row, cur_col, cur_col + d);
		break;
	}
	case L'@': insert_chars(n ? n : 1); break;   // ICH (문자 삽입)
	case L'P': delete_chars(n ? n : 1); break;   // DCH (문자 삭제)
	case L'L': insert_lines(n ? n : 1); break;   // IL  (줄 삽입)
	case L'M': delete_lines(n ? n : 1); break;   // DL  (줄 삭제)
	case L'S': scroll_up_region(n ? n : 1); break;     // SU (영역 위로 스크롤)
	case L'T': scroll_down_region(n ? n : 1); break;   // SD (영역 아래로 스크롤)

	case L'r': {   // DECSTBM (스크롤 영역 상/하단 설정). 프라이빗(?r=DEC 모드 복원)은 무시한다.
		if (vt_priv) break;
		int t = (vt_nparam >= 1 && vt_params[0]) ? vt_params[0] : 1;
		int b = (vt_nparam >= 2 && vt_params[1]) ? vt_params[1] : rows;
		t--; b--;   // 1-based -> 0-based
		if (t < 0) t = 0;
		if (b > rows - 1) b = rows - 1;
		if (t < b) {
			scroll_top = t;
			scroll_bot = b;
			// DECSTBM 설정 후 커서는 홈(화면 좌상단)으로 이동한다.
			cur_row = 0;
			cur_col = 0;
		}
		break;
	}

	case L'm': {   // SGR (색/속성)
		if (vt_nparam == 0) {
			// 파라미터 없음 = 리셋(SGR 0).
			cur_fg = DEFAULT_FG; cur_bg = DEFAULT_BG;
			cur_bold = false; cur_reverse = false;
			break;
		}
		for (int i = 0; i < vt_nparam; ++i) {
			int p = vt_params[i];
			if (p == 0) { cur_fg = DEFAULT_FG; cur_bg = DEFAULT_BG; cur_bold = false; cur_reverse = false; }
			else if (p == 1) cur_bold = true;
			else if (p == 22) cur_bold = false;
			else if (p == 7) cur_reverse = true;
			else if (p == 27) cur_reverse = false;
			else if (p >= 30 && p <= 37) cur_fg = Xterm256ToRgb((cur_bold ? 8 : 0) + (p - 30));
			else if (p >= 40 && p <= 47) cur_bg = Xterm256ToRgb(p - 40);
			else if (p >= 90 && p <= 97) cur_fg = Xterm256ToRgb(8 + (p - 90));
			else if (p >= 100 && p <= 107) cur_bg = Xterm256ToRgb(8 + (p - 100));
			else if (p == 39) cur_fg = DEFAULT_FG;
			else if (p == 49) cur_bg = DEFAULT_BG;
			else if (p == 38 || p == 48) {
				// 확장 색: 38;5;n (256색) 또는 38;2;r;g;b (트루컬러). 48 은 배경.
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
	case L'l': {   // 모드 설정(h)/해제(l)
		bool set = (fin == L'h');
		if (vt_priv) {
			if (n == 25) {
				cursor_visible = set;   // DECTCEM: 커서 표시
			}
			else if (n == 1) {
				app_cursor_keys = set;  // DECCKM: 앱 커서 키 모드(방향키를 ESC O x 로)
			}
			else if (n == 2004) {
				bracketed_paste = set;  // bracketed paste 모드
			}
			else if (n == 1049 || n == 1047 || n == 47) {
				// 대체 화면(alt screen) 전환. vim/htop 등 풀스크린 TUI 가 쓴다.
				// (?1049 는 커서 저장/복원까지 묶지만, 우리는 진입/복귀에서 커서를 함께 다룬다.)
				if (set) enter_alt_screen();
				else leave_alt_screen();
			}
			else if (n == 1048) {
				// 커서 저장(h)/복원(l). DECSC/DECRC 와 같은 동작.
				if (set) { saved_row = cur_row; saved_col = cur_col; }
				else { cur_row = saved_row; cur_col = saved_col; clamp_cursor(); }
			}
			// 그 밖의 프라이빗 모드(?7 autowrap=항상 켬, ?2004 bracketed paste=M5 등)는 무시한다.
		}
		break;
	}
	case L's': saved_row = cur_row; saved_col = cur_col; break;                 // 커서 저장
	case L'u': cur_row = saved_row; cur_col = saved_col; clamp_cursor(); break; // 커서 복원

	case L'n':   // DSR (디바이스 상태 보고) — 자식의 질의에 입력 싱크로 응답한다.
		if (!vt_priv && input_sink != nullptr) {
			if (n == 6) {
				// CPR: 커서 위치를 ESC[{row};{col}R 로 보고한다(1-based, 화면 기준).
				char buf[32];
				int len = wsprintfA(buf, "\x1b[%d;%dR", cur_row + 1, cur_col + 1);
				send_input_bytes(buf, len);
			}
			else if (n == 5) {
				// 단말 상태 OK 보고.
				send_input_bytes("\x1b[0n", 4);
			}
		}
		break;

	case L'c':   // DA (디바이스 속성) — Primary DA 질의에 VT102 호환으로 응답한다.
		if (input_sink != nullptr && (n == 0)) {
			// ESC[?6c = VT102. 자식이 단말 능력을 식별하는 용도다.
			send_input_bytes("\x1b[?6c", 5);
		}
		break;

	default:
		break;   // 그 밖의 미지원 CSI 는 무시한다.
	}
}

void CConBox::update_scrollbar()
{
	if (!::IsWindow(m_hWnd))
		return;

	// 대체 화면(alt screen)에서는 스크롤백이 동결되어 보이지 않는다. 스크롤바를 숨기고
	// 보기를 화면 시작(scrollback 다음)에 고정한다.
	if (alt_active) {
		ShowScrollBar(SB_VERT, FALSE);
		view_top = (int)scrollback.size();
		return;
	}

	int total = (int)scrollback.size() + rows;
	if (total <= rows) {
		// 스크롤백이 없으면(화면만) 스크롤바를 숨기고 맨 위로 맞춘다.
		ShowScrollBar(SB_VERT, FALSE);
		view_top = 0;
		return;
	}

	// 내용이 넘치면 스크롤바를 보이고 범위/페이지/위치를 설정한다.
	ShowScrollBar(SB_VERT, TRUE);

	SCROLLINFO si;
	ZeroMemory(&si, sizeof(si));
	si.cbSize = sizeof(si);
	si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
	si.nMin = 0;
	si.nMax = total - 1;       // 마지막 화면 줄 인덱스
	si.nPage = rows;           // 한 번에 보이는 줄 수
	si.nPos = view_top;
	SetScrollInfo(SB_VERT, &si, TRUE);
}

BOOL CConBox::OnEraseBkgnd(CDC* dc)
{
	// 깜빡임을 막기 위해 배경 지우기는 OnPaint 에서 직접 처리한다.
	// 여기서 TRUE 를 돌려주어 시스템의 기본 배경 지우기를 막는다.
	return TRUE;
}

void CConBox::OnSize(UINT type, int cx, int cy)
{
	CWnd::OnSize(type, cx, cy);

	// 창 크기가 바뀌면 화면 칸 수를 다시 계산한다.
	update_metrics();

	// 스크롤 위치가 범위를 벗어나면 맞춰 준다.
	int maxtop = (int)scrollback.size();
	if (maxtop < 0) maxtop = 0;
	if (view_top > maxtop) view_top = maxtop;

	update_scrollbar();
	Invalidate();
}

UINT CConBox::OnGetDlgCode()
{
	// 부모 대화상자가 가로채지 않도록, 방향키/Tab/Enter/일반문자를 모두 직접 받는다.
	return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB;
}

void CConBox::OnVScroll(UINT code, UINT pos, CScrollBar* sb)
{
	// 대체 화면에서는 스크롤백 보기가 동결되므로 스크롤 입력을 무시한다.
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
		// 드래그 중인 실제 위치를 읽어 온다.
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
	// 대체 화면에서는 스크롤백 보기가 동결되므로 휠 스크롤을 무시한다(자식이 화면을 점유).
	if (alt_active)
		return TRUE;

	// 휠 한 칸(120)당 세 줄씩 스크롤한다. 위로 굴리면 위쪽(이전 내용)으로 간다.
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

LRESULT CConBox::OnImeStart(WPARAM w, LPARAM l)
{
	// 조합/후보 창을 커서 위치로 옮기고 조합 글자를 한글 폰트로 맞춘다.
	// 조합 글자는 우리가 직접 그리므로 기본 조합창은 만들지 않는다(0 반환).
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

			// 후보 목록 창은 커서 바로 아래에 뜨도록 한다.
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
	// 조합 시작 시 깜빡임 상태를 켜 둔다.
	bump_cursor();
	// 조합 글자는 시스템 기본 인라인 표시에 맡긴다(ConBox 직접 조합 렌더는 이후 단계 M5).
	// 위에서 조합/후보 창만 커서 위치로 옮겨 두었다.
	return Default();
}

LRESULT CConBox::OnImeComp(WPARAM w, LPARAM l)
{
	// 터미널(raw) 모드: 확정(GCS_RESULTSTR)된 글자만 UTF-8 로 자식에 보내고, 미완성 조합은
	// 시스템 기본 인라인 표시에 맡긴다(로컬 버퍼에 삽입하지 않는다).
	// (조합 중 방향키 등으로 글자가 옮겨진 칸에서 완성되는 순서 문제의 근본 해결은 M5 에서
	//  강제 확정 후 전송 순서를 보장하는 방식으로 다룬다 - 요구사양서 3.1 참조.)
	if (input_sink != nullptr) {
		HIMC himc = ImmGetContext(m_hWnd);
		if (himc == NULL)
			return Default();
		if (l & GCS_RESULTSTR) {
			// 확정분을 추출해 자식으로 보낸다. 기본 처리(Default)를 부르지 않고 메시지를
			// 소비(return 0)해, 시스템이 같은 확정분을 WM_CHAR/WM_IME_CHAR 로 또 보내는
			// 중복을 막는다.
			LONG bytes = ImmGetCompositionStringW(himc, GCS_RESULTSTR, NULL, 0);
			int n = bytes / (int)sizeof(wchar_t);
			if (n > 0) {
				std::wstring r;
				r.resize(n);
				ImmGetCompositionStringW(himc, GCS_RESULTSTR, &r[0], bytes);
				send_input_wide(r.c_str(), n);
			}
			ImmReleaseContext(m_hWnd, himc);
			bump_cursor();
			return 0;
		}
		// 미완성 조합(GCS_COMPSTR 등)은 시스템 기본 인라인 표시에 맡긴다.
		ImmReleaseContext(m_hWnd, himc);
		bump_cursor();
		return Default();
	}

	// 입력 싱크가 없는 읽기 전용 뷰어 모드에서는 로컬 조합 편집을 하지 않는다.
	// 조합 표시는 시스템 기본 인라인에 맡긴다.
	bump_cursor();
	return Default();
}

LRESULT CConBox::OnImeEnd(WPARAM w, LPARAM l)
{
	// 로컬 버퍼에 삽입한 조합 글자가 없으므로(조합 표시는 시스템 기본 인라인에 맡긴다)
	// 정리할 것이 없다. 조합이 끝났으니 본문 블록 커서 깜빡임을 재개하고 기본 처리에 맡긴다.
	bump_cursor();
	return Default();
}

LRESULT CConBox::OnImeNotify(WPARAM w, LPARAM l)
{
	// 한/영 키 등으로 변환 모드가 바뀌면 통지가 온다. 이때 커서 영역을 다시 그려
	// 한글/영문 모드에 맞는 폭(긴/짧은 언더바)을 즉시 반영한다.
	// 모드 변경 외의 통지(후보창 등)는 기본 처리에 맡긴다.
	LRESULT res = Default();
	if (w == IMN_SETCONVERSIONMODE) {
		CRect rc;
		if (get_cursor_rect(rc)) {
			// 폭이 한 칸에서 두 칸으로(또는 반대로) 바뀌므로 두 칸 영역을 모두 갱신한다.
			rc.right = rc.left + cell_w * 2;
			InvalidateRect(rc);
		}
	}
	return res;
}

// 입력 싱크(터미널/raw 모드)를 지정/해제한다.
void CConBox::set_input_sink(void (*sink)(const char* bytes, int len, void* user), void* user)
{
	input_sink = sink;
	input_sink_user = user;
}

// 리사이즈 싱크(터미널/raw 모드)를 지정/해제한다.
void CConBox::set_resize_sink(void (*sink)(int cols, int rows, void* user), void* user)
{
	resize_sink = sink;
	resize_sink_user = user;
}

// 터미널 모드에서 UTF-8 바이트열을 그대로 입력 싱크로 보낸다.
void CConBox::send_input_bytes(const char* bytes, int len)
{
	if (input_sink != nullptr && bytes != nullptr && len > 0)
		input_sink(bytes, len, input_sink_user);
}

// 터미널 모드에서 UTF-16 글자들을 UTF-8 로 변환해 입력 싱크로 보낸다. (문자/IME 확정 글자용)
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

// 터미널 모드에서 방향키/편집키 등 비문자 키를 VT 시퀀스로 인코딩해 자식에 보낸다.
// 인쇄 가능 문자/Enter/Tab/Backspace/Esc/Ctrl+letter 는 WM_CHAR 경로(OnChar)에서 처리하므로
// 여기서는 다루지 않는다(중복 방지). 여기서 보냈으면 true, 처리 안 했으면 false.
bool CConBox::terminal_keydown(UINT vk, bool ctrl, bool shift)
{
	// Ctrl+C: 자식에 인터럽트(0x03)를 보낸다(터미널 관례).
	// (선택 텍스트 복사는 3a-2 에서 그리드 좌표 선택을 복구할 때 함께 되살린다.)
	if (ctrl && (vk == 'C')) {
		send_input_bytes("\x03", 1);
		return true;
	}
	// Ctrl+V: 클립보드 텍스트를 자식 stdin 으로 보낸다(bracketed paste 면 감싼다).
	if (ctrl && (vk == 'V')) {
		paste_clipboard();
		return true;
	}
	// Shift+Tab: 역방향 탭(backtab). 보통 메뉴/포커스 역이동에 쓴다.
	if (vk == VK_TAB && shift) {
		send_input_bytes("\x1b[Z", 3);
		return true;
	}

	// 수정자 코드. xterm 규약은 파라미터로 1 + Shift(1) + Alt(2) + Ctrl(4) 를 보낸다.
	// (여기서는 Shift/Ctrl 만 다룬다. 수정자가 있으면 ESC[1;{mod}{최종문자} 형식을 쓴다.)
	int mod = (shift ? 1 : 0) + (ctrl ? 4 : 0);

	// 방향키/Home/End 는 최종 문자(A/B/C/D/H/F)로 인코딩한다. 수정자가 없으면 앱 커서 키 모드
	// (DECCKM)에 따라 ESC[ 또는 ESC O 접두사를 쓴다. 수정자가 있으면 ESC[1;{mod} 형식이다.
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

	// 그 밖의 편집키/펑션키 -> 고정 VT 시퀀스. (수정자 조합은 claude 등에서 거의 쓰지 않아 단순화)
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
	// 클립보드의 유니코드 텍스트를 자식 stdin 으로 보낸다. bracketed paste 모드면 앞뒤를
	// ESC[200~ / ESC[201~ 로 감싸 자식이 "붙여넣기 덩어리"를 한 번에 처리하게 한다.
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
	// 한글 IME 조합 중이면 강제로 확정(완성)시킨다. CPS_COMPLETE 는 동기적으로
	// WM_IME_COMPOSITION(GCS_RESULTSTR)을 디스패치하므로, 이 함수가 반환하기 전에 OnImeComp 가
	// 완성 글자의 UTF-8 을 자식에 먼저 보낸다. 그래서 트리거 키 전송 직전에 부르면 "제자리 완성
	// 후 트리거" 순서가 보장된다(요구사양서 3.1).
	//
	// 한글 IME 가 방향키 등으로 조합을 스스로 먼저 확정하는 경우도 있는데(WM_IME_COMPOSITION 이
	// WM_KEYDOWN 보다 먼저 옴), 그때는 이 함수 시점에 조합이 남아 있지 않아 no-op 이 된다(안전망).
	// 어느 경로든 완성분이 트리거보다 먼저 전송된다. 중복 확정 메시지는 OnImeComp 가 빈
	// RESULTSTR(n==0)을 걸러 이중 전송되지 않는다.
	HIMC himc = ImmGetContext(m_hWnd);
	if (himc == NULL)
		return false;

	bool had = false;
	LONG bytes = ImmGetCompositionStringW(himc, GCS_COMPSTR, NULL, 0);
	if (bytes > 0) {
		// 조합 중인 글자가 있다 -> 강제 확정. (동기적으로 WM_IME_COMPOSITION 이 처리된다.)
		ImmNotifyIME(himc, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
		had = true;
	}
	ImmReleaseContext(m_hWnd, himc);
	return had;
}

void CConBox::OnChar(UINT ch, UINT rep, UINT flags)
{
	// 터미널(raw) 모드: 로컬 편집/에코 없이 문자/제어문자를 자식에게 바이트로 보낸다.
	// (WM_CHAR 는 키보드 레이아웃/IME 를 거친 결과라 인쇄 문자·제어문자 전송에 알맞다.)
	if (input_sink != nullptr) {
		for (UINT k = 0; k < rep; ++k) {
			if (ch == 0x08) {
				// Backspace 는 DEL(0x7F)로 보낸다(readline/유닉스 계열 관례).
				send_input_bytes("\x7f", 1);
			}
			else if (ch == L'\r' || ch == L'\n') {
				// Enter 는 CR(0x0D)로 보낸다.
				send_input_bytes("\r", 1);
			}
			else if (ch < 0x80) {
				// 인쇄 가능 ASCII + 그 밖의 제어문자(Tab=0x09, Esc=0x1B, Ctrl+letter 등)는 1바이트로.
				char b = (char)ch;
				send_input_bytes(&b, 1);
			}
			else {
				// 한글 등 비ASCII(완성형 글자, IME 확정분 포함)는 UTF-8 로 변환해 보낸다.
				wchar_t wc = (wchar_t)ch;
				send_input_wide(&wc, 1);
			}
		}
		bump_cursor();
		return;
	}

	// 입력 싱크가 없는 읽기 전용 뷰어 모드에서는 로컬 편집/에코를 하지 않는다.
}

void CConBox::OnKeyDown(UINT vk, UINT rep, UINT flags)
{
	// 터미널(raw) 모드: 방향키/편집키는 VT 시퀀스로 자식에 보내고, 인쇄 문자/Enter/Backspace/
	// Tab/Esc 등은 WM_CHAR(OnChar) 경로에 맡긴다(중복 방지). 로컬 줄편집은 하지 않는다.
	if (input_sink != nullptr) {
		bool tctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
		bool tshift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;

		// 조합 종료 트리거 키(방향키/Home/End/Delete/Insert/Page/Enter/Tab/Esc)면 먼저 IME
		// 조합을 강제 확정해, 완성 글자가 트리거 키보다 먼저 자식에 전송되게 한다(요구사양서 3.1).
		switch (vk) {
		case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
		case VK_HOME: case VK_END: case VK_DELETE: case VK_INSERT:
		case VK_PRIOR: case VK_NEXT: case VK_RETURN: case VK_TAB: case VK_ESCAPE:
			finalize_composition();
			break;
		default:
			break;
		}

		if (terminal_keydown(vk, tctrl, tshift))
			bump_cursor();
		return;
	}

	// 입력 싱크가 없는 읽기 전용 뷰어 모드: 로컬 줄편집은 없고 스크롤 단축키만 처리한다.
	// (PageUp/PageDown, Ctrl+Home/End. 그 밖의 키는 기본 처리에 맡긴다.)
	// 대체 화면에서는 스크롤백 보기가 동결되므로 스크롤 단축키도 기본 처리에 맡긴다.
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

// 더블 버퍼링용 메모리 DC/비트맵을 (필요하면) 준비한다.
// 메모리 DC 는 한 번만 만들어 계속 재사용하고, 비트맵은 클라이언트 크기가 바뀐
// 경우에만 다시 만든다. 커서 깜빡임처럼 잦은 다시그리기에서 매번 CreateCompatibleDC/
// CreateCompatibleBitmap 을 하던 비용을 없앤다.
void CConBox::ensure_back_buffer(CDC* ref, int w, int h)
{
	// 메모리 DC 는 처음 한 번만 만든다.
	if (back_dc.GetSafeHdc() == nullptr)
		back_dc.CreateCompatibleDC(ref);

	// 비트맵이 아직 없거나 크기가 달라졌으면 다시 만든다.
	if (back_bmp.GetSafeHandle() == nullptr || w != back_w || h != back_h) {
		// 이전 비트맵을 DC 에서 떼어내고(원래 비트맵으로 되돌린 뒤) 버린다.
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

void CConBox::OnPaint()
{
	CPaintDC paintDC(this);

	// 클라이언트 영역 크기를 구한다.
	CRect rc;
	GetClientRect(&rc);

	// 더블 버퍼링: 화면에 직접 그리면 "전체 배경 칠 -> 글자 그리기" 의 중간 상태가 잠깐
	// 보여, 빠른 키 입력으로 매번 전체를 다시 그릴 때 화면이 깜빡인다. 그래서 메모리 DC 에
	// 한 프레임을 모두 그린 뒤 마지막에 화면으로 한 번에 복사한다. 메모리 DC/비트맵은
	// 멤버로 캐싱해 두고 크기가 바뀔 때만 다시 만든다.
	ensure_back_buffer(&paintDC, rc.Width(), rc.Height());

	// 이하 기존 그리기 코드는 모두 메모리 DC(dc)에 그린다.
	CDC& dc = back_dc;

	// 클라이언트 영역 전체를 현재 배경색으로 채운다.
	dc.FillSolidRect(rc, cur_bg);

	// 글자는 칸의 배경을 칠한 뒤 투명 모드로 그린다.
	// 기준선(TA_BASELINE) 정렬로 그려 한글과 영문의 글자 바닥을 맞춘다.
	dc.SetBkMode(TRANSPARENT);
	UINT old_align = dc.SetTextAlign(TA_LEFT | TA_BASELINE);
	CFont* old_font = dc.SelectObject(&efont);

	// 화면에 보이는 줄들을 위에서 아래로 그린다. (scrollback + screen 통합 인덱스 view_top+row)
	int total = (int)scrollback.size() + rows;
	for (int row = 0; row < rows; ++row) {
		int idx = view_top + row;
		if (idx < 0 || idx >= total)
			break;

		const Row& line = line_at(idx);

		int ncell = (int)line.size();
		if (ncell > cols) ncell = cols;

		// 칸 인덱스(i)가 곧 가로 칸 좌표다. 2배 폭 글자는 다음 칸(trail)을 함께 칠하고 건너뛴다.
		for (int i = 0; i < ncell; ++i) {
			const CharInfo& c = line[i];
			int w = c.wide ? 2 : 1;

			int px = margin_left + i * cell_w;
			int py = margin_top + row * cell_h;

			COLORREF fg = c.fg;
			COLORREF bg = c.bg;

			// 칸 배경을 칠한다. (2배 폭이면 두 칸을 함께 칠한다.)
			CRect cell(px, py, px + w * cell_w, py + cell_h);
			dc.FillSolidRect(cell, bg);

			// 빈 칸(공백)과 2배 폭의 trail 칸(ch=0)은 글자를 그리지 않는다.
			// 그 밖의 글자는 블록/박스면 도형으로, 아니면 폰트로 그린다(2배 폭은 한글 폰트).
			if (c.ch != 0 && c.ch != L' ') {
				bool drawn = false;
				if (glyph_level >= 1)
					drawn = DrawBlockElement(dc, c.ch, px, py, cell_w, cell_h, fg);
				if (!drawn && glyph_level >= 2)
					drawn = DrawBoxLine(dc, c.ch, px, py, cell_w, cell_h, fg);
				if (!drawn) {
					// 기준선 정렬이므로 세로 위치는 칸 위쪽 + 기준선만큼 내린 곳이다.
					dc.SelectObject(c.wide ? &kfont : &efont);
					dc.SetTextColor(fg);
					dc.TextOutW(px, py + cell_base, &c.ch, 1);
				}
			}

			if (w == 2) ++i;   // trail 칸 건너뜀
		}
	}

	dc.SelectObject(old_font);
	dc.SetTextAlign(old_align);

	// 커서를 그린다. 칸을 꽉 채우는 블록 커서를 cursor_on 이고 커서가 표시 상태(cursor_visible)일
	// 때만 그려 깜빡인다. 화면 밖이면 그리지 않는다. 커서 칸에 이미 글자가 있으면 블록 위에 전경색
	// 으로 글자를 다시 그려 가려지지 않게 한다(한글 모드 2칸 블록이 다음 칸 글자까지 덮는 경우 포함).
	if (cursor_on && cursor_visible) {
		CRect cur;
		if (get_cursor_rect(cur)) {
			COLORREF cblk = blend_cursor_color();
			dc.FillSolidRect(cur, cblk);

			if (cur_row >= 0 && cur_row < (int)screen.size()) {
				const Row& line = screen[cur_row];

				// 블록이 덮는 칸의 글자를 cur_col 부터 차례로 다시 그려 가려지지 않게 한다.
				// 블록 밖으로 삐져나온 부분은 본문이 이미 제 색으로 그렸으므로, 블록 사각형으로
				// 클리핑해 블록 안쪽만 전경색으로 덮는다.
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

				// SaveDC 이전 상태(폰트/정렬/클립)로 한 번에 원복한다.
				dc.RestoreDC(saved);
			}
		}
	}

	// 메모리 DC 에 완성된 한 프레임을 화면으로 한 번에 복사한다. (깜빡임 제거)
	// 비트맵은 멤버로 계속 살려 두므로 여기서 떼어내지 않는다. (소멸자에서 정리)
	paintDC.BitBlt(0, 0, rc.Width(), rc.Height(), &back_dc, 0, 0, SRCCOPY);
}
