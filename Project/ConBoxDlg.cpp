
// ConBoxDlg.cpp: 구현 파일
//

#include "pch.h"
#include "framework.h"
#include "ConBoxApp.h"
#include "ConBoxDlg.h"
#include "afxdialogex.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif


// CConBoxDlg 대화 상자



CConBoxDlg::CConBoxDlg(CWnd* pParent /*=nullptr*/)
	: CDialog(IDD_CONBOX_DIALOG, pParent)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CConBoxDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CConBoxDlg, CDialog)
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_WM_SIZE()
END_MESSAGE_MAP()


// CConBoxDlg 메시지 처리기

BOOL CConBoxDlg::OnInitDialog()
{
	CDialog::OnInitDialog();

	// 이 대화 상자의 아이콘을 설정합니다.  응용 프로그램의 주 창이 대화 상자가 아닐 경우에는
	//  프레임워크가 이 작업을 자동으로 수행합니다.
	SetIcon(m_hIcon, TRUE);			// 큰 아이콘을 설정합니다.
	SetIcon(m_hIcon, FALSE);		// 작은 아이콘을 설정합니다.

	// 타이틀바에 최소화/최대화 버튼을 추가한다.
	// 캡션 버튼은 시스템 메뉴(WS_SYSMENU)가 있어야 보이므로 함께 켠다.
	// 최소화 버튼으로 창을 작업표시줄 아이콘으로 보낼 수 있다. (EXSTYLE 에 WS_EX_APPWINDOW 가 있어 항상 표시됨)
	ModifyStyle(0, WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
	SetWindowPos(NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

	// 데모에서는 확인/취소 버튼을 쓰지 않으므로 제거한다.
	if (CWnd* ok = GetDlgItem(IDOK))
		ok->DestroyWindow();
	if (CWnd* cancel = GetDlgItem(IDCANCEL))
		cancel->DestroyWindow();

	// 폰트는 창 생성 전에 지정한다. (기본값과 동일하지만 API 사용 예시를 겸한다.)
	// 한글은 크기를 0 으로 주어 영문 높이에 맞추는 match 모드로 둔다(기본 동작).
	// 영문 폰트로 Cascadia Mono 를 쓴다. (둥근 모서리 ╭╮╰╯ 와 로고의 사분면 블록
	//  ▛▜▘▝ 글리프를 가져 도표가 깨지지 않는다. Consolas 에는 이 글리프들이 없다.)
	con_box.set_efont("Cascadia Mono", 13, "B");
	con_box.set_kfont("Malgun Gothic", 0, "B");

	// match 모드의 칸 폭(장평) 비율. 기본값과 동일하지만 API 사용 예시를 겸한다.
	con_box.set_kfont_fill(0.92f, 0.7f);

	// ConBox 를 자식 창으로 만든다. (위치/크기는 아래 resize_to_grid 가 정한다.)
	con_box.open(this, 0, 0, 10, 10);

	// 위에서 지정한 폰트 설정 기준으로 ConBox 가 96x32 칸(영문)이 되도록 메인 창
	// 크기를 다시 잡고 화면 중앙으로 옮긴다. (셀 크기는 open 에서 확정되었다.)
	resize_to_grid(96, 32);

	// 초기 화면: ConBox 개요/특징 도표 (둥근 모서리). 로고 칸만 살구색.
	// C++20 에서 u8 리터럴은 char8_t 형이므로 const char* 로 캐스팅한다.
	// (도표 문자열은 한글=2칸 폭을 계산해 정렬되어 있다. Temp/gen_box.py 로 생성.)
	const COLORREF c_fg   = RGB(255, 255, 255);   // 본문/테두리: 흰색
	const COLORREF c_logo = RGB(214, 112, 84);    // 로고: 살구색(PNG 색감)

	con_box.set_color(c_fg);
	con_box.print(reinterpret_cast<const char*>(u8"╭───────────────────────────────────────────────────────────╮\n"));

	// 로고 헤더 3줄: 흰색 여백 + 살구색 로고 + 흰색 타이틀
	con_box.set_color(c_fg);   con_box.print(reinterpret_cast<const char*>(u8"│ "));
	con_box.set_color(c_logo); con_box.print(reinterpret_cast<const char*>(u8" ▐▛███▜▌ "));
	con_box.set_color(c_fg);   con_box.print(reinterpret_cast<const char*>(u8"  ConBox                                         │\n"));
	con_box.set_color(c_fg);   con_box.print(reinterpret_cast<const char*>(u8"│ "));
	con_box.set_color(c_logo); con_box.print(reinterpret_cast<const char*>(u8"▝▜█████▛▘"));
	con_box.set_color(c_fg);   con_box.print(reinterpret_cast<const char*>(u8"  이식 가능한 커스텀 터미널 컨트롤               │\n"));
	con_box.set_color(c_fg);   con_box.print(reinterpret_cast<const char*>(u8"│ "));
	con_box.set_color(c_logo); con_box.print(reinterpret_cast<const char*>(u8"  ▘▘ ▝▝  "));
	con_box.set_color(c_fg);   con_box.print(reinterpret_cast<const char*>(u8"  MFC CWnd 파생 · UTF-8 문자열 API               │\n"));

	// 본문(표 + 안내) 은 모두 흰색이라 한 덩어리로 출력한다.
	con_box.set_color(c_fg);
	con_box.print(reinterpret_cast<const char*>(
		u8"├──────────┬────────────────────────────────────────────────┤\n"
		u8"│ 렌더링   │ 고정 그리드 · 한글 2배폭 · 자동 줄바꿈/스크롤  │\n"
		u8"├──────────┼────────────────────────────────────────────────┤\n"
		u8"│ 폰트     │ 영문/한글 개별 설정 · 한글 높이 영문에 맞춤    │\n"
		u8"├──────────┼────────────────────────────────────────────────┤\n"
		u8"│ 입력     │ IME 한글 조합(삽입) · 한/영 커서폭 전환        │\n"
		u8"├──────────┼────────────────────────────────────────────────┤\n"
		u8"│ 편집     │ 커서 이동 · 선택/클립보드 · 화살표 입력영역    │\n"
		u8"├──────────┼────────────────────────────────────────────────┤\n"
		u8"│ 색상     │ 문자 단위 전경색·배경색 지정                   │\n"
		u8"├──────────┼────────────────────────────────────────────────┤\n"
		u8"│ 커서     │ 블록 커서 깜빡임 · 조합 중 테두리 커서         │\n"
		u8"├──────────┴────────────────────────────────────────────────┤\n"
		u8"│ 타이프(Type) 쳐서 동작을 확인해 보세요.                   │\n"
		u8"╰───────────────────────────────────────────────────────────╯\n"));

	// M2 검증: ConExe 로 상호작용 셸(cmd)을 ConPTY 로 실행해 입출력 왕복을 확인한다.
	// attach() 가 출력(자식->ConBox)과 입력(ConBox->자식)을 모두 연결하므로, 이후 ConBox 에
	// 타이핑하면 cmd 로 전달되고 결과가 ConBox 에 표시된다. 로컬 에코는 자동으로 꺼진다.
	// (이 단계에서는 VT 시퀀스 미해석 - 일부 제어문자가 그대로 보일 수 있다. VT 파서/셀
	//  그리드는 이후 M3 에서 도입한다.)
	con_box.print("\n");
	con_exe.attach(&con_box);
	// 자식이 종료되면(예: powershell 에서 exit) ConBox 에 안내를 출력하도록 콜백을 건다(M5).
	con_exe.set_exit_callback(&CConBoxDlg::on_child_exit, this);
	con_exe.start("powershell -NoLogo", con_box.grid_cols(), con_box.grid_rows());

	// 키보드 입력을 받도록 ConBox 에 포커스를 준다.
	con_box.SetFocus();
	return FALSE;  // 포커스를 직접 설정했으므로 FALSE 를 반환한다.
}

void CConBoxDlg::on_child_exit(void* user)
{
	// ConExe 자식(powershell)이 종료되면 호출된다. 호출 시점에는 이미 ConExe 정리가 끝나
	// is_running()==false 다. 데모에서는 ConBox 에 종료 안내만 출력한다(원하면 여기서 재시작
	// 하거나 창을 닫을 수도 있다).
	CConBoxDlg* self = (CConBoxDlg*)user;
	if (self != nullptr)
		self->con_box.print(reinterpret_cast<const char*>(
			u8"\r\n\x1b[33m[프로세스가 종료되었습니다. 창을 닫아 주세요.]\x1b[m\r\n"));
}

void CConBoxDlg::layout_children()
{
	// ConBox 가 아직 없으면(초기화 도중) 아무것도 하지 않는다.
	if (con_box.GetSafeHwnd() == NULL)
		return;

	CRect client;
	GetClientRect(&client);

	const int margin = 20;

	// 확인/취소 버튼이 없으므로 ConBox 가 사방 20px 마진으로 클라이언트 영역을 꽉 채운다.
	int box_w = client.Width() - 2 * margin;
	int box_h = client.Height() - 2 * margin;
	if (box_w < 10) box_w = 10;
	if (box_h < 10) box_h = 10;
	con_box.MoveWindow(margin, margin, box_w, box_h);
}

void CConBoxDlg::resize_to_grid(int cols, int rows)
{
	if (con_box.GetSafeHwnd() == NULL)
		return;

	// 가로 cols 칸 x 세로 rows 줄을 담는 데 필요한 ConBox 클라이언트 픽셀(안쪽 여백 포함).
	int bw = 0, bh = 0;
	con_box.client_size_for_grid(cols, rows, bw, bh);

	// ConBox 는 세로 스크롤바를 가진 자식 창이라, 스크롤바가 보일 때는 그 폭만큼
	// 클라이언트가 줄어 칸 수가 모자란다. 스크롤바 폭을 미리 더해 칸 수를 보장한다.
	bw += ::GetSystemMetrics(SM_CXVSCROLL);

	// 데모는 ConBox 를 사방 20px 마진으로 배치하므로(layout_children 과 같은 값),
	// 대화상자 클라이언트는 그만큼 더 크다.
	const int margin = 20;
	CRect want(0, 0, bw + 2 * margin, bh + 2 * margin);

	// 클라이언트 사각형을 현재 창 스타일 기준의 창 사각형(테두리/타이틀바 포함)으로 키운다.
	CalcWindowRect(&want, CWnd::adjustBorder);
	int ww = want.Width();
	int wh = want.Height();

	// 주 모니터의 작업 영역(작업표시줄 제외) 중앙으로 옮긴다.
	RECT wa = { 0 };
	::SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
	int x = wa.left + ((wa.right - wa.left) - ww) / 2;
	int y = wa.top + ((wa.bottom - wa.top) - wh) / 2;

	SetWindowPos(NULL, x, y, ww, wh, SWP_NOZORDER);

	// 크기가 우연히 그대로여서 OnSize 가 오지 않더라도 ConBox 가 새 크기에 맞도록
	// 한 번 더 배치해 둔다. (중복 호출은 무해하다.)
	layout_children();
}

void CConBoxDlg::OnSize(UINT type, int cx, int cy)
{
	CDialog::OnSize(type, cx, cy);
	layout_children();
}

// 대화 상자에 최소화 단추를 추가할 경우 아이콘을 그리려면
//  아래 코드가 필요합니다.  문서/뷰 모델을 사용하는 MFC 애플리케이션의 경우에는
//  프레임워크에서 이 작업을 자동으로 수행합니다.

void CConBoxDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this); // 그리기를 위한 디바이스 컨텍스트입니다.

		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);

		// 클라이언트 사각형에서 아이콘을 가운데에 맞춥니다.
		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;

		// 아이콘을 그립니다.
		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialog::OnPaint();
	}
}

// 사용자가 최소화된 창을 끄는 동안에 커서가 표시되도록 시스템에서
//  이 함수를 호출합니다.
HCURSOR CConBoxDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

