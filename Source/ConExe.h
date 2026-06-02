// ConExe.h
//
// 이식 가능한 "터미널 기반 응용프로그램 실행기" 헬퍼 클래스 CConExe 의 선언.
// ConPTY(의사 콘솔)로 자식 프로세스를 실행하고, 그 콘솔 출력(stdout/stderr 이 합쳐진
// 콘솔 출력)을 지정된 ConBox 인스턴스로 흘려보낸다. 키 입력(ConBox→자식 stdin)과 창 크기
// 변경(그리드→자식 콘솔)도 함께 연결한다.
//
// 이식 단위는 ConExe.h / ConExe.cpp 두 파일뿐이다.
// "사용"만 할 때는 이 헤더만 읽으면 충분하다 - 구현 세부(ConExe.cpp)는 동작을 "수정"할 때만 본다.
//
// --- 요구사항 / 의존성 ---
//   - Windows 10 1809(빌드 17763) 이상. ConPTY API(CreatePseudoConsole 등)에 의존한다.
//   - 출력 대상으로 CConBox(ConBox.h) 를 쓴다. 이 헤더는 전방 선언만 하고, 실제 포함은
//     ConExe.cpp 가 한다(헤더를 가볍게 유지).
//   - 자식 출력 폴링용으로 메시지 전용 창(HWND_MESSAGE)을 하나 만들어 타이머를 건다.
//     따라서 호스트에 메시지 펌프(보통의 GUI 앱)가 돌고 있어야 펌프가 동작한다.
//   - .h/.cpp 는 UTF-8 BOM 으로 저장한다(BOM 이 없으면 MSVC 가 CP949 로 오인해 한글 주석이 깨진다).
//
// --- 동작 개요 ---
//   start() 가 입력/출력 파이프 쌍을 만들고 CreatePseudoConsole 로 PTY 를 구성한 뒤,
//   STARTUPINFOEX + PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE_HANDLE 로 자식을 CreateProcess 한다.
//   이후 내부 타이머가 주기적으로 출력 파이프를 PeekNamedPipe -> ReadFile 해서 ConBox 의
//   print() 로 전달하고, ConBox 가 그 UTF-8/VT 시퀀스를 셀 그리드 위에서 해석해 화면을 그린다.
//   ConBox 키 입력은 attach 가 건 입력 싱크로 자식 stdin 에, 그리드 크기 변경은 리사이즈 싱크로
//   ResizePseudoConsole 에 전달된다. 자식이 끝나면(프로세스 핸들 폴링으로 감지) 스스로 정리하고,
//   set_exit_callback 으로 등록한 콜백이 있으면 한 번 통지한다.
//
// --- 최소 사용 예 (호스트 창에서) ---
//     CConExe exe;                    // 보통 멤버로 보유한다
//     exe.attach(&con_box);           // 출력 + 입력 + 리사이즈 연결
//     exe.start("powershell", con_box.grid_cols(), con_box.grid_rows());  // 자식 실행
//     // 이후 자식 콘솔 입출력이 con_box 와 자동으로 오간다. 소멸 시 자동 정리된다.

#pragma once

#include <windows.h>   // ConPTY(HPCON), 파이프/프로세스/창/타이머 Win32 API

class CConBox;          // 출력 대상. 실제 정의는 ConExe.cpp 에서 ConBox.h 로 포함한다.

// ConPTY 로 터미널 기반 자식 프로그램을 실행하고 그 출력을 ConBox 로 전달하는 실행기.
// 사용 순서: attach() 로 출력 대상을 지정한 뒤 start() 로 자식을 실행한다.
class CConExe
{
public:
	CConExe();
	~CConExe();

	// 자식 콘솔 출력을 받을 ConBox 를 지정한다. start() 전에 호출한다.
	// 이때 ConBox 의 입력 싱크로도 자신을 등록해, ConBox 키 입력이 자동으로 자식 stdin 으로
	// 전달되도록 양방향을 연결한다(출력: 자식->ConBox, 입력: ConBox->자식).
	void attach(CConBox* box);

	// 자식 프로세스의 stdin 으로 바이트열을 보낸다. (ConBox 입력 싱크가 호출한다)
	// data 는 UTF-8/VT 바이트열, len 은 바이트 수. 자식이 실행 중이 아니면 무시한다.
	void write(const char* data, int len);

	// 자식 콘솔(의사 콘솔)의 칸/줄 크기를 바꾼다(ResizePseudoConsole). ConBox 그리드 크기가
	// 바뀌면 리사이즈 싱크를 통해 호출되어, 자식이 새 크기로 화면을 다시 그리게 한다.
	// 실행 중이 아니면 무시한다. (ConBox 가 attach() 에서 등록한 싱크로 자동 연결된다.)
	void resize(int cols, int rows);

	// 자식 프로그램을 ConPTY 로 실행한다.
	//   cmdline : 실행할 명령줄 (UTF-8). 예: "cmd /c dir", "powershell"
	//   cols/rows : 의사 콘솔의 칸/줄 크기. 보통 출력 대상 ConBox 의 그리드와 맞춘다.
	// 성공하면 true. 이미 실행 중이면 먼저 정리하고 새로 시작한다.
	bool start(const char* cmdline, int cols, int rows);

	// 자식 종료 콜백을 등록한다. 자식이 끝나(출력 파이프가 닫혀) 내부적으로 정리될 때 한 번
	// 호출된다. 콜백 시점에는 이미 정리(stop)가 끝나 is_running() 이 false 다(콜백 안에서
	// 새 start() 를 호출해 재시작해도 안전). user 는 콜백에 그대로 되돌려주는 컨텍스트다.
	// 명시적 stop() 으로 끝낸 경우에는 호출되지 않는다(자식의 자연 종료만 통지).
	void set_exit_callback(void (*cb)(void* user), void* user);

	// 출력 파이프에 쌓인 바이트를 읽어 ConBox 로 전달한다.
	// 내부 타이머가 주기적으로 호출하지만, 호스트가 직접 호출해도 된다.
	// 파이프가 닫혔으면(자식 종료) 내부적으로 stop() 해서 정리한다.
	void pump();

	// 자식/PTY/파이프/타이머를 모두 정리한다. 여러 번 불러도 안전하다.
	void stop();

	// 현재 자식이 실행 중이고 출력 경로가 살아 있는지 조회한다.
	bool is_running() const;

private:
	// 자식 종료(출력 파이프 닫힘)를 감지했을 때 정리하고 종료 콜백을 발화한다.
	// 먼저 stop() 으로 정리한 뒤 콜백을 불러, 콜백이 즉시 재시작해도 안전하게 한다.
	void handle_child_exit();

	// 내부 타이머 창의 WM_TIMER 에서 호출되는 펌프 진입점(= pump()).
	// 정적 창 프로시저가 GWLP_USERDATA 에 보관한 인스턴스로 라우팅해 부른다.
	void on_timer_tick();

	// 메시지 전용 타이머 창의 정적 윈도우 프로시저.
	static LRESULT CALLBACK timer_wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);

	// ConBox 입력 싱크로 등록되는 정적 함수. user 로 받은 CConExe 인스턴스의 write 로 라우팅한다.
	static void input_thunk(const char* bytes, int len, void* user);

	// ConBox 리사이즈 싱크로 등록되는 정적 함수. user 로 받은 인스턴스의 resize 로 라우팅한다.
	static void resize_thunk(int cols, int rows, void* user);

	CConBox* out_box;     // 출력 대상 ConBox (attach 로 지정, 없으면 출력 버림)

	HPCON h_pc;           // 의사 콘솔 핸들 (CreatePseudoConsole)
	HANDLE in_write;      // 자식 stdin 에 쓰는 쪽 (M2 에서 입력 전송에 사용)
	HANDLE out_read;      // 자식 콘솔 출력을 읽는 쪽

	PROCESS_INFORMATION proc;  // 자식 프로세스/스레드 핸들

	HWND timer_wnd;       // 출력 폴링용 메시지 전용 창 (타이머 소유)
	bool running;         // 실행 중 여부

	void (*exit_cb)(void* user);  // 자식 자연 종료 콜백 (없으면 nullptr)
	void* exit_cb_user;           // 콜백에 되돌려줄 컨텍스트
};
