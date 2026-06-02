// ConExe.cpp
//
// CConExe 의 구현. 사용법만 알면 되는 경우 ConExe.h 만 읽으면 충분하다.
// 이 파일은 동작을 "수정"할 때만 본다. 아래 목차로 필요한 함수만 찾아(Grep) 부분만 읽는다.
//
// === 파일 구성 ===
//   보조(static) : Utf8ToWide(UTF-8->UTF-16 변환), CreatePtyPipes(파이프+의사콘솔 구성)
//   생성/소멸    : CConExe, ~CConExe
//   설정/입력    : attach(출력+입력+리사이즈 연결), write, resize, input_thunk, resize_thunk
//   실행/정리    : start, set_exit_callback, stop, is_running
//   출력 펌프    : pump, handle_child_exit, on_timer_tick, timer_wnd_proc

// ConPTY API(CreatePseudoConsole, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE_HANDLE 등)는
// Windows 10 1809(RS5) 이상에서만 선언된다. Source 파일은 호스트의 targetver 를 거치지
// 않으므로(개발노트 1.2), windows.h 가 끌려오기 전에 최소 버전을 직접 지정한다.
// 호스트가 더 높은 버전을 정의했다면 그 값을 존중하도록 #ifndef 로 감싼다.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00            // Windows 10
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006       // NTDDI_WIN10_RS5 (ConPTY 최소 버전)
#endif

// ConBox.h(afxwin.h)를 ConExe.h(<windows.h>)보다 먼저 포함한다. MFC 는 windows.h 보다
// 먼저 들어와야 하며(winsock2 순서 문제), afxwin.h 가 windows.h 를 끌어오므로 순서를 지킨다.
#include "ConBox.h"        // out_box->print() 호출을 위해 실제 정의가 필요하다.
#include "ConExe.h"
#include <vector>
#include <string>

// 일부 SDK/헤더 구성에서는 CreatePseudoConsole/HPCON 은 노출되지만 이 속성 매크로가
// 노출되지 않는 경우가 있어, 정의되어 있지 않으면 직접 정의한다.
// 값은 ProcThreadAttributeValue(ProcThreadAttributePseudoConsole=22, Thread=FALSE,
// Input=TRUE, Additive=FALSE) = 22 | PROC_THREAD_ATTRIBUTE_INPUT(0x00020000) = 0x00020016
// 으로 SDK 전역에서 고정된 값이다.
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE_HANDLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE_HANDLE 0x00020016
#endif

// 출력 폴링 타이머 간격(밀리초). 16~30ms 범위면 화면 갱신이 부드럽다.
static const UINT PUMP_INTERVAL_MS = 16;
// 타이머 ID (메시지 전용 창 안에서만 쓰므로 충돌 걱정 없음).
static const UINT_PTR PUMP_TIMER_ID = 1;
// 메시지 전용 타이머 창의 클래스 이름.
static const wchar_t* TIMER_WND_CLASS = L"ConExeTimerWnd";

// UTF-8 문자열을 UTF-16 으로 변환해 돌려준다. (널 종료 포함)
// 실패하거나 빈 문자열이면 빈 벡터를 돌려준다.
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


// === 생성/소멸 ===

CConExe::CConExe()
	: out_box(nullptr)
	, h_pc(nullptr)
	, in_write(nullptr)
	, out_read(nullptr)
	, timer_wnd(nullptr)
	, running(false)
	, exit_cb(nullptr)
	, exit_cb_user(nullptr)
{
	ZeroMemory(&proc, sizeof(proc));
}

CConExe::~CConExe()
{
	// ConBox 에 등록해 둔 입력 싱크를 먼저 해제한다(해제 후 ConBox 가 죽은 this 로
	// 입력을 보내지 않도록). 그 다음 PTY/파이프/타이머를 정리한다.
	if (out_box != nullptr) {
		out_box->set_input_sink(nullptr, nullptr);
		out_box->set_resize_sink(nullptr, nullptr);
	}
	stop();
}


// === 설정 ===

void CConExe::attach(CConBox* box)
{
	out_box = box;
	// 출력(자식->ConBox)뿐 아니라 입력(ConBox->자식)과 리사이즈(그리드 변경->PTY)도 연결한다.
	// ConBox 가 키 입력을 인코딩해 input_thunk 로 보내면 this->write 로 자식 stdin 에 쓰고,
	// 그리드 크기가 바뀌면 resize_thunk 로 this->resize(ResizePseudoConsole)를 부른다.
	if (out_box != nullptr) {
		out_box->set_input_sink(&CConExe::input_thunk, this);
		out_box->set_resize_sink(&CConExe::resize_thunk, this);
	}
}

void CConExe::write(const char* data, int len)
{
	if (in_write == nullptr || data == nullptr || len <= 0)
		return;
	DWORD written = 0;
	::WriteFile(in_write, data, (DWORD)len, &written, NULL);
}

void CConExe::input_thunk(const char* bytes, int len, void* user)
{
	CConExe* self = (CConExe*)user;
	if (self != nullptr)
		self->write(bytes, len);
}

void CConExe::resize(int cols, int rows)
{
	// 의사 콘솔이 살아 있을 때만 크기를 바꾼다. 자식은 콘솔 크기 변화를 감지해 화면을 다시 그린다.
	if (h_pc == nullptr || !running)
		return;
	if (cols < 1) cols = 1;
	if (rows < 1) rows = 1;
	COORD size;
	size.X = (SHORT)cols;
	size.Y = (SHORT)rows;
	::ResizePseudoConsole(h_pc, size);
}

void CConExe::resize_thunk(int cols, int rows, void* user)
{
	CConExe* self = (CConExe*)user;
	if (self != nullptr)
		self->resize(cols, rows);
}


// === 실행/정리 ===

// 입력/출력 파이프 쌍을 만들고 CreatePseudoConsole 로 의사 콘솔을 구성한다.
// PTY 측 끝(자식이 읽을 입력 read 끝, 자식이 쓸 출력 write 끝)은 호출부가 spawn 후 닫는다.
// 우리가 계속 쓰는 끝(in_write, out_read)은 멤버에 보관한다.
static bool CreatePtyPipes(int cols, int rows,
	HPCON& h_pc_out, HANDLE& in_write_out, HANDLE& out_read_out,
	HANDLE& pty_in_read_out, HANDLE& pty_out_write_out)
{
	// 자식 stdin 용 파이프: 우리가 in_write 에 쓰면 자식이 pty_in_read 로 읽는다.
	HANDLE in_read = nullptr, in_write = nullptr;
	// 자식 stdout 용 파이프: 자식이 pty_out_write 에 쓰면 우리가 out_read 로 읽는다.
	HANDLE out_read = nullptr, out_write = nullptr;

	if (!::CreatePipe(&in_read, &in_write, NULL, 0))
		return false;
	if (!::CreatePipe(&out_read, &out_write, NULL, 0)) {
		::CloseHandle(in_read);
		::CloseHandle(in_write);
		return false;
	}

	// 의사 콘솔을 만든다. 입력 read 끝과 출력 write 끝을 PTY 에 넘긴다.
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
	// PTY 측 끝은 호출부가 자식 생성 후 닫는다(그래야 자식 종료 시 out_read 가 EOF 를 본다).
	pty_in_read_out = in_read;
	pty_out_write_out = out_write;
	return true;
}

bool CConExe::start(const char* cmdline, int cols, int rows)
{
	// 이미 돌고 있으면 먼저 정리하고 새로 시작한다.
	stop();

	if (cols < 1) cols = 1;
	if (rows < 1) rows = 1;

	// 파이프 + 의사 콘솔 구성. PTY 측 끝(pty_in_read/pty_out_write)은 자식 생성 후 닫는다.
	HANDLE pty_in_read = nullptr, pty_out_write = nullptr;
	if (!CreatePtyPipes(cols, rows, h_pc, in_write, out_read, pty_in_read, pty_out_write)) {
		stop();
		return false;
	}

	// 자식이 의사 콘솔에 붙도록 STARTUPINFOEX 의 속성 목록을 구성한다.
	STARTUPINFOEXW si;
	ZeroMemory(&si, sizeof(si));
	si.StartupInfo.cb = sizeof(STARTUPINFOEXW);

	// 속성 1개(PSEUDOCONSOLE_HANDLE)를 담을 목록 크기를 먼저 구한 뒤 할당한다.
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

	// 명령줄은 CreateProcessW 가 쓰기 가능 버퍼를 요구하므로 가변 벡터에 담는다.
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
		NULL,            // 모듈 이름은 명령줄에서 찾게 한다
		cmd.data(),      // 가변 명령줄 버퍼
		NULL, NULL,
		FALSE,           // 핸들 상속 안 함 (의사 콘솔 속성으로 콘솔만 연결)
		EXTENDED_STARTUPINFO_PRESENT,
		NULL, NULL,
		&si.StartupInfo,
		&proc);

	// 속성 목록은 더 이상 필요 없다.
	::DeleteProcThreadAttributeList(si.lpAttributeList);
	::HeapFree(::GetProcessHeap(), 0, si.lpAttributeList);

	// PTY 측 끝은 이제 닫는다. 의사 콘솔이 자체 복제본을 들고 있으므로 우리 쪽은 불필요하며,
	// 출력 write 끝을 닫아야 자식 종료 시 out_read 가 EOF/broken pipe 를 보게 된다.
	::CloseHandle(pty_in_read);
	::CloseHandle(pty_out_write);

	if (!ok) {
		ZeroMemory(&proc, sizeof(proc));
		stop();
		return false;
	}

	// 출력 폴링용 메시지 전용 창을 만들고 타이머를 건다.
	// 창 클래스는 한 번만 등록한다(이미 있으면 RegisterClassExW 가 실패하지만 무해).
	static bool class_registered = false;
	if (!class_registered) {
		WNDCLASSEXW wc;
		ZeroMemory(&wc, sizeof(wc));
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = &CConExe::timer_wnd_proc;
		wc.hInstance = ::GetModuleHandleW(NULL);
		wc.lpszClassName = TIMER_WND_CLASS;
		::RegisterClassExW(&wc);
		class_registered = true;
	}

	timer_wnd = ::CreateWindowExW(0, TIMER_WND_CLASS, L"",
		0, 0, 0, 0, 0, HWND_MESSAGE, NULL, ::GetModuleHandleW(NULL), NULL);
	if (timer_wnd == nullptr) {
		stop();
		return false;
	}
	// WM_TIMER 에서 인스턴스를 찾을 수 있도록 this 를 창에 보관한다.
	::SetWindowLongPtrW(timer_wnd, GWLP_USERDATA, (LONG_PTR)this);
	::SetTimer(timer_wnd, PUMP_TIMER_ID, PUMP_INTERVAL_MS, NULL);

	running = true;
	return true;
}

void CConExe::set_exit_callback(void (*cb)(void* user), void* user)
{
	exit_cb = cb;
	exit_cb_user = user;
}

void CConExe::stop()
{
	// 타이머 창부터 정리한다(타이머 콜백이 더 이상 오지 않게).
	if (timer_wnd != nullptr) {
		::KillTimer(timer_wnd, PUMP_TIMER_ID);
		::DestroyWindow(timer_wnd);
		timer_wnd = nullptr;
	}
	// 의사 콘솔을 닫으면 자식 콘솔 세션이 끝난다.
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
	if (proc.hThread != nullptr) {
		::CloseHandle(proc.hThread);
		proc.hThread = nullptr;
	}
	if (proc.hProcess != nullptr) {
		::CloseHandle(proc.hProcess);
		proc.hProcess = nullptr;
	}
	ZeroMemory(&proc, sizeof(proc));
	running = false;
}

bool CConExe::is_running() const
{
	return running;
}


// === 출력 펌프 ===

void CConExe::pump()
{
	if (!running || out_read == nullptr)
		return;

	// 파이프에 쌓인 바이트 수를 먼저 확인한다(블로킹 ReadFile 회피).
	// PeekNamedPipe 가 실패하면 파이프가 닫힌 것(자식 종료)이므로 정리한다.
	DWORD avail = 0;
	if (!::PeekNamedPipe(out_read, NULL, 0, NULL, &avail, NULL)) {
		handle_child_exit();
		return;
	}

	// 쌓인 만큼만 읽어 ConBox 로 전달한다. 한 번의 펌프에서 여러 번 나눠 읽을 수 있다.
	// 받은 바이트는 UTF-8 이며, 이 단계(M1)에서는 VT 시퀀스를 해석하지 않고 그대로 print 한다.
	while (avail > 0) {
		char buf[4096];
		DWORD want = (avail < sizeof(buf) - 1) ? avail : (DWORD)(sizeof(buf) - 1);
		DWORD got = 0;
		if (!::ReadFile(out_read, buf, want, &got, NULL) || got == 0) {
			handle_child_exit();
			return;
		}
		buf[got] = '\0';   // print() 가 널 종료 UTF-8 을 받으므로 종료문자를 둔다.
		if (out_box != nullptr)
			out_box->print(buf);
		avail -= got;
	}

	// 자식 종료 감지. ConPTY 에서는 자식이 끝나도 conhost(의사 콘솔)가 출력 파이프의 write 끝을
	// 계속 쥐고 있어 PeekNamedPipe 가 EOF 를 보지 못한다. 그래서 자식 프로세스 핸들을 직접
	// 폴링해 종료를 확인한다. 종료가 확인되면 남은 출력을 모두 비운 뒤에 정리/콜백을 한다.
	if (proc.hProcess != nullptr &&
		::WaitForSingleObject(proc.hProcess, 0) == WAIT_OBJECT_0) {
		DWORD more = 0;
		if (::PeekNamedPipe(out_read, NULL, 0, NULL, &more, NULL) && more > 0)
			return;   // 아직 읽을 출력이 남았으면 다음 펌프에서 마저 읽는다.
		handle_child_exit();
	}
}

void CConExe::handle_child_exit()
{
	// 자식이 자연 종료했다(출력 파이프 닫힘). 콜백 정보를 먼저 챙긴 뒤 정리하고, 정리가 끝난
	// 상태(is_running()==false)에서 콜백을 부른다. 그러면 콜백이 곧바로 새 start() 를 호출해
	// 재시작해도 우리 정리와 충돌하지 않는다.
	void (*cb)(void*) = exit_cb;
	void* user = exit_cb_user;
	stop();
	if (cb != nullptr)
		cb(user);
}

void CConExe::on_timer_tick()
{
	pump();
}

LRESULT CALLBACK CConExe::timer_wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_TIMER) {
		CConExe* self = (CConExe*)::GetWindowLongPtrW(wnd, GWLP_USERDATA);
		if (self != nullptr)
			self->on_timer_tick();
		return 0;
	}
	return ::DefWindowProcW(wnd, msg, wp, lp);
}
