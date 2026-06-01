
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


// 데모용: Enter 콜백에서 ConBox 에 접근하기 위한 전역 포인터.
static CConBox* demoBox = nullptr;

// 데모용 Enter 콜백. 입력한 내용을 그대로 되울려(echo) 동작을 확인한다.
static void OnConBoxEnter(const char* input)
{
	if (demoBox == nullptr)
		return;
	// "입력: " 안내는 UTF-8 바이트로 전달한다. input 은 이미 UTF-8 이다.
	demoBox->print(reinterpret_cast<const char*>(u8"입력하신 내용: "));
	demoBox->print(input);
	demoBox->print("\n");
}


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

	// 폰트는 창 생성 전에 지정한다. (기본값과 동일하지만 API 사용 예시를 겸한다.)
	con_box.set_efont("Consolas", 13, "B");
	con_box.set_kfont("Malgun Gothic", 13, "B");

	// ConBox 를 자식 창으로 만든 뒤 현재 창 크기에 맞춰 배치한다.
	// (실제 위치/크기는 layout_children() 이 정한다.)
	con_box.open(this, 0, 0, 10, 10);
	layout_children();

	// Enter 콜백을 연결한다. (데모: 입력을 되울려 줌)
	demoBox = &con_box;
	con_box.on_enter = OnConBoxEnter;

	// 초기 안내 문구를 출력한다. (UTF-8 바이트로 전달한다.)
	// C++20 에서 u8 리터럴은 char8_t 형이므로 const char* 로 캐스팅한다.
	con_box.print(reinterpret_cast<const char*>(
		u8"이 창은 ConBox의 초기 화면입니다.\n\n타이프(Type) 쳐서 동작을 확인해 보세요.\n"));

	// 글자별 색상 데모: set_color/set_bg_color 로 색을 바꾼 뒤 print 하면 그 색으로 찍힌다.
	con_box.print(reinterpret_cast<const char*>(u8"\n[색상 데모] "));
	con_box.set_color(RGB(255, 80, 80));   con_box.print(reinterpret_cast<const char*>(u8"빨강 "));
	con_box.set_color(RGB(80, 220, 80));   con_box.print(reinterpret_cast<const char*>(u8"초록 "));
	con_box.set_color(RGB(90, 170, 255));  con_box.print(reinterpret_cast<const char*>(u8"파랑 "));
	con_box.set_bg_color(RGB(70, 70, 70));
	con_box.set_color(RGB(255, 230, 0));   con_box.print(reinterpret_cast<const char*>(u8"노랑글자+회색배경"));

	// 색상을 기본값(검은 바탕에 흰 글씨)으로 되돌린다. 이후 사용자 입력은 기본색으로 표시된다.
	con_box.set_bg_color(RGB(0, 0, 0));
	con_box.set_color(RGB(255, 255, 255));
	con_box.print("\n");

	// 키보드 입력을 받도록 ConBox 에 포커스를 준다.
	con_box.SetFocus();
	return FALSE;  // 포커스를 직접 설정했으므로 FALSE 를 반환한다.
}

void CConBoxDlg::layout_children()
{
	// ConBox 가 아직 없으면(초기화 도중) 아무것도 하지 않는다.
	if (con_box.GetSafeHwnd() == NULL)
		return;

	CRect client;
	GetClientRect(&client);

	const int margin = 20;

	// 확인/취소 버튼을 오른쪽 아래에 둔다. 버튼 크기는 현재 값을 그대로 쓴다.
	int bw = 50, bh = 14, gap = 6;
	CWnd* ok = GetDlgItem(IDOK);
	CWnd* cancel = GetDlgItem(IDCANCEL);
	if (ok != NULL) {
		CRect r;
		ok->GetWindowRect(&r);
		bw = r.Width();
		bh = r.Height();
	}

	int btn_y = client.bottom - margin - bh;
	if (cancel != NULL)
		cancel->SetWindowPos(NULL, client.right - margin - bw, btn_y, 0, 0,
			SWP_NOSIZE | SWP_NOZORDER);
	if (ok != NULL)
		ok->SetWindowPos(NULL, client.right - margin - bw - gap - bw, btn_y, 0, 0,
			SWP_NOSIZE | SWP_NOZORDER);

	// ConBox 는 위/좌/우 20px 마진, 아래쪽은 버튼 위 20px 까지 채운다.
	int box_w = client.right - 2 * margin;
	int box_h = (btn_y - margin) - margin;
	if (box_w < 10) box_w = 10;
	if (box_h < 10) box_h = 10;
	con_box.MoveWindow(margin, margin, box_w, box_h);
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

