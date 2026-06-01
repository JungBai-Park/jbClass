// ConBox.h
//
// 이식 가능한 터미널 컨트롤 ConBox 의 클래스 선언.
// wt.exe 처럼 텍스트 입력과 출력을 모두 처리하는 MFC CWnd 파생 컨트롤이다.
// 외부로 노출되는 모든 문자열 API 는 UTF-8(char*) 을 사용한다.
//
// 이 파일은 데모 프로젝트(Project/)와 독립적으로 동작하도록
// 미리 컴파일된 헤더(pch.h)에 의존하지 않고 필요한 헤더를 직접 포함한다.

#pragma once

#include <afxwin.h>   // MFC 코어 (CWnd, CDC, CFont 등)
#include <vector>     // 내부 버퍼
#include <string>     // IME 조합 문자열

// 화면 한 칸(셀)에 들어가는 한 글자의 정보.
// 한글처럼 2배 폭 글자는 wide=true 로 표시하며, 항목 하나가 글자 하나에 대응한다.
// (2배 폭 글자는 그릴 때 가로로 두 칸을 차지한다.)
struct CharInfo {
	wchar_t  ch;     // 글자 (UTF-16)
	COLORREF fg;     // 전경색
	COLORREF bg;     // 배경색
	bool     wide;   // 2배 폭 글자 여부 (한글, CJK 등)
};

// 논리 줄: 사용자가 Enter 를 친 단위. Shift+Enter 는 같은 논리 줄 안에서 처리한다.
struct LogLine {
	std::vector<CharInfo> chars;
};

// 화면 줄: 자동 줄바꿈을 적용한 뒤 실제로 한 줄에 그려지는 범위.
// 하나의 논리 줄(log_idx)에서 [char_start, char_end) 구간을 가리킨다.
struct ScreenLine {
	int log_idx;
	int char_start;
	int char_end;
};

// 터미널 컨트롤 본체.
// 사용 순서: set_efont/set_kfont 로 폰트를 먼저 지정한 뒤 open() 으로 창을 만든다.
class CConBox : public CWnd
{
public:
	CConBox();
	virtual ~CConBox();

	// 부모 창 안에 좌표 (x0,y0)-(x1,y1) 위치로 자식 창을 생성한다.
	// 리소스 에디터 없이 좌표만으로 창이 만들어진다.
	void open(CWnd* parent, int x0, int y0, int x1, int y1);

	// 영문 폰트를 지정한다. 크기 단위는 포인트, opts 는 추가설정 문자열이다.
	// (명세의 char* 대신 const char* 로 받는다. 문자열 리터럴 호출을 위해서이다.)
	void set_efont(const char* name, int size, const char* opts);

	// 한글 폰트를 지정한다. 인자 규칙은 set_efont 와 같다.
	void set_kfont(const char* name, int size, const char* opts);

	// 텍스트를 출력한다. 항상 버퍼 끝(현재 출력 위치)에 이어서 쓴다.
	// text 는 UTF-8 이며 특수문자를 다음과 같이 처리한다.
	//   \n : 다음 줄 맨 처음으로 이동, \r : 현재 줄 맨 처음으로 이동(덮어쓰기), \t : 4칸 탭
	// 출력 후에는 강제로 맨 아래까지 스크롤한다.
	void print(const char* text);

	// 현재 전경색(글자색)을 지정한다. 이후 출력/입력에 적용된다.
	void set_color(COLORREF fg);

	// 현재 배경색을 지정한다. 이후 출력/입력에 적용된다.
	void set_bg_color(COLORREF bg);

	// 커서 높이를 칸 높이 대비 백분율로 지정한다. (기본: 약간 두꺼운 언더바)
	void set_cursor_size(int height_pct);

	// 커서 깜빡임 간격(ms)을 지정한다. 0이면 깜빡이지 않고 항상 켜 둔다.
	void set_cursor_blink(int interval_ms);

	// Enter 로 입력이 확정되면 호출되는 콜백.
	// input 은 확정된 입력 전체(UTF-8)이며, Shift+Enter 로 넣은 줄바꿈은 \n 으로 포함된다.
	// nullptr 이면 호출하지 않는다. 사용 측에서 직접 대입한다. (예: box.on_enter = MyFunc;)
	void (*on_enter)(const char* input);

protected:
	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC* dc);
	afx_msg void OnSize(UINT type, int cx, int cy);
	afx_msg void OnTimer(UINT_PTR id);
	afx_msg void OnChar(UINT ch, UINT rep, UINT flags);
	afx_msg void OnKeyDown(UINT vk, UINT rep, UINT flags);
	afx_msg UINT OnGetDlgCode();
	afx_msg void OnVScroll(UINT code, UINT pos, CScrollBar* sb);
	afx_msg BOOL OnMouseWheel(UINT flags, short zDelta, CPoint pt);
	// 한글 IME 처리 (Windows 표준 방식의 조합 과정 표시)
	afx_msg LRESULT OnImeStart(WPARAM w, LPARAM l);
	afx_msg LRESULT OnImeComp(WPARAM w, LPARAM l);
	afx_msg LRESULT OnImeEnd(WPARAM w, LPARAM l);
	// 마우스 선택 및 붙여넣기
	afx_msg void OnLButtonDown(UINT f, CPoint p);
	afx_msg void OnMouseMove(UINT f, CPoint p);
	afx_msg void OnLButtonUp(UINT f, CPoint p);
	afx_msg void OnRButtonDown(UINT f, CPoint p);
	DECLARE_MESSAGE_MAP()

private:
	// 폰트 한 벌을 만들어 font 에 채우고, 만든 LOGFONT 를 lf_out 에 남긴다.
	void make_font(CFont& font, LOGFONTW& lf_out, const char* name, int size, const char* opts);

	// 아직 지정되지 않은 폰트에 기본값(영문 Consolas, 한글 맑은 고딕)을 채운다.
	void apply_default_fonts();

	// 현재 폰트를 기준으로 고정 그리드 한 칸의 가로/세로 픽셀 크기를 잰다.
	void calc_cell_size();

	// 클라이언트 영역 크기로부터 화면에 들어가는 가로 칸 수(cols), 세로 줄 수(rows)를 구한다.
	void update_metrics();

	// 논리 줄 버퍼를 가로폭(cols) 기준으로 자동 줄바꿈하여 화면 줄 목록을 다시 만든다.
	void rebuild_scr_lines();

	// 스크롤을 강제로 맨 아래로 내린다.
	void scroll_to_bottom();

	// 세로 스크롤바의 범위/위치를 갱신하고, 내용이 다 보이면 숨긴다.
	void update_scrollbar();

	// 현재 출력 위치(cur_log/cur_col)에 글자 하나를 쓴다. (덮어쓰기 또는 끝에 추가)
	void put_wchar(wchar_t wc);

	// 커서 위치에 글자 하나를 끼워 넣는다. (사용자 입력용, 덮어쓰지 않고 삽입)
	void insert_wchar(wchar_t wc);

	// 현재 입력을 확정한다. 콜백을 호출하고 새 줄로 이동한다.
	void commit_input();

	// cur_col 위치까지의 시각적 가로 칸 수(2배 폭 글자는 2로 계산)를 구한다.
	int visual_col() const;

	// 현재 커서 위치(cur_log/cur_col)를 화면 좌표로 변환한다.
	// row_out 은 화면 맨 위 기준 줄 번호, vx_out 은 줄 안에서의 가로 칸이다.
	// 커서가 어느 화면 줄에도 대응되지 않으면 false 를 돌려준다.
	bool cursor_screen_pos(int& row_out, int& vx_out) const;

	// 현재 커서가 그려질 사각형(언더바)을 구한다. 화면 밖이면 false.
	bool get_cursor_rect(CRect& rc) const;

	// 마우스 픽셀 좌표를 버퍼 위치(논리 줄, 글자 인덱스)로 변환한다.
	void hit_test(CPoint pt, int& out_log, int& out_col) const;

	// 선택 영역의 시작/끝을 항상 앞->뒤 순서로 정렬해 돌려준다.
	void get_sel_range(int& slog, int& scol, int& elog, int& ecol) const;

	// (log, idx) 위치의 글자가 현재 선택 영역에 들어가는지 판정한다.
	bool pos_selected(int log, int idx) const;

	// 선택된 텍스트를 모은다. (논리 줄 경계와 소프트 줄바꿈은 \n 으로)
	std::wstring collect_selection() const;

	// 선택된 텍스트를 클립보드에 복사한다.
	void copy_selection_to_clipboard();

	// 클립보드의 텍스트를 커서 위치에 붙여 넣는다.
	void paste_from_clipboard();

	COLORREF cur_fg;   // 현재 전경색 (기본 흰색)
	COLORREF cur_bg;   // 현재 배경색 (기본 검정)

	CFont efont;       // 영문 폰트
	CFont kfont;       // 한글 폰트
	LOGFONTW efont_lf; // 영문 폰트의 LOGFONT 사본
	LOGFONTW kfont_lf; // 한글 폰트의 LOGFONT 사본

	int cell_w;        // 영문 한 글자 칸의 가로 픽셀 (한글은 2배)
	int cell_h;        // 한 줄 칸의 세로 픽셀
	int cell_base;     // 칸 위쪽에서 글자 기준선까지의 픽셀 (베이스라인 정렬용)

	int cols;          // 화면에 들어가는 가로 칸 수
	int rows;          // 화면에 들어가는 세로 줄 수

	std::vector<LogLine>    log_buf;    // 논리 줄 버퍼 (전체 내용)
	std::vector<ScreenLine> scr_lines;  // 자동 줄바꿈 적용된 화면 줄 목록

	int scroll_top;    // 화면 맨 위에 보이는 scr_lines 인덱스
	int cur_log;       // 현재 출력/입력 위치(커서)의 논리 줄 인덱스
	int cur_col;       // 현재 출력/입력 위치(커서)의 줄 내 글자 인덱스

	int edit_log;      // 편집 가능 영역이 시작되는 논리 줄 (보통 마지막 줄)
	int edit_col;      // 그 줄에서 편집이 시작되는 글자 인덱스 (경계 이전은 읽기전용)

	int  cursor_pct;       // 커서 높이 (칸 높이 대비 %)
	int  cursor_blink_ms;  // 깜빡임 간격 (ms), 0이면 깜빡이지 않음
	bool cursor_on;        // 깜빡임에 따른 현재 표시 상태

	std::wstring ime_comp; // IME 로 조합 중인(미확정) 문자열. 커서 위치에 임시로 그린다.

	bool selecting;        // 마우스 드래그로 선택 중인지
	bool sel_valid;        // 유효한 선택 영역이 있는지
	int sel_a_log, sel_a_col;  // 선택 고정점(드래그 시작)
	int sel_c_log, sel_c_col;  // 선택 이동점(드래그 현재)

	// 커서 깜빡임 타이머 식별자
	enum { CURSOR_TIMER = 1 };
};
