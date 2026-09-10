/*
    main.cpp - CWinApp entry + main() for jbTerm: FrameBox-based app whose
    client area is entirely filled by a single ConBox.

    cJbTermFrame (FrameBox subclass) adds a title-bar system menu (WM_SYSCOMMAND) with
    Save Text/PDF/EMF, Start/Stop Logging, and Create Clone (Korean labels), exercising ConBox's
    export/logging/settings-serialization API.

    Standard-handle modes (cmd.exe only -- PowerShell has no '<' operator; a pipe works in both).
    Both are detected once with GetFileType, so an ordinary launch takes neither path and pays
    nothing for the check:
      jbTerm.exe < settings.ini   the piped text REPLACES the embedded jbTerm.ini as the base
                                  settings layer (not layered on top of it) and is applied to THIS
                                  process's own settings exactly like an "@settings.ini" argument
                                  otherwise would be; any argv @file/key=value can still override it
                                  (see the setup() call order in main() below).
      jbTerm.exe > settings.ini   the settings actually in effect after every layer above (embedded
                                  globals OR stdin, then every argv layer) are written out as a full
                                  INI file. Always runs LAST, so it reflects stdin too.
    The title-bar system menu's "복제본 생성..." writes the CURRENT settings into a NEW copy of this
    exe at a user-chosen path (GetSaveFileNameW), by patching the copy's embedded RCDATA resource --
    the same mechanism "> settings.ini" and "@file" rely on to read/write settings, aimed at another
    exe's resource instead of a stream.
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
static const UINT ID_CLONE     = 0xE050;

// Timer id + TIMERPROC for the brief title-bar status message "복제본 생성..." shows (instead of a
// MessageBoxW popup like the other Save items, per how this menu item was specified). A TIMERPROC
// (not the window's own message map) means WM_TIMER for this id is intercepted by DispatchMessage
// and never reaches cJbTermFrame::WindowProc -- so this stays entirely inside main.cpp with no
// FrameBox.cpp change, and CLONE_MSG_TIMER_ID only needs to differ from FrameBox's own
// MODAL_TIMER_ID (1, see FrameBox.cpp) to avoid colliding on the same HWND.
static const UINT_PTR CLONE_MSG_TIMER_ID = 9001;
static const UINT     CLONE_MSG_MS       = 3000;
static const UINT     SCALE_MSG_MS       = 1000;   // WM_DPICHANGED / Ctrl+Wheel zoom status display

// GetTickCount64() deadline while a ShowTempTitle() message is showing; OnTitleChanged() (below)
// checks this and suppresses applying any shell OSC 0/2 title update to the title bar until it
// passes (the update is still recorded into g_last_client_title either way). Without this, a child
// that sets its own title at startup or on every prompt (e.g. a shell.bat with a "title ..." line)
// can overwrite the status message within milliseconds -- invisible in practice even though it
// really was set. 0 = no message showing (the common case; OnTitleChanged applies titles normally).
static ULONGLONG g_temp_title_until = 0;

// Most recent title the shell asked for via OSC 0/2, whether or not it was actually applied to the
// title bar (see OnTitleChanged() below). Starts at the fixed startup title so RevertTitleProc has
// something correct to restore even if the shell never sent one. RevertTitleProc restores this --
// not a fixed string -- so the status message never clobbers whatever title the shell legitimately
// owns, including one it set while the status message was showing.
static std::wstring g_last_client_title = L"jbTerm";

static void CALLBACK RevertTitleProc(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    ::KillTimer(hwnd, id);
    g_temp_title_until = 0;
    ::SetWindowTextW(hwnd, g_last_client_title.c_str());
}

// Put text in the title bar for ms milliseconds (immune to OnTitleChanged() during that window),
// then revert to whatever the shell most recently asked for (g_last_client_title). Re-calling this
// while already showing reuses the same timer id, which restarts SetTimer's countdown from ms --
// so a repeat of the triggering event within the display window extends it instead of being ignored.
static void ShowTempTitle(HWND hwnd, const wchar_t* text, UINT ms = CLONE_MSG_MS) {
    g_temp_title_until = ::GetTickCount64() + ms;
    ::SetWindowTextW(hwnd, text);
    ::SetTimer(hwnd, CLONE_MSG_TIMER_ID, ms, RevertTitleProc);
}

// Screen-scale status shown for SCALE_MSG_MS on WM_DPICHANGED (monitor DPI change) and on Ctrl+Wheel
// zoom (apply_zoom override, below), prefixed onto whatever title the shell currently owns so the
// client's own title text is never hidden by the status. percent is always eff_dpi()/96*100 --
// dpi and zoom_pm combined into the one scale actually applied on screen -- so either trigger
// reports the same continuous number instead of two independent baselines (raw dpi/96*100 vs raw
// zoom_pm/10) that can jump apart: e.g. showing 125% for a DPI change, then 97% right after a tiny
// Ctrl+Wheel nudge, even though the real applied scale barely moved (125% -> ~121%).
static void ShowScaleTitle(HWND hwnd, int percent) {
    wchar_t prefix[32];
    ::swprintf_s(prefix, L"[화면배율 = %d%%] ", percent);
    ShowTempTitle(hwnd, (prefix + g_last_client_title).c_str(), SCALE_MSG_MS);
}

// Forward declarations: cJbTermFrame::WindowProc (ID_CLONE, below) needs these; their bodies come
// after the class (grouped with the rest of the settings/resource helpers they belong with).
static std::wstring GetSelfExePath();
static std::string  ToIniFileBytes(const std::string& ini_text);
static std::wstring Utf8ToWide(const std::string& s);
static std::string  CreateResourcePatchedCopy(const std::wstring& target_path, const std::string& ini_bytes);

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

    // Ctrl+Wheel zoom: show the resulting scale in the title bar for SCALE_MSG_MS. Base call
    // first (updates zoom_pm) so the percent shown here reflects the new value.
    void apply_zoom(int new_pm, bool cursor_anchor) override {
        FrameBox::apply_zoom(new_pm, cursor_anchor);
        ShowScaleTitle(m_hWnd, ::MulDiv(eff_dpi(), 100, 96));
    }

    // show_clone: whether to offer "복제본 생성..." (and the separator that closes its group).
    // It is left out when this run took no stdin settings and no command-line arguments, because
    // the settings in effect are then exactly the ones already embedded in this exe -- a clone
    // would just be a copy of the running file with nothing added.
    //
    // The three Save formats are nested under one "저장" popup item instead of listed flat: the
    // system menu (GetSystemMenu's HMENU) accepts MF_POPUP like any other menu, so a submenu
    // created with CreatePopupMenu() and attached via AppendMenuW(..., MF_POPUP, (UINT_PTR)hSave, ...)
    // becomes owned by hSys and is destroyed with it -- no separate cleanup needed. WM_SYSCOMMAND
    // delivery for the leaf items (ID_SAVE_TEXT/EMF/PDF) is unaffected by the nesting depth.
    void setup_sysmenu(bool show_clone) {
        HMENU hSys = ::GetSystemMenu(m_hWnd, FALSE);
        if (!hSys) return;
        ::AppendMenuW(hSys, MF_SEPARATOR, 0, nullptr);
        if (show_clone) {
            ::AppendMenuW(hSys, MF_STRING, ID_CLONE, L"복제본 생성...");
            ::AppendMenuW(hSys, MF_SEPARATOR, 0, nullptr);
        }
        HMENU hSave = ::CreatePopupMenu();
        ::AppendMenuW(hSave, MF_STRING, ID_SAVE_TEXT, L"Text로 저장...");
        ::AppendMenuW(hSave, MF_STRING, ID_SAVE_EMF,  L"EMF로 저장...");
        ::AppendMenuW(hSave, MF_STRING, ID_SAVE_PDF,  L"PDF로 저장...");
        ::AppendMenuW(hSys, MF_POPUP, (UINT_PTR)hSave, L"저장");
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
        if (msg == WM_DPICHANGED) {
            // Base call first: it sets dpi = LOWORD(wp) before returning, so eff_dpi() below
            // already reflects the new monitor DPI.
            LRESULT r = FrameBox::WindowProc(msg, wp, lp);
            ShowScaleTitle(m_hWnd, ::MulDiv(eff_dpi(), 100, 96));
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
            if (id == ID_CLONE) {
                std::wstring self = GetSelfExePath();
                std::wstring self_name = L"jbTerm.exe";   // fallback if GetModuleFileNameW ever fails
                if (!self.empty()) {
                    size_t slash = self.find_last_of(L'\\');
                    self_name = (slash == std::wstring::npos) ? self : self.substr(slash + 1);
                }

                // Default directory = Desktop. A failure just leaves lpstrInitialDir null, which
                // GetSaveFileNameW treats as "use its own current-directory default" -- never fatal.
                wchar_t desktop[MAX_PATH] = {};
                PWSTR desktop_pidl = nullptr;
                if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop_pidl)) && desktop_pidl) {
                    wcsncpy_s(desktop, desktop_pidl, _TRUNCATE);
                    ::CoTaskMemFree(desktop_pidl);
                }

                wchar_t file[MAX_PATH] = {};
                wcsncpy_s(file, self_name.c_str(), _TRUNCATE);   // default file name = actual exe name
                OPENFILENAMEW ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner   = m_hWnd;
                ofn.lpstrFilter = L"실행 파일 (*.exe)\0*.exe\0모든 파일 (*.*)\0*.*\0";
                ofn.lpstrFile   = file;
                ofn.nMaxFile    = MAX_PATH;
                ofn.lpstrInitialDir = desktop[0] ? desktop : nullptr;
                ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
                ofn.lpstrDefExt = L"exe";
                if (::GetSaveFileNameW(&ofn)) {
                    std::string ini_bytes = ToIniFileBytes(con_box->create_current_ini());
                    std::string err = CreateResourcePatchedCopy(file, ini_bytes);
                    ShowTempTitle(m_hWnd, err.empty() ? L"복제본을 생성했습니다."
                                                       : (L"복제본 생성 실패: " + Utf8ToWide(err)).c_str());
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

// ConBox title callback: the shell sent an OSC 0/2 "set title" sequence (title is UTF-8). Always
// recorded into g_last_client_title, but applying it to the frame's title bar is suppressed while a
// ShowTempTitle() status message is showing (see g_temp_title_until) so a child that retitles itself
// (at startup or per-prompt) cannot clobber it; RevertTitleProc restores g_last_client_title once the
// status message's timer expires, so this update is not lost -- only its on-screen display is deferred.
static void OnTitleChanged(const char* title) {
    int wlen = ::MultiByteToWideChar(CP_UTF8, 0, title, -1, nullptr, 0);
    if (wlen <= 0) return;
    std::vector<wchar_t> wtitle(wlen);
    ::MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle.data(), wlen);
    g_last_client_title.assign(wtitle.data());
    if (g_temp_title_until != 0 && ::GetTickCount64() < g_temp_title_until) return;
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

// ConBox layout-changed callback: the shell pushed new settings in with OSC 99 and ConBox has already
// resized itself to the new font/grid. Nothing else would notice -- the frame keeps its old size --
// so re-wrap it around ConBox exactly as the startup path does (same call, same reason).
static void OnLayoutChanged() {
    if (::IsWindow(Top.m_hWnd)) Top.fit_to_children();
}

// ConBox window-move callback: a runtime settings block (OSC 99) carried start_x/start_y. ConBox is
// a WS_CHILD and cannot move the frame it lives in, so the move happens here. CW_USEDEFAULT on an
// axis means the block did not name it -- keep the frame's current value for that axis.
static void OnMoveWindow(int x, int y) {
    if (!::IsWindow(Top.m_hWnd)) return;
    RECT wr = {};
    ::GetWindowRect(Top.m_hWnd, &wr);
    if (x == CW_USEDEFAULT) x = wr.left;
    if (y == CW_USEDEFAULT) y = wr.top;
    ::SetWindowPos(Top.m_hWnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// ConBox position-query callback: asked once at the start of a runtime-settings session so OSC 99
// param 0 can restore to wherever the user last dragged the frame, rather than only the startup INI's
// start_x/y. ConBox cannot read this itself (WS_CHILD, no handle to the frame).
static bool OnGetPosition(int* x, int* y) {
    if (!::IsWindow(Top.m_hWnd)) return false;
    RECT wr = {};
    ::GetWindowRect(Top.m_hWnd, &wr);
    *x = wr.left;
    *y = wr.top;
    return true;
}

// RCDATA id embedded in jbTerm.rc (jbTerm.ini content as of build time). No resource.h in this
// project -- keep this value in sync with the numeric id used in jbTerm.rc's "129 RCDATA ..." line.
static const int IDR_DEFAULT_INI = 129;

// Language of that resource: jbTerm.rc puts it under "LANGUAGE 18, 1" (LANG_KOREAN/SUBLANG_KOREAN).
// CreateResourcePatchedCopy must patch the SAME language id so it REPLACES the existing entry --
// writing a different one would leave two variants in the copy and let FindResourceW's
// language-fallback search return either, depending on the machine's locale.
static const WORD IDR_DEFAULT_INI_LANG = MAKELANGID(LANG_KOREAN, SUBLANG_KOREAN);

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

static std::wstring Utf8ToWide(const std::string& s) {
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// Resolve path the same way an explicit "@path" argument is resolved (relative to the CURRENT
// WORKING DIRECTORY, via GetFullPathNameW) and load it as a settings layer. Shared by the '@'
// branch and the bare-argument-with-a-recognized-extension branch of the argv loop in main() --
// both end up doing exactly this.
static void ApplyIniFileArg(ConBox* conBox, const wchar_t* path) {
    wchar_t abs_path[MAX_PATH] = {};
    DWORD len = ::GetFullPathNameW(path, MAX_PATH, abs_path, nullptr);
    const wchar_t* resolved = (len > 0 && len < MAX_PATH) ? abs_path : path;
    conBox->setup_from_ini(WideToUtf8(resolved).c_str());
}

// True if warg names a path ending in .ini, .txt, or .jbt (case-insensitive -- Windows extensions
// are). A bare argument (no leading '@', no '=') with one of these extensions is treated as a
// settings file exactly like an explicit "@path" argument (see the argv loop in main()), so a
// settings file can be dropped onto jbTerm or double-clicked without needing the '@' marker.
static bool HasSettingsFileExtension(const wchar_t* warg) {
    static const wchar_t* const exts[] = { L".ini", L".txt", L".jbt" };
    size_t len = wcslen(warg);
    for (const wchar_t* ext : exts) {
        size_t elen = wcslen(ext);
        if (len >= elen && _wcsicmp(warg + (len - elen), ext) == 0)
            return true;
    }
    return false;
}

// Quote a single argument for embedding in a Windows command-line string, following the same rules
// CreateProcessW's own argv splitter (and CommandLineToArgvW) expect: a run of backslashes is only
// special immediately before a double quote, where it must be doubled (plus one more backslash to
// escape the quote itself); elsewhere backslashes are literal. An argument with no space/tab/quote
// needs no quoting at all and is returned unchanged (keeps the common case readable).
static std::wstring QuoteWindowsArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
        return arg;

    std::wstring out(1, L'"');
    for (auto it = arg.begin(); ; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') { ++it; ++backslashes; }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');   // doubled: the closing quote below follows
            break;
        } else if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');   // doubled, plus one more to escape the quote
            out.push_back(*it);
        } else {
            out.append(backslashes, L'\\');   // not before a quote: literal
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

// Reconstruct a valid Windows command-line string from __wargv[first..argc-1]. NOT a plain
// space-join of the already-dequoted tokens: a token that came from a quoted phrase (e.g. "fix bug"
// from `-m "fix bug"`) contains a space that is part of ONE argument, and joining it back with a
// bare space would let CreateProcessW's own re-tokenizing later split it into two. QuoteWindowsArg
// re-quotes only the tokens that actually need it, so the child receives exactly the original argv
// -- not necessarily byte-identical formatting, but the same argument values.
static std::wstring RebuildCommandLine(int first, int argc) {
    std::wstring out;
    for (int i = first; i < argc; ++i) {
        if (i > first) out.push_back(L' ');
        out += QuoteWindowsArg(__wargv[i]);
    }
    return out;
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

// True when a standard handle was redirected to a file or a pipe ("jbTerm.exe < a.ini",
// "> a.ini", or either side of a pipe). A console handle (FILE_TYPE_CHAR -- the normal launch from
// a terminal, including a ConPTY one) and an absent handle (NULL, e.g. started from Explorer) both
// count as "not redirected". GetFileType only asks the kernel what the handle is: it never reads,
// writes or blocks, so the ordinary launch path is not delayed by this probe.
static bool IsRedirected(DWORD std_handle) {
    HANDLE h = ::GetStdHandle(std_handle);
    if (h == NULL || h == INVALID_HANDLE_VALUE) return false;
    DWORD type = ::GetFileType(h);
    return (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE);
}

// Read redirected stdin to EOF. Bytes are passed through verbatim (a UTF-8 BOM and CRLF line
// endings are both fine: ConBox's INI parser skips the BOM and tolerates the CR). Only called when
// IsRedirected(STD_INPUT_HANDLE) already said the handle is a file/pipe, so this never waits on a
// console. A closed pipe ends the loop via ReadFile failing with ERROR_BROKEN_PIPE, which is normal
// EOF here rather than an error to report.
static std::string ReadAllStdin() {
    std::string data;
    HANDLE h = ::GetStdHandle(STD_INPUT_HANDLE);
    if (h == NULL || h == INVALID_HANDLE_VALUE) return data;
    char buf[4096];
    DWORD n = 0;
    while (::ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0)
        data.append(buf, n);
    return data;
}

// Full path of this running exe, or empty on failure (GetModuleFileNameW itself failing, or the
// path being MAX_PATH or longer).
static std::wstring GetSelfExePath() {
    wchar_t path[MAX_PATH] = {};
    DWORD len = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return std::wstring();
    return path;
}

// Convert create_current_ini()'s '\n'-only text into the on-disk/embedded byte layout used
// everywhere else in this project: UTF-8 BOM + CRLF. Shared by WriteSettingsToStdout ("> file") and
// the "복제본 생성..." menu item -- both ultimately write the same create_current_ini() text, just to
// a stream vs. a copy's embedded resource.
static std::string ToIniFileBytes(const std::string& ini_text) {
    std::string out = "\xEF\xBB\xBF";
    out.reserve(ini_text.size() + ini_text.size() / 16 + 8);
    for (char c : ini_text) {
        if (c == '\n') out += "\r\n";
        else out.push_back(c);
    }
    return out;
}

// Write the settings in effect to redirected stdout as a complete INI file, so "jbTerm.exe > my.ini"
// produces a file that "jbTerm.exe @my.ini" reads back to the identical settings.
static void WriteSettingsToStdout(const std::string& ini_text) {
    HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == NULL || h == INVALID_HANDLE_VALUE) return;
    std::string out = ToIniFileBytes(ini_text);
    DWORD written = 0;
    ::WriteFile(h, out.data(), (DWORD)out.size(), &written, nullptr);
}

// Copy this exe to target_path (overwriting anything already there without asking -- the caller,
// the "복제본 생성..." menu handler, already confirmed that via GetSaveFileNameW's
// OFN_OVERWRITEPROMPT) and replace the copy's embedded INI resource (IDR_DEFAULT_INI) with
// ini_bytes, producing a standalone exe that carries those settings as its own compiled-in globals.
// The RUNNING image cannot be patched (the file is locked while it executes), which is why a copy is
// made first -- BeginUpdateResourceW then opens that fresh copy exclusively. Picking target_path ==
// the running exe's own path fails harmlessly at the CopyFileW step (Windows refuses to overwrite a
// file mapped for execution), so no special guard is needed for that case.
// Returns an empty string on success, or a UTF-8 error message on failure; on any failure AFTER the
// copy exists, the copy is deleted again so no half-configured exe is left behind.
static std::string CreateResourcePatchedCopy(const std::wstring& target_path, const std::string& ini_bytes) {
    std::wstring self = GetSelfExePath();
    if (self.empty())
        return "실행 파일의 경로를 확인하지 못했습니다.";

    if (!::CopyFileW(self.c_str(), target_path.c_str(), FALSE))
        return "파일을 만들지 못했습니다. (오류 " + std::to_string(::GetLastError()) + ")";

    std::string fail;
    HANDLE res = ::BeginUpdateResourceW(target_path.c_str(), FALSE);
    if (!res) {
        fail = "리소스 갱신을 시작하지 못했습니다. (오류 " + std::to_string(::GetLastError()) + ")";
    // (LPCWSTR)RT_RCDATA: RT_RCDATA expands to MAKEINTRESOURCEA (= LPCSTR) in an MBCS build, which
    // would not convert to UpdateResourceW's LPCWSTR parameter; the cast keeps this compiling under
    // either character-set setting.
    } else if (!::UpdateResourceW(res, (LPWSTR)RT_RCDATA, MAKEINTRESOURCEW(IDR_DEFAULT_INI),
                                   IDR_DEFAULT_INI_LANG,
                                   (LPVOID)ini_bytes.data(), (DWORD)ini_bytes.size())) {
        DWORD err = ::GetLastError();
        ::EndUpdateResourceW(res, TRUE);   // TRUE = discard the pending update
        fail = "설정 리소스를 기록하지 못했습니다. (오류 " + std::to_string(err) + ")";
    } else if (!::EndUpdateResourceW(res, FALSE)) {
        fail = "설정 리소스를 반영하지 못했습니다. (오류 " + std::to_string(::GetLastError()) + ")";
    }

    if (!fail.empty()) {
        ::DeleteFileW(target_path.c_str());   // rolls back only the file this function just created
        return fail;
    }
    return std::string();
}

int main(int argc, const char* argv[]) {
    // Probed once, up front (see IsRedirected): a normal launch takes neither branch below and is
    // not slowed down, matching the pre-existing startup behavior exactly.
    const bool stdin_given  = IsRedirected(STD_INPUT_HANDLE);
    const bool stdout_given = IsRedirected(STD_OUTPUT_HANDLE);

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
    conBox->set_layout_changed_cb(OnLayoutChanged);    // re-wrap the frame after an OSC 99 relayout
    conBox->set_move_cb(OnMoveWindow);                 // runtime start_x/start_y moves the frame
    conBox->set_get_position_cb(OnGetPosition);         // lets OSC 99 param 0 restore the pre-session spot

    // Global settings come from the jbTerm.ini embedded in jbTerm.rc (see IDR_DEFAULT_INI above) --
    // UNLESS stdin was given, in which case stdin REPLACES the embedded ini as the base layer
    // entirely rather than being layered on top of it (see the stdin_given branch below). No
    // external file is consulted for the embedded-ini case, so a single jbTerm.exe is fully
    // self-contained; only an explicit "@file" argument (below) loads settings from disk.
    if (!stdin_given)
        conBox->setup(LoadEmbeddedDefaultIni().c_str());

    // Redirected stdin is applied exactly like an "@file" argument would be as the very FIRST
    // setup() call (see the !stdin_given guard above): any key stdin does not mention resolves to
    // ConBox's compiled-in constructor default, not to the embedded jbTerm.ini's value, since that
    // embedded ini was deliberately skipped above. argv-given layers below can still override a key
    // stdin also set. Must run BEFORE the argv loop for that ordering to hold, and BEFORE the stdout
    // dump further down so ">"-redirected output reflects it.
    if (stdin_given)
        conBox->setup(ReadAllStdin().c_str());

    // Local settings: command-line arguments (argv[1..]) layer on top of the global settings above.
    // __wargv is used (not argv) so non-ASCII values survive intact -- the console/command-line text
    // is not UTF-8, but ConBox expects UTF-8, so each wide argument is converted explicitly below.
    // Each argument is classified in this order:
    //   1. Starts with '@': the rest is a file/path (a real filename that itself starts with '@' is
    //      given as "@@name" -- stripping only the marker leaves the literal "@name"). A bare "@"
    //      with nothing after it is ignored.
    //   2. Contains '=': a "key = value" settings line (collected into kv_args, applied together
    //      below -- see the trigger-pairing note further down).
    //   3. Ends in .ini/.txt/.jbt (case-insensitive): treated exactly like an "@" argument (see 1)
    //      even without the marker, so a settings file can be dropped onto jbTerm or given by path
    //      alone (`jbTerm.exe D:\profiles\work.ini`). Missing file: same as a missing "@file" --
    //      ConBox reports it and settings stay at whatever the previous layer resolved (no crash,
    //      no fallback to treating it as a command).
    //   4. Anything else: this argument AND EVERY ONE AFTER IT (verbatim, not reclassified -- an
    //      '@'/'='/.ini-looking argument here is a literal argument to the child, not reinterpreted
    //      as jbTerm's own) are the child's command line. RebuildCommandLine re-quotes only the
    //      __wargv tokens that need it (a token can already contain a space if the user quoted it,
    //      e.g. `-m "fix bug"`; joining with a bare space would let CreateProcessW's own
    //      re-tokenizing later split it back into two) before the whole string is pre-escaped
    //      (EscapeForAutoCmdline) and given a "cmdline=" key. The loop stops here: at most one
    //      command line can be produced per invocation now, always the first one found. A
    //      "cmdline=..." the user writes explicitly (case 2) is NOT pre-escaped, so \x3d/\x3b remain
    //      available there for a command that genuinely needs a literal '='/';'.
    // '@'/.ini-file arguments (cases 1 and 3) are applied immediately, in argv order; every "="
    // argument (case 2) plus the one auto-"cmdline=" (case 4, if any) are joined with '\n' and
    // applied in a single setup() call last, so a match=/send= trigger pair given as separate
    // arguments still lands in one setup() call and registers correctly ([triggers] groups must
    // share one call to pair up).
    std::string kv_args;
    for (int i = 1; i < argc; ++i) {
        const wchar_t* warg = __wargv[i];
        if (warg[0] == L'@') {
            const wchar_t* file_arg = warg + 1;
            if (file_arg[0] == L'\0') continue;   // bare "@": nothing to load
            ApplyIniFileArg(conBox, file_arg);
        } else if (std::wstring(warg).find(L'=') != std::wstring::npos) {
            if (!kv_args.empty()) kv_args += "\n";
            kv_args += WideToUtf8(warg);
        } else if (HasSettingsFileExtension(warg)) {
            ApplyIniFileArg(conBox, warg);
        } else {
            if (!kv_args.empty()) kv_args += "\n";
            kv_args += "cmdline=" + EscapeForAutoCmdline(WideToUtf8(RebuildCommandLine(i, argc).c_str()));
            break;   // everything from here on belongs to the child, not to jbTerm's own arguments
        }
    }
    if (!kv_args.empty())
        conBox->setup(kv_args.c_str());

    // Settings are fully resolved at this point (embedded globals, stdin, every command-line layer),
    // so this is where a redirected stdout gets the dump: what is written is exactly what this
    // process is about to run with, stdin included.
    if (stdout_given)
        WriteSettingsToStdout(conBox->create_current_ini());

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
    // Title-bar icon menu: Save Text/EMF/PDF, Start Logging, plus "복제본 생성..." only when this run
    // actually has settings of its own to bake into a clone (stdin and/or command-line arguments).
    Top.setup_sysmenu(stdin_given || argc > 1);
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
