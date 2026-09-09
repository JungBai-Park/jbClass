/*
    main.cpp - CWinApp entry + main() for jbTerm: FrameBox-based app whose
    client area is entirely filled by a single ConBox.

    cJbTermFrame (FrameBox subclass) adds a title-bar system menu (WM_SYSCOMMAND) with
    Save Text/PDF/EMF and Start/Stop Logging (Korean labels), exercising ConBox's export/logging API.
*/

#include "..\..\Source\FrameBox.h"
#include "..\..\Source\ConBox.h"
#include <vector>
#include <string>
#include <dwmapi.h>
#include <shlobj.h>    // SHBrowseForFolderW, SHGetPathFromIDListW
#pragma comment(lib, "dwmapi.lib")

// System menu IDs: must be multiples of 0x10, below 0xF000.
static const UINT ID_SAVE_EMF  = 0xE010;
static const UINT ID_SAVE_TEXT = 0xE020;
static const UINT ID_SAVE_PDF  = 0xE030;
static const UINT ID_LOG       = 0xE040;

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

// FrameBox subclass adding a system-menu (title-bar icon menu) with ConBox export/logging
// actions -- Save Text/PDF/EMF write a scrollback+screen snapshot, Start/Stop Logging toggles
// raw child-output capture. Ported from the old DemoApp system-menu implementation, reordered
// and relabeled in Korean for jbTerm.
class cJbTermFrame : public FrameBox {
public:
    ConBox* con_box = nullptr;

    // Register under "jbTerm" instead of the shared "FrameBox" default, so the
    // window class name is visible as "jbTerm" in tools like Spy++.
    const wchar_t* window_class_name() const override { return L"jbTerm"; }

    void setup_sysmenu() {
        HMENU hSys = ::GetSystemMenu(m_hWnd, FALSE);
        if (!hSys) return;
        ::AppendMenuW(hSys, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(hSys, MF_STRING, ID_SAVE_TEXT, L"Text로 저장...");
        ::AppendMenuW(hSys, MF_STRING, ID_SAVE_PDF,  L"PDF로 저장...");
        ::AppendMenuW(hSys, MF_STRING, ID_SAVE_EMF,  L"EMF로 저장...");
        ::AppendMenuW(hSys, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(hSys, MF_STRING, ID_LOG,       L"기록 시작...");
    }

    // Esc must not close jbTerm the way FrameBox::PreTranslateMessage closes a
    // generic dialog-like frame: consume it here before it reaches the base class.
    BOOL PreTranslateMessage(MSG* pMsg) override {
        // Keyboard input must always reach ConBox: with nothing else in the client area
        // ever needing it instead, any drift away (unclicked at startup, or focus landing
        // on the frame after a system-menu dialog closes with hwndOwner == m_hWnd) is
        // corrected here before the message is processed further. Once focus is on
        // ConBox, FrameBox::PreTranslateMessage's own DLGC_WANTALLKEYS check already
        // defers to normal TranslateMessage/DispatchMessage (which is what generates
        // WM_CHAR for Esc/Enter -- ConBox::OnChar, not OnKeyDown, sends those bytes to
        // the child) -- no special-casing of individual keys needed here.
        if (con_box && (pMsg->message == WM_KEYDOWN || pMsg->message == WM_CHAR) &&
            GetFocus() != con_box)
            con_box->SetFocus();
        return FrameBox::PreTranslateMessage(pMsg);
    }

    LRESULT WindowProc(UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg == WM_ENDSESSION && wp && con_box) {
            // Session is actually ending (shutdown/logoff/restart), not just being queried:
            // force-kill the ConPTY child synchronously. The normal WM_CLOSE -> OnDestroy path
            // relies on an async grace-period timer (CLOSE_TIMER) that may never fire if Windows
            // tears this process down before it elapses, leaving the child as an orphan. terminate()
            // is idempotent, so this is harmless even if the child already exited on its own.
            con_box->terminate();
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
                    char utf8[MAX_PATH * 3] = {};
                    ::WideCharToMultiByte(CP_UTF8, 0, file, -1, utf8, sizeof(utf8), NULL, NULL);
                    if (con_box->save_text(utf8))
                        ::MessageBoxW(m_hWnd, L"Text saved.", L"Info", MB_OK | MB_ICONINFORMATION);
                    else
                        ::MessageBoxW(m_hWnd, L"Text save failed.", L"Error", MB_OK | MB_ICONERROR);
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
                    if (hSys) ::ModifyMenuW(hSys, ID_LOG, MF_BYCOMMAND | MF_STRING, ID_LOG, L"기록 시작...");
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
                            if (hSys) ::ModifyMenuW(hSys, ID_LOG, MF_BYCOMMAND | MF_STRING, ID_LOG, L"기록 중지");
                        } else {
                            ::MessageBoxW(m_hWnd, L"Failed to open log file.", L"Error", MB_OK | MB_ICONERROR);
                        }
                    }
                }
                return 0;
            }
        }
        LRESULT result = FrameBox::WindowProc(msg, wp, lp);
        // Clicking the title bar (non-client area) reactivates this top-level window
        // without any client-area mouse message ever reaching con_box, so nothing calls
        // SetFocus on it -- FrameBox is a plain CWnd (not CFrameWnd), which has no
        // built-in last-focus save/restore on WM_ACTIVATE. con_box is the only control
        // in the client area, so focus always belongs to it: force it unconditionally
        // whenever this window becomes active. Must run AFTER the base WindowProc call
        // above (which reaches DefWindowProc for WM_ACTIVATE) -- DefWindowProc's own
        // default WM_ACTIVATE handling sets focus to this top-level window itself, so
        // calling SetFocus(con_box) before it just gets overwritten by that default.
        if (msg == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE && con_box)
            con_box->SetFocus();
        return result;
    }
};

cJbTermApp    App;
cJbTermFrame  Top;

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

// RCDATA id embedded in jbTerm.rc (jbTerm.ini content as of build time). No resource.h in this
// project -- keep this value in sync with the numeric id used in jbTerm.rc's "129 RCDATA ..." line.
static const int IDR_DEFAULT_INI = 129;

// Read the embedded default jbTerm.ini back out as a UTF-8 string (raw resource bytes -- content
// is whatever jbTerm.ini held at build time, BOM included; ConBox::setup()/ParseIni() already skip
// a leading UTF-8 BOM so no stripping is needed here). Returns empty string if the resource is
// missing for some reason (e.g. a build with a stale jbTerm.rc), in which case the caller ends up
// applying no extra settings and ConBox's compiled-in constructor defaults stand.
static std::string LoadEmbeddedDefaultIni() {
    HMODULE mod = ::GetModuleHandleW(nullptr);
    HRSRC res = ::FindResourceW(mod, MAKEINTRESOURCEW(IDR_DEFAULT_INI), RT_RCDATA);
    if (!res) return std::string();
    HGLOBAL data = ::LoadResource(mod, res);
    if (!data) return std::string();
    const char* ptr = static_cast<const char*>(::LockResource(data));
    DWORD size = ::SizeofResource(mod, res);
    if (!ptr || size == 0) return std::string();
    return std::string(ptr, size);
}

static std::string WideToUtf8(const wchar_t* w) {
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// Escape a bare command string before it is implicitly given a "cmdline=" key, so ConBox's universal
// escape decoding / INI comment-cut do not reinterpret it: '\' becomes "\\" (DecodeEscapes' \\ rule
// then restores exactly one '\', so it never combines with the next character into an unrelated
// escape like '\t'), and ';' becomes the literal 4-char "\x3b" (ParseIniLine's raw-';' comment cut
// never sees it, and DecodeEscapes decodes \x3b back to ';'). The '\' pass runs first so the
// backslash introduced for ';' is not itself re-doubled. Net effect: a bare cmdline argument behaves
// as literal text with no escaping to think about, matching what "@" file paths already get.
static std::string EscapeForAutoCmdline(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (c == '\\') out += "\\\\";
        else if (c == ';') out += "\\x3b";
        else out += c;
    }
    return out;
}

int main(int argc, const char* argv[]) {
    const int width = 900, height = 600;   // placeholder only (fit_to_children() below sets
                                            // the real size from jbTerm.ini)

    // ConBox is created and configured (settings resolved) BEFORE Top.OpenFrame() below, purely
    // so the start_x/start_y INI keys are available in time to pass to OpenFrame's x/y args. This
    // is safe: setup()/setup_from_ini() only parse strings into fields (ConBox's own window is not
    // created yet either -- that happens later, at conBox->open(&Top, ...)), and the callbacks
    // registered here reference Top.m_hWnd only when actually invoked, long after Top exists.
    ConBox* conBox = new ConBox;
    // Callbacks registered before setup_from_ini(): set_titlebar_color_cb must be registered first,
    // or a freshly created default INI (jbTerm.ini not found yet) omits the [titlebar] block.
    conBox->set_exit_callback(OnShellExit);            // close jbTerm when the shell exits
    conBox->set_title_cb(OnTitleChanged);              // reflect the shell's OSC title in the title bar
    conBox->set_titlebar_color_cb(OnTitlebarColor);    // apply the ini's title-bar colors (Win11+)

    // Global settings come exclusively from the jbTerm.ini embedded in jbTerm.rc (see
    // IDR_DEFAULT_INI above) -- no external file is consulted for the global tier, so a single
    // jbTerm.exe is fully self-contained. Only an explicit "@file" argument (below) loads
    // settings from disk.
    conBox->setup(LoadEmbeddedDefaultIni().c_str());

    // Local settings: command-line arguments (argv[1..]) layer on top of the global settings above.
    // __wargv is used (not argv) so non-ASCII values survive intact -- the console/command-line text
    // is not UTF-8, but ConBox expects UTF-8, so each wide argument is converted explicitly below.
    // An argument starting with '@' is a file/path: everything after that single leading '@' is the
    // literal path (a real filename that itself starts with '@' is given as "@@name" -- stripping only
    // the marker leaves the literal "@name"); it is resolved against the CURRENT WORKING DIRECTORY via
    // GetFullPathNameW (unlike the exe-relative paths above) and its contents loaded the same way as
    // an INI file. A bare "@" with nothing after it is ignored. Any other argument containing '=' is a
    // "key = value" settings line; one with neither '@' nor '=' is taken as a shell command line and
    // gets "cmdline=" prepended automatically (e.g. `jbTerm "ssh user@host"`), with its '\' and ';'
    // pre-escaped (EscapeForAutoCmdline) so it is taken literally -- only the LAST such bare argument
    // survives if more than one is given (same last-one-wins rule as any repeated key -- see ParseIni).
    // A "cmdline=..." the user writes explicitly is NOT pre-escaped, so \x3d/\x3b remain available
    // there for a command that genuinely needs a literal '='/';'. File arguments are applied first, in
    // argv order; every "=" (including the auto-"cmdline=") argument is then joined with '\n' and
    // applied in a single setup() call last, so a match=/send= trigger pair given as separate
    // arguments still lands in one setup() call and registers correctly ([triggers] groups must share
    // one call to pair up).
    std::string kv_args;
    for (int i = 1; i < argc; ++i) {
        const wchar_t* warg = __wargv[i];
        if (warg[0] == L'@') {
            const wchar_t* file_arg = warg + 1;
            if (file_arg[0] == L'\0') continue;   // bare "@": nothing to load
            wchar_t abs_path[MAX_PATH] = {};
            DWORD len = ::GetFullPathNameW(file_arg, MAX_PATH, abs_path, nullptr);
            const wchar_t* resolved = (len > 0 && len < MAX_PATH) ? abs_path : file_arg;
            conBox->setup_from_ini(WideToUtf8(resolved).c_str());
        } else {
            if (!kv_args.empty()) kv_args += "\n";
            if (std::wstring(warg).find(L'=') != std::wstring::npos)
                kv_args += WideToUtf8(warg);
            else
                kv_args += "cmdline=" + EscapeForAutoCmdline(WideToUtf8(warg));
        }
    }
    if (!kv_args.empty())
        conBox->setup(kv_args.c_str());

    // start_x/start_y INI keys (config_start_x/y()) resolve to CW_USEDEFAULT when unset, which
    // OpenFrame passes straight through to CreateWindow -- same as the previous hardcoded
    // CW_USEDEFAULT, letting the system pick the top-left (and thereby the startup DPI monitor);
    // see FrameBox::open_core() for how CW_USEDEFAULT is resolved there.
    Top.OpenFrame(&App, conBox->config_start_x(), conBox->config_start_y(), width, height);  // created hidden; shown on the first wait() below

    // Normal dialog-style frame: title bar + close/minimize; maximize box shown but
    // disabled (WS_MAXIMIZEBOX omitted while WS_MINIMIZEBOX stays); fixed size, no
    // resize border (WS_THICKFRAME dropped). Set before fit_to_children() so its
    // AdjustWindowRectEx call sees the final frame style.
    // WS_CLIPCHILDREN: ConBox covers the client area exactly (margin 0), so excluding it
    // from the frame's own paint/erase region avoids a visible flash of FrameBox's default
    // WM_ERASEBKGND fill (COLOR_BTNFACE) whenever the frame is invalidated (e.g. uncovered
    // by another window) before ConBox's own double-buffered repaint catches up. Not the
    // FrameBox default (see Documents/1. FrameBox/PITFALLS.md #14: transparent AddStatic
    // controls elsewhere rely on the parent painting under them) -- safe here since jbTerm's
    // only child is the opaque, fully-covering ConBox.
    LONG_PTR style = ::GetWindowLongPtrW(Top.m_hWnd, GWL_STYLE);
    style = (style & ~(WS_THICKFRAME | WS_MAXIMIZEBOX)) | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    ::SetWindowLongPtrW(Top.m_hWnd, GWL_STYLE, style);
    ::SetWindowPos(Top.m_hWnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    ::SetWindowTextW(Top.m_hWnd, L"jbTerm");
    Top.set_margin(0);                       // keep the frame snug around ConBox (also on zoom/DPI changes)

    conBox->open(&Top, 0, 0);                // sizes itself from the ini's font/rows/cols/margin
    Top.AddNew(0, 0, 0, 0, conBox);          // attach-only: register without moving/resizing it
    Top.con_box = conBox;
    Top.setup_sysmenu();                     // title-bar icon menu: Save EMF/Text/PDF, Start Logging
    Top.fit_to_children();                   // resize Top to wrap conBox exactly (uses ConBox's computed size)
    conBox->SetFocus();                      // start with the cursor showing -- the WM_ACTIVATE-driven
                                              // focus fix (see WindowProc's WM_ACTIVATE handling) does not
                                              // reliably fire on the very first ShowWindow(SW_SHOW) below
                                              // (inside Top.wait()), so focus would otherwise sit on Top
                                              // until the user clicks the title bar or presses a key

    while (::IsWindow(Top)) {
        CWnd* ev = Top.wait();
        if (!ev) break;   // window closed
    }
    return 0;
}
