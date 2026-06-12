/*
    DemoApp.cpp - CWinApp entry + DemoMain() exercising cParasite.

    Mirrors the original Reference/main.cpp style: a listen() modal loop
    dispatching on the returned pointer. The demo uses New* hidden-control
    factories so call sites do not repeat Create styles or manual id allocation.
    InitInstance runs DemoMain() then returns FALSE (DemoMain drives everything
    through listen(), like the old MainBox).

    The LayOut() call sites below are the live-edit / source-rewrite targets:
    middle-click a control, move/resize it, press Enter, and the four integer
    literals on that control's New line are rewritten in place.
*/

#if defined _M_IX86
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")
#elif defined _M_X64
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")
#else
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif
#include "..\Source\LayOut.h"
#include "..\Source\ConBox.h"

void DemoMain();

class cDemoApp : public CWinApp {
public:
    virtual BOOL InitInstance() {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES };
        InitCommonControlsEx(&icc);
        CWinApp::InitInstance();
        DemoMain();
        return FALSE;        // DemoMain ran its own loop; exit the app
    }
    // ExitInstance normally returns AfxGetCurrentMessage()->wParam (the last pumped message's
    // wParam), which is meaningless when InitInstance drives its own modal loop and returns FALSE.
    virtual int ExitInstance() { return 0; }
};

class cDemoWnd : public cModalFrame {
};

cDemoApp theApp;
cDemoWnd theWnd;
cConBox  conBox;

void DemoMain() {
    theWnd.open();
    cModalFrame* Top = static_cast<cModalFrame*>(LayOut(this, &theWnd, 603, -945, 1492, -125));
    Top->CenterWindow();
    theApp.m_pMainWnd = Top;
    Top->timer(500);   // 0.5s repeating timer

    conBox.setup_from_ini("..\\..\\Documents\\ConBox.ini");
    conBox.open(Top, 5, 5);

    CEdit*     edit   = LayOut(Edit,   Top, 476, 623, 851, 658);
    CButton*   button = LayOut(Button, Top, 685, 715, 855, 745, "Close");
    CStatic*   label  = LayOut(Static, Top, 591, 669, 841, 693, "강누리 만세");
    CComboBox* combo  = LayOut(Combo,  Top, 422, 716, 672, 739, "Left,Center,Right");

    AlignText(edit, 5);
    AlignText(label, 3);

    int tick = 0;
    while (::IsWindow(*Top)) {
        CWnd* ev = Top->listen(edit, button, label, combo);
        if (ev == 0)             break;   // window closed
        if (ev == button) break;   // Close button -> quit
        if (ev == Top) {                  // timer fired
            wchar_t title[64];
            swprintf_s(title, _countof(title), L"cParasite Demo - %d", ++tick);
            ::SetWindowTextW(Top->m_hWnd, title);
        }
    }

    if (conBox.GetSafeHwnd()) conBox.DestroyWindow();  // detach before global dtor to avoid phantom MFC handle-map leak
    DeleteLayOutWindows();   // destroys children (edit/button/label/combo) then Top, reverse-registration order
    theApp.m_pMainWnd = nullptr;
}
