
// ConBoxDlg.cpp: 구현 파일
//

#include "pch.h"
#include "framework.h"
#include "ConBoxApp.h"
#include "ConBoxDlg.h"
#include "afxdialogex.h"
#include <shlobj.h>    // SHBrowseForFolder, SHGetPathFromIDList

#ifdef _DEBUG
#define new DEBUG_NEW
#endif


// cConBoxDlg 대화 상자



cConBoxDlg::cConBoxDlg(CWnd* pParent /*=nullptr*/)
    : CDialog(IDD_CONBOX_DIALOG, pParent)
{
    m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void cConBoxDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialog::DoDataExchange(pDX);
}

// System menu item IDs. Must be multiples of 0x10 (low 4 bits reserved by Windows)
// and below 0xF000 (above that are system-defined SC_ values handled by DefWindowProc).
static const UINT ID_SAVE_EMF  = 0xE010;
static const UINT ID_SAVE_TEXT = 0xE020;
static const UINT ID_SAVE_PDF  = 0xE030;
static const UINT ID_LOG       = 0xE040;

BEGIN_MESSAGE_MAP(cConBoxDlg, CDialog)
    ON_WM_PAINT()
    ON_WM_QUERYDRAGICON()
    ON_WM_SIZE()
    ON_WM_DESTROY()
    ON_WM_SYSCOMMAND()
END_MESSAGE_MAP()


// cConBoxDlg 메시지 처리기

BOOL cConBoxDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    // 이 대화 상자의 아이콘을 설정합니다.  응용 프로그램의 주 창이 대화 상자가 아닐 경우에는
    //  프레임워크가 이 작업을 자동으로 수행합니다.
    SetIcon(m_hIcon, TRUE);         // 큰 아이콘을 설정합니다.
    SetIcon(m_hIcon, FALSE);        // 작은 아이콘을 설정합니다.

    // 타이틀바에 최소화/최대화 버튼을 추가한다.
    // 캡션 버튼은 시스템 메뉴(WS_SYSMENU)가 있어야 보이므로 함께 켠다.
    // 최소화 버튼으로 창을 작업표시줄 아이콘으로 보낼 수 있다. (EXSTYLE 에 WS_EX_APPWINDOW 가 있어 항상 표시됨)
    ModifyStyle(0, WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    SetWindowPos(NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    // 시스템 메뉴(타이틀바 왼쪽 아이콘 클릭 메뉴)에 저장/로깅 항목을 추가한다.
    CMenu* sys_menu = GetSystemMenu(FALSE);
    if (sys_menu) {
        sys_menu->AppendMenu(MF_SEPARATOR);
        sys_menu->AppendMenu(MF_STRING, ID_SAVE_EMF,  _T("Save EMF..."));
        sys_menu->AppendMenu(MF_STRING, ID_SAVE_TEXT, _T("Save Text..."));
        sys_menu->AppendMenu(MF_STRING, ID_SAVE_PDF,  _T("Save PDF..."));
        sys_menu->AppendMenu(MF_SEPARATOR);
        sys_menu->AppendMenu(MF_STRING, ID_LOG,       _T("Start Logging..."));
    }

    // 데모에서는 확인/취소 버튼을 쓰지 않으므로 제거한다.
    if (CWnd* ok = GetDlgItem(IDOK))
        ok->DestroyWindow();
    if (CWnd* cancel = GetDlgItem(IDCANCEL))
        cancel->DestroyWindow();

    // setup_from_ini() 로 폰트, 색상, 커서, 마진, 그리드 크기, 자식 명령줄을 한 번에 불러온다.
    // 파일이 없으면 기본값으로 자동 생성된다.
    SetCurrentDirectory(_T("D:\\Work\\Study\\ConBox"));
    con_box.setup_from_ini("..\\..\\Documents\\ConBox.ini");

    // ConBox 를 자식 창으로 만든다. 크기는 INI 의 grid_cols/rows 설정을 따른다.
    // cmdline 이 비어 있지 않으면 open() 내부에서 start() 를 자동 호출한다.
    con_box.open(this, 5, 5);

    // open() 이 결정한 ConBox 크기에 맞춰 대화상자 크기를 조정하고 화면 중앙으로 배치한다.
    resize_to_grid();

    // 자식이 종료되면 대화상자를 닫는다.
    con_box.set_exit_callback(&cConBoxDlg::on_child_exit, this);

    // 키보드 입력을 받도록 ConBox 에 포커스를 준다.
    con_box.SetFocus();
    return FALSE;  // 포커스를 직접 설정했으므로 FALSE 를 반환한다.
}

void cConBoxDlg::OnDestroy()
{
    // 창이 닫힐 때(타이틀바 X 포함) 자식/출력펌프/타이머를 먼저 깔끔히 정리한다. 그래야
    // 출력 펌프가 파괴 중인 ConBox 를 건드려 죽지 않고, 디버그 로그/캡처도 정상 마감된다.
    // (con_box 자신의 OnDestroy 도 stop 하지만, 여기서 먼저 불러 종료 경로를 명확히 한다. 멱등.)
    con_box.stop();
    CDialog::OnDestroy();
}

void cConBoxDlg::on_child_exit(void* user)
{
    // ConExe 자식(cmd.exe)이 종료되면 호출된다. 호출 시점에는 이미 ConExe 정리가 끝나
    // is_running()==false 다. 데모에서는 자식이 끝나면 메인 윈도우를 닫는다.
    // 이 콜백은 ConExe 출력 펌프(타이머) 안에서 호출되므로, 펌프 스택을 빠져나온 뒤
    // 안전하게 닫히도록 즉시 닫지 않고 WM_CLOSE 를 게시(PostMessage)한다.
    cConBoxDlg* self = (cConBoxDlg*)user;
    if (self != nullptr && self->GetSafeHwnd() != NULL)
        self->PostMessage(WM_CLOSE);
}

void cConBoxDlg::layout_children()
{
    // ConBox 가 아직 없으면(초기화 도중) 아무것도 하지 않는다.
    if (con_box.GetSafeHwnd() == NULL)
        return;

    CRect client;
    GetClientRect(&client);

    const int margin = 5;

    // 확인/취소 버튼이 없으므로 ConBox 가 사방 5px 마진(베젤)으로 클라이언트 영역을 꽉 채운다.
    int box_w = client.Width() - 2 * margin;
    int box_h = client.Height() - 2 * margin;
    if (box_w < 10) box_w = 10;
    if (box_h < 10) box_h = 10;
    con_box.MoveWindow(margin, margin, box_w, box_h);
}

void cConBoxDlg::resize_to_grid()
{
    if (con_box.GetSafeHwnd() == NULL)
        return;

    // open() 이 결정한 ConBox 클라이언트 크기를 직접 읽는다.
    // ConBox 는 오버레이 스크롤바(클라이언트를 잠식하지 않음)를 쓰므로 스크롤바 폭 보정이 불필요하다.
    CRect box_rc;
    con_box.GetClientRect(&box_rc);

    // 데모는 ConBox 를 사방 5px 마진(베젤)으로 배치하므로(layout_children 과 같은 값),
    // 대화상자 클라이언트는 그만큼 더 크다.
    const int margin = 5;
    CRect want(0, 0, box_rc.Width() + 2 * margin, box_rc.Height() + 2 * margin);

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

void cConBoxDlg::OnSize(UINT type, int cx, int cy)
{
    CDialog::OnSize(type, cx, cy);
    layout_children();
}

// 대화 상자에 최소화 단추를 추가할 경우 아이콘을 그리려면
//  아래 코드가 필요합니다.  문서/뷰 모델을 사용하는 MFC 애플리케이션의 경우에는
//  프레임워크에서 이 작업을 자동으로 수행합니다.

void cConBoxDlg::OnPaint()
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
HCURSOR cConBoxDlg::OnQueryDragIcon()
{
    return static_cast<HCURSOR>(m_hIcon);
}

void cConBoxDlg::OnSysCommand(UINT id, LPARAM lparam)
{
    // WM_SYSCOMMAND: low 4 bits of id carry system-internal flags; mask them before comparing.
    if ((id & 0xFFF0) == ID_SAVE_EMF) {
        // Show the classic folder-browse dialog (no COM/OLE init required).
        BROWSEINFO bi = {};
        bi.hwndOwner = GetSafeHwnd();
        bi.lpszTitle = _T("Select folder to save EMF files");
        bi.ulFlags   = BIF_RETURNONLYFSDIRS;
        LPITEMIDLIST pidl = ::SHBrowseForFolder(&bi);
        if (pidl) {
            wchar_t path[MAX_PATH] = {};
            if (::SHGetPathFromIDListW(pidl, path)) {
                char utf8[MAX_PATH * 3] = {};
                ::WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), NULL, NULL);
                if (con_box.save_emf(utf8))
                    AfxMessageBox(_T("EMF saved."), MB_OK | MB_ICONINFORMATION);
                else
                    AfxMessageBox(_T("EMF save failed."), MB_OK | MB_ICONERROR);
            }
            ::CoTaskMemFree(pidl);
        }
    } else if ((id & 0xFFF0) == ID_SAVE_TEXT) {
        CFileDialog dlg(FALSE, _T("txt"), _T("ConBox"),
                        OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY,
                        _T("Text Files (*.txt)|*.txt|All Files (*.*)|*.*||"));
        if (dlg.DoModal() == IDOK) {
            CString wpath = dlg.GetPathName();
            HANDLE hf = ::CreateFile(wpath, GENERIC_WRITE, 0, NULL,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf != INVALID_HANDLE_VALUE) {
                std::vector<std::string> lines = con_box.get_text_lines();
                for (const std::string& line : lines) {
                    DWORD written;
                    ::WriteFile(hf, line.c_str(), (DWORD)line.size(), &written, NULL);
                    ::WriteFile(hf, "\r\n", 2, &written, NULL);
                }
                ::CloseHandle(hf);
                AfxMessageBox(_T("Text saved."), MB_OK | MB_ICONINFORMATION);
            }
        }
    } else if ((id & 0xFFF0) == ID_SAVE_PDF) {
        CFileDialog dlg(FALSE, _T("pdf"), _T("ConBox"),
                        OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY,
                        _T("PDF Files (*.pdf)|*.pdf||"), this);
        if (dlg.DoModal() == IDOK) {
            CStringW wpath(dlg.GetPathName().GetString());
            char utf8[MAX_PATH * 3] = {};
            ::WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)wpath, -1, utf8, sizeof(utf8), NULL, NULL);
            if (con_box.save_pdf(utf8))
                AfxMessageBox(_T("PDF saved."), MB_OK | MB_ICONINFORMATION);
            else
                AfxMessageBox(_T("PDF save failed.\nNo PDF printer found or print job failed."),
                              MB_OK | MB_ICONERROR);
        }
    } else if ((id & 0xFFF0) == ID_LOG) {
        if (con_box.is_logging()) {
            // Stop logging.
            con_box.save_log(nullptr);
            CMenu* sys_menu = GetSystemMenu(FALSE);
            if (sys_menu)
                sys_menu->ModifyMenu(ID_LOG, MF_BYCOMMAND | MF_STRING, ID_LOG, _T("Start Logging..."));
        } else {
            // Ask for a log file path and start logging.
            CFileDialog dlg(FALSE, _T("log"), _T("ConBox"),
                            OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY,
                            _T("Log Files (*.log)|*.log|All Files (*.*)|*.*||"), this);
            if (dlg.DoModal() == IDOK) {
                CStringW wpath(dlg.GetPathName().GetString());
                char utf8[MAX_PATH * 3] = {};
                ::WideCharToMultiByte(CP_UTF8, 0, (LPCWSTR)wpath, -1, utf8, sizeof(utf8), NULL, NULL);
                if (con_box.save_log(utf8) == 0) {
                    CMenu* sys_menu = GetSystemMenu(FALSE);
                    if (sys_menu)
                        sys_menu->ModifyMenu(ID_LOG, MF_BYCOMMAND | MF_STRING, ID_LOG, _T("Stop Logging"));
                } else {
                    AfxMessageBox(_T("Failed to open log file."), MB_OK | MB_ICONERROR);
                }
            }
        }
    } else {
        CDialog::OnSysCommand(id, lparam);
    }
}

