// ConBox.h
//
// 이식 가능한 터미널 컨트롤 ConBox 의 클래스 선언.
// wt.exe 처럼 터미널 기반 자식 프로그램의 콘솔 출력을 표시하고 키 입력을 자식으로 보내는
// MFC CWnd 파생 컨트롤이다. 내부는 cols x rows 셀 그리드 화면 버퍼 + 스크롤백으로 동작한다.
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
//     box.print("hello\n");                   // 출력 (UTF-8) — 셀 그리드에 흐른다
//
// ConBox 는 set_input_sink 로 입력 싱크가 걸린 "터미널(raw) 모드"로 쓰는 것을 전제한다.
// 이 모드에서는 키를 로컬에서 편집하지 않고(로컬 에코 끔) VT 시퀀스/UTF-8 로 인코딩해 싱크로
// 보낸다(화면은 자식이 그린다). ConExe 가 attach() 에서 자신을 싱크로 등록한다. ConPTY 자식
// 실행은 ConExe.h 참고. 싱크가 없으면 print() 출력만 표시하는 읽기 전용 뷰어로 동작한다.

#pragma once

#include <afxwin.h>   // MFC 코어 (CWnd, CDC, CFont 등)
#include <vector>     // 내부 셀 그리드 버퍼

// 화면 한 칸(셀)에 들어가는 정보. 셀 하나 = 화면의 한 칸이다.
// 2배 폭 글자(한글/CJK)는 lead 칸(ch=글자, wide=true)이 차지하고, 바로 다음 칸은 trail 칸
// (ch=0)으로 비워 둔다(렌더는 lead 가 두 칸을 그리고 trail 은 건너뛴다). 빈 칸은 ch=L' '.
struct CharInfo {
	wchar_t  ch;     // 글자 (UTF-16). 0 = wide 글자의 trail 칸(렌더 생략)
	COLORREF fg;     // 전경색
	COLORREF bg;     // 배경색
	bool     wide;   // 2배 폭 글자의 lead 칸 여부 (한글, CJK 등)
};

// 화면 한 줄. cols 개 셀(칸)로 이루어진다(빈 칸 포함 고정 폭). 2배 폭 글자는 lead+trail 두 셀.
typedef std::vector<CharInfo> Row;

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

	// 박스/블록 그리기 문자를 폰트 글리프 대신 ConBox 가 도형으로 직접 그리는 수준을 정한다.
	// 폰트의 블록 글리프는 칸을 가로/세로로 꽉 채우지 않아(특히 세로) 인접 칸 사이에 틈이
	// 생기는데, 칸을 직접 칠하면 틈 없이 이어지고 폰트에 의존하지 않는다.
	//   0 = 끔(전부 폰트로 그림)
	//   1 = 블록 요소(U+2580~259F: 반칸/사분면/풀블록)만 직접 그림 (기본값)
	//   2 = 1 에 더해 박스선도 직접 그림: 단선 직교/정션(─│┌┐└┘├┤┬┴┼),
	//       둥근 모서리(╭╮╰╯, 직각으로 대체), 대각선(╱╲╳), 순수 이중선(═║╔╗╚╝╠╣╦╩╬).
	// 단/이중 혼합선·점선·굵은선·음영은 어느 수준에서도 폰트로 그린다.
	void set_builtin_glyphs(int level);

	// 텍스트를 출력한다. 현재 커서 위치에서 이어서 셀 그리드에 쓰며, 폭(cols)을 넘으면 자동
	// 줄바꿈하고, 화면 맨 아래를 넘으면 위 줄을 스크롤백으로 밀어내며 스크롤한다.
	// text 는 UTF-8 이며 특수문자를 다음과 같이 처리한다.
	//   \n : 다음 줄 맨 처음으로 이동, \r : 현재 줄 맨 처음으로 이동(덮어쓰기), \t : 4칸 탭
	// 출력 후에는 강제로 맨 아래까지 스크롤한다.
	void print(const char* text);

	// 현재 전경색(글자색)을 지정한다. 이후 출력에 적용된다.
	void set_color(COLORREF fg);

	// 현재 배경색을 지정한다. 이후 출력에 적용된다.
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

	// 가로 cols 칸(영문 기준) x 세로 rows 줄을 모두 담는 데 필요한 클라이언트 영역
	// 픽셀 크기를 w/h 로 돌려준다. 안쪽 여백(set_margin)도 포함한 값이다.
	// 호스트가 "80x24 로 띄우기" 처럼 그리드 기준으로 창 크기를 정할 때 쓴다.
	// 한 칸 픽셀은 폰트/DPI 로 정해지므로 open() 이후(셀 크기 확정 후) 호출해야 정확하다.
	void client_size_for_grid(int cols, int rows, int& w, int& h) const;

	// 현재 화면 그리드의 가로 칸 수 / 세로 줄 수를 돌려준다. open() 이후(메트릭 확정 후) 유효하다.
	// 호스트가 자식 콘솔(PTY) 크기를 ConBox 그리드에 맞춰 시작할 때 쓴다.
	int grid_cols() const { return cols; }
	int grid_rows() const { return rows; }

	// 입력 싱크(터미널/raw 모드)를 지정한다. sink 가 설정되면 ConBox 는 키 입력을 로컬에서
	// 편집하지 않고(로컬 에코 끔), 키/문자를 VT 시퀀스 및 UTF-8 바이트로 인코딩해 sink 로
	// 흘려보낸다(자식 프로세스의 stdin 으로 보내는 용도). bytes/len 은 UTF-8 바이트열이며,
	// user 는 sink 호출 시 그대로 되돌려주는 사용자 컨텍스트다. nullptr 을 주면 읽기 전용
	// 뷰어 모드로 되돌아간다. (ConExe 는 attach() 에서 이 API 로 자신을 등록한다.)
	void set_input_sink(void (*sink)(const char* bytes, int len, void* user), void* user);

	// 리사이즈 싱크를 지정한다. 창 크기 변경 등으로 화면 그리드(cols/rows)가 바뀌면 새 칸/줄
	// 수를 이 콜백으로 통지한다(자식 콘솔의 의사 콘솔 크기를 맞추는 용도). nullptr 이면 통지하지
	// 않는다. (ConExe 는 attach() 에서 이 API 로 자신을 등록해 ResizePseudoConsole 로 연결한다.)
	void set_resize_sink(void (*sink)(int cols, int rows, void* user), void* user);

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
	// 한글 IME 처리. 터미널 모드에서는 확정분만 자식에 보내고 미완성 조합은 시스템 기본
	// 인라인 표시에 맡긴다(ConBox 직접 조합 렌더는 이후 단계 M5).
	afx_msg LRESULT OnImeStart(WPARAM w, LPARAM l);
	afx_msg LRESULT OnImeComp(WPARAM w, LPARAM l);
	afx_msg LRESULT OnImeEnd(WPARAM w, LPARAM l);
	// 한/영 토글 등 IME 상태 변화 통지. 한글 모드로 바뀌면 커서 모양을 즉시 갱신한다.
	afx_msg LRESULT OnImeNotify(WPARAM w, LPARAM l);
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

	// 스크롤을 강제로 맨 아래로 내린다. (view_top 을 마지막 화면이 보이는 위치로)
	void scroll_to_bottom();

	// 세로 스크롤바의 범위/위치를 갱신하고, 내용이 다 보이면 숨긴다.
	void update_scrollbar();

	// VT 파서: 한 글자(또는 제어문자)를 상태기계에 넣는다. ESC/CSI/OSC 시퀀스를 해석하고
	// 그 외 글자는 put_char 로 화면에 쓴다. 파서 상태는 멤버라 print 호출(청크) 사이에 유지된다.
	void vt_feed(wchar_t wc);

	// 완성된 CSI 시퀀스(최종 바이트 fin)를 해석해 커서 이동/지우기/SGR 등을 수행한다.
	void dispatch_csi(wchar_t fin);

	// 현재 커서 칸에 글자 하나를 쓴다. 폭(cols)을 넘으면 먼저 자동 줄바꿈한다. 2배 폭 글자는
	// lead 칸에 쓰고 다음 칸을 trail(ch=0)로 둔다. 현재 SGR 색/속성을 셀에 반영한다.
	void put_char(wchar_t wc);

	// 커서를 다음 줄로 내린다. 커서가 스크롤 영역 하단(scroll_bot)에 있으면 영역을 한 줄
	// 스크롤한다(scroll_up_region). 그 밖에는 화면 끝을 넘지 않는 선에서 한 줄 내린다.
	void line_feed();

	// 스크롤 영역 [scroll_top, scroll_bot] 을 위로 n줄 스크롤한다(윗줄이 사라지고 아래에 빈 줄).
	// 주 화면이고 영역 상단이 화면 맨 위(scroll_top==0)일 때만 밀려난 줄을 scrollback 으로 보존한다.
	// (LF 가 하단에서 줄을 내릴 때, SU(CSI S) 에서 쓴다.)
	void scroll_up_region(int n);

	// 스크롤 영역 [scroll_top, scroll_bot] 을 아래로 n줄 스크롤한다(아래줄이 사라지고 위에 빈 줄).
	// (RI(역인덱스)가 상단에서 줄을 올릴 때, SD(CSI T) 에서 쓴다. scrollback 에는 관여하지 않는다.)
	void scroll_down_region(int n);

	// 줄 범위 [top, bot] 안에서 위/아래로 n줄 회전한다(빈 줄로 채움). scrollback 에는 관여하지
	// 않는 순수 줄 이동이며, 스크롤 영역 스크롤과 IL/DL 의 공통 일꾼이다.
	void scroll_lines_up(int top, int bot, int n);
	void scroll_lines_down(int top, int bot, int n);

	// IL/DL: 커서가 스크롤 영역 안에 있을 때 커서 줄부터 빈 줄을 n개 삽입(insert_lines)하거나
	// n줄 삭제(delete_lines)한다. 영역 하단(scroll_bot)까지의 줄들이 밀려나며, 커서는 1열로 간다.
	void insert_lines(int n);
	void delete_lines(int n);

	// ICH/DCH: 커서 칸부터 같은 줄에서 빈 칸을 n개 삽입(insert_chars)하거나 n칸 삭제(delete_chars)
	// 한다. 줄 폭(cols)은 유지되며 오른쪽으로 밀려 넘친 칸은 버리고, 삭제 시 끝에 빈 칸을 채운다.
	void insert_chars(int n);
	void delete_chars(int n);

	// 대체 화면(alt screen) 진입/복귀. vim/htop 등 풀스크린 TUI 가 ?1049(/?1047/?47) 로 쓴다.
	// 진입 시 현재 주 화면을 main_saved 로 백업하고 빈 대체 화면으로 바꾸며(스크롤백은 동결되어
	// 보이지 않는다), 복귀 시 주 화면을 되돌린다.
	void enter_alt_screen();
	void leave_alt_screen();

	// cols 칸의 빈 줄(공백 셀, 현재 배경색)을 만들어 돌려준다.
	Row blank_row() const;

	// screen[row] 의 [c0, c1) 칸을 공백 셀(현재 배경색)로 지운다. (ED/EL 공통)
	void erase_cells(int row, int c0, int c1);

	// 커서 좌표를 화면 범위 [0,rows-1] / [0,cols] 안으로 맞춘다.
	void clamp_cursor();

	// 화면(screen)을 현재 cols/rows 크기에 맞게 재구성한다(행 수/줄 폭 맞춤, 커서 clamp).
	void reset_screen();

	// (scrollback + screen) 통합 인덱스 idx 의 줄을 돌려준다. 렌더/스크롤에서 쓴다.
	const Row& line_at(int idx) const;

	// 현재 커서 위치(cur_row/cur_col)를 화면 좌표로 변환한다.
	// row_out 은 화면 맨 위(view_top) 기준 줄 번호, vx_out 은 줄 안에서의 가로 칸(칸 좌표)이다.
	// 버퍼가 비었으면 false 를 돌려준다(화면 밖 여부는 호출 측에서 판정).
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

	// 터미널(raw) 모드에서 키 입력을 자식에게 보낸다.
	// send_input_bytes : UTF-8 바이트열을 그대로 입력 싱크로 보낸다.
	// send_input_wide  : UTF-16 글자들을 UTF-8 로 변환해 보낸다(문자/IME 확정 글자용).
	// terminal_keydown : 방향키/Home/End/Delete 등 비문자 키를 VT 시퀀스로 보낸다. 보냈으면 true.
	void send_input_bytes(const char* bytes, int len);
	void send_input_wide(const wchar_t* ws, int n);
	bool terminal_keydown(UINT vk, bool ctrl, bool shift);

	// 클립보드 텍스트를 자식 stdin 으로 보낸다. bracketed_paste 가 켜져 있으면
	// ESC[200~ ... ESC[201~ 로 감싼다(자식이 붙여넣기를 타이핑과 구분하게).
	void paste_clipboard();

	// 한글 IME 조합 중이면 강제로 확정(완성)시켜, 완성 글자의 UTF-8 이 먼저 자식에 전송되게 한다.
	// 조합 종료 트리거(방향키/Home/End/Delete/Enter/Tab/Esc 등, 마우스 클릭)를 자식에 보내기
	// 직전에 호출해 "제자리 완성 후 트리거" 순서를 보장한다(요구사양서 3.1). 조합이 있었으면 true.
	bool finalize_composition();

	// 더블 버퍼링용 메모리 DC/비트맵을 (필요하면) 만들어 둔다. 클라이언트 크기가
	// 바뀐 경우에만 비트맵을 다시 만들어, 잦은 다시그리기에서 매번 GDI 객체를
	// 할당/해제하지 않게 한다. ref 는 호환 기준이 되는 화면 DC, w/h 는 필요한 크기.
	void ensure_back_buffer(CDC* ref, int w, int h);

	// 입력 싱크(터미널/raw 모드). 설정되면 로컬 편집 대신 키를 인코딩해 이 싱크로 보낸다.
	void (*input_sink)(const char* bytes, int len, void* user);
	void* input_sink_user;

	// 리사이즈 싱크. 그리드(cols/rows)가 바뀌면 새 크기를 통지한다(자식 PTY 크기 동기화용).
	void (*resize_sink)(int cols, int rows, void* user);
	void* resize_sink_user;

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

	int glyph_level;       // 박스/블록 문자 직접 렌더 수준 (0=끔, 1=블록요소만(기본), 2=+직선/정션 박스선)

	int cell_w;        // 영문 한 글자 칸의 가로 픽셀 (한글은 2배)
	int cell_h;        // 한 줄 칸의 세로 픽셀
	int cell_base;     // 칸 위쪽에서 글자 기준선까지의 픽셀 (베이스라인 정렬용)

	int cols;          // 화면에 들어가는 가로 칸 수
	int rows;          // 화면에 들어가는 세로 줄 수

	int margin_top;    // 클라이언트 영역 안쪽 여백(픽셀). 이 안쪽에만 글자를 그린다.
	int margin_bottom;
	int margin_left;
	int margin_right;

	// 화면 셀 그리드. screen 은 현재 화면(항상 rows 개 줄, 각 줄 cols 칸), scrollback 은 화면
	// 위로 밀려난 줄들이다. 커서 (cur_row, cur_col) 는 화면 내 0-based 칸 좌표다(VT 절대 좌표 제어).
	std::vector<Row> screen;      // 현재 화면 (rows 개 줄, 각 줄 cols 칸 셀)
	std::vector<Row> scrollback;  // 화면 위로 밀려난 줄들 (가변, 상한 트림)
	int view_top;                 // 보기 맨 위 = (scrollback + screen) 통합 인덱스 (휠/스크롤바로 조정)
	int cur_row;                  // 커서 화면 행 (0..rows-1)
	int cur_col;                  // 커서 화면 칸 (0..cols)

	bool cursor_visible;          // DECTCEM(?25): 커서 표시 여부 (기본 true)
	int  saved_row, saved_col;    // DECSC/DECRC 로 저장한 커서 위치

	// 스크롤 영역(DECSTBM). LF/RI/IL/DL/SU/SD 가 존중하는 줄 범위다. 기본은 화면 전체 [0, rows-1].
	int scroll_top;
	int scroll_bot;

	// 대체 화면(alt screen). 풀스크린 TUI 가 ?1049(/?1047/?47) 로 전환한다.
	std::vector<Row> main_saved;  // 대체 화면 진입 시 백업해 둔 주 화면 (복귀 시 되돌린다)
	bool alt_active;              // 대체 화면이 활성인지 (이때 스크롤백은 동결되어 보이지 않는다)
	int  saved_main_row;          // 대체 화면 진입 시 저장한 주 화면 커서 (복귀 시 되돌린다)
	int  saved_main_col;

	// VT 파서 상태 (print 가 청크로 불려도 시퀀스가 이어지도록 멤버로 유지)
	int  vt_state;                // 0=GROUND 1=ESC 2=CSI 3=OSC
	int  vt_params[16];           // CSI 숫자 파라미터
	int  vt_nparam;               // 채워진 파라미터 수
	bool vt_priv;                 // CSI '?' (DEC 프라이빗) 마커
	bool vt_gtlt;                 // CSI '<' '=' '>' 접두 마커 (2차DA/kitty/XTMODKEYS 등, 전부 무시)

	// SGR 현재 속성. put_char 가 셀에 반영한다. (색은 cur_fg/cur_bg 사용)
	bool cur_bold;                // 밝은색 매핑
	bool cur_reverse;             // 전경/배경 스왑

	// 입력 모드(자식이 DEC 프라이빗 모드로 켠다). 키 인코딩/붙여넣기 방식을 바꾼다.
	bool app_cursor_keys;         // DECCKM(?1): 켜지면 방향키를 ESC O x (예: ESC O A) 로 보낸다
	bool bracketed_paste;         // ?2004: 켜지면 붙여넣기를 ESC[200~ ... ESC[201~ 로 감싼다

	// 더블 버퍼링용 캐시. OnPaint 가 매 프레임 새로 만들지 않고 재사용한다.
	CDC back_dc;            // 영속 메모리 DC (한 번 만들어 계속 쓴다)
	CBitmap back_bmp;       // 백버퍼 비트맵 (클라이언트 크기와 같다)
	CBitmap* back_old_bmp;  // back_dc 에 원래 들어있던 비트맵 (정리 시 되돌리기용)
	int back_w;             // 현재 back_bmp 의 가로 픽셀 (크기 변경 감지용)
	int back_h;             // 현재 back_bmp 의 세로 픽셀
};
