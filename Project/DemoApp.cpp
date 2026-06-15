/*
    DemoApp.cpp - CWinApp entry + DemoMain() exercising FrameBox.

    A listen() modal loop dispatching on the returned pointer. The root window is
    a cDemoBox (FrameBox subclass, stack local) that OWNS its controls (Add*
    member factories) and a cConBox attached via AddNew. InitInstance runs
    DemoMain() then returns FALSE; DemoMain drives everything through listen() and
    ~FrameBox tears down all children + clears m_pMainWnd at scope exit.

    cDemoBox overrides WindowProc to:
      - WM_DPICHANGED: update title bar with current DPI + received count (verification).
      - WM_SYSCOMMAND: handle custom system menu items (Save EMF/Text/PDF, Log).

    cDemoSub (FrameBox subclass for the modal sub-dialog) overrides WindowProc to:
      - WM_DPICHANGED: update title bar with current DPI + received count (verification).

    DemoSub() shows a modal sub-dialog: Sub.OpenFrame(owner,...) disables owner
    until the sub-frame goes out of scope (close() re-enables owner).

    DPI awareness: PerMonitorV2 is declared in jbClass.manifest (embedded in EXE
    at link time via AdditionalManifestFiles). Code-based SetProcessDpiAwarenessContext
    does NOT work with UseOfMfc=Static -- see Learned.md section 1.4.

    The OpenFrame()/Add* call sites below are the live-edit / source-rewrite
    targets: middle-click a control (or the frame), move/resize it, press Enter,
    and the four coordinate literals on that call's line are rewritten in place
    (OpenFrame coords start at arg 2; member-call coords at arg 1).
*/

#if defined _M_IX86
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")
#elif defined _M_X64
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")
#else
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif
#include "..\Source\FrameBox.h"
#include "..\Source\ConBox.h"
#include <shlobj.h>    // SHBrowseForFolderW, SHGetPathFromIDListW

// System menu IDs: must be multiples of 0x10, below 0xF000.
static const UINT ID_SAVE_EMF  = 0xE010;
static const UINT ID_SAVE_TEXT = 0xE020;
static const UINT ID_SAVE_PDF  = 0xE030;
static const UINT ID_LOG       = 0xE040;

void DemoMain();
void DemoSub(CWnd* owner);

class cDemoApp : public CWinApp {
public:
    virtual BOOL InitInstance() {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES };
        InitCommonControlsEx(&icc);
        CWinApp::InitInstance();
        DemoMain();          // drives its own listen() loop and deletes the root frame
        return FALSE;        // DemoMain ran its own loop; exit the app
    }
    // ExitInstance normally returns AfxGetCurrentMessage()->wParam (the last pumped message's
    // wParam), which is meaningless when InitInstance drives its own modal loop and returns FALSE.
    virtual int ExitInstance() { return 0; }
};

class cDemoBox : public FrameBox {
public:
    cConBox* con_box = nullptr;
    int dpi_changed_count = 0;

    void update_title() {
        wchar_t buf[80];
        swprintf_s(buf, _countof(buf), L"cParasite Demo [DPI: %d, DPICHANGED: %d]",
                   ::GetDpiForWindow(m_hWnd), dpi_changed_count);
        ::SetWindowTextW(m_hWnd, buf);
    }

    void setup_sysmenu() {
        HMENU hSys = ::GetSystemMenu(m_hWnd, FALSE);
        if (!hSys) return;
        ::AppendMenuW(hSys, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(hSys, MF_STRING, ID_SAVE_EMF,  L"Save EMF...");
        ::AppendMenuW(hSys, MF_STRING, ID_SAVE_TEXT, L"Save Text...");
        ::AppendMenuW(hSys, MF_STRING, ID_SAVE_PDF,  L"Save PDF...");
        ::AppendMenuW(hSys, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(hSys, MF_STRING, ID_LOG,       L"Start Logging...");
    }

    LRESULT WindowProc(UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg == WM_DPICHANGED) {
            TRACE(L"[cDemoBox] WM_DPICHANGED received: DPI=%d\n", (int)LOWORD(wp));
            ++dpi_changed_count;
            LRESULT r = FrameBox::WindowProc(msg, wp, lp);
            update_title();
            return r;
        }
        if (msg == WM_SYSCOMMAND && con_box) {
            UINT id = (UINT)(wp & 0xFFF0);
            if (id == ID_SAVE_EMF) {
                BROWSEINFOW bi = {};
                bi.hwndOwner = m_hWnd;
                bi.lpszTitle = L"Select folder to save EMF files";
                bi.ulFlags   = BIF_RETURNONLYFSDIRS;
                LPITEMIDLIST pidl = ::SHBrowseForFolderW(&bi);
                if (pidl) {
                    wchar_t path[MAX_PATH] = {};
                    if (::SHGetPathFromIDListW(pidl, path)) {
                        char utf8[MAX_PATH * 3] = {};
                        ::WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), NULL, NULL);
                        if (con_box->save_emf(utf8))
                            ::MessageBoxW(m_hWnd, L"EMF saved.", L"Info", MB_OK | MB_ICONINFORMATION);
                        else
                            ::MessageBoxW(m_hWnd, L"EMF save failed.", L"Error", MB_OK | MB_ICONERROR);
                    }
                    ::CoTaskMemFree(pidl);
                }
                return 0;
            }
            if (id == ID_SAVE_TEXT) {
                wchar_t file[MAX_PATH] = L"ConBox";
                OPENFILENAMEW ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner   = m_hWnd;
                ofn.lpstrFilter = L"Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
                ofn.lpstrFile   = file;
                ofn.nMaxFile    = MAX_PATH;
                ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
                ofn.lpstrDefExt = L"txt";
                if (::GetSaveFileNameW(&ofn)) {
                    HANDLE hf = ::CreateFileW(file, GENERIC_WRITE, 0, NULL,
                                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (hf != INVALID_HANDLE_VALUE) {
                        std::vector<std::string> lines = con_box->get_text_lines();
                        for (const std::string& line : lines) {
                            DWORD written;
                            ::WriteFile(hf, line.c_str(), (DWORD)line.size(), &written, NULL);
                            ::WriteFile(hf, "\r\n", 2, &written, NULL);
                        }
                        ::CloseHandle(hf);
                        ::MessageBoxW(m_hWnd, L"Text saved.", L"Info", MB_OK | MB_ICONINFORMATION);
                    }
                }
                return 0;
            }
            if (id == ID_SAVE_PDF) {
                wchar_t file[MAX_PATH] = L"ConBox";
                OPENFILENAMEW ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner   = m_hWnd;
                ofn.lpstrFilter = L"PDF Files (*.pdf)\0*.pdf\0\0";
                ofn.lpstrFile   = file;
                ofn.nMaxFile    = MAX_PATH;
                ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
                ofn.lpstrDefExt = L"pdf";
                if (::GetSaveFileNameW(&ofn)) {
                    char utf8[MAX_PATH * 3] = {};
                    ::WideCharToMultiByte(CP_UTF8, 0, file, -1, utf8, sizeof(utf8), NULL, NULL);
                    if (con_box->save_pdf(utf8))
                        ::MessageBoxW(m_hWnd, L"PDF saved.", L"Info", MB_OK | MB_ICONINFORMATION);
                    else
                        ::MessageBoxW(m_hWnd, L"PDF save failed.\nNo PDF printer found or print job failed.",
                                      L"Error", MB_OK | MB_ICONERROR);
                }
                return 0;
            }
            if (id == ID_LOG) {
                if (con_box->is_logging()) {
                    con_box->save_log(nullptr);
                    HMENU hSys = ::GetSystemMenu(m_hWnd, FALSE);
                    if (hSys) ::ModifyMenuW(hSys, ID_LOG, MF_BYCOMMAND | MF_STRING, ID_LOG, L"Start Logging...");
                } else {
                    wchar_t file[MAX_PATH] = L"ConBox";
                    OPENFILENAMEW ofn = {};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner   = m_hWnd;
                    ofn.lpstrFilter = L"Log Files (*.log)\0*.log\0All Files (*.*)\0*.*\0";
                    ofn.lpstrFile   = file;
                    ofn.nMaxFile    = MAX_PATH;
                    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
                    ofn.lpstrDefExt = L"log";
                    if (::GetSaveFileNameW(&ofn)) {
                        char utf8[MAX_PATH * 3] = {};
                        ::WideCharToMultiByte(CP_UTF8, 0, file, -1, utf8, sizeof(utf8), NULL, NULL);
                        if (con_box->save_log(utf8) == 0) {
                            HMENU hSys = ::GetSystemMenu(m_hWnd, FALSE);
                            if (hSys) ::ModifyMenuW(hSys, ID_LOG, MF_BYCOMMAND | MF_STRING, ID_LOG, L"Stop Logging");
                        } else {
                            ::MessageBoxW(m_hWnd, L"Failed to open log file.", L"Error", MB_OK | MB_ICONERROR);
                        }
                    }
                }
                return 0;
            }
        }
        return FrameBox::WindowProc(msg, wp, lp);
    }
};

class cDemoSub : public FrameBox {
public:
    int dpi_changed_count = 0;

    void update_title() {
        wchar_t buf[80];
        swprintf_s(buf, _countof(buf), L"Modal Sub [DPI: %d, DPICHANGED: %d]",
                   ::GetDpiForWindow(m_hWnd), dpi_changed_count);
        ::SetWindowTextW(m_hWnd, buf);
    }

    LRESULT WindowProc(UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg == WM_DPICHANGED) {
            TRACE(L"[cDemoSub] WM_DPICHANGED received: DPI=%d\n", (int)LOWORD(wp));
            ++dpi_changed_count;
            LRESULT r = FrameBox::WindowProc(msg, wp, lp);
            update_title();
            return r;
        }
        return FrameBox::WindowProc(msg, wp, lp);
    }
};

cDemoApp theApp;

void DemoMain() {
    cDemoBox Top;
    Top.OpenFrame(&theApp, 100, 100, 1000, 800);   // main + self live-edit
    Top.CenterWindow();
    Top.update_title();   // show initial DPI (count=0)

    cConBox* conBox = new cConBox;
    conBox->setup_from_ini("..\\..\\Documents\\ConBox.ini");
    conBox->open(&Top, 5, 5);
    Top.AddNew(0, 0, 0, 0, conBox);   // coords-first; Top owns conBox
    Top.con_box = conBox;
    Top.setup_sysmenu();

    CEdit*     edit   = Top.AddEdit  (25, 430, 410, 490);

    CButton*   button = Top.AddButton(25, 579, 129, 611, "Close");
    CButton*   subBtn = Top.AddButton(140, 579, 244, 611, "Sub");
    CStatic*   label  = Top.AddStatic(29, 508, 279, 532, "강누리 만세");
    CComboBox* combo  = Top.AddCombo (35, 540, 208, 563, "Left,Center,Right");

    AlignText(edit, 5);
    AlignText(label, 3);

    while (::IsWindow(Top)) {
        CWnd* ev = Top.listen(edit, button, subBtn, label, combo);
        if (ev == 0)      break;          // window closed
        if (ev == button) break;          // Close button -> quit
        if (ev == subBtn) DemoSub(&Top);  // open the modal sub-dialog
    }
}

// Modal sub-dialog: Sub.OpenFrame(owner,...) opens an owned popup and disables
// owner (CWnd* overload); ~Sub at scope exit re-enables owner via close().
void DemoSub(CWnd* owner) {
    cDemoSub Sub;
    Sub.OpenFrame(owner, 929, -628, 1229, -408);   // centered on the top (secondary) monitor
    Sub.update_title();   // show initial DPI (count=0)
    CStatic* msg = Sub.AddStatic(22, 124, 262, 148, "모달 서브 프레임");
    CButton* ok  = Sub.AddButton(40, 70, 160, 102, "OK");
    AlignText(msg, 5);
    while (::IsWindow(Sub)) {
        CWnd* ev = Sub.listen(ok);
        if (ev == 0 || ev == ok) break;   // closed or OK -> leave (re-enables owner)
    }
}
