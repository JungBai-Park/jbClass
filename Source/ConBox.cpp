// ConBox.cpp
//
// CConBox 의 구현. 단계적으로 기능을 채워 나간다.
// 현재 구현 범위: 창 생성과 배경 채우기.

#include "ConBox.h"
#include "Accessories.h"
#include <string>
#include <imm.h>            // 한글 IME (Input Method Manager)
#pragma comment(lib, "imm32.lib")

// 메시지 맵: 그리기/배경/크기/타이머/키보드/스크롤/IME 를 처리한다.
BEGIN_MESSAGE_MAP(CConBox, CWnd)
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
	ON_WM_SIZE()
	ON_WM_TIMER()
	ON_WM_CHAR()
	ON_WM_KEYDOWN()
	ON_WM_GETDLGCODE()
	ON_WM_VSCROLL()
	ON_WM_MOUSEWHEEL()
	ON_MESSAGE(WM_IME_STARTCOMPOSITION, &CConBox::OnImeStart)
	ON_MESSAGE(WM_IME_COMPOSITION, &CConBox::OnImeComp)
	ON_MESSAGE(WM_IME_ENDCOMPOSITION, &CConBox::OnImeEnd)
	ON_WM_LBUTTONDOWN()
	ON_WM_MOUSEMOVE()
	ON_WM_LBUTTONUP()
	ON_WM_RBUTTONDOWN()
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

CConBox::CConBox()
{
	// 색상 기본값: 검은 바탕에 흰 글씨.
	cur_fg = RGB(255, 255, 255);
	cur_bg = RGB(0, 0, 0);

	// 셀 크기는 폰트 측정 전까지 쓰일 안전한 임시값으로 둔다.
	cell_w = 8;
	cell_h = 16;
	cell_base = 12;

	cols = 1;
	rows = 1;

	scroll_top = 0;
	cur_log = 0;
	cur_col = 0;
	edit_log = 0;
	edit_col = 0;

	// Enter 콜백은 기본적으로 없다. 사용 측에서 대입한다.
	on_enter = nullptr;

	// 커서 기본값: 칸 높이의 20% 두께 언더바, 0.5초 간격 깜빡임.
	cursor_pct = 20;
	cursor_blink_ms = 500;
	cursor_on = true;

	// 선택 상태 초기화
	selecting = false;
	sel_valid = false;
	sel_a_log = sel_a_col = 0;
	sel_c_log = sel_c_col = 0;

	ZeroMemory(&efont_lf, sizeof(efont_lf));
	ZeroMemory(&kfont_lf, sizeof(kfont_lf));
}

CConBox::~CConBox()
{
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
	rebuild_scr_lines();
	update_scrollbar();

	// 커서 깜빡임 타이머를 시작한다. (간격이 0이면 깜빡이지 않는다.)
	cursor_on = true;
	if (cursor_blink_ms > 0)
		SetTimer(CURSOR_TIMER, cursor_blink_ms, NULL);
}

void CConBox::make_font(CFont& font, LOGFONTW& lf_out, const char* name, int size, const char* opts)
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
	// 명세의 기본값: 영문 Consolas 13pt Bold, 한글 맑은 고딕 13pt Bold.
	if (efont.GetSafeHandle() == NULL)
		make_font(efont, efont_lf, "Consolas", 13, "B");
	if (kfont.GetSafeHandle() == NULL)
		make_font(kfont, kfont_lf, "Malgun Gothic", 13, "B");
}

void CConBox::calc_cell_size()
{
	// 창이 있으면 창 DC, 없으면 화면 DC 로 폰트 크기를 측정한다.
	BOOL has_wnd = ::IsWindow(m_hWnd);
	HWND meas_wnd = has_wnd ? m_hWnd : NULL;
	HDC hdc = ::GetDC(meas_wnd);

	// 영문 폰트로 한 글자 폭과 높이, 기준선(ascent)을 잰다. (고정폭 폰트 기준)
	HGDIOBJ old = ::SelectObject(hdc, efont.GetSafeHandle());
	TEXTMETRICW tm;
	::GetTextMetricsW(hdc, &tm);
	SIZE sz;
	::GetTextExtentPoint32W(hdc, L"W", 1, &sz);
	cell_w = sz.cx;
	int eh = tm.tmHeight + tm.tmExternalLeading;
	int ea = tm.tmAscent;

	// 한글 폰트 높이와 기준선도 재서 더 큰 쪽을 채택한다.
	::SelectObject(hdc, kfont.GetSafeHandle());
	::GetTextMetricsW(hdc, &tm);
	int kh = tm.tmHeight + tm.tmExternalLeading;
	int ka = tm.tmAscent;

	// 줄 높이는 두 폰트 중 큰 쪽, 기준선도 두 폰트 중 아래쪽(큰 ascent)에 맞춘다.
	// 이렇게 하면 한글과 영문이 같은 기준선 위에 놓여 글자 바닥이 가지런해진다.
	cell_h = (eh > kh) ? eh : kh;
	cell_base = (ea > ka) ? ea : ka;

	::SelectObject(hdc, old);
	::ReleaseDC(meas_wnd, hdc);
}

void CConBox::set_efont(const char* name, int size, const char* opts)
{
	make_font(efont, efont_lf, name, size, opts);

	// 창이 이미 떠 있으면 칸 크기를 다시 재고 화면을 갱신한다.
	if (::IsWindow(m_hWnd)) {
		calc_cell_size();
		Invalidate();
	}
}

void CConBox::set_kfont(const char* name, int size, const char* opts)
{
	make_font(kfont, kfont_lf, name, size, opts);

	if (::IsWindow(m_hWnd)) {
		calc_cell_size();
		Invalidate();
	}
}

void CConBox::set_color(COLORREF fg)
{
	cur_fg = fg;
}

void CConBox::set_bg_color(COLORREF bg)
{
	cur_bg = bg;
}

void CConBox::set_cursor_size(int height_pct)
{
	cursor_pct = height_pct;
	if (height_pct < 1) cursor_pct = 1;
	if (height_pct > 100) cursor_pct = 100;

	if (::IsWindow(m_hWnd)) {
		CRect rc;
		if (get_cursor_rect(rc))
			InvalidateRect(&rc, FALSE);
	}
}

void CConBox::set_cursor_blink(int interval_ms)
{
	cursor_blink_ms = interval_ms;

	if (::IsWindow(m_hWnd)) {
		// 기존 타이머를 끄고 다시 건다. 간격이 0이면 깜빡임 없이 항상 켜 둔다.
		KillTimer(CURSOR_TIMER);
		cursor_on = true;
		if (cursor_blink_ms > 0)
			SetTimer(CURSOR_TIMER, cursor_blink_ms, NULL);
		Invalidate();
	}
}

bool CConBox::cursor_screen_pos(int& row_out, int& vx_out) const
{
	if (log_buf.empty())
		return false;

	// 커서가 속한 화면 줄을 찾는다.
	for (int sidx = 0; sidx < (int)scr_lines.size(); ++sidx) {
		const ScreenLine& sl = scr_lines[sidx];
		if (sl.log_idx != cur_log)
			continue;
		if (cur_col < sl.char_start || cur_col > sl.char_end)
			continue;

		// 줄바꿈 경계(구간 끝)에 걸쳐 있고 같은 논리 줄의 다음 구간이 이어지면,
		// 커서를 다음 줄 맨 앞에 놓기 위해 이 구간은 건너뛴다.
		if (cur_col == sl.char_end
			&& sidx + 1 < (int)scr_lines.size()
			&& scr_lines[sidx + 1].log_idx == cur_log
			&& scr_lines[sidx + 1].char_start == cur_col) {
			continue;
		}

		// 구간 시작부터 커서 직전까지의 시각적 칸 수를 더한다.
		const LogLine& line = log_buf[sl.log_idx];
		int vx = 0;
		for (int i = sl.char_start; i < cur_col; ++i)
			vx += line.chars[i].wide ? 2 : 1;

		row_out = sidx - scroll_top;
		vx_out = vx;
		return true;
	}
	return false;
}

bool CConBox::get_cursor_rect(CRect& rc) const
{
	int row, vx;
	if (!cursor_screen_pos(row, vx))
		return false;
	if (row < 0 || row >= rows)
		return false;

	// 칸 아래쪽에 cursor_pct 비율 두께의 언더바를 둔다.
	int ch = cell_h * cursor_pct / 100;
	if (ch < 2) ch = 2;

	int px = vx * cell_w;
	int py = row * cell_h;
	rc.SetRect(px, py + cell_h - ch, px + cell_w, py + cell_h);
	return true;
}

void CConBox::update_metrics()
{
	// 클라이언트 크기를 한 칸 크기로 나누어 가로 칸 수와 세로 줄 수를 구한다.
	CRect rc;
	GetClientRect(&rc);

	cols = (cell_w > 0) ? (rc.Width() / cell_w) : 1;
	rows = (cell_h > 0) ? (rc.Height() / cell_h) : 1;
	if (cols < 1) cols = 1;
	if (rows < 1) rows = 1;
}

int CConBox::visual_col() const
{
	// 현재 줄에서 cur_col 직전까지의 글자들이 차지하는 시각적 칸 수를 합산한다.
	int vc = 0;
	const LogLine& line = log_buf[cur_log];
	for (int i = 0; i < cur_col && i < (int)line.chars.size(); ++i)
		vc += line.chars[i].wide ? 2 : 1;
	return vc;
}

void CConBox::put_wchar(wchar_t wc)
{
	// 현재 출력 위치에 글자 하나를 쓴다.
	// cur_col 이 줄 끝이면 추가하고, 중간이면 덮어쓴다(\r 이후의 덮어쓰기 등).
	CharInfo c;
	c.ch = wc;
	c.fg = cur_fg;
	c.bg = cur_bg;
	c.wide = IsWideChar(wc);

	LogLine& line = log_buf[cur_log];
	if (cur_col < (int)line.chars.size())
		line.chars[cur_col] = c;
	else
		line.chars.push_back(c);
	cur_col++;
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

	// 버퍼가 비어 있으면 첫 논리 줄을 만든다.
	if (log_buf.empty()) {
		log_buf.push_back(LogLine());
		cur_log = 0;
		cur_col = 0;
	}

	// 변환된 문자열을 한 글자씩 처리한다. (마지막 원소는 널 종료라 제외)
	for (int i = 0; i < wlen - 1; ++i) {
		wchar_t wc = ws[i];

		if (wc == L'\r') {
			// 현재 줄 맨 처음으로 이동 (이후 출력은 덮어쓰기)
			cur_col = 0;
		}
		else if (wc == L'\n') {
			// 다음 줄 맨 처음으로 이동. 새 논리 줄을 버퍼 끝에 만든다.
			log_buf.push_back(LogLine());
			cur_log = (int)log_buf.size() - 1;
			cur_col = 0;
		}
		else if (wc == L'\t') {
			// 다음 4칸 탭 정지 위치까지 공백으로 채운다.
			int adv = 4 - (visual_col() % 4);
			for (int k = 0; k < adv; ++k)
				put_wchar(L' ');
		}
		else {
			put_wchar(wc);
		}
	}

	// 출력이 끝난 위치가 곧 편집 경계가 된다. (이전 내용은 읽기전용)
	edit_log = cur_log;
	edit_col = cur_col;

	// 줄바꿈을 다시 계산하고 강제로 맨 아래까지 스크롤한 뒤 화면을 갱신한다.
	rebuild_scr_lines();
	scroll_to_bottom();
	update_scrollbar();
	Invalidate();
}

void CConBox::rebuild_scr_lines()
{
	// 모든 논리 줄을 가로폭(cols) 기준으로 잘라 화면 줄 목록을 만든다.
	// 2배 폭 글자가 줄 끝에서 잘리지 않도록, 들어가지 않으면 통째로 다음 줄로 넘긴다.
	scr_lines.clear();

	for (int li = 0; li < (int)log_buf.size(); ++li) {
		const LogLine& line = log_buf[li];
		int n = (int)line.chars.size();

		if (n == 0) {
			// 빈 줄도 화면에서 한 줄을 차지한다.
			ScreenLine sl = { li, 0, 0 };
			scr_lines.push_back(sl);
			continue;
		}

		int start = 0;
		int x = 0;
		for (int i = 0; i < n; ++i) {
			// Shift+Enter 로 넣은 소프트 줄바꿈 마커(L'\n')는 강제로 줄을 끊는다.
			// 마커 자체는 어느 화면 줄에도 포함하지 않아 그려지지 않는다.
			if (line.chars[i].ch == L'\n') {
				ScreenLine sl = { li, start, i };
				scr_lines.push_back(sl);
				start = i + 1;
				x = 0;
				continue;
			}

			int w = line.chars[i].wide ? 2 : 1;
			if (x + w > cols && i > start) {
				// 이번 글자를 넣으면 폭을 넘으므로 여기서 줄을 끊는다.
				ScreenLine sl = { li, start, i };
				scr_lines.push_back(sl);
				start = i;
				x = 0;
			}
			x += w;
		}
		// 남은 마지막 구간을 추가한다. (소프트 줄바꿈으로 끝났다면 빈 구간이 된다.)
		ScreenLine sl = { li, start, n };
		scr_lines.push_back(sl);
	}
}

void CConBox::scroll_to_bottom()
{
	int maxtop = (int)scr_lines.size() - rows;
	scroll_top = (maxtop > 0) ? maxtop : 0;
}

void CConBox::update_scrollbar()
{
	if (!::IsWindow(m_hWnd))
		return;

	int total = (int)scr_lines.size();
	if (total <= rows) {
		// 내용이 한 화면에 다 들어오면 스크롤바를 숨기고 맨 위로 맞춘다.
		ShowScrollBar(SB_VERT, FALSE);
		scroll_top = 0;
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
	si.nPos = scroll_top;
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

	// 창 크기가 바뀌면 화면 칸 수와 줄바꿈을 다시 계산한다.
	update_metrics();
	rebuild_scr_lines();

	// 스크롤 위치가 범위를 벗어나면 맞춰 준다.
	int maxtop = (int)scr_lines.size() - rows;
	if (maxtop < 0) maxtop = 0;
	if (scroll_top > maxtop) scroll_top = maxtop;

	update_scrollbar();
	Invalidate();
}

void CConBox::OnTimer(UINT_PTR id)
{
	if (id == CURSOR_TIMER) {
		// 커서 표시 상태를 뒤집고 커서 자리만 다시 그린다.
		cursor_on = !cursor_on;

		// IME 조합 중에는 조합 언더바(여러 칸 폭)가 커서 역할을 하므로
		// 그 영역 전체를 무효화해 깜빡임이 한 칸만 갱신되지 않게 한다.
		if (!ime_comp.empty()) {
			int row, vx;
			if (cursor_screen_pos(row, vx) && row >= 0 && row < rows) {
				int vw = 0;
				for (size_t k = 0; k < ime_comp.size(); ++k)
					vw += IsWideChar(ime_comp[k]) ? 2 : 1;
				CRect rc(vx * cell_w, row * cell_h,
					(vx + vw) * cell_w, (row + 1) * cell_h);
				InvalidateRect(&rc, FALSE);
			}
			return;
		}

		CRect rc;
		if (get_cursor_rect(rc))
			InvalidateRect(&rc, FALSE);
		return;
	}
	CWnd::OnTimer(id);
}

UINT CConBox::OnGetDlgCode()
{
	// 부모 대화상자가 가로채지 않도록, 방향키/Tab/Enter/일반문자를 모두 직접 받는다.
	return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB;
}

void CConBox::OnVScroll(UINT code, UINT pos, CScrollBar* sb)
{
	int maxtop = (int)scr_lines.size() - rows;
	if (maxtop < 0) maxtop = 0;

	int nt = scroll_top;
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
	if (nt != scroll_top) {
		scroll_top = nt;
		update_scrollbar();
		Invalidate();
	}
}

BOOL CConBox::OnMouseWheel(UINT flags, short zDelta, CPoint pt)
{
	// 휠 한 칸(120)당 세 줄씩 스크롤한다. 위로 굴리면 위쪽(이전 내용)으로 간다.
	int maxtop = (int)scr_lines.size() - rows;
	if (maxtop < 0) maxtop = 0;

	int nt = scroll_top - (zDelta / 120) * 3;
	if (nt < 0) nt = 0;
	if (nt > maxtop) nt = maxtop;
	if (nt != scroll_top) {
		scroll_top = nt;
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
			cf.ptCurrentPos.x = vx * cell_w;
			cf.ptCurrentPos.y = row * cell_h;
			ImmSetCompositionWindow(himc, &cf);

			// 후보 목록 창은 커서 바로 아래에 뜨도록 한다.
			CANDIDATEFORM caf;
			ZeroMemory(&caf, sizeof(caf));
			caf.dwIndex = 0;
			caf.dwStyle = CFS_CANDIDATEPOS;
			caf.ptCurrentPos.x = vx * cell_w;
			caf.ptCurrentPos.y = (row + 1) * cell_h;
			ImmSetCandidateWindow(himc, &caf);

			ImmSetCompositionFontW(himc, &kfont_lf);
		}
		ImmReleaseContext(m_hWnd, himc);
	}
	return 0;
}

LRESULT CConBox::OnImeComp(WPARAM w, LPARAM l)
{
	HIMC himc = ImmGetContext(m_hWnd);
	if (himc == NULL)
		return Default();

	// 확정된 결과 문자열은 곧바로 버퍼에 삽입한다.
	if (l & GCS_RESULTSTR) {
		LONG bytes = ImmGetCompositionStringW(himc, GCS_RESULTSTR, NULL, 0);
		int n = bytes / (int)sizeof(wchar_t);
		if (n > 0) {
			std::wstring r;
			r.resize(n);
			ImmGetCompositionStringW(himc, GCS_RESULTSTR, &r[0], bytes);
			for (int i = 0; i < n; ++i)
				insert_wchar(r[i]);
		}
		ime_comp.clear();
	}

	// 조합 중(미확정) 문자열은 따로 보관해 두었다가 커서 위치에 그린다.
	if (l & GCS_COMPSTR) {
		LONG bytes = ImmGetCompositionStringW(himc, GCS_COMPSTR, NULL, 0);
		int n = bytes / (int)sizeof(wchar_t);
		ime_comp.resize(n > 0 ? n : 0);
		if (n > 0)
			ImmGetCompositionStringW(himc, GCS_COMPSTR, &ime_comp[0], bytes);
	}

	ImmReleaseContext(m_hWnd, himc);

	cursor_on = true;
	rebuild_scr_lines();
	scroll_to_bottom();
	update_scrollbar();
	Invalidate();
	return 0;   // 기본 인라인 조합 그리기를 막고 우리가 직접 그린다.
}

LRESULT CConBox::OnImeEnd(WPARAM w, LPARAM l)
{
	// 조합이 끝나면 미확정 문자열을 비운다.
	ime_comp.clear();
	Invalidate();
	return 0;
}

void CConBox::hit_test(CPoint pt, int& out_log, int& out_col) const
{
	out_log = 0;
	out_col = 0;
	if (scr_lines.empty())
		return;

	// 픽셀 y -> 화면 줄 -> 화면 줄 목록 인덱스
	int row = (cell_h > 0) ? (pt.y / cell_h) : 0;
	int sidx = scroll_top + row;
	if (sidx < 0) sidx = 0;
	if (sidx >= (int)scr_lines.size()) sidx = (int)scr_lines.size() - 1;

	const ScreenLine& sl = scr_lines[sidx];
	const LogLine& line = log_buf[sl.log_idx];

	// 픽셀 x -> 줄 안에서의 글자 인덱스 (칸 절반을 기준으로 앞/뒤를 가른다)
	int target = (cell_w > 0) ? (pt.x / cell_w) : 0;
	int x = 0;
	for (int i = sl.char_start; i < sl.char_end; ++i) {
		if (line.chars[i].ch == L'\n')
			break;
		int w = line.chars[i].wide ? 2 : 1;
		if (target < x + w) {
			out_log = sl.log_idx;
			out_col = (target < x + (w + 1) / 2) ? i : i + 1;
			return;
		}
		x += w;
	}

	// 줄 끝을 넘겨 클릭하면 그 화면 줄의 끝 위치로 둔다.
	out_log = sl.log_idx;
	out_col = sl.char_end;
}

void CConBox::get_sel_range(int& slog, int& scol, int& elog, int& ecol) const
{
	// 두 끝점을 앞->뒤 순서로 정렬한다.
	bool a_first = (sel_a_log < sel_c_log)
		|| (sel_a_log == sel_c_log && sel_a_col <= sel_c_col);
	if (a_first) {
		slog = sel_a_log; scol = sel_a_col;
		elog = sel_c_log; ecol = sel_c_col;
	} else {
		slog = sel_c_log; scol = sel_c_col;
		elog = sel_a_log; ecol = sel_a_col;
	}
}

bool CConBox::pos_selected(int log, int idx) const
{
	int slog, scol, elog, ecol;
	get_sel_range(slog, scol, elog, ecol);

	// (log, idx) 가 [시작, 끝) 범위에 드는지 (논리 줄, 글자) 순서로 비교한다.
	bool after_start = (log > slog) || (log == slog && idx >= scol);
	bool before_end = (log < elog) || (log == elog && idx < ecol);
	return after_start && before_end;
}

std::wstring CConBox::collect_selection() const
{
	int slog, scol, elog, ecol;
	get_sel_range(slog, scol, elog, ecol);

	std::wstring out;
	for (int li = slog; li <= elog && li < (int)log_buf.size(); ++li) {
		const LogLine& line = log_buf[li];
		int n = (int)line.chars.size();
		int from = (li == slog) ? scol : 0;
		int to = (li == elog) ? ecol : n;
		if (from < 0) from = 0;
		if (to > n) to = n;

		for (int i = from; i < to; ++i)
			out.push_back(line.chars[i].ch);   // 소프트 줄바꿈 마커(L'\n')도 그대로

		// 논리 줄 사이에는 줄바꿈을 넣는다.
		if (li != elog)
			out.push_back(L'\n');
	}
	return out;
}

void CConBox::copy_selection_to_clipboard()
{
	if (!sel_valid)
		return;

	std::wstring s = collect_selection();
	if (s.empty())
		return;

	if (!OpenClipboard())
		return;
	EmptyClipboard();

	size_t bytes = (s.size() + 1) * sizeof(wchar_t);
	HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (hmem) {
		void* dst = GlobalLock(hmem);
		if (dst) {
			memcpy(dst, s.c_str(), bytes);
			GlobalUnlock(hmem);
			SetClipboardData(CF_UNICODETEXT, hmem);
		}
	}
	CloseClipboard();
}

void CConBox::paste_from_clipboard()
{
	if (log_buf.empty())
		return;
	if (!IsClipboardFormatAvailable(CF_UNICODETEXT))
		return;
	if (!OpenClipboard())
		return;

	HANDLE hmem = GetClipboardData(CF_UNICODETEXT);
	if (hmem) {
		const wchar_t* src = (const wchar_t*)GlobalLock(hmem);
		if (src) {
			// 커서 위치(편집 영역)에 글자를 끼워 넣는다.
			// 줄바꿈은 소프트 줄바꿈으로 넣어 현재 입력 줄 안에 머무르게 한다.
			for (const wchar_t* p = src; *p != L'\0'; ++p) {
				wchar_t c = *p;
				if (c == L'\r')
					continue;
				if (c == L'\n')
					insert_wchar(L'\n');
				else
					insert_wchar(c);
			}
			GlobalUnlock(hmem);
		}
	}
	CloseClipboard();

	sel_valid = false;
	cursor_on = true;
	rebuild_scr_lines();
	scroll_to_bottom();
	update_scrollbar();
	Invalidate();
}

void CConBox::OnLButtonDown(UINT f, CPoint p)
{
	// 클릭하면 포커스를 가져오고 선택의 고정점을 정한다.
	SetFocus();

	int log, col;
	hit_test(p, log, col);
	sel_a_log = sel_c_log = log;
	sel_a_col = sel_c_col = col;
	sel_valid = false;   // 아직 드래그 전이므로 선택 없음
	selecting = true;
	SetCapture();
	Invalidate();
}

void CConBox::OnMouseMove(UINT f, CPoint p)
{
	if (!selecting)
		return;

	int log, col;
	hit_test(p, log, col);
	sel_c_log = log;
	sel_c_col = col;
	// 고정점과 달라지면 유효한 선택으로 본다.
	sel_valid = (sel_a_log != sel_c_log) || (sel_a_col != sel_c_col);
	Invalidate();
}

void CConBox::OnLButtonUp(UINT f, CPoint p)
{
	if (!selecting)
		return;
	selecting = false;
	ReleaseCapture();

	// 드래그로 선택된 텍스트가 있으면 자동으로 클립보드에 복사한다.
	if (sel_valid)
		copy_selection_to_clipboard();
	Invalidate();
}

void CConBox::OnRButtonDown(UINT f, CPoint p)
{
	// 우클릭: 포커스를 가져오고 커서 위치에 붙여넣는다.
	SetFocus();
	paste_from_clipboard();
}

void CConBox::insert_wchar(wchar_t wc)
{
	// 커서 위치에 글자를 끼워 넣는다. (덮어쓰지 않는다)
	CharInfo c;
	c.ch = wc;
	c.fg = cur_fg;
	c.bg = cur_bg;
	c.wide = IsWideChar(wc);

	LogLine& line = log_buf[cur_log];
	if (cur_col > (int)line.chars.size())
		cur_col = (int)line.chars.size();
	line.chars.insert(line.chars.begin() + cur_col, c);
	cur_col++;
}

void CConBox::OnChar(UINT ch, UINT rep, UINT flags)
{
	// 제어문자(Enter, Tab, Backspace 등)는 OnKeyDown 에서 처리하므로 무시한다.
	if (ch < 0x20 || ch == 0x7F)
		return;
	if (log_buf.empty())
		return;

	// 글자를 입력하면 기존 선택 표시는 해제한다.
	sel_valid = false;

	for (UINT k = 0; k < rep; ++k)
		insert_wchar((wchar_t)ch);

	cursor_on = true;
	rebuild_scr_lines();
	scroll_to_bottom();
	update_scrollbar();
	Invalidate();
}

void CConBox::OnKeyDown(UINT vk, UINT rep, UINT flags)
{
	if (log_buf.empty()) {
		CWnd::OnKeyDown(vk, rep, flags);
		return;
	}

	bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
	bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;

	// Ctrl+C: 복사, Ctrl+V: 붙여넣기. (복사는 선택을 지우지 않는다.)
	if (ctrl && (vk == 'C' || vk == 'c')) {
		copy_selection_to_clipboard();
		return;
	}
	if (ctrl && (vk == 'V' || vk == 'v')) {
		paste_from_clipboard();
		return;
	}

	// 그 밖의 키 조작은 선택 표시를 해제한다.
	sel_valid = false;

	LogLine& line = log_buf[cur_log];
	int len = (int)line.chars.size();

	switch (vk) {
	case VK_LEFT:
		// 편집 경계 이전으로는 갈 수 없다.
		if (cur_col > edit_col)
			cur_col--;
		break;

	case VK_RIGHT:
		if (cur_col < len)
			cur_col++;
		break;

	case VK_HOME:
		if (ctrl) {
			// Ctrl+Home: 버퍼 맨 위로 스크롤만 한다. (커서는 그대로)
			scroll_top = 0;
			update_scrollbar();
			Invalidate();
			return;
		}
		cur_col = edit_col;
		break;

	case VK_END:
		if (ctrl) {
			// Ctrl+End: 맨 아래로 스크롤만 한다.
			scroll_to_bottom();
			update_scrollbar();
			Invalidate();
			return;
		}
		cur_col = len;
		break;

	case VK_BACK:
		// 편집 경계 안에서만 앞 글자를 지운다.
		if (cur_col > edit_col) {
			line.chars.erase(line.chars.begin() + (cur_col - 1));
			cur_col--;
		}
		break;

	case VK_DELETE:
		// 커서 위치의 글자를 지운다. (편집 영역 안에서만)
		if (cur_col >= edit_col && cur_col < len)
			line.chars.erase(line.chars.begin() + cur_col);
		break;

	case VK_TAB: {
		// 다음 4칸 탭 정지 위치까지 공백을 끼워 넣는다.
		int adv = 4 - (visual_col() % 4);
		for (int k = 0; k < adv; ++k)
			insert_wchar(L' ');
		break;
	}

	case VK_RETURN:
		if (shift)
			insert_wchar(L'\n');   // Shift+Enter: 소프트 줄바꿈(미확정)
		else
			commit_input();        // Enter: 입력 확정 + 콜백
		break;

	default:
		// 그 밖의 키는 기본 처리에 맡긴다.
		CWnd::OnKeyDown(vk, rep, flags);
		return;
	}

	cursor_on = true;
	rebuild_scr_lines();
	scroll_to_bottom();
	update_scrollbar();
	Invalidate();
}

void CConBox::commit_input()
{
	// 편집 영역(edit_col 부터 줄 끝까지)의 글자를 모아 콜백 문자열을 만든다.
	// 소프트 줄바꿈 마커(L'\n')도 그대로 담아 \n 으로 전달한다.
	std::wstring w;
	{
		LogLine& line = log_buf[edit_log];
		for (int i = edit_col; i < (int)line.chars.size(); ++i)
			w.push_back(line.chars[i].ch);
	}

	// 입력 내용은 버퍼에 그대로 남아 읽기전용이 되고, 새 논리 줄로 이동한다.
	log_buf.push_back(LogLine());
	cur_log = (int)log_buf.size() - 1;
	cur_col = 0;
	edit_log = cur_log;
	edit_col = 0;

	rebuild_scr_lines();
	scroll_to_bottom();
	update_scrollbar();
	Invalidate();

	// 콜백을 호출한다. 콜백이 print() 를 불러도 안전하도록 모든 상태 갱신 뒤에 부른다.
	if (on_enter != nullptr) {
		int len = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
		std::vector<char> u8(len + 1, 0);
		if (len > 0)
			::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), u8.data(), len, NULL, NULL);
		on_enter(u8.data());
	}
}

void CConBox::OnPaint()
{
	CPaintDC dc(this);

	// 클라이언트 영역 전체를 현재 배경색으로 채운다.
	CRect rc;
	GetClientRect(&rc);
	dc.FillSolidRect(rc, cur_bg);

	// 글자는 칸의 배경을 칠한 뒤 투명 모드로 그린다.
	// 기준선(TA_BASELINE) 정렬로 그려 한글과 영문의 글자 바닥을 맞춘다.
	dc.SetBkMode(TRANSPARENT);
	UINT old_align = dc.SetTextAlign(TA_LEFT | TA_BASELINE);
	CFont* old_font = dc.SelectObject(&efont);

	// 화면에 보이는 줄들을 위에서 아래로 그린다.
	for (int row = 0; row < rows; ++row) {
		int sidx = scroll_top + row;
		if (sidx < 0 || sidx >= (int)scr_lines.size())
			break;

		const ScreenLine& sl = scr_lines[sidx];
		const LogLine& line = log_buf[sl.log_idx];

		int x = 0;   // 현재 줄에서의 가로 칸 위치
		for (int i = sl.char_start; i < sl.char_end; ++i) {
			const CharInfo& c = line.chars[i];

			// 소프트 줄바꿈 마커는 그리지 않는다. (폭도 차지하지 않음)
			if (c.ch == L'\n')
				continue;

			int w = c.wide ? 2 : 1;

			int px = x * cell_w;
			int py = row * cell_h;

			// 선택된 글자는 전경색과 배경색을 뒤집어 강조한다.
			COLORREF fg = c.fg;
			COLORREF bg = c.bg;
			if (sel_valid && pos_selected(sl.log_idx, i)) {
				fg = c.bg;
				bg = c.fg;
			}

			// 칸 배경을 칠한다. (기본 배경과 다를 수 있으므로 항상 칠한다.)
			CRect cell(px, py, px + w * cell_w, py + cell_h);
			dc.FillSolidRect(cell, bg);

			// 글자를 그린다. 2배 폭 글자는 한글 폰트, 그 외는 영문 폰트를 쓴다.
			// 기준선 정렬이므로 세로 위치는 칸 위쪽 + 기준선만큼 내린 곳이다.
			dc.SelectObject(c.wide ? &kfont : &efont);
			dc.SetTextColor(fg);
			dc.TextOutW(px, py + cell_base, &c.ch, 1);

			x += w;
		}
	}

	// IME 로 조합 중인(미확정) 문자열을 커서 위치에 임시로 그린다.
	// 조합 중임을 나타내기 위해 글자 아래에 두꺼운 언더바를 깐다.
	// 이 언더바가 곧 조합 중의 커서 역할을 하므로, 글자는 항상 보이되
	// 언더바는 cursor_on 상태에 맞춰 깜빡인다.
	if (!ime_comp.empty()) {
		int row, vx;
		if (cursor_screen_pos(row, vx) && row >= 0 && row < rows) {
			// 조합 언더바 두께. 일반 커서보다 살짝 얇게 잡되 최소 2px 는 보장한다.
			int uth = cell_h * cursor_pct / 100;
			if (uth < 2) uth = 2;

			int x = vx;
			for (size_t k = 0; k < ime_comp.size(); ++k) {
				wchar_t c = ime_comp[k];
				bool wide = IsWideChar(c);
				int w = wide ? 2 : 1;
				int px = x * cell_w;
				int py = row * cell_h;

				dc.FillSolidRect(px, py, w * cell_w, cell_h, cur_bg);
				dc.SelectObject(wide ? &kfont : &efont);
				dc.SetTextColor(cur_fg);
				dc.TextOutW(px, py + cell_base, &c, 1);

				// 조합 언더바는 깜빡임 상태일 때만 그린다.
				if (cursor_on)
					dc.FillSolidRect(px, py + cell_h - uth, w * cell_w, uth, cur_fg);

				x += w;
			}
		}
	}

	dc.SelectObject(old_font);
	dc.SetTextAlign(old_align);

	// 커서(언더바)를 그린다. 깜빡임으로 꺼진 상태이거나 화면 밖이면 그리지 않는다.
	// IME 조합 중에는 위에서 조합 언더바가 커서 역할을 하므로 일반 커서는 생략한다.
	if (cursor_on && ime_comp.empty()) {
		CRect cur;
		if (get_cursor_rect(cur))
			dc.FillSolidRect(cur, cur_fg);
	}
}
