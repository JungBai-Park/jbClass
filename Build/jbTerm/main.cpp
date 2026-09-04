/*
    main.cpp - CWinApp entry + main() for jbTerm: FrameBox-based app whose
    client area is entirely filled by a single ConBox.
*/

#include "..\..\Source\FrameBox.h"
#include "..\..\Source\ConBox.h"
#include <vector>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")


class cJbTermApp : public CWinApp {
    int exit_code = 0;
public:
    BOOL InitInstance() override {
        int main(int argc, const char *argv[]);

        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES };
        InitCommonControlsEx(&icc);
        CWinApp::InitInstance();

        exit_code = main(__argc, const_cast<const char**>(__argv));
        return FALSE;
    }

    int ExitInstance() override {
        return(exit_code);
    }
};

cJbTermApp App;
FrameBox   Top;

// ConBox exit callback: the shell (child process) exited on its own, so close the
// window the same way the close button does (WM_CLOSE -> DestroyWindow -> wait()
// returns nullptr, ending the main loop below).
static void OnShellExit() {
    ::PostMessageW(Top.m_hWnd, WM_CLOSE, 0, 0);
}

// ConBox title callback: the shell sent an OSC 0/2 "set title" sequence (title is UTF-8); apply
// it to the frame's title bar.
static void OnTitleChanged(const char* title) {
    int wlen = ::MultiByteToWideChar(CP_UTF8, 0, title, -1, nullptr, 0);
    if (wlen <= 0) return;
    std::vector<wchar_t> wtitle(wlen);
    ::MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle.data(), wlen);
    ::SetWindowTextW(Top.m_hWnd, wtitle.data());
}

// ConBox title-bar color callback: apply the ini's titlebar_caption/text/border to the frame via
// DWM (Windows 11 22000+ only; DwmSetWindowAttribute fails harmlessly on older Windows, leaving
// the system default title bar). CLR_INVALID means "not set in the ini" -- skip that attribute.
static void OnTitlebarColor(COLORREF caption, COLORREF text, COLORREF border) {
    if (caption != CLR_INVALID) ::DwmSetWindowAttribute(Top.m_hWnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    if (text    != CLR_INVALID) ::DwmSetWindowAttribute(Top.m_hWnd, DWMWA_TEXT_COLOR,    &text,    sizeof(text));
    if (border  != CLR_INVALID) ::DwmSetWindowAttribute(Top.m_hWnd, DWMWA_BORDER_COLOR,  &border,  sizeof(border));
}

int main(int argc, const char* argv[]) {
    const int width = 900, height = 600;   // placeholder only (picks the startup DPI monitor);
                                            // fit_to_children() below sets the real size from jbTerm.ini

    Top.OpenFrame(&App, 0, 0, width, height);   // created hidden; shown on the first wait() below

    // Normal dialog-style frame: title bar + close/minimize; maximize box shown but
    // disabled (WS_MAXIMIZEBOX omitted while WS_MINIMIZEBOX stays); fixed size, no
    // resize border (WS_THICKFRAME dropped). Set before fit_to_children() so its
    // AdjustWindowRectEx call sees the final frame style.
    LONG_PTR style = ::GetWindowLongPtrW(Top.m_hWnd, GWL_STYLE);
    style = (style & ~(WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_MINIMIZEBOX;
    ::SetWindowLongPtrW(Top.m_hWnd, GWL_STYLE, style);
    ::SetWindowPos(Top.m_hWnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    ::SetWindowTextW(Top.m_hWnd, L"jbTerm");
    Top.set_margin(0);                       // keep the frame snug around ConBox (also on zoom/DPI changes)

    ConBox* conBox = new ConBox;
    // Callbacks registered before setup_from_ini(): set_titlebar_color_cb must be registered first,
    // or a freshly created default INI (jbTerm.ini not found yet) omits the [titlebar] block.
    conBox->set_exit_callback(OnShellExit);            // close jbTerm when the shell exits
    conBox->set_title_cb(OnTitleChanged);              // reflect the shell's OSC title in the title bar
    conBox->set_titlebar_color_cb(OnTitlebarColor);    // apply the ini's title-bar colors (Win11+)
    conBox->setup_from_ini("..\\..\\..\\Documents\\jbTerm.ini");   // shared with DemoApp
    conBox->open(&Top, 0, 0);                // sizes itself from the ini's font/rows/cols/margin
    Top.AddNew(0, 0, 0, 0, conBox);          // attach-only: register without moving/resizing it
    Top.fit_to_children();                   // resize Top to wrap conBox exactly (uses ConBox's computed size)
    Top.CenterWindow();                      // now that the final size is known

    while (::IsWindow(Top)) {
        CWnd* ev = Top.wait();
        if (!ev) break;   // window closed
    }
    return 0;
}
