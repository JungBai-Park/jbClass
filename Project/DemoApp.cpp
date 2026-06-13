/*
    DemoApp.cpp - CWinApp entry + DemoMain() exercising FrameBox.

    A listen() modal loop dispatching on the returned pointer. The root window is
    a FrameBox that OWNS its controls (Add* member factories) and a cConBox
    attached via AddNew. InitInstance runs DemoMain() then returns FALSE; DemoMain
    drives everything through listen() and `delete`s the root frame at the end
    (which tears down all children + clears m_pMainWnd).

    The Open()/Add* call sites below are the live-edit / source-rewrite targets:
    middle-click a control, move/resize it, press Enter, and the four leading
    integer literals on that call's line are rewritten in place.
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

void DemoMain();

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

cDemoApp theApp;

void DemoMain() {
    FrameBox* Top = new FrameBox;
    Top->attach(&theApp);                  // sets m_pMainWnd = Top
    Top->Open(393, -955, 1168, -280);      // create top-level + self live-edit
    ::SetWindowTextW(Top->m_hWnd, L"cParasite Demo");
    Top->CenterWindow();
    Top->timer(500);                       // 0.5s repeating timer

    cConBox* conBox = new cConBox;
    conBox->setup_from_ini("..\\..\\Documents\\ConBox.ini");
    conBox->open(Top, 5, 5);
    Top->AddNew(25, 19, 685, 414, conBox);   // coords-first; Top owns conBox

    CEdit*     edit   = Top->AddEdit  (25, 430, 410, 490);
    CButton*   button = Top->AddButton(25, 579, 129, 611, "Close");
    CStatic*   label  = Top->AddStatic(29, 508, 279, 532, "강누리 만세");
    CComboBox* combo  = Top->AddCombo (26, 535, 176, 558, "Left,Center,Right");

    AlignText(edit, 5);
    AlignText(label, 3);

    int tick = 0;
    while (::IsWindow(*Top)) {
        CWnd* ev = Top->listen(edit, button, label, combo);
        if (ev == 0)      break;   // window closed
        if (ev == button) break;   // Close button -> quit
        if (ev == Top) {           // timer fired
            wchar_t title[64];
            swprintf_s(title, _countof(title), L"cParasite Demo - %d", ++tick);
            ::SetWindowTextW(Top->m_hWnd, title);
        }
    }

    delete Top;   // recursive teardown: controls + conBox + self_layout; clears m_pMainWnd
}
