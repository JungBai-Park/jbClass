/*
    LayOut.cpp - cLayOut implementation.

    See LayOut.h for the public contract, ownership rules and the host
    protocol. This file is the subclass procedure plus the live
    layout editor and the source-code rewriter.

    SECTION INDEX (grep these)
    --------------------------
    [LIFETIME]   ctor / dtor / attach / open / initialize / active()
    [FACTORY]    cLayOutZone class + LayOut* hidden-control factories + LayOutAsItIs/LayOutNew + DeleteLayOutWindows
    [UTIL]       AlignText
    [DETECT]     signal_map + set_default_signal_codes
    [PROC]       proc (static) -> dispatch (instance message router)
    [EDIT]       toggle/enter/leave edit, on_key, on_drag, paint_frame, get_rect
    [SIGNAL]     signal()  (post WM_EVOL_REPORT)
    [REWRITE]    find_char + LayOutRewrite  (rewrite LayOut(...) literals)
    [HOST]       cModalFrame: ctor, PreTranslateMessage (ESC/Enter), timer,
                 open, WindowProc, surveil, listen_core, listen overloads

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

#include <vector>
#include <string>
#include "LayOut.h"

// ======================================================================
// [LIFETIME]
// ======================================================================

cLayOut::cLayOut()
    : target(nullptr), signal_count(0), report(nullptr)
#if defined(_DEBUG)
    , file(nullptr), line(0), editing(false), drag_edge(0)
#endif
{
#if defined(_DEBUG)
    last_rect.SetRectEmpty();
#endif
}

cLayOut::~cLayOut() {
#if defined(_DEBUG)
    if (active() == this) active() = nullptr;
#endif
    if (target && ::IsWindow(target->m_hWnd))
        ::RemoveWindowSubclass(target->m_hWnd, proc, ID_LAYOUT);
    target = nullptr;
}

void cLayOut::attach(CWnd* t) {
    target = t;
}

#if defined(_DEBUG)
cLayOut*& cLayOut::active() {
    static cLayOut* p = nullptr;
    return p;
}

HWND cLayOut::editing_hwnd() {
    cLayOut* p = active();
    return (p && p->target) ? p->target->m_hWnd : NULL;
}
#endif

// ======================================================================
// [FACTORY]  hidden-control factories owned by an internal registry
// ======================================================================

namespace {
    const int NEW_WINDOW_FIRST_ID = 1001;

    struct NewWindowEntry { CWnd* wnd; cLayOut* layout; };

    std::vector<NewWindowEntry>& new_window_registry() {
        static std::vector<NewWindowEntry> list;
        return list;
    }

    int& next_new_window_id() {
        static int id = NEW_WINDOW_FIRST_ID;
        return id;
    }

    int allocate_new_window_id() {
        return next_new_window_id()++;
    }

    struct NEW_WINDOW_CLEANUP_GUARD {
        ~NEW_WINDOW_CLEANUP_GUARD() { DeleteLayOutWindows(); }
    };

    void ensure_cleanup_guard() {
        static NEW_WINDOW_CLEANUP_GUARD guard;
        UNREFERENCED_PARAMETER(guard);
    }

    // Returns the process-wide UI font (Segoe UI 9pt on Vista+).
    // Controls created via Create() default to the legacy SYSTEM_FONT bitmap font;
    // sending WM_SETFONT here makes them match dialog/Explorer appearance.
    // The HFONT is intentionally leaked -- it must outlive all controls.
    HFONT default_ui_font() {
        static HFONT font = nullptr;
        if (!font) {
            NONCLIENTMETRICSW ncm = { sizeof(ncm) };
            ::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
            font = ::CreateFontIndirectW(&ncm.lfMessageFont);
        }
        return font;
    }

    // Register (wnd, proc) pair, then attach/open/initialize. Returns wnd.
    // ASSERT fires in debug on creation failure; returns nullptr on release.
    template <class T>
    T* finish_new_window(T* wnd, BOOL created,
                         CWnd* parent, int x0, int y0, int x1, int y1,
                         const char* file, int line,
                         const char* init) {
        ASSERT(created);
        if (!created) {
            delete wnd;
            return nullptr;
        }
        ::SendMessageW(wnd->m_hWnd, WM_SETFONT,
                       reinterpret_cast<WPARAM>(default_ui_font()), FALSE);
        ensure_cleanup_guard();
        cLayOut* p = new cLayOut;
        new_window_registry().push_back({ wnd, p });
        p->attach(wnd);
        p->open(x0, y0, x1, y1, file, line);
        p->initialize(init);
        return wnd;
    }
}

// cLayOutZone: container child window with COLOR_BTNFACE background.
// Created via RegisterClassExW + CreateWindowExW + SubclassWindow so that
// MFC's OnNcDestroy fires and clears m_hWnd, preventing a double-destroy in
// ~CWnd() when DeleteLayOutWindows() calls DestroyWindow() then delete.
// PostNcDestroy is a no-op because DeleteLayOutWindows manages the lifetime.
// WS_EX_CONTROLPARENT lets Tab navigate through the Zone's child controls.
class cLayOutZone : public CWnd {
public:
    BOOL CreateZone(CWnd* parent, UINT id, int x0, int y0, int x1, int y1) {
        static bool done = false;
        if (!done) {
            WNDCLASSEXW wc = { sizeof(wc) };
            wc.style         = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc   = ::DefWindowProcW;
            wc.hInstance     = ::GetModuleHandleW(NULL);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.hCursor       = ::LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
            wc.lpszClassName = L"cLayOutZone";
            ::RegisterClassExW(&wc);
            done = true;
        }
        HWND hw = ::CreateWindowExW(
            WS_EX_CONTROLPARENT, L"cLayOutZone", nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            x0, y0, x1 - x0, y1 - y0,
            parent->GetSafeHwnd(), (HMENU)(UINT_PTR)id,
            ::GetModuleHandleW(NULL), nullptr);
        if (!hw) return FALSE;
        // Replace DefWindowProcW with AfxWndProc so MFC routes WM_NCDESTROY
        // through CWnd::OnNcDestroy, which clears m_hWnd cleanly.
        SubclassWindow(hw);
        return TRUE;
    }
    virtual void PostNcDestroy() override {}   // lifetime managed by DeleteLayOutWindows
protected:
    afx_msg BOOL OnEraseBkgnd(CDC* pDC) {
        CRect rc; GetClientRect(&rc);
        pDC->FillSolidRect(&rc, ::GetSysColor(COLOR_BTNFACE));
        return TRUE;
    }
    DECLARE_MESSAGE_MAP()
};
BEGIN_MESSAGE_MAP(cLayOutZone, CWnd)
    ON_WM_ERASEBKGND()
END_MESSAGE_MAP()

CButton* LayOutButton(CWnd* parent, int x0, int y0, int x1, int y1,
                   const char* file, int line, const char* init) {
    CButton* w = new CButton;
    BOOL ok = w->Create(nullptr, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                        CRect(x0, y0, x1, y1), parent, allocate_new_window_id());
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, init);
}

CEdit* LayOutEdit(CWnd* parent, int x0, int y0, int x1, int y1,
               const char* file, int line, const char* init) {
    CEdit* w = new CEdit;
    // ES_MULTILINE: enables SetRect() for vertical alignment (EM_SETRECT is
    // ignored on single-line edits). ES_WANTRETURN is NOT set so Enter still
    // routes to the default button; ES_AUTOHSCROLL is replaced by ES_AUTOVSCROLL.
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_TABSTOP,
                        CRect(x0, y0, x1, y1), parent, allocate_new_window_id());
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, init);
}

CStatic* LayOutStatic(CWnd* parent, int x0, int y0, int x1, int y1,
                   const char* file, int line, const char* init) {
    CStatic* w = new CStatic;
    // SS_LEFT: left-align text; SS_CENTERIMAGE: vertically center text
    BOOL ok = w->Create(nullptr, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOTIFY,
                        CRect(x0, y0, x1, y1), parent, allocate_new_window_id());
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, init);
}

CComboBox* LayOutCombo(CWnd* parent, int x0, int y0, int x1, int y1,
                    const char* file, int line, const char* init) {
    CComboBox* w = new CComboBox;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                        CRect(x0, y0, x1, y1), parent, allocate_new_window_id());
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, init);
}

CListBox* LayOutList(CWnd* parent, int x0, int y0, int x1, int y1,
                  const char* file, int line, const char* init) {
    CListBox* w = new CListBox;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL |
                        WS_TABSTOP, CRect(x0, y0, x1, y1), parent,
                        allocate_new_window_id());
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, init);
}

CRichEditCtrl* LayOutRichEdit(CWnd* parent, int x0, int y0, int x1, int y1,
                           const char* file, int line, const char* init) {
    AfxInitRichEdit2();
    CRichEditCtrl* w = new CRichEditCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
                        ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL |
                        WS_TABSTOP, CRect(x0, y0, x1, y1), parent,
                        allocate_new_window_id());
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, init);
}

CListCtrl* LayOutListCtrl(CWnd* parent, int x0, int y0, int x1, int y1,
                       const char* file, int line, const char* init) {
    CListCtrl* w = new CListCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL |
                        WS_TABSTOP, CRect(x0, y0, x1, y1), parent,
                        allocate_new_window_id());
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, init);
}

CTreeCtrl* LayOutTreeCtrl(CWnd* parent, int x0, int y0, int x1, int y1,
                       const char* file, int line, const char* init) {
    CTreeCtrl* w = new CTreeCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | WS_BORDER | TVS_HASBUTTONS |
                        TVS_HASLINES | TVS_LINESATROOT | WS_TABSTOP,
                        CRect(x0, y0, x1, y1), parent, allocate_new_window_id());
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, init);
}

CTabCtrl* LayOutTabCtrl(CWnd* parent, int x0, int y0, int x1, int y1,
                     const char* file, int line, const char* init) {
    CTabCtrl* w = new CTabCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | TCS_TABS | WS_TABSTOP,
                        CRect(x0, y0, x1, y1), parent, allocate_new_window_id());
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, init);
}

CDateTimeCtrl* LayOutDateTime(CWnd* parent, int x0, int y0, int x1, int y1,
                           const char* file, int line, const char* init) {
    CDateTimeCtrl* w = new CDateTimeCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | DTS_SHORTDATEFORMAT | WS_TABSTOP,
                        CRect(x0, y0, x1, y1), parent, allocate_new_window_id());
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, init);
}

CMonthCalCtrl* LayOutMonthCal(CWnd* parent, int x0, int y0, int x1, int y1,
                           const char* file, int line, const char* init) {
    CMonthCalCtrl* w = new CMonthCalCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP,
                        CRect(x0, y0, x1, y1), parent, allocate_new_window_id());
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, init);
}

CSpinButtonCtrl* LayOutSpin(CWnd* parent, int x0, int y0, int x1, int y1,
                         const char* file, int line, const char* init) {
    CSpinButtonCtrl* w = new CSpinButtonCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | UDS_ARROWKEYS | UDS_SETBUDDYINT |
                        WS_TABSTOP, CRect(x0, y0, x1, y1), parent,
                        allocate_new_window_id());
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, init);
}

CSliderCtrl* LayOutSlider(CWnd* parent, int x0, int y0, int x1, int y1,
                       const char* file, int line, const char* init) {
    CSliderCtrl* w = new CSliderCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | WS_TABSTOP,
                        CRect(x0, y0, x1, y1), parent, allocate_new_window_id());
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, init);
}

CScrollBar* LayOutScrollBar(CWnd* parent, int x0, int y0, int x1, int y1,
                         const char* file, int line, const char* init) {
    CScrollBar* w = new CScrollBar;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | SBS_HORZ, CRect(x0, y0, x1, y1), parent,
                        allocate_new_window_id());
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, init);
}

CIPAddressCtrl* LayOutIPAddress(CWnd* parent, int x0, int y0, int x1, int y1,
                             const char* file, int line, const char* init) {
    CIPAddressCtrl* w = new CIPAddressCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP,
                        CRect(x0, y0, x1, y1), parent, allocate_new_window_id());
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, init);
}

CHotKeyCtrl* LayOutHotKey(CWnd* parent, int x0, int y0, int x1, int y1,
                       const char* file, int line, const char* init) {
    CHotKeyCtrl* w = new CHotKeyCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP,
                        CRect(x0, y0, x1, y1), parent, allocate_new_window_id());
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, init);
}

CProgressCtrl* LayOutProgress(CWnd* parent, int x0, int y0, int x1, int y1,
                           const char* file, int line, const char* init) {
    CProgressCtrl* w = new CProgressCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | PBS_SMOOTH, CRect(x0, y0, x1, y1), parent,
                        allocate_new_window_id());
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, init);
}

CStatusBarCtrl* LayOutStatus(CWnd* parent, int x0, int y0, int x1, int y1,
                          const char* file, int line, const char* init) {
    CStatusBarCtrl* w = new CStatusBarCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, CRect(x0, y0, x1, y1), parent,
                        allocate_new_window_id());
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, init);
}

CHeaderCtrl* LayOutHeader(CWnd* parent, int x0, int y0, int x1, int y1,
                       const char* file, int line, const char* init) {
    CHeaderCtrl* w = new CHeaderCtrl;
    BOOL ok = w->Create(WS_CHILD | WS_VISIBLE | HDS_BUTTONS | HDS_HORZ,
                        CRect(x0, y0, x1, y1), parent, allocate_new_window_id());
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, init);
}

cModalFrame* LayOutModal(CWnd* parent, int x0, int y0, int x1, int y1,
                      const char* file, int line) {
    cModalFrame* w = new cModalFrame;
    // open() creates+shows only; finish_new_window registers { w, p } and
    // p subclasses the frame, so the frame itself is live-editable.
    BOOL ok = w->open(parent, x0, y0, x1, y1);
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, nullptr);
}

CWnd* LayOutZone(CWnd* parent, int x0, int y0, int x1, int y1,
                 const char* file, int line) {
    cLayOutZone* w = new cLayOutZone;
    BOOL ok = w->CreateZone(parent, static_cast<UINT>(allocate_new_window_id()),
                            x0, y0, x1, y1);
    return finish_new_window(w, ok, parent, x0, y0, x1, y1, file, line, nullptr);
}

// Borrowed attach: registry stores only the cLayOut, caller owns the CWnd.
// Prefer LayOutNew for windows you create for the UI; use this only for a window whose
// lifetime is managed elsewhere, and NEVER a global/static object (deferred destruction
// past the leak snapshot -> phantom empty-dump leak).
CWnd* LayOutAsItIs(CWnd* self, int x0, int y0, int x1, int y1,
                   const char* file, int line) {
    ensure_cleanup_guard();
    cLayOut* p = new cLayOut;
    // wnd=nullptr: registry owns only the cLayOut; caller owns the CWnd.
    new_window_registry().push_back({ nullptr, p });
    p->attach(self);
    if (x0 == 0 && y0 == 0 && x1 == 0 && y1 == 0) {
        // (0,0,0,0): attach-only mode. Read current rect so last_rect is valid for ESC restore.
        CRect rc;
        self->GetWindowRect(&rc);
        CWnd* par = self->GetParent();
        if (par) par->ScreenToClient(&rc);
        p->open(rc.left, rc.top, rc.right, rc.bottom, file, line);
    } else {
        p->open(x0, y0, x1, y1, file, line);
        self->MoveWindow(CRect(x0, y0, x1, y1));
    }
    // cModalFrame::open(parent) creates hidden; no-op if already visible.
    self->ShowWindow(SW_SHOW);
    return self;
}

// Same as LayOutAsItIs but the registry OWNS the new'd window: { self, p } instead of
// { nullptr, p }. DeleteLayOutWindows() then DestroyWindow()s and deletes it, so a
// heap-allocated external CWnd (e.g. new cConBox) is fully torn down inside the host loop,
// freeing its STL members before the CRT leak snapshot at exit (no phantom empty dump).
CWnd* LayOutNew(CWnd* self, int x0, int y0, int x1, int y1,
                const char* file, int line) {
    ensure_cleanup_guard();
    cLayOut* p = new cLayOut;
    new_window_registry().push_back({ self, p });   // { self, ... }: registry owns the CWnd
    p->attach(self);
    if (x0 == 0 && y0 == 0 && x1 == 0 && y1 == 0) {
        // (0,0,0,0): attach-only mode. Read current rect so last_rect is valid for ESC restore.
        CRect rc;
        self->GetWindowRect(&rc);
        CWnd* par = self->GetParent();
        if (par) par->ScreenToClient(&rc);
        p->open(rc.left, rc.top, rc.right, rc.bottom, file, line);
    } else {
        p->open(x0, y0, x1, y1, file, line);
        self->MoveWindow(CRect(x0, y0, x1, y1));
    }
    self->ShowWindow(SW_SHOW);
    return self;
}

void DeleteLayOutWindows() {
    std::vector<NewWindowEntry>& list = new_window_registry();
    for (int i = static_cast<int>(list.size()) - 1; i >= 0; i--) {
        NewWindowEntry& e = list[static_cast<size_t>(i)];
        delete e.layout;   // removes subclass before HWND is destroyed
        if (e.wnd) {
            if (e.wnd->m_hWnd && ::IsWindow(e.wnd->m_hWnd))
                e.wnd->DestroyWindow();
            delete e.wnd;
        }
    }
    list.clear();
    list.shrink_to_fit();   // release internal buffer so the CRT leak checker is silent
    next_new_window_id() = NEW_WINDOW_FIRST_ID;
}

// Convert a UTF-8 string to a std::wstring for Windows API calls.
static std::wstring utf8_to_wide(const char* s) {
    if (!s || !*s) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n, 0);
    ::MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

void cLayOut::initialize(const char* init) {
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
    // matching command/notify/scroll code as WM_EVOL_CALLBACK lParam.
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

void cLayOut::clear_signal_codes() {
    signal_count = 0;
}

bool cLayOut::add_signal_code(int code) {
    if (code == -1) return true;
    if (has_signal_code(code)) return true;
    if (signal_count >= MAX_SIGNAL_CODES) return false;
    signal_codes[signal_count++] = code;
    return true;
}

void cLayOut::set_signal_code(int code) {
    clear_signal_codes();
    if (code != -1) add_signal_code(code);
}

void cLayOut::set_signal_codes(const int* codes, int count) {
    clear_signal_codes();
    if (!codes || count <= 0) return;
    for (int i = 0; i < count; i++) add_signal_code(codes[i]);
}

bool cLayOut::has_signal_code(int code) const {
    for (int i = 0; i < signal_count; i++)
        if (signal_codes[i] == code) return true;
    return false;
}

void cLayOut::set_default_signal_codes(const wchar_t* cls) {
    clear_signal_codes();
    for (const LAYOUT_SIGMAP& e : signal_map) {
        if (_wcsicmp(cls, e.cls) == 0) {
            set_signal_codes(e.codes, e.count);
            return;
        }
    }
}

bool cLayOut::open(int x0, int y0, int x1, int y1,
                        const char* f, int ln) {
    if (!target || !::IsWindow(target->m_hWnd)) return false;

#if defined(_DEBUG)
    file = f;
    line = ln;
    last_rect.SetRect(x0, y0, x1, y1);
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

LRESULT CALLBACK cLayOut::proc(HWND hWnd, UINT msg, WPARAM wParam,
                                        LPARAM lParam, UINT_PTR, DWORD_PTR ref) {
    cLayOut* self = reinterpret_cast<cLayOut*>(ref);
    return self->dispatch(hWnd, msg, wParam, lParam);
}

LRESULT cLayOut::dispatch(HWND h, UINT msg, WPARAM w, LPARAM l) {
    // -- signal protocol (works in both normal and edit mode) --
    switch (msg) {
    case WM_EVOL_SURVEIL:
        report = reinterpret_cast<CWnd*>(w);   // host pointer, or 0 to clear
        return reinterpret_cast<LRESULT>(target);
    case WM_EVOL_CALLBACK:
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

void cLayOut::toggle_edit(HWND h) {
    if (editing) leave_edit(h, false);        // middle-click exit: no commit
    else          enter_edit(h);
}

void cLayOut::enter_edit(HWND h) {
    if (active() && active() != this) {        // drop the previous active one
        cLayOut* prev = active();
        if (prev->target) prev->leave_edit(prev->target->m_hWnd, false);
    }
    editing = true;
    active() = this;
    ::SetFocus(h);
    paint_frame(h);
}

void cLayOut::leave_edit(HWND h, bool commit) {
    editing = false;
    if (active() == this) active() = nullptr;

    if (!commit) {
        // Restore to last committed rect (parent-client coords).
        HWND parent = ::GetParent(h);
        if (parent)
            ::MoveWindow(h, last_rect.left, last_rect.top,
                         last_rect.Width(), last_rect.Height(), TRUE);
        else
            ::SetWindowPos(h, NULL, last_rect.left, last_rect.top,
                           last_rect.Width(), last_rect.Height(),
                           SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (commit && file) {
        CRect rc;
        get_rect(h, rc);
        if (rc != last_rect) {
            LayOutRewrite(file, line, rc);
            last_rect = rc;
        }
    }

    HWND parent = ::GetParent(h);              // erase the red frame
    if (parent) ::InvalidateRect(parent, NULL, TRUE);
    ::InvalidateRect(h, NULL, TRUE);
}

void cLayOut::on_key(HWND h, UINT vk) {
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

void cLayOut::on_drag(HWND h, LPARAM l) {
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

void cLayOut::paint_frame(HWND h) {
    CWnd* w = CWnd::FromHandle(h);
    CClientDC dc(w);
    CRect rc;
    w->GetClientRect(&rc);
    CBrush red;
    red.CreateSolidBrush(RGB(255, 0, 0));
    dc.FrameRect(&rc, &red);
}

void cLayOut::get_rect(HWND h, CRect& out) {
    ::GetWindowRect(h, &out);                  // screen coords
    HWND parent = ::GetParent(h);
    if (parent)                                // -> parent client coords
        ::MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&out), 2);
}

#endif // _DEBUG

// ======================================================================
// [SIGNAL]
// ======================================================================

void cLayOut::signal() {
    if (report && ::IsWindow(report->m_hWnd))
        ::PostMessage(report->m_hWnd, WM_EVOL_REPORT,
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
    ::MessageBoxW(nullptr, buff, L"cLayOut rewrite error", MB_OK | MB_ICONERROR);
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

        // Find "LayOut(" and skip 2 leading commas (Type, parent) before the coords.
        const char* tok = strstr(buff, "LayOut(");
        const char* lp  = tok ? find_char('(', tok) : nullptr;

        const char* sep = lp;
        for (int k = 0; k < 2 && sep; k++)
            sep = find_char(',', sep + 1);
        const char* c_par = sep;   // comma right after the last skipped arg

        // Find 3 more commas to step past x0, y0, x1 (landing after x1 -> y1).
        const char* c3 = c_par ? find_char(',', c_par + 1) : nullptr;
        const char* c4 = c3   ? find_char(',', c3   + 1) : nullptr;
        const char* c5 = c4   ? find_char(',', c4   + 1) : nullptr;
        // Optional extra arg after y1 (e.g. init string).
        const char* c6 = c5   ? find_char(',', c5   + 1) : nullptr;
        const char* rp = lp   ? find_char(')', lp   + 1) : nullptr;
        // tail: everything after y1's value (", extra)" or just ")").
        const char* tail = (c6 && c6 < rp) ? c6 : rp;

        if (tok && c_par && c5 && rp && c_par < c5 && c5 < rp) {
            const char* s;
            for (s = buff; s <= c_par; s++) putc(*s, dst);
            fprintf(dst, " %d, %d, %d, %d",
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
// [HOST]   cModalFrame -- surveil/listen/report host implementation
// ======================================================================

BEGIN_MESSAGE_MAP(cModalFrame, CFrameWnd)
END_MESSAGE_MAP()

static const UINT_PTR MODAL_TIMER_ID = 1;

cModalFrame::cModalFrame() : waiting(false), timer_fired(false), event(nullptr), timer_id(0) {
}

// ESC and Enter are consumed in BOTH normal and edit mode (CDialog-like behaviour).
// TRAP: the editing_hwnd() guard does NOT restrict this to edit mode -- it only yields
// to the cLayOut subclass proc when a control is actively being repositioned (so Enter
// commits and ESC cancels the drag). In normal mode both keys are still always consumed,
// EXCEPT when the focused window returns DLGC_WANTALLKEYS (e.g. cConBox terminal widget).
BOOL cModalFrame::PreTranslateMessage(MSG* pMsg) {
    if (pMsg->message == WM_KEYDOWN) {
        // Yield to cLayOut only when the message target is the editing window.
        if (cLayOut::editing_hwnd() == pMsg->hwnd) return FALSE;
        // If the focused window claims all keys (e.g. ConBox terminal), let it handle
        // Esc/Enter directly instead of consuming them here.
        CWnd* focus = GetFocus();
        if (focus && (focus->SendMessage(WM_GETDLGCODE) & DLGC_WANTALLKEYS))
            return CFrameWnd::PreTranslateMessage(pMsg);
        if (pMsg->wParam == VK_ESCAPE) {
            PostMessage(WM_CLOSE);
            return TRUE;
        }
        if (pMsg->wParam == VK_RETURN) {
            CWnd* focus = GetFocus();
            if (focus) {
                wchar_t cls[32] = {};
                ::GetClassNameW(focus->m_hWnd, cls, _countof(cls));
                // Only a multiline edit WITH ES_WANTRETURN claims Enter
                // for itself (newline). Without it, Enter routes to the button.
                if (_wcsicmp(cls, L"Edit") == 0 &&
                    (focus->GetStyle() & ES_MULTILINE) &&
                    (focus->GetStyle() & ES_WANTRETURN))
                    return CFrameWnd::PreTranslateMessage(pMsg);
                // Push button: simulate click.
                DWORD btype = focus->GetStyle() & 0x0F;
                if (btype == BS_PUSHBUTTON || btype == BS_DEFPUSHBUTTON)
                    focus->SendMessage(BM_CLICK);
            }
            return TRUE;  // always consume Enter so it never becomes WM_CHAR '\r'
        }
    }
    return CFrameWnd::PreTranslateMessage(pMsg);
}

int cModalFrame::timer(int period) {
    if (timer_id) { KillTimer(timer_id); timer_id = 0; }
    if (period <= 0) return 0;
    timer_id = SetTimer(MODAL_TIMER_ID, static_cast<UINT>(period), nullptr);
    if (timer_id == 0) return static_cast<int>(::GetLastError());
    return 0;
}

bool cModalFrame::open(CWnd* parent, int x0, int y0, int x1, int y1) {
    if (!::IsWindow(m_hWnd)) {
        // Pass nullptr for class/title to avoid wide literals in TCHAR parameters;
        // set the title via SetWindowTextW immediately after creation.
        // No subclassing here: LayOut(Modal,...) attaches a registry-owned
        // cLayOut after this call, like every other factory.
        Create(nullptr, nullptr, WS_OVERLAPPEDWINDOW, CRect(x0, y0, x1, y1), parent);
        if (::IsWindow(m_hWnd))
            ::SetWindowTextW(m_hWnd, L"cLayOut Demo");
    }
    else {
        MoveWindow(x0, y0, x1 - x0, y1 - y0, TRUE);
    }
    ShowWindow(SW_SHOW);
    return ::IsWindow(m_hWnd) != 0;
}

bool cModalFrame::open(CWnd* parent) {
    // Create only, stay hidden: the caller positions, shows, and attaches the
    // editor afterwards via LayOut(New, &wnd, x0,y0,x1,y1) (or LayOut(Modal,...)).
    if (!::IsWindow(m_hWnd)) {
        Create(nullptr, nullptr, WS_OVERLAPPEDWINDOW, CRect(0, 0, 0, 0), parent);
        if (::IsWindow(m_hWnd)) ::SetWindowTextW(m_hWnd, L"cLayOut Demo");
    }
    return ::IsWindow(m_hWnd) != 0;
}

CWnd* cModalFrame::listen_core(int count, CWnd** list) {
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

void cModalFrame::surveil(int count, CWnd** list, bool on) {
    WPARAM token = on ? reinterpret_cast<WPARAM>(this) : 0;
    for (int i = 0; i < count; i++) {
        if (list[i] && ::IsWindow(list[i]->m_hWnd))
            ::SendMessage(list[i]->m_hWnd, WM_EVOL_SURVEIL, token, 0);
    }
}

LRESULT cModalFrame::WindowProc(UINT msg, WPARAM wParam, LPARAM lParam) {
    // Paint background with the dialog face color instead of the CFrameWnd default.
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
    if (waiting && msg == WM_EVOL_REPORT) {
        event = reinterpret_cast<CWnd*>(wParam);
        m_nFlags &= ~WF_CONTINUEMODAL;
        return 0;
    }

    // Reflect control notifications to the control HWND (HWND-based, so the
    // proc subclass proc receives them). lParam carries the full signed
    // code for WM_NOTIFY and the scroll request code for scrollbar/trackbar.
    if (msg == WM_COMMAND && lParam != 0) {
        HWND ctrl = reinterpret_cast<HWND>(lParam);
        if (::IsWindow(ctrl))
            ::SendMessage(ctrl, WM_EVOL_CALLBACK, 0,
                          static_cast<LPARAM>(static_cast<int>(HIWORD(wParam))));
        return 0;
    }
    if (msg == WM_NOTIFY) {
        NMHDR* nm = reinterpret_cast<NMHDR*>(lParam);
        if (nm && ::IsWindow(nm->hwndFrom))
            ::SendMessage(nm->hwndFrom, WM_EVOL_CALLBACK, 0,
                          static_cast<LPARAM>(nm->code));
        return 0;
    }
    if ((msg == WM_HSCROLL || msg == WM_VSCROLL) && lParam != 0) {
        HWND ctrl = reinterpret_cast<HWND>(lParam);
        if (::IsWindow(ctrl))
            ::SendMessage(ctrl, WM_EVOL_CALLBACK, 0,
                          static_cast<LPARAM>(static_cast<int>(LOWORD(wParam))));
        return 0;
    }

    // window going away while in a modal listen: break out with no event.
    if (msg == WM_DESTROY) {
        event = nullptr;
        m_nFlags &= ~WF_CONTINUEMODAL;
    }

    return CFrameWnd::WindowProc(msg, wParam, lParam);
}
