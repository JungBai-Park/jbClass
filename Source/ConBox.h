// ConBox.h
//
// 이식 가능한 터미널 컨트롤 ConBox 의 클래스 선언.
// wt.exe 처럼 텍스트 입력과 출력을 모두 처리하는 MFC CWnd 파생 컨트롤이다.
//
// 이식 단위는 ConBox.h / ConBox.cpp 두 파일뿐이다.
// "사용"만 할 때는 이 헤더만 읽으면 충분하다 — 구현 세부(ConBox.cpp)는 동작을 "수정"할 때만
// 본다. (cpp 통독은 토큰 낭비이며, 그 파일 상단 목차로 필요한 함수만 찾아 부분만 보면 된다.)
//
// --- 요구사항 / 의존성 ---
//   - MFC(유니코드) + Windows 전용. afxwin.h 에 의존한다.
//   - 한글 IME 용 imm32.lib 는 ConBox.cpp 가 #pragma comment 로 자동 링크한다(호스트 설정 불필요).
//   - 글자를 선명하게 하려면 "호스트 앱"이 시작 시 SetProcessDPIAware() 를 호출한다
//     (DPI 인식은 프로세스 단위 속성이라 이 컨트롤이 아니라 호스트가 켠다).
//   - 외부로 노출되는 모든 문자열 API 는 UTF-8(const char*) 을 받는다.
//   - 이 파일은 미리 컴파일된 헤더(pch.h)에 의존하지 않고 필요한 헤더를 직접 포함한다(단독 컴파일 가능).
//   - .h/.cpp 는 UTF-8 BOM 으로 저장한다(BOM 이 없으면 MSVC 가 CP949 로 오인해 한글 주석이 깨진다).
//
// --- 사용 예 (호스트의 부모 창에서) ---
//     CConBox box;                            // 보통 멤버로 보유한다
//     box.set_efont("Consolas", 13, "B");     // (선택) 폰트 지정. 생략하면 기본값을 쓴다
//     box.open(parent, 0, 0, 400, 300);       // 좌표로 자식 창 생성
//     box.on_enter = OnEnter;                 // (선택) Enter 확정 콜백 연결
//     box.print("hello\n");                   // 출력 (UTF-8)
//     // 콜백 시그니처:  void OnEnter(const char* utf8) { ... }

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
	void set_efont(const char* name, float size, const char* opts);

	// 한글 폰트를 지정한다. 인자 규칙은 set_efont 와 같다.
	// 단 size 가 0 이하이면 영문 폰트와 같은 픽셀(em) 높이로 맞추는 match 모드가 켜진다.
	// 이 경우 size 값 자체는 무시되고, 한글을 영문과 같은 높이로 다시 만들어 줄 높이가
	// 영문 기준으로 촘촘해진다(박스 그리기 문자의 세로선 연결에 유리). 양수이면 match
	// 모드가 꺼져 한글은 그 크기 그대로이고 줄 높이는 두 폰트 중 큰 쪽이 된다.
	// 아무 설정도 하지 않은 기본 상태는 match 켜짐이다.
	void set_kfont(const char* name, float size, const char* opts);

	// match 모드에서 한 칸 폭(장평)을 정하는 두 비율을 지정한다. match 모드일 때만 효과가 있다.
	// fill_ratio: 한글 한 글자가 두 칸을 채우는 비율(기본 0.92). 키우면 영문 칸이 좁아져
	//   한글 자간이 줄고, 줄이면 자간이 넓어진다.
	// emin_ratio: 영문을 자연 폭의 이 비율 밑으로는 좁히지 않는 가독성 하한(기본 0.7).
	void set_kfont_fill(float fill_ratio, float emin_ratio);

	// 텍스트를 출력한다. 항상 버퍼 끝(현재 출력 위치)에 이어서 쓴다.
	// text 는 UTF-8 이며 특수문자를 다음과 같이 처리한다.
	//   \n : 다음 줄 맨 처음으로 이동, \r : 현재 줄 맨 처음으로 이동(덮어쓰기), \t : 4칸 탭
	// 출력 후에는 강제로 맨 아래까지 스크롤한다.
	void print(const char* text);

	// 현재 전경색(글자색)을 지정한다. 이후 출력/입력에 적용된다.
	void set_color(COLORREF fg);

	// 현재 배경색을 지정한다. 이후 출력/입력에 적용된다.
	void set_bg_color(COLORREF bg);

	// 블록 커서 색을 배경색과 전경색의 혼합으로 정할 때의 비율을 지정한다.
	// (배경:전경) 가중치이며 기본값은 6:4 (배경에 가까운 색). 합으로 정규화하므로
	// (6,4) 와 (60,40) 은 같다. 합이 0 이면 무시한다.
	void set_cursor_blend(int bg_weight, int fg_weight);

	// 커서 깜빡임 간격(밀리초)을 지정한다. interval_ms 가 0 이면 깜빡이지 않고 항상
	// 켜 둔다. 기본값은 시스템 캐럿 깜빡임 속도(GetCaretBlinkTime)를 따른다.
	void set_cursor_blink(int interval_ms);

	// 클라이언트 영역 안쪽 여백(픽셀)을 지정한다. 여백 안쪽에만 글자가 그려진다.
	// CSS 단축 스타일로 생략한 변은 다른 변을 따른다.
	//   left   가 음수면 top 을 따른다.
	//   bottom 이 음수면 top 을 따른다.
	//   right  가 음수면 (위에서 결정된) left 를 따른다.
	// 예: set_margin(5) -> 사방 5,  set_margin(5,8) -> 상하 5 / 좌우 8.
	// 기본값은 사방 10 이며, open 이후 호출하면 resize 처럼 칸 수/줄 수를 재계산한다.
	void set_margin(int top, int left = -1, int bottom = -1, int right = -1);

	// Enter 로 입력이 확정되면 호출되는 콜백.
	// input 은 확정된 입력 전체(UTF-8)이며, Shift+Enter 로 넣은 줄바꿈은 \n 으로 포함된다.
	// nullptr 이면 호출하지 않는다. 사용 측에서 직접 대입한다. (예: box.on_enter = MyFunc;)
	void (*on_enter)(const char* input);

protected:
	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC* dc);
	afx_msg void OnSize(UINT type, int cx, int cy);
	afx_msg void OnChar(UINT ch, UINT rep, UINT flags);
	afx_msg void OnKeyDown(UINT vk, UINT rep, UINT flags);
	afx_msg UINT OnGetDlgCode();
	afx_msg void OnVScroll(UINT code, UINT pos, CScrollBar* sb);
	afx_msg BOOL OnMouseWheel(UINT flags, short zDelta, CPoint pt);
	// 커서 깜빡임 타이머. 표시 상태를 뒤집고 커서 자리만 다시 그린다.
	afx_msg void OnTimer(UINT_PTR id);
	// 한글 IME 처리 (Windows 표준 방식의 조합 과정 표시)
	afx_msg LRESULT OnImeStart(WPARAM w, LPARAM l);
	afx_msg LRESULT OnImeComp(WPARAM w, LPARAM l);
	afx_msg LRESULT OnImeEnd(WPARAM w, LPARAM l);
	// 한/영 토글 등 IME 상태 변화 통지. 한글 모드로 바뀌면 커서 모양을 즉시 갱신한다.
	afx_msg LRESULT OnImeNotify(WPARAM w, LPARAM l);
	// 마우스 선택 및 붙여넣기
	afx_msg void OnLButtonDown(UINT f, CPoint p);
	afx_msg void OnLButtonDblClk(UINT f, CPoint p);
	afx_msg void OnMouseMove(UINT f, CPoint p);
	afx_msg void OnLButtonUp(UINT f, CPoint p);
	afx_msg void OnRButtonDown(UINT f, CPoint p);
	DECLARE_MESSAGE_MAP()

private:
	// 폰트 한 벌을 만들어 font 에 채우고, 만든 LOGFONT 를 lf_out 에 남긴다.
	void make_font(CFont& font, LOGFONTW& lf_out, const char* name, float size, const char* opts);

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

	// 버퍼에 삽입해 둔 IME 조합 중 글자들을 제거하고 커서를 조합 시작점으로 되돌린다.
	void clear_ime_comp();

	// 현재 입력을 확정한다. 콜백을 호출하고 새 줄로 이동한다.
	void commit_input();

	// 위/아래 화살표로 커서를 화면 줄 단위로 이동한다. (dir = -1 위, +1 아래)
	// 현재 입력 중인 논리 줄(편집 영역) 안에서만 움직이며, 가로 위치는 desired_vx 로 유지한다.
	void move_cursor_row(int dir);

	// cur_col 위치까지의 시각적 가로 칸 수(2배 폭 글자는 2로 계산)를 구한다.
	int visual_col() const;

	// 현재 커서 위치(cur_log/cur_col)를 화면 좌표로 변환한다.
	// row_out 은 화면 맨 위 기준 줄 번호, vx_out 은 줄 안에서의 가로 칸이다.
	// 커서가 어느 화면 줄에도 대응되지 않으면 false 를 돌려준다.
	bool cursor_screen_pos(int& row_out, int& vx_out) const;

	// 현재 커서가 그려질 사각형(칸을 채우는 블록)을 구한다. 화면 밖이면 false.
	bool get_cursor_rect(CRect& rc) const;

	// 블록 커서를 채울 색을 cur_bg/cur_fg 와 혼합 비율로 계산한다.
	COLORREF blend_cursor_color() const;

	// 커서를 즉시 보이게 하고(cursor_on=true) 깜빡임 타이머를 다시 시작한다.
	// 입력/이동 직후 한 박자 동안은 커서가 확실히 보이게 하기 위한 것이다.
	void bump_cursor();

	// 지금 IME 가 한글(초성/한글 자모) 입력 모드인지 조회한다.
	// 조합이 시작되기 전에도 한/영 상태를 알 수 있어, 커서 폭을 미리 정하는 데 쓴다.
	bool is_hangul_mode() const;

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

	int cursor_bg_weight;  // 블록 커서 색 혼합 비율의 배경 가중치 (기본 6)
	int cursor_fg_weight;  // 블록 커서 색 혼합 비율의 전경 가중치 (기본 4)

	bool cursor_on;        // 깜빡임 표시 상태 (true 면 현재 커서가 보임)
	int cursor_blink_ms;   // 깜빡임 토글 간격(밀리초). 0 이면 깜빡임 없이 항상 표시

	CFont efont;       // 영문 폰트
	CFont kfont;       // 한글 폰트
	LOGFONTW efont_lf; // 영문 폰트의 LOGFONT 사본
	LOGFONTW kfont_lf; // 한글 폰트의 LOGFONT 사본 (사용자가 지정한 원본 크기/스타일 보존)

	bool kfont_match_efont; // 켜지면 한글 폰트를 영문과 같은 높이로 맞추고 줄 높이를 영문 기준으로 잡는다 (set_kfont 의 size<=0 으로 켜진다, 기본 true)

	float kfill_ratio;     // match 모드 칸 폭: 한글이 두 칸을 채우는 비율 (기본 0.92)
	float emin_ratio;      // match 모드 칸 폭: 영문을 자연 폭의 이 비율 밑으로 좁히지 않는 하한 (기본 0.7)

	int cell_w;        // 영문 한 글자 칸의 가로 픽셀 (한글은 2배)
	int cell_h;        // 한 줄 칸의 세로 픽셀
	int cell_base;     // 칸 위쪽에서 글자 기준선까지의 픽셀 (베이스라인 정렬용)

	int cols;          // 화면에 들어가는 가로 칸 수
	int rows;          // 화면에 들어가는 세로 줄 수

	int margin_top;    // 클라이언트 영역 안쪽 여백(픽셀). 이 안쪽에만 글자를 그린다.
	int margin_bottom;
	int margin_left;
	int margin_right;

	std::vector<LogLine>    log_buf;    // 논리 줄 버퍼 (전체 내용)
	std::vector<ScreenLine> scr_lines;  // 자동 줄바꿈 적용된 화면 줄 목록

	int scroll_top;    // 화면 맨 위에 보이는 scr_lines 인덱스
	int cur_log;       // 현재 출력/입력 위치(커서)의 논리 줄 인덱스
	int cur_col;       // 현재 출력/입력 위치(커서)의 줄 내 글자 인덱스

	int edit_log;      // 편집 가능 영역이 시작되는 논리 줄 (보통 마지막 줄)
	int edit_col;      // 그 줄에서 편집이 시작되는 글자 인덱스 (경계 이전은 읽기전용)

	int desired_vx;    // 위/아래 이동 시 유지할 목표 가로 칸. -1 이면 미설정(다음 세로 이동에서 현재 위치로 초기화)

	std::wstring ime_comp; // IME 로 조합 중인(미확정) 문자열. 버퍼에 삽입해 두고 조합 블록색으로 그린다.
	int ime_anchor;        // 조합이 시작된 cur_log 줄 내 글자 인덱스. ime_comp 가 비어있지 않은 동안만 유효.

	bool selecting;        // 마우스 드래그로 선택 중인지
	bool sel_valid;        // 유효한 선택 영역이 있는지
	int sel_a_log, sel_a_col;  // 선택 고정점(드래그 시작)
	int sel_c_log, sel_c_col;  // 선택 이동점(드래그 현재)
};
