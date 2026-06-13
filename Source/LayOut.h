/*
    LayOut.h - cLayOut + cModalFrame.

    cLayOut : live layout editor by window subclassing.
    cModalFrame  : minimal host implementing the surveil/listen/report protocol.

    PURPOSE (cLayOut)
    -------
    cLayOut "attaches to" an existing MFC control (CEdit/CStatic/CButton/
    CComboBox/...) via SetWindowSubclass (comctl32) and overlays two features:

      1) Live layout editing. While the app runs, toggle a control into edit
         mode (middle button), then move/resize it with mouse + arrow keys.
         On commit (Enter / left double-click) the new rectangle is written
         back into the *source code* literal that produced it, using the
         __FILE__/__LINE__ captured by the LayOut() macro. Re-compile to make
         it permanent. This is a DEVELOPMENT-time tool (needs the source file
         present at its compile-time path).

      2) Signal interop with a host that follows the surveil/listen/report
         protocol (see "HOST CONTRACT" below). Each control type gets a
         default list of natural notifications (button=click, edit=text
         change, combo=selection/edit change, etc.); controls with no useful
         user-change signal get an empty list.

    DEPENDENCIES
    ------------
    Standard MFC (CWnd/CRect/CClientDC) + comctl32 SetWindowSubclass. No other
    libraries. Build: Unicode, x64. Link: comctl32 (MFC links it already).

    OWNERSHIP / LIFETIME (read before use)
    --------------------------------------
    - LayOut* factory functions own both the CWnd and the cLayOut. The
      returned cLayOut& is valid until DeleteLayOutWindows() is called.
      Do not use the reference after DeleteLayOutWindows().
    - For direct (non-factory) use: cLayOut does NOT own target.
      attach() stores a borrowed CWnd* only. The target must outlive the
      cLayOut; destroy it before the CWnd.
    - If the parent window is destroyed first, Windows destroys the child HWND
      and MFC normally detaches m_hWnd (=> NULL). The cLayOut destructor then
      skips RemoveWindowSubclass and never deletes the target object.

    MINIMAL USAGE (host = a window implementing the contract below)
    --------------------------------------------------------------
        CEdit*    edit   = LayOut(Edit,   parent, 10, 10, 200, 30);
        CButton*  button = LayOut(Button, parent, 10, 50, 100, 30, "OK");
        CComboBox*combo  = LayOut(Combo,  parent, 10, 90, 200, 60, "A,B,C");
        CWnd*     zone   = LayOut(Zone,   parent, 10,120, 200,250);  // container
        // in a modal loop:
        CWnd* ev = host.listen(edit, button, combo);
        if (ev == button) { ... }
        DeleteLayOutWindows();   // once, at end of host window lifetime

    HOST CONTRACT (message protocol; constants below are shared with the host)
    -------------------------------------------------------------------------
    The host (e.g. cModalFrame / a real MainBox) must:
      - reflect control notifications to the control HWND as WM_EVOL_CALLBACK
        via ::SendMessage (NOT a direct C++ WindowProc() call - that bypasses
        the subclass chain and never reaches the subclass proc):
            WM_COMMAND : SendMessage(ctrlHwnd, WM_EVOL_CALLBACK, 0,
                                     (LPARAM)(int)HIWORD(wParam))
            WM_NOTIFY  : SendMessage(nmhdr.hwndFrom, WM_EVOL_CALLBACK, 0,
                                     (LPARAM)(int)nmhdr.code)
            WM_HSCROLL/WM_VSCROLL when lParam is a control HWND:
                         SendMessage(ctrlHwnd, WM_EVOL_CALLBACK, 0,
                                     (LPARAM)(int)LOWORD(wParam))
      - register/unregister reporting via WM_EVOL_SURVEIL (wParam = host CWnd*,
        or 0 to clear) sent to each control HWND. (surveil already uses
        ::SendMessage, so it is compatible as-is.)
      - receive WM_EVOL_REPORT (wParam = the signaling control's CWnd*) to end
        its modal loop and return that pointer from listen().
*/

#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <afxwin.h>
#include <afxcmn.h>
#include <afxdtctl.h>
#include <CommCtrl.h>   // SetWindowSubclass / DefSubclassProc

// ---- Shared message protocol (identical values to 05-box.c++ MainBox) -------
// Keep these in sync with any real host (MainBox) so they interoperate.
#ifndef WM_EVOL
#define WM_EVOL          (WM_USER + 1024)
#endif
enum {
    WM_EVOL_CALLBACK     = WM_EVOL - 1,   // host -> control: reflected notify
    WM_EVOL_REPORT       = WM_EVOL - 2,   // control -> host: a signal fired
    WM_EVOL_SURVEIL      = WM_EVOL - 3    // host -> control: set/clear report
};

// Subclass id used by SetWindowSubclass for the subclass procedure.
#define ID_LAYOUT      0xCAFE


class cLayOut {
public:
    cLayOut();
    ~cLayOut();

    // Store a borrowed target pointer only. Does not create, subclass, or take
    // ownership. The caller must have already Create()d the control (hidden is
    // fine) and must keep the CWnd object valid longer than this cLayOut.
    void attach(CWnd* target);

    // Subclass the target, auto-detect signal codes from the window class,
    // and store file/line/rect for the source rewriter (debug only).
    // Positioning is done by the factory (via Create) before this call.
    // Use the LayOut() macro at call sites so file/line are captured.
    bool open(int x0, int y0, int x1, int y1,
              const char* file, int line);

    // Apply a type-specific init string after open(). Called by New* factories.
    // nullptr -> no-op. CComboBox: comma-split, trim, AddString each, SetCurSel(0).
    // Others: SetWindowTextW.
    void initialize(const char* init);

    // Signal-code customization. Codes are full signed notification/scroll
    // values carried in WM_EVOL_CALLBACK lParam; do not truncate with LOWORD.
    // set_signal_code(-1) and clear_signal_codes() both disable signaling.
    void clear_signal_codes();
    bool add_signal_code(int code);
    void set_signal_code(int code);
    void set_signal_codes(const int* codes, int count);
    bool has_signal_code(int code) const;

    // Expose the editing control so this object can go into a host's
    // listen()/surveil() list and be compared against listen()'s return value.
    operator CWnd*() const { return target; }

    static LRESULT CALLBACK proc(HWND hWnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam, UINT_PTR id, DWORD_PTR ref);

    // Returns the HWND currently in layout-edit mode, or NULL if none (debug only).
    // Hosts should yield Enter/Esc to cLayOut (return FALSE from PreTranslateMessage)
    // when pMsg->hwnd matches this value, so cLayOut can commit or cancel.
#if defined(_DEBUG)
    static HWND editing_hwnd();
#else
    static HWND editing_hwnd() { return NULL; }
#endif

private:
    // non-copyable: it stores target identity and subclass state.
    cLayOut(const cLayOut&);
    cLayOut& operator=(const cLayOut&);

    // -- instance message handling (called from the static subclass proc) --
    LRESULT dispatch(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void    signal();
    void    set_default_signal_codes(const wchar_t* cls);

#if defined(_DEBUG)
    // Live layout editing -- compiled out in non-debug builds.
    void    toggle_edit(HWND hWnd);
    void    enter_edit(HWND hWnd);
    void    leave_edit(HWND hWnd, bool commit);
    void    on_key(HWND hWnd, UINT vk);
    void    on_drag(HWND hWnd, LPARAM lParam);
    void    paint_frame(HWND hWnd);
    void    get_rect(HWND hWnd, CRect& out);
    static cLayOut*& active();
#endif

    enum { MAX_SIGNAL_CODES = 12 };

    CWnd*       target;
    int         signal_codes[MAX_SIGNAL_CODES];
    int         signal_count;
    CWnd*       report;       // host registered via WM_EVOL_SURVEIL

#if defined(_DEBUG)
    const char* file;
    int         line;
    bool        editing;     // layout edit mode on/off
    CRect       last_rect;    // rect at open() time, updated on each commit; used for ESC restore and no-op rewrite skip
    // Manual drag state (child controls only; top-level uses OS WM_NCLBUTTONDOWN loop)
    CPoint      drag_start;     // cursor screen pos at WM_LBUTTONDOWN
    CRect       drag_rect;      // window screen rect at WM_LBUTTONDOWN
    int         drag_edge;      // 0=none, HTCAPTION=move, HTxxx=resize corner
#endif
};

// LayOut(Type, parent, x0,y0,x1,y1 [, init]) -- hidden-control factory macro.
// Expands to LayOut##Type(..., __FILE__, __LINE__ [, init]).
// Returns a typed MFC pointer to the created control (valid until DeleteLayOutWindows()).
//
//   Type       Return type       init string behaviour
//   --------   ---------------   ----------------------------------------
//   Edit       CEdit*            SetWindowTextW
//   Button     CButton*          SetWindowTextW
//   Static     CStatic*          SetWindowTextW
//   Combo      CComboBox*        comma-split, trim, AddString, SetCurSel(0)
//   List       CListBox*         (not yet initialised by string)
//   RichEdit   CRichEditCtrl*    SetWindowTextW
//   ListCtrl   CListCtrl*        (not yet initialised by string)
//   TreeCtrl   CTreeCtrl*        (not yet initialised by string)
//   TabCtrl    CTabCtrl*         (not yet initialised by string)
//   DateTime   CDateTimeCtrl*    (not yet initialised by string)
//   MonthCal   CMonthCalCtrl*    (not yet initialised by string)
//   Spin       CSpinButtonCtrl*  (not yet initialised by string)
//   Slider     CSliderCtrl*      (not yet initialised by string)
//   ScrollBar  CScrollBar*       (not yet initialised by string)
//   IPAddress  CIPAddressCtrl*   (not yet initialised by string)
//   HotKey     CHotKeyCtrl*      (not yet initialised by string)
//   Progress   CProgressCtrl*    (not yet initialised by string)
//   Status     CStatusBarCtrl*   (not yet initialised by string)
//   Header     CHeaderCtrl*      (not yet initialised by string)
//   Modal      cModalFrame*      creates + shows the top-level host window
//   Zone       CWnd*             container child window (COLOR_BTNFACE bg); use as parent
//   New        CWnd*             attach + move an already-created (new'd) window; registry OWNS it
//                                (DeleteLayOutWindows DestroyWindow+deletes it -- pass a new'd obj)
//   AsItIs     CWnd*             attach + move a BORROWED window (registry won't destroy it);
//                                prefer New -- AsItIs must NOT be a global/static (see note below)
//
// Examples:
//   CEdit*    e = LayOut(Edit,   parent, 10,10,200,30);
//   CButton*  b = LayOut(Button, parent, 10,50,100,30, "OK");
//   CComboBox*c = LayOut(Combo,  parent, 10,90,200,60, "A,B,C");
//   cFoo*     w = new cFoo;       // LayOut(New,...): allocate first, then open,
//   w->open(parent, ...);        //   then register -- open() signature varies per class
//   LayOut(New, w, 10,10,200,30);//   registry owns w; DeleteLayOutWindows() deletes it
//   CWnd*     x = LayOut(AsItIs, myWnd,  10,10,200,30);    // borrowed; you own myWnd (not global)
// _DEBUG: pass __FILE__/__LINE__ so live editing can rewrite the call site.
// Release: pass nullptr/0 -- keeps source paths OUT of the release binary
// (the whole edit/rewrite machinery is compiled out; only signal interop remains).
#if defined(_DEBUG)
#define LayOut(Type, parent, x0, y0, x1, y1, ...) \
    LayOut##Type((parent),(x0),(y0),(x1),(y1), __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define LayOut(Type, parent, x0, y0, x1, y1, ...) \
    LayOut##Type((parent),(x0),(y0),(x1),(y1), nullptr, 0, ##__VA_ARGS__)
#endif

CButton*         LayOutButton   (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
CEdit*           LayOutEdit     (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
CStatic*         LayOutStatic   (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
CComboBox*       LayOutCombo    (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
CListBox*        LayOutList     (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
CRichEditCtrl*   LayOutRichEdit (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
CListCtrl*       LayOutListCtrl (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
CTreeCtrl*       LayOutTreeCtrl (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
CTabCtrl*        LayOutTabCtrl  (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
CDateTimeCtrl*   LayOutDateTime (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
CMonthCalCtrl*   LayOutMonthCal (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
CSpinButtonCtrl* LayOutSpin     (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
CSliderCtrl*     LayOutSlider   (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
CScrollBar*      LayOutScrollBar(CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
CIPAddressCtrl*  LayOutIPAddress(CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
CHotKeyCtrl*     LayOutHotKey   (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
CProgressCtrl*   LayOutProgress (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
CStatusBarCtrl*  LayOutStatus   (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
CHeaderCtrl*     LayOutHeader   (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
class cModalFrame;
cModalFrame*     LayOutModal    (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln);
CWnd*            LayOutZone     (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln);
// LayOut(AsItIs, wnd, x0,y0,x1,y1): attach an already-open window and move it.
// LayOut(AsItIs, wnd, 0,0,0,0)    : attach-only; window stays at its current position.
// wnd must be already Create()d. The registry owns the cLayOut only (not the CWnd):
// DeleteLayOutWindows() removes the subclass but does NOT destroy or delete wnd.
// PREFER LayOut(New, new ...) for windows you create for the UI. Use AsItIs only for a
// BORROWED window whose lifetime is managed elsewhere. OK for a stack/member window
// (destroyed at a scope before app exit); NEVER a global/static object -- its destruction
// is deferred past the CRT leak snapshot and triggers a phantom empty-dump leak (Learned 4.8).
CWnd*            LayOutAsItIs   (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln);
// LayOut(New, wnd, x0,y0,x1,y1): like LayOut(AsItIs,...) EXCEPT the registry OWNS wnd.
// "New" signals that wnd was heap-allocated with new -- DeleteLayOutWindows() removes the
// subclass AND DestroyWindow()+delete the wnd, so wnd MUST be a new'd pointer (never stack/
// member/global). Recommended for externally-created CWnd-derived windows (e.g. new cConBox):
// torn down INSIDE the host loop, their heap members (std::string/vector/deque) are freed
// before the CRT leak snapshot -- avoiding the phantom empty-dump leak a global object causes.
CWnd*            LayOutNew      (CWnd* p,int x0,int y0,int x1,int y1,const char* f,int ln);

// Call once at the end of the host window lifetime.
// Iterates the registry in REVERSE registration order (children before parent):
//   LayOut(Edit/Button/...)/LayOut(New,...): delete cLayOut (removes subclass) -> DestroyWindow -> delete wnd
//   LayOut(AsItIs, ...):                    delete cLayOut only -- the CWnd was borrowed, not created here
// TRAP: Do NOT call DestroyWindow() yourself on LayOut-registered controls before
//   calling this. It is redundant (DeleteLayOutWindows skips already-destroyed HWNDs)
//   and the USAGE example that shows Top->DestroyWindow() first is WRONG -- do not copy it.
// TRAP: CWinApp::ExitInstance() returns AfxGetCurrentMessage()->wParam, which is the
//   last pumped message's wParam -- a meaningless exit code when InitInstance drives its
//   own modal loop and returns FALSE. Override ExitInstance() to return 0 explicitly.
void             DeleteLayOutWindows();

// Align text in a window using numpad layout (1=top-left ... 9=bot-right).
// Dispatches by window class: Static/Edit/Button/CComboBox.
// CStatic: SS_ h-align + SS_CENTERIMAGE for v-center (no native v-bottom).
// CEdit  : ES_ h-align + SetRect() y-offset for vertical.
// CButton: BS_ h+v styles. CComboBox: h-align via child Edit only.
void AlignText(CWnd* wnd, int number);

// Rewrite the 4 coordinate literals of the LayOut(...) call at file:line.
// Returns false (and shows a message box) on failure. Integer literals only;
// the call must be on a single line; ANSI/UTF-8 source (BOM/UTF-16 rejected).
// Compiled only in _DEBUG builds; not available in release.
#if defined(_DEBUG)
bool LayOutRewrite(const char* file, int line, const RECT& rect);
#endif

// ============================================================================
// cModalFrame -- minimal host implementing the surveil/listen/report protocol.
// ============================================================================
// A plain top-level CFrameWnd that:
//   - is live-editable itself: the LayOut(Modal,...) factory attaches a
//     registry-owned cLayOut, so its position/size can be edited like any control
//     (middle button to toggle,
//     drag/arrow keys to move/resize, Enter to commit),
//   - paints its client area with COLOR_BTNFACE (dialog background color),
//   - applies the system UI font (lfMessageFont) to all New*-created child controls,
//   - supports a single repeating timer via timer(); listen() returns this on fire,
//   - reflects child control notifications (WM_COMMAND/WM_NOTIFY/WM_HSCROLL/
//     WM_VSCROLL when lParam is a control HWND) to the control HWND as
//     WM_EVOL_CALLBACK via ::SendMessage (direct C++ WindowProc() would bypass
//     the subclass chain),
//   - registers/unregisters reporting on a set of controls (surveil),
//   - blocks in a modal loop (listen) until one of them posts WM_EVOL_REPORT,
//     then returns that control's CWnd* (matchable against (CWnd*)a_layout_obj),
//   - intercepts ESC and Enter in PreTranslateMessage in BOTH normal and edit mode,
//     matching CDialog keyboard behaviour:
//       * ESC always posts WM_CLOSE.
//       * Enter clicks the focused push button (consumed even if no button focused).
//       * Exception 1: focused window returns DLGC_WANTALLKEYS -> both keys pass
//         through unmodified (e.g. a terminal/ConBox widget that needs raw Esc/Enter).
//       * Exception 2: message target == editing_hwnd() -> returns FALSE so cLayOut's
//         subclass proc handles commit (Enter) / cancel (ESC) for the repositioned ctrl.
//     TRAP: the editing_hwnd() guard does NOT mean ESC/Enter are only consumed in
//     edit mode. In NORMAL mode they are ALWAYS consumed -- that check merely yields
//     to the subclass proc when a control is being repositioned.
//
// USAGE
//     cModalFrame* Top = LayOut(Modal, 0, 560,275, 1360,875);
//     Top->timer(500);                     // optional: fire every 500 ms
//     CWnd* ev = Top->listen(a, b, c, d);  // any number of controls
//     if (ev == Top)       ...             // timer fired
//     if (ev == (CWnd*)b) ...             // b signaled
//     // NOTE: do NOT call Top->DestroyWindow() -- DeleteLayOutWindows() does it.
//     // For other CWnd-derived windows (e.g. cConBox), new them and attach via
//     // LayOut(New, wnd, ...) so DeleteLayOutWindows() deletes them here too. Avoid a
//     // global/static instance: its STL members are freed only at static destruction
//     // (after the CRT leak snapshot), producing a phantom empty-dump "memory leaks".
//     DeleteLayOutWindows();  // reverse order: remove subclass -> DestroyWindow -> delete

class cModalFrame : public CFrameWnd {
public:
    cModalFrame();

    // Create (first call) or reposition the window, then show it. No
    // subclassing here: the LayOut(Modal,...) factory attaches the
    // registry-owned cLayOut afterwards (like every other factory).
    bool open(CWnd* parent, int x0, int y0, int x1, int y1);
    // Create only (hidden, zero rect). Position + show + attach afterwards
    // via LayOut(New, &wnd, x0,y0,x1,y1) (or LayOut(Modal,...) which does it all).
    bool open(CWnd* parent = nullptr);

    // Set a repeating timer. period > 0: fire every period ms, listen() returns
    // this. period <= 0: cancel. Only one timer is supported; re-calling with a
    // different period just changes the interval. Returns 0 on success, or the
    // GetLastError() code on failure.
    int timer(int period);

    // Modal wait until one of the listed controls signals. Returns its CWnd*
    // (or 0 if the window was closed, or this if the timer fired).
    // Accepts any number of CWnd* / cLayOut* controls (no fixed upper limit).
    template<class... Args>
    CWnd* listen(Args*... controls) {
        CWnd* list[] = { static_cast<CWnd*>(controls)... };
        return listen_core(static_cast<int>(sizeof...(controls)), list);
    }

protected:
    virtual LRESULT WindowProc(UINT msg, WPARAM wParam, LPARAM lParam);
    // Suppress CFrameWnd::PostNcDestroy()'s "delete this" -- heap object managed
    // by DeleteLayOutWindows(); self-delete from PostNcDestroy would double-free.
    virtual BOOL PreTranslateMessage(MSG* pMsg) override;
    virtual void PostNcDestroy() override {}
    DECLARE_MESSAGE_MAP()

private:
    CWnd* listen_core(int count, CWnd** list);
    void  surveil(int count, CWnd** list, bool on);

    bool         waiting;
    bool         timer_fired;  // set by WM_TIMER while waiting, consumed by listen_core
    CWnd*        event;
    UINT_PTR     timer_id;     // 0 = no active timer
};
