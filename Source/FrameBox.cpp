/*
    FrameBox.cpp - Parasite implementation.

    See FrameBox.h for the public contract, ownership rules and the host
    protocol. This file is the subclass procedure plus the live
    layout editor and the source-code rewriter.

    SECTION INDEX (grep these)
    --------------------------
    [LIFETIME]   ctor / dtor / attach / open / initialize / active()
    [UTIL]       AlignText
    [DETECT]     signal_map + set_default_signal_codes
    [PROC]       proc (static) -> dispatch (instance message router)
    [EDIT]       toggle/enter/leave edit, on_key, on_drag, paint_frame, get_rect
    [SIGNAL]     signal()  (post WM_PARASITE_REPORT)
    [REWRITE]    find_char + LayOutRewrite  (rewrite OpenFrame/Add... literals)
    [HOST]       FrameBox: ctor/dtor/close, create_window, open, attach, timer,
                 add_* member factories + finish_child, WindowProc,
                 PreTranslateMessage (ESC/Enter), surveil, listen_core

    BEHAVIOUR
    ---------
    - Middle button toggles edit mode. In edit mode ALL native mouse/keyboard
      behavior of the control is suppressed; only move/resize gestures work.
    - Exiting via middle button does NOT commit. Enter or left double-click
      commits (rewrites source). ESC cancels and restores the pre-edit rect
      stored in last_rect (no source rewrite).
    - On Enter commit and ESC cancel, PeekMessageW discards the trailing
      WM_CHAR ('\r' or '\x1b') that TranslateMessage posts before DispatchMessage
      runs, so the key does not leak to the child window as input.
    - Only one control is in edit mode at a time (active()). Entering edit on
      another control silently drops the previous one (no commit).
    - Arrow = move snapping left/top to a 5px grid; Ctrl+Arrow = 1px move;
      Shift+Arrow = resize (right/bottom edge) snapping to grid;
      Ctrl+Shift+Arrow = 1px resize. Right/Down snap to the next multiple,
      Left/Up to the previous multiple.
    - Left-drag: proximity-based hit_zone(8px margin from edge) determines
      action. Interior (HTCAPTION) = move; near edge/corner = resize that
      side/corner. WM_SETCURSOR shows IDC_HAND (move) or directional resize
      arrows accordingly. For top-level windows the OS handles drag via
      WM_NCLBUTTONDOWN; for child windows SetCapture + WM_MOUSEMOVE is used.
*/

#include "FrameBox.h"  // must come first: defines _WIN32_WINNT before pulling in afxwin.h
#include <afxpriv.h>   // AfxHookWindowCreate / AfxUnhookWindowCreate (MFC window creation hook).
                       // The Add*/OpenFrame factory macros are function-like with >=4 args, so
                       // they do NOT collide with MFC's own Open(...) members; order is free now.
#include <shellscalingapi.h>  // GetDpiForMonitor
#pragma comment(lib, "Shcore.lib")
#include <vector>
#include <string>

// ======================================================================
// [LIFETIME]
// ======================================================================

Parasite::Parasite()
    : target(nullptr), signal_count(0), report(nullptr)
#if defined(_DEBUG)
    , file(nullptr), line(0), editing(false), drag_edge(0)
#endif
{
    last_rect.SetRectEmpty();  // needed in release builds for DPI reposition (D4)
}

Parasite::~Parasite() {
#if defined(_DEBUG)
    if (active() == this) active() = nullptr;
#endif
    if (target && ::IsWindow(target->m_hWnd))
        ::RemoveWindowSubclass(target->m_hWnd, proc, ID_LAYOUT);
    target = nullptr;
}

void Parasite::attach(CWnd* t) {
    target = t;
}

#if defined(_DEBUG)
Parasite*& Parasite::active() {
    static Parasite* p = nullptr;
    return p;
}

HWND Parasite::editing_hwnd() {
    Parasite* p = active();
    return (p && p->target) ? p->target->m_hWnd : NULL;
}
#endif

// ======================================================================
// [HELPERS]  DPI font helpers; process-wide UI font fallback
// ======================================================================

namespace {
    // Fallback: process-wide 96 DPI UI font (Segoe UI 9pt). Used when sys_font
    // is not yet set (e.g. before open() is called). Intentionally leaked.
    HFONT default_ui_font() {
        static HFONT font = nullptr;
        if (!font) {
            NONCLIENTMETRICSW ncm = { sizeof(ncm) };
            ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
            font = ::CreateFontIndirectW(&ncm.lfMessageFont);
        }
        return font;
    }

    // Create a DPI-correct message font. Caller owns the returned HFONT.
    // Falls back to non-DPI SystemParametersInfoW if the DPI variant fails.
    HFONT make_dpi_font(int font_dpi) {
        NONCLIENTMETRICSW ncm = { sizeof(ncm) };
        bool ok = (::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,
                   sizeof(ncm), &ncm, 0, (UINT)font_dpi) != FALSE);
        if (!ok)
            ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        return ::CreateFontIndirectW(&ncm.lfMessageFont);
    }
}

// Convert a UTF-8 string to a std::wstring for Windows API calls.
static std::wstring utf8_to_wide(const char* s) {
    if (!s || !*s) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n, 0);
    ::MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

void Parasite::initialize(const char* init) {
    if (!init || !target || !::IsWindow(target->m_hWnd)) return;

    wchar_t cls[64] = {};
    ::GetClassNameW(target->m_hWnd, cls, _countof(cls));

    if (_wcsicmp(cls, L"ComboBox") == 0) {
        // Parse comma-separated UTF-8 tokens; add each via CB_ADDSTRING (W form).
        std::string s(init);
        size_t pos = 0;
        int added = 0;
        while (true) {
            size_t comma = s.find(',', pos);
            std::string tok = s.substr(pos, comma == std::string::npos
                                            ? std::string::npos : comma - pos);
            size_t ts = tok.find_first_not_of(" \t");
            size_t te = tok.find_last_not_of(" \t");
            if (ts != std::string::npos) {
                std::wstring wtok = utf8_to_wide(tok.substr(ts, te - ts + 1).c_str());
                ::SendMessageW(target->m_hWnd, CB_ADDSTRING, 0,
                               reinterpret_cast<LPARAM>(wtok.c_str()));
                added++;
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        if (added > 0) static_cast<CComboBox*>(target)->SetCurSel(0);
    }
    else {
        ::SetWindowTextW(target->m_hWnd, utf8_to_wide(init).c_str());
    }
}

// ======================================================================
// [UTIL]  standalone utility functions
// ======================================================================

// AlignText: set horizontal + vertical text alignment for a window.
// number follows the numpad layout:
//   1=top-left    2=top-center    3=top-right
//   4=mid-left    5=mid-center    6=mid-right
//   7=bot-left    8=bot-center    9=bot-right
//
// Supported window classes:
//   "Static"  : SS_ horizontal + SS_CENTERIMAGE for v-center (no native v-bottom).
//   "Edit"    : ES_ horizontal + CEdit::SetRect() for vertical.
//   "Button"  : BS_ horizontal + vertical styles.
//   "ComboBox": horizontal only (via child Edit); v-align not applicable.
void AlignText(CWnd* wnd, int number) {
    if (!wnd || !::IsWindow(wnd->m_hWnd)) return;
    if (number < 1 || number > 9) return;

    int h = (number - 1) % 3; // 0=left, 1=center, 2=right
    int v = (number - 1) / 3; // 0=top,  1=middle, 2=bottom

    wchar_t cls[64] = {};
    ::GetClassNameW(wnd->m_hWnd, cls, _countof(cls));

    if (_wcsicmp(cls, L"Static") == 0) {
        DWORD hadd = (h == 0) ? SS_LEFT : (h == 1) ? SS_CENTER : SS_RIGHT;
        // SS_CENTERIMAGE = v-center; no standard SS_ for bottom (maps to top).
        DWORD vadd = (v == 1) ? SS_CENTERIMAGE : 0;
        wnd->ModifyStyle(SS_LEFT | SS_CENTER | SS_RIGHT | SS_CENTERIMAGE,
                         hadd | vadd);
        wnd->Invalidate();
    }
    else if (_wcsicmp(cls, L"Edit") == 0) {
        DWORD hadd = (h == 0) ? ES_LEFT : (h == 1) ? ES_CENTER : ES_RIGHT;
        wnd->ModifyStyle(ES_LEFT | ES_CENTER | ES_RIGHT, hadd);

        // Vertical: shift the formatting rect's top to the desired position.
        CEdit* edit = static_cast<CEdit*>(wnd);
        CRect client;
        edit->GetClientRect(&client);

        CClientDC dc(edit);
        CFont* font = edit->GetFont();
        CFont* old_font = font ? dc.SelectObject(font) : nullptr;
        TEXTMETRICW tm = {};
        ::GetTextMetricsW(dc.GetSafeHdc(), &tm);
        if (old_font) dc.SelectObject(old_font);
        int fh = tm.tmHeight;

        // GetRect() returns Windows' own formatting rect (already has correct
        // left/right insets). Only top is adjusted; touching left/right would
        // corrupt the border rendering.
        CRect fmt;
        edit->GetRect(&fmt);
        if (v == 0)      fmt.top = fmt.left; // match the natural horizontal inset
        else if (v == 1) fmt.top = (client.Height() - fh) / 2;
        else             fmt.top = client.Height() - fh - fmt.left;
        edit->SetRect(&fmt);
    }
    else if (_wcsicmp(cls, L"Button") == 0) {
        DWORD hadd = (h == 0) ? BS_LEFT   : (h == 1) ? BS_CENTER  : BS_RIGHT;
        DWORD vadd = (v == 0) ? BS_TOP    : (v == 1) ? BS_VCENTER : BS_BOTTOM;
        wnd->ModifyStyle(BS_LEFT | BS_RIGHT | BS_TOP | BS_BOTTOM, hadd | vadd);
        wnd->Invalidate();
    }
    else if (_wcsicmp(cls, L"ComboBox") == 0) {
        // ComboBox has no H/V alignment styles of its own; delegate to its child Edit.
        DWORD hadd = (h == 0) ? ES_LEFT : (h == 1) ? ES_CENTER : ES_RIGHT;
        CWnd* child = wnd->GetWindow(GW_CHILD);
        while (child) {
            wchar_t ccls[32] = {};
            ::GetClassNameW(child->m_hWnd, ccls, _countof(ccls));
            if (_wcsicmp(ccls, L"Edit") == 0) {
                child->ModifyStyle(ES_LEFT | ES_CENTER | ES_RIGHT, hadd);
                child->Invalidate();
                break;
            }
            child = child->GetNextWindow();
        }
        wnd->Invalidate();
    }
}

// ======================================================================
// [DETECT]  auto signal-code list from window class (extension point)
// ======================================================================

namespace {
#define SIG_CODE(code) static_cast<int>(code)

    struct LAYOUT_SIGMAP {
        const wchar_t* cls;
        const int* codes;
        int count;
    };

    const int sig_button[]   = { SIG_CODE(BN_CLICKED) };
    const int sig_edit[]     = { SIG_CODE(EN_CHANGE) };
    const int sig_combo[]    = { SIG_CODE(CBN_SELCHANGE),
                                 SIG_CODE(CBN_EDITCHANGE) };
    const int sig_listbox[]  = { SIG_CODE(LBN_SELCHANGE),
                                 SIG_CODE(LBN_DBLCLK) };
    const int sig_richedit[] = { SIG_CODE(EN_CHANGE), SIG_CODE(EN_SELCHANGE),
                                 SIG_CODE(EN_LINK) };
    const int sig_listview[] = { SIG_CODE(LVN_ITEMCHANGED),
                                 SIG_CODE(LVN_ENDLABELEDIT),
                                 SIG_CODE(LVN_COLUMNCLICK),
                                 SIG_CODE(NM_DBLCLK) };
    const int sig_treeview[] = { SIG_CODE(TVN_SELCHANGED),
                                 SIG_CODE(TVN_ITEMEXPANDED),
                                 SIG_CODE(TVN_ENDLABELEDIT),
                                 SIG_CODE(NM_DBLCLK) };
    const int sig_tab[]      = { SIG_CODE(TCN_SELCHANGE) };
    const int sig_datetime[] = { SIG_CODE(DTN_DATETIMECHANGE) };
    const int sig_monthcal[] = { SIG_CODE(MCN_SELCHANGE),
                                 SIG_CODE(MCN_SELECT) };
    const int sig_updown[]   = { SIG_CODE(UDN_DELTAPOS) };
    const int sig_trackbar[] = { SIG_CODE(TB_THUMBPOSITION),
                                 SIG_CODE(TB_ENDTRACK) };
    const int sig_scrollbar[]= { SIG_CODE(SB_THUMBPOSITION),
                                 SIG_CODE(SB_ENDSCROLL) };
    const int sig_ipaddr[]   = { SIG_CODE(IPN_FIELDCHANGED) };
    const int sig_hotkey[]   = { SIG_CODE(EN_CHANGE) };
    const int sig_status[]   = { SIG_CODE(NM_CLICK), SIG_CODE(NM_DBLCLK) };
    const int sig_header[]   = { SIG_CODE(HDN_ITEMCLICK),
                                 SIG_CODE(HDN_ITEMCHANGED),
                                 SIG_CODE(HDN_ENDTRACK),
                                 SIG_CODE(HDN_ITEMDBLCLICK) };

    // Add a row to support a new control type. The host must reflect the
    // matching command/notify/scroll code as WM_PARASITE_CALLBACK lParam.
    const LAYOUT_SIGMAP signal_map[] = {
        { L"Button",             sig_button,   _countof(sig_button)   },
        { L"Edit",               sig_edit,     _countof(sig_edit)     },
        { L"ComboBox",           sig_combo,    _countof(sig_combo)    },
        { L"ListBox",            sig_listbox,  _countof(sig_listbox)  },
        { L"Static",             nullptr,      0                      },
        { L"RICHEDIT50W",        sig_richedit, _countof(sig_richedit) },
        { L"RichEdit20W",        sig_richedit, _countof(sig_richedit) },
        { L"RichEdit20A",        sig_richedit, _countof(sig_richedit) },
        { L"RICHEDIT",           sig_richedit, _countof(sig_richedit) },
        { L"SysListView32",      sig_listview, _countof(sig_listview) },
        { L"SysTreeView32",      sig_treeview, _countof(sig_treeview) },
        { L"SysTabControl32",    sig_tab,      _countof(sig_tab)      },
        { L"SysDateTimePick32",  sig_datetime, _countof(sig_datetime) },
        { L"SysMonthCal32",      sig_monthcal, _countof(sig_monthcal) },
        { L"msctls_updown32",    sig_updown,   _countof(sig_updown)   },
        { L"msctls_trackbar32",  sig_trackbar, _countof(sig_trackbar) },
        { L"ScrollBar",          sig_scrollbar,_countof(sig_scrollbar)},
        { L"SysIPAddress32",     sig_ipaddr,   _countof(sig_ipaddr)   },
        { L"msctls_hotkey32",    sig_hotkey,   _countof(sig_hotkey)   },
        { L"msctls_progress32",  nullptr,      0                      },
        { L"msctls_statusbar32", sig_status,   _countof(sig_status)   },
        { L"SysHeader32",        sig_header,   _countof(sig_header)   },
    };

#undef SIG_CODE
}

void Parasite::clear_signal_codes() {
    signal_count = 0;
}

bool Parasite::add_signal_code(int code) {
    if (code == -1) return true;
    if (has_signal_code(code)) return true;
    if (signal_count >= MAX_SIGNAL_CODES) return false;
    signal_codes[signal_count++] = code;
    return true;
}

void Parasite::set_signal_code(int code) {
    clear_signal_codes();
    if (code != -1) add_signal_code(code);
}

void Parasite::set_signal_codes(const int* codes, int count) {
    clear_signal_codes();
    if (!codes || count <= 0) return;
    for (int i = 0; i < count; i++) add_signal_code(codes[i]);
}

bool Parasite::has_signal_code(int code) const {
    for (int i = 0; i < signal_count; i++)
        if (signal_codes[i] == code) return true;
    return false;
}

void Parasite::set_default_signal_codes(const wchar_t* cls) {
    clear_signal_codes();
    for (const LAYOUT_SIGMAP& e : signal_map) {
        if (_wcsicmp(cls, e.cls) == 0) {
            set_signal_codes(e.codes, e.count);
            return;
        }
    }
}

bool Parasite::layout(int x0, int y0, int x1, int y1,
                        const char* f, int ln) {
    if (!target || !::IsWindow(target->m_hWnd)) return false;

    last_rect.SetRect(x0, y0, x1, y1);  // 96 DPI logical coords from the Add*/OpenFrame macro
#if defined(_DEBUG)
    file = f;
    line = ln;
#else
    UNREFERENCED_PARAMETER(f);
    UNREFERENCED_PARAMETER(ln);
#endif

    wchar_t cls[64] = {};
    ::GetClassNameW(target->m_hWnd, cls, _countof(cls));
    set_default_signal_codes(cls);

    ::SetWindowSubclass(target->m_hWnd, proc, ID_LAYOUT, (DWORD_PTR)this);
    return true;
}

// ======================================================================
// [PROC]  static subclass proc -> instance dispatch
// ======================================================================
// Helper: HT zone from screen-coord pt. Within 8px of any edge = resize zone.
// Compiled only in _DEBUG (called only from the editing block in dispatch / on_drag).
#if defined(_DEBUG)
static int hit_zone(HWND h, POINT pt) {
    CRect wr; ::GetWindowRect(h, &wr);
    const int M = 8;
    bool nl = pt.x < wr.left   + M,  nr = pt.x > wr.right  - M;
    bool nt = pt.y < wr.top    + M,  nb = pt.y > wr.bottom - M;
    if (nt && nl) return HTTOPLEFT;    if (nt && nr) return HTTOPRIGHT;
    if (nb && nl) return HTBOTTOMLEFT; if (nb && nr) return HTBOTTOMRIGHT;
    if (nl) return HTLEFT;  if (nr) return HTRIGHT;
    if (nt) return HTTOP;   if (nb) return HTBOTTOM;
    return HTCAPTION;
}
static HCURSOR cursor_for_zone(int zone) {
    switch (zone) {
    case HTTOP:  case HTBOTTOM:             return ::LoadCursorW(NULL, (LPCWSTR)IDC_SIZENS);
    case HTLEFT: case HTRIGHT:              return ::LoadCursorW(NULL, (LPCWSTR)IDC_SIZEWE);
    case HTTOPLEFT:  case HTBOTTOMRIGHT:    return ::LoadCursorW(NULL, (LPCWSTR)IDC_SIZENWSE);
    case HTTOPRIGHT: case HTBOTTOMLEFT:     return ::LoadCursorW(NULL, (LPCWSTR)IDC_SIZENESW);
    default:                                return ::LoadCursorW(NULL, (LPCWSTR)IDC_HAND);
    }
}
#endif

LRESULT CALLBACK Parasite::proc(HWND hWnd, UINT msg, WPARAM wParam,
                                        LPARAM lParam, UINT_PTR, DWORD_PTR ref) {
    Parasite* self = reinterpret_cast<Parasite*>(ref);
    return self->dispatch(hWnd, msg, wParam, lParam);
}

LRESULT Parasite::dispatch(HWND h, UINT msg, WPARAM w, LPARAM l) {
    // -- signal protocol (works in both normal and edit mode) --
    switch (msg) {
    case WM_PARASITE_SURVEIL:
        report = reinterpret_cast<CWnd*>(w);   // host pointer, or 0 to clear
        return reinterpret_cast<LRESULT>(target);
    case WM_PARASITE_CALLBACK:
        if (has_signal_code(static_cast<int>(l))) signal();
        return 0;
    case WM_NCDESTROY: {
        ::RemoveWindowSubclass(h, proc, ID_LAYOUT);
#if defined(_DEBUG)
        if (active() == this) active() = nullptr;
        editing = false;
#endif
        report = nullptr;
        target = nullptr;
        return ::DefSubclassProc(h, msg, w, l);
    }
#if defined(_DEBUG)
    case WM_MBUTTONDOWN:                        // toggle edit mode (_DEBUG only)
        toggle_edit(h);
        return 0;
#endif
    }

#if defined(_DEBUG)
    // -- edit-mode-only handling (compiled out in non-debug builds) --
    if (editing) {
        switch (msg) {
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTARROWS;
        case WM_NCHITTEST:
            return HTCLIENT;                    // route mouse as client messages
        case WM_PAINT: {
            LRESULT r = ::DefSubclassProc(h, msg, w, l);
            paint_frame(h);
            return r;
        }
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                POINT pt; ::GetCursorPos(&pt);
                int zone = drag_edge ? drag_edge : hit_zone(h, pt);
                ::SetCursor(cursor_for_zone(zone));
                return TRUE;   // prevent DefSubclassProc from resetting the cursor
            }
            break;
        case WM_LBUTTONDOWN:
            on_drag(h, l);
            return 0;
        case WM_MOUSEMOVE:
            if (drag_edge) {
                CPoint cur(GET_X_LPARAM(l), GET_Y_LPARAM(l));
                ::ClientToScreen(h, &cur);
                int dx = cur.x - drag_start.x;
                int dy = cur.y - drag_start.y;
                CRect nr = drag_rect;
                if (drag_edge == HTCAPTION) {
                    nr.OffsetRect(dx, dy);
                } else {
                    if (drag_edge==HTTOPLEFT ||drag_edge==HTBOTTOMLEFT ||drag_edge==HTLEFT)   nr.left   += dx;
                    if (drag_edge==HTTOPRIGHT||drag_edge==HTBOTTOMRIGHT||drag_edge==HTRIGHT)  nr.right  += dx;
                    if (drag_edge==HTTOPLEFT ||drag_edge==HTTOPRIGHT   ||drag_edge==HTTOP)    nr.top    += dy;
                    if (drag_edge==HTBOTTOMLEFT||drag_edge==HTBOTTOMRIGHT||drag_edge==HTBOTTOM) nr.bottom += dy;
                }
                // nr is in screen coords; convert to parent-client coords for MoveWindow
                HWND par = ::GetAncestor(h, GA_PARENT);
                POINT tl = { nr.left, nr.top }, br = { nr.right, nr.bottom };
                if (par) { ::ScreenToClient(par, &tl); ::ScreenToClient(par, &br); }
                ::MoveWindow(h, tl.x, tl.y, br.x - tl.x, br.y - tl.y, TRUE);
                paint_frame(h);
            }
            return 0;
        case WM_LBUTTONUP:
            if (drag_edge) { ::ReleaseCapture(); drag_edge = 0; }
            return 0;
        case WM_CAPTURECHANGED:
            drag_edge = 0;
            return 0;
        case WM_LBUTTONDBLCLK:
            leave_edit(h, true);               // commit
            return 0;
        case WM_KEYDOWN: {
            UINT vk = static_cast<UINT>(w);
            if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP ||
                vk == VK_DOWN || vk == VK_RETURN)
                on_key(h, vk);
            else if (vk == VK_ESCAPE) {
                leave_edit(h, false);           // cancel without commit
                MSG dummy;
                ::PeekMessageW(&dummy, h, WM_CHAR, WM_CHAR, PM_REMOVE);
            }
            return 0;                           // eat ALL keydowns in edit mode
        }
        case WM_CHAR:
            return 0;
        }
        // Eat all remaining mouse and keyboard messages: in edit mode only
        // move/resize gestures are active; control-native behavior must be silent.
        if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
            (msg >= WM_KEYFIRST   && msg <= WM_KEYLAST))
            return 0;
    }
#endif

    return ::DefSubclassProc(h, msg, w, l);
}

// ======================================================================
// [EDIT]  -- entire section compiled only in _DEBUG builds
// ======================================================================
#if defined(_DEBUG)

void Parasite::toggle_edit(HWND h) {
    if (editing) leave_edit(h, false);        // middle-click exit: no commit
    else          enter_edit(h);
}

void Parasite::enter_edit(HWND h) {
    if (active() && active() != this) {        // drop the previous active one
        Parasite* prev = active();
        if (prev->target) prev->leave_edit(prev->target->m_hWnd, false);
    }
    // Capture current physical position as logical coords for ESC restore.
    CRect phys;
    get_rect(h, phys);
    int tdpi = ::GetDpiForWindow(h);
    if (tdpi <= 0) tdpi = 96;
    last_rect.SetRect(MulDiv(phys.left,   96, tdpi),
                      MulDiv(phys.top,    96, tdpi),
                      MulDiv(phys.right,  96, tdpi),
                      MulDiv(phys.bottom, 96, tdpi));
    editing = true;
    active() = this;
    ::SetFocus(h);
    paint_frame(h);
}

void Parasite::leave_edit(HWND h, bool commit) {
    editing = false;
    if (active() == this) active() = nullptr;

    if (!commit) {
        // Restore pre-edit position: last_rect holds 96 DPI logical coords,
        // convert to physical at current DPI. WS_CHILD vs popup: same rule as get_rect.
        int tdpi = ::GetDpiForWindow(h);
        if (tdpi <= 0) tdpi = 96;
        CRect phys(MulDiv(last_rect.left,   tdpi, 96),
                   MulDiv(last_rect.top,    tdpi, 96),
                   MulDiv(last_rect.right,  tdpi, 96),
                   MulDiv(last_rect.bottom, tdpi, 96));
        bool is_child = (::GetWindowLongW(h, GWL_STYLE) & WS_CHILD) != 0;
        if (is_child)
            ::MoveWindow(h, phys.left, phys.top, phys.Width(), phys.Height(), TRUE);
        else
            ::SetWindowPos(h, NULL, phys.left, phys.top, phys.Width(), phys.Height(),
                           SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (commit && file) {
        CRect phys;
        get_rect(h, phys);
        int tdpi = ::GetDpiForWindow(h);
        if (tdpi <= 0) tdpi = 96;
        // Convert physical to logical for comparison and storage.
        // NOTE: LayOutRewrite still receives physical coords here; Step 4 will
        // change it to pass logical coords so the source file stores 96 DPI values.
        CRect logical(MulDiv(phys.left,   96, tdpi),
                      MulDiv(phys.top,    96, tdpi),
                      MulDiv(phys.right,  96, tdpi),
                      MulDiv(phys.bottom, 96, tdpi));
        if (logical != last_rect) {
            LayOutRewrite(file, line, phys);
            last_rect = logical;
        }
    }

    HWND parent = ::GetParent(h);              // erase the red frame
    if (parent) ::InvalidateRect(parent, NULL, TRUE);
    ::InvalidateRect(h, NULL, TRUE);
}

void Parasite::on_key(HWND h, UINT vk) {
    if (vk == VK_RETURN) {
        leave_edit(h, true);
        // TranslateMessage already posted WM_CHAR '\r' before DispatchMessage ran this
        // subclass proc. Discard it so the commit Enter does not leak to the child window.
        MSG dummy;
        ::PeekMessageW(&dummy, h, WM_CHAR, WM_CHAR, PM_REMOVE);
        return;
    }

    bool shift = (::GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    bool ctrl  = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;

    CRect rc;
    get_rect(h, rc);
    int cx = rc.Width(), cy = rc.Height(), d;

    switch (vk) {
    case VK_RIGHT:
        if (shift) { if (ctrl) rc.right += 1;
                     else { d = 5 - (cx % 5); rc.right += d; } }
        else       { if (ctrl) { rc.left += 1; rc.right += 1; }
                     else { d = 5 - (rc.left % 5); rc.left += d; rc.right += d; } }
        break;
    case VK_LEFT:
        if (ctrl) { if (shift) rc.right -= 1;
                    else { rc.left -= 1; rc.right -= 1; } }
        else      { if (shift) { d = cx % 5; if (!d) d = 5; rc.right -= d; }
                    else { d = rc.left % 5; if (!d) d = 5; rc.left -= d; rc.right -= d; } }
        break;
    case VK_UP:
        if (ctrl) { if (shift) rc.bottom -= 1;
                    else { rc.top -= 1; rc.bottom -= 1; } }
        else      { if (shift) { d = cy % 5; if (!d) d = 5; rc.bottom -= d; }
                    else { d = rc.top % 5; if (!d) d = 5; rc.top -= d; rc.bottom -= d; } }
        break;
    case VK_DOWN:
        if (shift) { if (ctrl) rc.bottom += 1;
                     else { d = 5 - (cy % 5); rc.bottom += d; } }
        else       { if (ctrl) { rc.top += 1; rc.bottom += 1; }
                     else { d = 5 - (rc.top % 5); rc.top += d; rc.bottom += d; } }
        break;
    default:
        return;
    }

    ::MoveWindow(h, rc.left, rc.top, rc.Width(), rc.Height(), TRUE);
    paint_frame(h);
}

void Parasite::on_drag(HWND h, LPARAM l) {
    bool is_child = (::GetWindowLongW(h, GWL_STYLE) & WS_CHILD) != 0;

    CPoint mouse(GET_X_LPARAM(l), GET_Y_LPARAM(l));
    ::ClientToScreen(h, &mouse);

    if (!is_child) {
        // Top-level: pass the hit zone to the OS; it handles move/resize cursor and loop.
        // LPARAM must be screen coords (mouse is already screen-converted above).
        ::PostMessage(h, WM_NCLBUTTONDOWN, hit_zone(h, mouse), MAKELPARAM(mouse.x, mouse.y));
        return;
    }

    // Child window: WM_NCLBUTTONDOWN is ignored without WS_CAPTION.
    // Use SetCapture + WM_MOUSEMOVE tracking.
    drag_start = mouse;
    ::GetWindowRect(h, &drag_rect);
    drag_edge = hit_zone(h, mouse);
    ::SetCapture(h);
}

void Parasite::paint_frame(HWND h) {
    CWnd* w = CWnd::FromHandle(h);
    CClientDC dc(w);
    CRect rc;
    w->GetClientRect(&rc);
    CBrush red;
    red.CreateSolidBrush(RGB(255, 0, 0));
    dc.FrameRect(&rc, &red);
}

void Parasite::get_rect(HWND h, CRect& out) {
    ::GetWindowRect(h, &out);                  // screen coords
    // Convert to parent-client coords ONLY for real children (WS_CHILD). A
    // WS_POPUP frame's GetParent() returns its OWNER, so testing GetParent here
    // would wrongly map it to owner-client coords; top-level and popup frames
    // must stay in screen coords (that is how they are created/moved).
    bool is_child = (::GetWindowLongW(h, GWL_STYLE) & WS_CHILD) != 0;
    HWND parent = ::GetParent(h);
    if (is_child && parent)
        ::MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&out), 2);
}

#endif // _DEBUG

// ======================================================================
// [SIGNAL]
// ======================================================================

void Parasite::signal() {
    if (report && ::IsWindow(report->m_hWnd))
        ::PostMessage(report->m_hWnd, WM_PARASITE_REPORT,
                      reinterpret_cast<WPARAM>(target), 0);
}

// ======================================================================
// [REWRITE]  -- entire section compiled only in _DEBUG builds
// ======================================================================
#if defined(_DEBUG)

// Find the first occurrence of ch at top level (outside (), [] and "..."),
// scanning from line. Returns NULL if not found.
static const char* find_char(int ch, const char* line) {
    bool quote = false, esc = false;
    int paren = 0, bracket = 0;

    for (const char* s = line; *s != 0; s++) {
        if (quote) {
            if (esc)            esc = false;
            else if (*s == '\\') esc = true;
            else if (*s == '"')  quote = false;
            continue;
        }
        if (*s == ch && paren == 0 && bracket == 0) return s;

        if (*s == '"')      quote = true;
        else if (*s == '(') paren++;
        else if (*s == ')') paren--;
        else if (*s == '[') bracket++;
        else if (*s == ']') bracket--;
    }
    return 0;
}

static void rewrite_error(const char* msg, const char* file, int line) {
    wchar_t buff[512];
    swprintf_s(buff, _countof(buff), L"%S\n file: %S\n line: %d", msg, file, line);
    ::MessageBoxW(nullptr, buff, L"Parasite rewrite error", MB_OK | MB_ICONERROR);
}

bool LayOutRewrite(const char* file, int line, const RECT& rect) {
    // 1) binary backup file -> file.bak, rejecting UTF-16 (BOM) sources.
    char bak[512];
    sprintf_s(bak, sizeof(bak), "%s.bak", file);

    FILE* src = 0;
    FILE* dst = 0;
    if (fopen_s(&src, file, "rb") != 0) {
        rewrite_error("Could not open source file.", file, line);
        return false;
    }
    int first = getc(src);
    if (first == 0xFF || first == 0xFE) {       // UTF-16 LE/BE BOM
        fclose(src);
        rewrite_error("Unicode (UTF-16) source is not supported.", file, line);
        return false;
    }
    if (fopen_s(&dst, bak, "wb") != 0) {
        fclose(src);
        rewrite_error("Could not create backup file.", file, line);
        return false;
    }
    for (int ch = first; ch != EOF; ch = getc(src)) putc(ch, dst);
    fclose(src);
    fclose(dst);

    // 2) read backup (text), write source (text), rewriting the target line.
    if (fopen_s(&src, bak, "rt") != 0) return false;
    if (fopen_s(&dst, file, "wt") != 0) { fclose(src); return false; }

    char buff[1024];
    bool ok = true;
    for (int n = 1; fgets(buff, sizeof(buff), src) != 0; n++) {
        if (n != line) { fputs(buff, dst); continue; }

        // Determine the layout-call form on this line; the form fixes where the
        // coordinates start. Both accepted forms are member calls (.Xxx / ->Xxx
        // where Xxx starts uppercase):
        //   obj.OpenFrame(p, x0,y0,x1,y1)   -> coords start at the 2nd argument
        //                                      (1st arg is the app/owner pointer)
        //   obj.AddXxx(x0,y0,x1,y1[, extra]) -> coords start at the 1st argument
        //                                      (arg1 is an integer literal)
        // lp = the call's '(' ; coord_anchor = the separator just BEFORE x0
        // (lp for AddXxx, or the 1st top-level comma for OpenFrame).
        const char* lp = nullptr;
        const char* coord_anchor = nullptr;

        for (const char* p = buff; *p; p++) {
            const char* id = (p[0] == '-' && p[1] == '>') ? p + 2
                           : (p[0] == '.')                ? p + 1 : nullptr;
            if (!id || !(*id >= 'A' && *id <= 'Z')) continue;
            const char* q = id;
            while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') ||
                   (*q >= '0' && *q <= '9') || *q == '_') q++;
            bool is_open = (q - id == 9) && strncmp(id, "OpenFrame", 9) == 0;
            while (*q == ' ' || *q == '\t') q++;
            if (*q != '(') continue;
            if (is_open) {                                         // skip the pointer arg
                lp = q; coord_anchor = find_char(',', lp + 1); break;
            }
            const char* a = q + 1;
            while (*a == ' ' || *a == '\t') a++;
            if (*a == '-' || (*a >= '0' && *a <= '9')) { lp = q; coord_anchor = q; break; }
        }

        // From coord_anchor, step past x0,y0,x1 (3 top-level commas); rp is the
        // closing paren. e4 is the comma before any trailing arg (init / wnd).
        const char* e1 = coord_anchor ? find_char(',', coord_anchor + 1) : nullptr;
        const char* e2 = e1 ? find_char(',', e1 + 1) : nullptr;
        const char* e3 = e2 ? find_char(',', e2 + 1) : nullptr;   // comma after x1
        const char* e4 = e3 ? find_char(',', e3 + 1) : nullptr;   // comma before a trailing arg
        const char* rp = lp ? find_char(')', lp + 1) : nullptr;
        const char* tail = (e4 && rp && e4 < rp) ? e4 : rp;       // ", extra)" or just ")"

        if (coord_anchor && e3 && rp && e3 < rp) {
            const char* pre = coord_anchor + 1;
            while (*pre == ' ' || *pre == '\t') pre++;            // first coord char
            const char* s;
            for (s = buff; s < pre; s++) putc(*s, dst);           // copy leading text verbatim
            fprintf(dst, "%d, %d, %d, %d",
                    (int)rect.left, (int)rect.top, (int)rect.right, (int)rect.bottom);
            for (s = tail; *s != 0; s++) putc(*s, dst);
        }
        else {
            rewrite_error("Could not parse the layout call on this line.", file, line);
            fputs(buff, dst);
            ok = false;
        }
    }
    fclose(src);
    fclose(dst);
    return ok;
}

#endif // _DEBUG

// ======================================================================
// [HOST]   FrameBox -- CWnd host: per-frame registry + surveil/listen/report
// ======================================================================

BEGIN_MESSAGE_MAP(FrameBox, CWnd)
END_MESSAGE_MAP()

static const UINT_PTR MODAL_TIMER_ID = 1;

FrameBox::FrameBox()
    : next_id(1001), app(nullptr), parent(nullptr), self_layout(nullptr),
      waiting(false), timer_fired(false), event(nullptr), timer_id(0),
      dpi(96), sys_font(nullptr) {
}

FrameBox::~FrameBox() { close(); }

// Bind as the app main window. Cleared in close() before any teardown so a
// later CWnd destruction can never leave a dangling m_pMainWnd.
void FrameBox::attach(CWinApp* a) {
    app = a;
    if (a) a->m_pMainWnd = this;
}

// Recursive teardown: children first (reverse registration order), then the
// self-edit subclass, then this window. A child-frame entry's `delete e.wnd`
// re-enters ~FrameBox to tear down grandchildren. Idempotent.
void FrameBox::close() {
    if (app) { app->m_pMainWnd = nullptr; app = nullptr; }
    for (int i = static_cast<int>(registry.size()) - 1; i >= 0; i--) {
        ChildEntry& e = registry[static_cast<size_t>(i)];
        delete e.layout;                 // removes subclass (skips if HWND already gone)
        if (e.wnd) {                     // owned: borrowed (add_asitis) entries have wnd==nullptr
            if (e.wnd->m_hWnd && ::IsWindow(e.wnd->m_hWnd))
                e.wnd->DestroyWindow();
            delete e.wnd;
        }
    }
    registry.clear();
    registry.shrink_to_fit();            // release buffer so the CRT leak checker stays silent
    if (sys_font) { ::DeleteObject(sys_font); sys_font = nullptr; }
    delete self_layout;
    self_layout = nullptr;
    // Re-enable AND reactivate the owner BEFORE destroying this window (modal
    // sub-dialog case). EnableWindow(TRUE) alone only restores input -- it does
    // not change activation/Z-order, so destroying the still-active owned popup
    // would let the system hand activation to whatever sits below it, dropping
    // the owner one step down. SetActiveWindow() makes the owner active first so
    // its Z-order is preserved. nullptr for main window / zone frames.
    if (parent) {
        if (::IsWindow(parent->m_hWnd)) {
            parent->EnableWindow(TRUE);
            parent->SetActiveWindow();
        }
        parent = nullptr;
    }
    if (::IsWindow(m_hWnd)) DestroyWindow();
}

// Register the shared "FrameBox" window class once, then create the window via
// the AfxHookWindowCreate path (full MFC message-map support, no SubclassWindow
// side effects). Background is painted in WindowProc (WM_ERASEBKGND).
bool FrameBox::create_window(DWORD exStyle, DWORD style, CWnd* parent, const CRect& rc) {
    WNDCLASSEXW wc = {};
    if (!::GetClassInfoExW(AfxGetInstanceHandle(), L"FrameBox", &wc)) {
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS; // DBLCLKS: edit-mode commit
        wc.lpfnWndProc   = ::DefWindowProcW;
        wc.hInstance     = AfxGetInstanceHandle();
        wc.hCursor       = ::LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = L"FrameBox";
        ::RegisterClassExW(&wc);
    }
    AfxHookWindowCreate(this);
    HWND hwnd = ::CreateWindowExW(exStyle, L"FrameBox", L"", style,
        rc.left, rc.top, rc.Width(), rc.Height(),
        parent ? parent->GetSafeHwnd() : nullptr, nullptr, AfxGetInstanceHandle(), nullptr);
    if (!AfxUnhookWindowCreate())
        PostNcDestroy();
    return hwnd != nullptr;
}

// open() public overloads -- selected by the first argument type. OpenFrame
// injects file/line at the call site. Both delegate to open_core() for the
// shared create/position/self-edit work; the type-specific extras (app binding
// vs owner-disable) wrap it.
bool FrameBox::open(CWinApp* a, int x0, int y0, int x1, int y1, const char* file, int line) {
    attach(a);                                    // main window: bind as m_pMainWnd
    return open_core(nullptr, x0, y0, x1, y1, file, line);   // top-level
}

bool FrameBox::open(CWnd* p, int x0, int y0, int x1, int y1, const char* file, int line) {
    bool ok = open_core(p, x0, y0, x1, y1, file, line);      // owned popup
    if (p && ::IsWindow(m_hWnd)) p->EnableWindow(FALSE);     // modal: disable owner
    return ok;
}

// Shared core: create the window, position it, and attach a self-editing Parasite
// so the frame's own rect is live-editable + source-rewritable. owner==nullptr ->
// a top-level window; owner!=nullptr -> an owned popup (modal sub-dialog), stored
// for close() to re-enable.
bool FrameBox::open_core(CWnd* p, int x0, int y0, int x1, int y1, const char* file, int line) {
    parent = p;
    DWORD style = p ? (WS_POPUP | WS_CAPTION | WS_SYSMENU) : WS_OVERLAPPEDWINDOW;
    if (!::IsWindow(m_hWnd)) {
        // Create at logical coords (source-code 96 DPI values), then query actual
        // DPI and scale to physical pixels before ShowWindow.
        // Determine target-monitor DPI from the center of the intended rect BEFORE
        // creating the window. GetDpiForWindow right after creation can return the
        // owner's DPI for a popup whose owner is on a different-DPI monitor;
        // MonitorFromPoint is always correct regardless of ownership.
        {
            POINT center = { (x0 + x1) / 2, (y0 + y1) / 2 };
            HMONITOR mon = ::MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
            UINT mdpi = 96, dummy = 0;
            ::GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &mdpi, &dummy);
            dpi = (int)mdpi;
        }
        if (!create_window(0, style, p, CRect(x0, y0, x1, y1)))
            return false;
        if (dpi != 96) {
            ::SetWindowPos(m_hWnd, nullptr,
                MulDiv(x0, dpi, 96), MulDiv(y0, dpi, 96),
                MulDiv(x1 - x0, dpi, 96), MulDiv(y1 - y0, dpi, 96),
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (sys_font) { ::DeleteObject(sys_font); }
        sys_font = make_dpi_font(dpi);
    } else {
        // Idempotent second call: reposition using current dpi.
        ::SetWindowPos(m_hWnd, nullptr,
            MulDiv(x0, dpi, 96), MulDiv(y0, dpi, 96),
            MulDiv(x1 - x0, dpi, 96), MulDiv(y1 - y0, dpi, 96),
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (!self_layout) {
        self_layout = new Parasite;
        self_layout->attach(this);
        self_layout->layout(x0, y0, x1, y1, file, line);  // stores logical last_rect
    }
    ShowWindow(SW_SHOW);
    return ::IsWindow(m_hWnd) != 0;
}

// ---- child-control factories ----------------------------------------------
// finish_child: apply the UI font, attach a Parasite, register { wnd, Parasite }
// in this frame's registry, then position + initialize. Mirrors the old
// finish_new_window but targets the per-instance registry.
template<class T>
T* FrameBox::finish_child(T* wnd, BOOL ok, int x0, int y0, int x1, int y1,
                          const char* f, int ln, const char* init) {
    ASSERT(ok);
    if (!ok) { delete wnd; return nullptr; }
    HFONT font = sys_font ? sys_font : default_ui_font();
    ::SendMessageW(wnd->m_hWnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
    Parasite* p = new Parasite;
    registry.push_back({ wnd, p });
    p->attach(wnd);
    p->layout(x0, y0, x1, y1, f, ln);  // stores logical last_rect
    p->initialize(init);
    // Scale from logical (96 DPI source coords) to physical pixels.
    if (dpi != 96) {
        wnd->MoveWindow(MulDiv(x0, dpi, 96), MulDiv(y0, dpi, 96),
                        MulDiv(x1 - x0, dpi, 96), MulDiv(y1 - y0, dpi, 96), FALSE);
    }
    return wnd;
}

CButton* FrameBox::add_button(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init) {
    CButton* w = new CButton;
    BOOL ok = w->Create(nullptr, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                        CRect(x0,y0,x1,y1), this, alloc_id());
    return finish_child(w, ok, x0,y0,x1,y1, f,ln, init);
}

CEdit* FrameBox::add_edit(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init) {
    CEdit* w = new CEdit;
    // ES_MULTILINE: enables SetRect() for vertical alignment (EM_SETRECT is ignored
    // on single-line edits). ES_WANTRETURN not set so Enter still routes to the button.
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_TABSTOP,
                        CRect(x0,y0,x1,y1), this, alloc_id());
    return finish_child(w, ok, x0,y0,x1,y1, f,ln, init);
}

CStatic* FrameBox::add_static(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init) {
    CStatic* w = new CStatic;
    BOOL ok = w->Create(nullptr, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY,
                        CRect(x0,y0,x1,y1), this, alloc_id());
    return finish_child(w, ok, x0,y0,x1,y1, f,ln, init);
}

CComboBox* FrameBox::add_combo(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init) {
    CComboBox* w = new CComboBox;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                        CRect(x0,y0,x1,y1), this, alloc_id());
    return finish_child(w, ok, x0,y0,x1,y1, f,ln, init);
}

CListBox* FrameBox::add_list(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init) {
    CListBox* w = new CListBox;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP,
                        CRect(x0,y0,x1,y1), this, alloc_id());
    return finish_child(w, ok, x0,y0,x1,y1, f,ln, init);
}

CRichEditCtrl* FrameBox::add_richedit(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init) {
    AfxInitRichEdit2();
    CRichEditCtrl* w = new CRichEditCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL |
                        ES_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP,
                        CRect(x0,y0,x1,y1), this, alloc_id());
    return finish_child(w, ok, x0,y0,x1,y1, f,ln, init);
}

CListCtrl* FrameBox::add_listctrl(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init) {
    CListCtrl* w = new CListCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | WS_TABSTOP,
                        CRect(x0,y0,x1,y1), this, alloc_id());
    return finish_child(w, ok, x0,y0,x1,y1, f,ln, init);
}

CTreeCtrl* FrameBox::add_treectrl(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init) {
    CTreeCtrl* w = new CTreeCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | WS_BORDER | TVS_HASBUTTONS | TVS_HASLINES |
                        TVS_LINESATROOT | WS_TABSTOP, CRect(x0,y0,x1,y1), this, alloc_id());
    return finish_child(w, ok, x0,y0,x1,y1, f,ln, init);
}

CTabCtrl* FrameBox::add_tabctrl(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init) {
    CTabCtrl* w = new CTabCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | TCS_TABS | WS_TABSTOP,
                        CRect(x0,y0,x1,y1), this, alloc_id());
    return finish_child(w, ok, x0,y0,x1,y1, f,ln, init);
}

CDateTimeCtrl* FrameBox::add_datetime(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init) {
    CDateTimeCtrl* w = new CDateTimeCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | DTS_SHORTDATEFORMAT | WS_TABSTOP,
                        CRect(x0,y0,x1,y1), this, alloc_id());
    return finish_child(w, ok, x0,y0,x1,y1, f,ln, init);
}

CMonthCalCtrl* FrameBox::add_monthcal(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init) {
    CMonthCalCtrl* w = new CMonthCalCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP,
                        CRect(x0,y0,x1,y1), this, alloc_id());
    return finish_child(w, ok, x0,y0,x1,y1, f,ln, init);
}

CSpinButtonCtrl* FrameBox::add_spin(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init) {
    CSpinButtonCtrl* w = new CSpinButtonCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | UDS_ARROWKEYS | UDS_SETBUDDYINT | WS_TABSTOP,
                        CRect(x0,y0,x1,y1), this, alloc_id());
    return finish_child(w, ok, x0,y0,x1,y1, f,ln, init);
}

CSliderCtrl* FrameBox::add_slider(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init) {
    CSliderCtrl* w = new CSliderCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | WS_TABSTOP,
                        CRect(x0,y0,x1,y1), this, alloc_id());
    return finish_child(w, ok, x0,y0,x1,y1, f,ln, init);
}

CScrollBar* FrameBox::add_scrollbar(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init) {
    CScrollBar* w = new CScrollBar;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | SBS_HORZ, CRect(x0,y0,x1,y1), this, alloc_id());
    return finish_child(w, ok, x0,y0,x1,y1, f,ln, init);
}

CIPAddressCtrl* FrameBox::add_ipaddress(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init) {
    CIPAddressCtrl* w = new CIPAddressCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP,
                        CRect(x0,y0,x1,y1), this, alloc_id());
    return finish_child(w, ok, x0,y0,x1,y1, f,ln, init);
}

CHotKeyCtrl* FrameBox::add_hotkey(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init) {
    CHotKeyCtrl* w = new CHotKeyCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP,
                        CRect(x0,y0,x1,y1), this, alloc_id());
    return finish_child(w, ok, x0,y0,x1,y1, f,ln, init);
}

CProgressCtrl* FrameBox::add_progress(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init) {
    CProgressCtrl* w = new CProgressCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | PBS_SMOOTH, CRect(x0,y0,x1,y1), this, alloc_id());
    return finish_child(w, ok, x0,y0,x1,y1, f,ln, init);
}

CStatusBarCtrl* FrameBox::add_status(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init) {
    CStatusBarCtrl* w = new CStatusBarCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, CRect(x0,y0,x1,y1), this, alloc_id());
    return finish_child(w, ok, x0,y0,x1,y1, f,ln, init);
}

CHeaderCtrl* FrameBox::add_header(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init) {
    CHeaderCtrl* w = new CHeaderCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | HDS_BUTTONS | HDS_HORZ,
                        CRect(x0,y0,x1,y1), this, alloc_id());
    return finish_child(w, ok, x0,y0,x1,y1, f,ln, init);
}

// ---- container child frames ------------------------------------------------
// Create a child FrameBox, attach a Parasite for live editing, and register
// { child, layout } so close() destroys it recursively.
FrameBox* FrameBox::make_child(DWORD exStyle, DWORD style,
                               int x0,int y0,int x1,int y1,const char* f,int ln) {
    FrameBox* child = new FrameBox;
    // Create at physical pixels (parent DPI; child zones are on the same monitor).
    CRect phys(MulDiv(x0,dpi,96), MulDiv(y0,dpi,96),
               MulDiv(x1,dpi,96), MulDiv(y1,dpi,96));
    if (!child->create_window(exStyle, style, this, phys)) {
        delete child;
        return nullptr;
    }
    child->dpi = dpi;
    child->sys_font = make_dpi_font(dpi);
    Parasite* p = new Parasite;
    registry.push_back({ child, p });
    p->attach(child);
    p->layout(x0, y0, x1, y1, f, ln);  // stores logical last_rect
    child->ShowWindow(SW_SHOW);
    return child;
}

FrameBox* FrameBox::add_zone(int x0,int y0,int x1,int y1,const char* f,int ln) {
    // WS_EX_CONTROLPARENT lets Tab navigate the zone's child controls.
    return make_child(WS_EX_CONTROLPARENT,
                      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                      x0,y0,x1,y1, f,ln);
}

FrameBox* FrameBox::add_frame(int x0,int y0,int x1,int y1,const char* f,int ln) {
    // Separate top-level-style window owned by this frame. For a true-modal use,
    // the caller disables this frame (EnableWindow(this,FALSE)) around the child's listen().
    return make_child(0,
                      WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                      x0,y0,x1,y1, f,ln);
}

// ---- external already-created window attach --------------------------------
CWnd* FrameBox::attach_external(CWnd* wnd, bool owned,
                                int x0,int y0,int x1,int y1,const char* f,int ln) {
    Parasite* p = new Parasite;
    registry.push_back({ owned ? wnd : nullptr, p });
    p->attach(wnd);
    if (x0 == 0 && y0 == 0 && x1 == 0 && y1 == 0) {
        // Attach-only: read current physical rect and convert to logical for last_rect.
        CRect rc;
        wnd->GetWindowRect(&rc);
        CWnd* par = wnd->GetParent();
        if (par) par->ScreenToClient(&rc);
        p->layout(MulDiv(rc.left,   96, dpi),
                  MulDiv(rc.top,    96, dpi),
                  MulDiv(rc.right,  96, dpi),
                  MulDiv(rc.bottom, 96, dpi), f, ln);
    } else {
        p->layout(x0, y0, x1, y1, f, ln);  // stores logical last_rect
        wnd->MoveWindow(MulDiv(x0, dpi, 96), MulDiv(y0, dpi, 96),
                        MulDiv(x1 - x0, dpi, 96), MulDiv(y1 - y0, dpi, 96));
    }
    wnd->ShowWindow(SW_SHOW);
    return wnd;
}

CWnd* FrameBox::add_new(int x0,int y0,int x1,int y1,const char* f,int ln, CWnd* wnd) {
    return attach_external(wnd, true, x0,y0,x1,y1, f,ln);
}

CWnd* FrameBox::add_asitis(int x0,int y0,int x1,int y1,const char* f,int ln, CWnd* wnd) {
    return attach_external(wnd, false, x0,y0,x1,y1, f,ln);
}

// ---- listen / surveil / message routing ------------------------------------
int FrameBox::timer(int period) {
    if (timer_id) { KillTimer(timer_id); timer_id = 0; }
    if (period <= 0) return 0;
    timer_id = SetTimer(MODAL_TIMER_ID, static_cast<UINT>(period), nullptr);
    if (timer_id == 0) return static_cast<int>(::GetLastError());
    return 0;
}

CWnd* FrameBox::listen_core(int count, CWnd** list) {
    if (!::IsWindow(m_hWnd)) return nullptr;

    event = nullptr;
    timer_fired = false;
    surveil(count, list, true);
    waiting = true;

    RunModalLoop();          // pumps messages until WF_CONTINUEMODAL is cleared

    waiting = false;
    surveil(count, list, false);
    if (timer_fired) return this;
    return event;
}

void FrameBox::surveil(int count, CWnd** list, bool on) {
    // Token = this frame, regardless of where the control physically lives. A
    // control reports to whoever surveilled it, so a parent can listen() controls
    // that live inside a child zone with no relay.
    WPARAM token = on ? reinterpret_cast<WPARAM>(this) : 0;
    for (int i = 0; i < count; i++) {
        if (list[i] && ::IsWindow(list[i]->m_hWnd))
            ::SendMessage(list[i]->m_hWnd, WM_PARASITE_SURVEIL, token, 0);
    }
}

LRESULT FrameBox::WindowProc(UINT msg, WPARAM wParam, LPARAM lParam) {
    // Paint background with the dialog face color instead of the CWnd default.
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        ::GetClientRect(m_hWnd, &rc);
        ::FillRect(reinterpret_cast<HDC>(wParam), &rc,
                   ::GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }

    // Timer fired: break out of the modal loop and return this from listen().
    if (msg == WM_TIMER && wParam == MODAL_TIMER_ID) {
        if (waiting) {
            timer_fired = true;
            m_nFlags &= ~WF_CONTINUEMODAL;
        }
        return 0;
    }

    // a surveilled control signaled: end the modal loop, remember which.
    if (waiting && msg == WM_PARASITE_REPORT) {
        event = reinterpret_cast<CWnd*>(wParam);
        m_nFlags &= ~WF_CONTINUEMODAL;
        return 0;
    }

    // Reflect control notifications to the control HWND (HWND-based, so the
    // subclass proc receives them). Runs regardless of listen() state, so a child
    // frame reflects its own controls even while a parent frame is in listen().
    // lParam carries the full signed code for WM_NOTIFY / scroll request code.
    if (msg == WM_COMMAND && lParam != 0) {
        HWND ctrl = reinterpret_cast<HWND>(lParam);
        if (::IsWindow(ctrl))
            ::SendMessage(ctrl, WM_PARASITE_CALLBACK, 0,
                          static_cast<LPARAM>(static_cast<int>(HIWORD(wParam))));
        return 0;
    }
    if (msg == WM_NOTIFY) {
        NMHDR* nm = reinterpret_cast<NMHDR*>(lParam);
        if (nm && ::IsWindow(nm->hwndFrom))
            ::SendMessage(nm->hwndFrom, WM_PARASITE_CALLBACK, 0,
                          static_cast<LPARAM>(nm->code));
        return 0;
    }
    if ((msg == WM_HSCROLL || msg == WM_VSCROLL) && lParam != 0) {
        HWND ctrl = reinterpret_cast<HWND>(lParam);
        if (::IsWindow(ctrl))
            ::SendMessage(ctrl, WM_PARASITE_CALLBACK, 0,
                          static_cast<LPARAM>(static_cast<int>(LOWORD(wParam))));
        return 0;
    }

    // window going away while in a modal listen: break out with no event.
    if (msg == WM_DESTROY) {
        event = nullptr;
        m_nFlags &= ~WF_CONTINUEMODAL;
    }

    return CWnd::WindowProc(msg, wParam, lParam);
}

// ESC and Enter are consumed in BOTH normal and edit mode (CDialog-like behaviour).
// TRAP: the editing_hwnd() guard does NOT restrict this to edit mode -- it only yields
// to the Parasite subclass proc when a control is actively being repositioned (so Enter
// commits and ESC cancels the drag). In normal mode both keys are still always consumed,
// EXCEPT when the focused window returns DLGC_WANTALLKEYS (e.g. cConBox terminal widget).
BOOL FrameBox::PreTranslateMessage(MSG* pMsg) {
    // While a window is in layout-edit mode, the edit gestures must stay with the
    // editing window and the OTHER controls must be inert. Messages destined for a
    // child walk up to this frame's PreTranslateMessage, so handle them here.
    HWND edit = Parasite::editing_hwnd();        // NULL in release builds
    if (edit) {
        UINT m = pMsg->message;
        // The editing window's own keydowns: yield to its subclass proc (it does
        // the move/resize and eats the rest; commit on Enter, cancel on Esc).
        if (m == WM_KEYDOWN && pMsg->hwnd == edit) return FALSE;
        // Edit keys while a different control holds focus: route to the editor and
        // consume, so the focused child does not move its caret / change selection.
        if (m == WM_KEYDOWN) {
            UINT vk = static_cast<UINT>(pMsg->wParam);
            if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
                vk == VK_RETURN || vk == VK_ESCAPE) {
                ::SendMessageW(edit, WM_KEYDOWN, vk, pMsg->lParam);
                return TRUE;
            }
        }
        // Keep non-editing controls inert: swallow their mouse input so they do
        // not hover-highlight or steal focus. WM_MBUTTONDOWN passes through so the
        // user can still toggle edit on another control (or off).
        if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST &&
            m != WM_MBUTTONDOWN && pMsg->hwnd != edit)
            return TRUE;
    }

    if (pMsg->message == WM_KEYDOWN) {
        // If the focused window claims all keys (e.g. ConBox terminal), let it handle
        // Esc/Enter directly instead of consuming them here.
        CWnd* focus = GetFocus();
        if (focus && (focus->SendMessage(WM_GETDLGCODE) & DLGC_WANTALLKEYS))
            return CWnd::PreTranslateMessage(pMsg);
        if (pMsg->wParam == VK_ESCAPE) {
            PostMessage(WM_CLOSE);
            return TRUE;
        }
        if (pMsg->wParam == VK_RETURN) {
            if (focus) {
                wchar_t cls[32] = {};
                ::GetClassNameW(focus->m_hWnd, cls, _countof(cls));
                // Only a multiline edit WITH ES_WANTRETURN claims Enter for itself
                // (newline). Without it, Enter routes to the button.
                if (_wcsicmp(cls, L"Edit") == 0 &&
                    (focus->GetStyle() & ES_MULTILINE) &&
                    (focus->GetStyle() & ES_WANTRETURN))
                    return CWnd::PreTranslateMessage(pMsg);
                // Push button: simulate click.
                DWORD btype = focus->GetStyle() & 0x0F;
                if (btype == BS_PUSHBUTTON || btype == BS_DEFPUSHBUTTON)
                    focus->SendMessage(BM_CLICK);
            }
            return TRUE;  // always consume Enter so it never becomes WM_CHAR '\r'
        }
    }
    return CWnd::PreTranslateMessage(pMsg);
}
