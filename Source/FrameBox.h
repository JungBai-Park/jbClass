/*
    FrameBox.h - Parasite + FrameBox.

    Parasite  : live layout editor by window subclassing.
    FrameBox : CWnd-derived host window that OWNS its child controls (per-frame
               registry) and implements the surveil/listen/report protocol.

    PURPOSE (Parasite)
    -------
    Parasite "attaches to" an existing MFC control (CEdit/CStatic/CButton/
    CComboBox/...) via SetWindowSubclass (comctl32) and overlays two features:

      1) Live layout editing. While the app runs, toggle a control into edit
         mode (middle button), then move/resize it with mouse + arrow keys.
         On commit (Enter / left double-click) the new rectangle is written
         back into the *source code* literal that produced it, using the
         __FILE__/__LINE__ captured by the Add.../Open macro. Re-compile to make
         it permanent. This is a DEVELOPMENT-time tool (needs the source file
         present at its compile-time path).

      2) Signal interop with a host (FrameBox) that follows the surveil/listen/
         report protocol (see "HOST CONTRACT" below). Each control type gets a
         default list of natural notifications (button=click, edit=text
         change, combo=selection/edit change, etc.); controls with no useful
         user-change signal get an empty list.

    ZOOM-AWARE EDITING
    ------------------
    All phys<->logical conversions in Parasite (enter_edit, leave_edit ESC restore,
    and commit/source rewrite) use eff_dpi() = MulDiv(real_dpi, zoom_pm, 1000) so
    source rewrites are correct at any zoom level. Parasite holds owner_frame
    (FrameBox*, set via set_owner()) and accesses FrameBox::eff_dpi() through the
    friend class Parasite declaration in FrameBox. Every FrameBox factory that creates
    a Parasite must call p->set_owner(this); omitting it silently falls back to
    GetDpiForWindow (zoom-unaware). Compiled only in _DEBUG builds.

    DEPENDENCIES
    ------------
    Standard MFC (CWnd/CRect/CClientDC) + comctl32 SetWindowSubclass. No other
    libraries. Build: Unicode or MBCS (explicit W-API), x64.

    OWNERSHIP / LIFETIME (read before use)
    --------------------------------------
    - A FrameBox owns the controls created through its Add* member factories:
      each Add* registers { wnd, Parasite } in the frame's per-instance registry.
      ~FrameBox() tears the registry down in reverse order (children first):
      delete Parasite (removes subclass) -> DestroyWindow -> delete wnd.
    - The ROOT frame is created with `new FrameBox`; the function that created it
      must `delete` it (which closes everything). A child frame created by
      add_zone()/add_frame() is owned by its parent's registry and is destroyed
      recursively when the parent is deleted.
    - Parasite itself does NOT own its target; attach() stores a borrowed CWnd*.
    - If a parent window is destroyed first, Windows destroys the child HWND and
      MFC detaches m_hWnd (=> NULL); ~Parasite then skips RemoveWindowSubclass.

    MINIMAL USAGE
    -------------
        // Default-construct, then OpenFrame(app,...) to create + show. The frame is
        // a stack local; ~FrameBox tears everything down at scope exit. Using a
        // member call (not a ctor) lets a FrameBox SUBCLASS override WindowProc etc.
        FrameBox  Top;  Top.OpenFrame(&theApp, 560,275, 1360,875);  // main + self live-edit
        Top.set_margin(5);  // optional: auto-fit frame to children + 5 logical px padding
        CEdit*    e = Top.AddEdit  (10, 10,200, 30);
        CButton*  b = Top.AddButton(10, 50,100, 30, "OK");
        CComboBox*c = Top.AddCombo (10, 90,200, 60, "A,B,C");
        while (::IsWindow(Top)) {
            CWnd* ev = Top.listen(e, b, c);
            if (ev == 0 || ev == b) break;
        }
        // Modal sub-dialog: FrameBox Sub;  Sub.OpenFrame(&Top, ...); disables Top
        // until Sub goes out of scope (close() re-enables Top).
        //
        // Ctrl+Wheel zoom: FrameBox intercepts WM_MOUSEWHEEL+MK_CONTROL via
        // PreTranslateMessage and calls apply_zoom(). Zoom is expressed as zoom_pm
        // (integer x1000; 1000=1.0x; range [500,3000]). eff_dpi() = dpi*zoom_pm/1000
        // is used by rescale_children() and sys_font build. WM_JBZOOM (WM_APP+100)
        // is broadcast to all registry children before MoveWindow so they apply zoom
        // before their OnSize fires. apply_zoom is virtual+protected so subclasses
        // can hook it (e.g. update a title bar) by overriding and calling base first.

    HOST CONTRACT (message protocol; constants below are shared with any host)
    -------------------------------------------------------------------------
    A host (FrameBox / a real MainBox) must:
      - reflect control notifications to the control HWND as WM_PARASITE_CALLBACK
        via ::SendMessage (NOT a direct C++ WindowProc() call - that bypasses
        the subclass chain and never reaches the subclass proc):
            WM_COMMAND : SendMessage(ctrlHwnd, WM_PARASITE_CALLBACK, 0,
                                     (LPARAM)(int)HIWORD(wParam))
            WM_NOTIFY  : SendMessage(nmhdr.hwndFrom, WM_PARASITE_CALLBACK, 0,
                                     (LPARAM)(int)nmhdr.code)
            WM_HSCROLL/WM_VSCROLL when lParam is a control HWND:
                         SendMessage(ctrlHwnd, WM_PARASITE_CALLBACK, 0,
                                     (LPARAM)(int)LOWORD(wParam))
        Reflection runs regardless of listen() state, so a child frame reflects
        its own controls even while a parent frame is the one in listen().
      - register/unregister reporting via WM_PARASITE_SURVEIL (wParam = host CWnd*,
        or 0 to clear) sent to each control HWND at listen() entry/exit. The
        report target is whichever frame called listen() - not necessarily the
        control's parent - so a parent can listen() controls living in a child
        zone with no relay.
      - receive WM_PARASITE_REPORT (wParam = the signaling control's CWnd*) to end
        its modal loop and return that pointer from listen().
*/

#pragma once

class FrameBox;   // forward decl for Parasite::set_owner / Parasite::eff_dpi

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <afxwin.h>
#include <afxcmn.h>
#include <afxdtctl.h>
#include <CommCtrl.h>   // SetWindowSubclass / DefSubclassProc
#include <vector>       // FrameBox per-instance child registry

// ---- Shared message protocol (identical values to 05-box.c++ MainBox) -------
// Keep these in sync with any real host (MainBox) so they interoperate.
#ifndef WM_PARASITE
#define WM_PARASITE          (WM_USER + 1024)
#endif
enum {
    WM_PARASITE_CALLBACK     = WM_PARASITE - 1,   // host -> control: reflected notify
    WM_PARASITE_REPORT       = WM_PARASITE - 2,   // control -> host: a signal fired
    WM_PARASITE_SURVEIL      = WM_PARASITE - 3    // host -> control: set/clear report
};

// Subclass id used by SetWindowSubclass for the subclass procedure.
#define ID_LAYOUT      0xCAFE

// Zoom message: wParam = zoom_pm (zoom x1000; 1000=1.0x). Sent by FrameBox to all registry children
// before rescale_children() so ConBox/TableBox update their internal zoom before MoveWindow fires OnSize.
#ifndef WM_JBZOOM
#define WM_JBZOOM  (WM_APP + 100)
#endif


class Parasite {
public:
    Parasite();
    ~Parasite();

    // Store a borrowed target pointer only. Does not create, subclass, or take
    // ownership. The caller must have already Create()d the control (hidden is
    // fine) and must keep the CWnd object valid longer than this Parasite.
    void attach(CWnd* target);

    // Subclass the target, auto-detect signal codes from the window class,
    // and store file/line/rect for the source rewriter (debug only).
    // Positioning is done by the factory (via Create) before this call.
    // Add.../Open macros capture file/line at the call site.
    bool layout(int x0, int y0, int x1, int y1,
               const char* file, int line);

    // Apply a type-specific init string after open(). Called by Add* factories.
    // nullptr -> no-op. CComboBox: comma-split, trim, AddString each, SetCurSel(0).
    // Others: SetWindowTextW.
    void initialize(const char* init);

    // Signal-code customization. Codes are full signed notification/scroll
    // values carried in WM_PARASITE_CALLBACK lParam; do not truncate with LOWORD.
    // set_signal_code(-1) and clear_signal_codes() both disable signaling.
    void clear_signal_codes();
    bool add_signal_code(int code);
    void set_signal_code(int code);
    void set_signal_codes(const int* codes, int count);
    bool has_signal_code(int code) const;

    // Expose the editing control so this object can go into a host's
    // listen()/surveil() list and be compared against listen()'s return value.
    operator CWnd*() const { return target; }

    // The control's 96 DPI logical rect (set at layout()/enter_edit(), updated on
    // commit). Used by FrameBox to reposition the target on a DPI change.
    const CRect& logical_rect() const { return last_rect; }

    // Cancel any control currently in layout-edit mode (restore via last_rect, no
    // source rewrite). Called by FrameBox before a DPI reposition. No-op in release.
#if defined(_DEBUG)
    static void cancel_edit();
#else
    static void cancel_edit() {}
#endif

    static LRESULT CALLBACK proc(HWND hWnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam, UINT_PTR id, DWORD_PTR ref);

    // Returns the HWND currently in layout-edit mode, or NULL if none (debug only).
    // Hosts should yield Enter/Esc to Parasite (return FALSE from PreTranslateMessage)
    // when pMsg->hwnd matches this value, so Parasite can commit or cancel.
#if defined(_DEBUG)
    static HWND editing_hwnd();
#else
    static HWND editing_hwnd() { return NULL; }
#endif

    // Set the owning FrameBox so Parasite can call eff_dpi() during layout edit,
    // making phys<->logical conversion zoom-aware. Called by FrameBox factories.
    // No-op in Release (edit machinery is compiled out).
    void set_owner(FrameBox* fb);

private:
    // non-copyable: it stores target identity and subclass state.
    Parasite(const Parasite&);
    Parasite& operator=(const Parasite&);

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
    int     eff_dpi() const;   // owner_frame->eff_dpi(), zoom-aware phys<->logical
    static Parasite*& active();
    FrameBox* owner_frame;     // set by set_owner(); null until a FrameBox factory attaches us
#endif

    enum { MAX_SIGNAL_CODES = 12 };

    CWnd*       target;
    int         signal_codes[MAX_SIGNAL_CODES];
    int         signal_count;
    CWnd*       report;       // host registered via WM_PARASITE_SURVEIL
    CRect       last_rect;    // 96 DPI logical rect; set at layout()/enter_edit(), updated on commit;
                              // used for ESC restore, no-op rewrite skip, and DPI reposition (Step 3+)

#if defined(_DEBUG)
    const char* file;
    int         line;
    bool        editing;     // layout edit mode on/off
    // Manual drag state (child controls only; top-level uses OS WM_NCLBUTTONDOWN loop)
    CPoint      drag_start;     // cursor screen pos at WM_LBUTTONDOWN
    CRect       drag_rect;      // window screen rect at WM_LBUTTONDOWN
    int         drag_edge;      // 0=none, HTCAPTION=move, HTxxx=resize corner
#endif
};

// Align text in a window using numpad layout (1=top-left ... 9=bot-right).
// Dispatches by window class: Static/Edit/Button/CComboBox.
// CStatic: SS_ h-align + SS_CENTERIMAGE for v-center (no native v-bottom).
// CEdit  : ES_ h-align + SetRect() y-offset for vertical.
// CButton: BS_ h+v styles. CComboBox: h-align via child Edit only.
void AlignText(CWnd* wnd, int number);

// Rewrite the 4 coordinate literals of the Add.../Open(...) call at file:line.
// `rect` is written verbatim: the caller (leave_edit) passes 96 DPI LOGICAL coords
// so the source stays DPI-invariant. Returns false (and shows a message box) on
// failure. Integer literals only; the call must be on a single line; ANSI/UTF-8
// source (BOM/UTF-16 rejected). Compiled only in _DEBUG builds.
#if defined(_DEBUG)
bool LayOutRewrite(const char* file, int line, const RECT& rect);
#endif

// ============================================================================
// FrameBox -- CWnd host that OWNS its children and runs surveil/listen/report.
// ============================================================================
// A plain CWnd (NOT CFrameWnd: avoids OnDestroy->WM_QUIT and supports a
// WS_CHILD "zone" form). One window class "FrameBox" is shared by all forms;
// the style passed to CreateWindowExW decides top-level vs child vs popup.
//
//   - open(app/owner,...): creates the frame window, positions it, and attaches a
//     self-editing Parasite so the frame's own rect is live-editable and source-
//     rewritable. Two overloads selected by the first argument type:
//     CWinApp* -> top-level main window (attach(app) first); CWnd* -> owned popup
//     modal sub-dialog (disables the owner). Use the OpenFrame(p,...) macro so
//     file/line are injected (see below / file header).
//   - add_*(): child-control factories (AddStatic/AddButton/...). Each creates
//     the control as a child of this frame, sends the DPI-correct system font,
//     attaches a Parasite, registers { wnd, Parasite }, and scales the control
//     from 96 DPI logical source coords to physical pixels at the frame's DPI.
//   - add_zone(): WS_CHILD|WS_EX_CONTROLPARENT container frame drawn inside this
//     frame (replaces the old ParasiteZone). add_frame(): WS_POPUP separate-window
//     child frame. Both are owned by this frame's registry (destroyed recursively).
//   - add_new()/add_asitis(): attach an already-created external CWnd (e.g. a
//     `new ConBox`). Coords-first; the window pointer is the trailing argument.
//     add_new OWNS it (registry destroys+deletes); add_asitis BORROWS it.
//   - attach(CWinApp*): bind as the app main window (m_pMainWnd = this), cleared
//     on close so CWnd teardown never leaves a dangling main window.
//   - listen(): block in a modal loop until one surveilled control posts
//     WM_PARASITE_REPORT (or the timer fires -> returns this, or window closed -> 0).
//   - WindowProc reflects child notifications (WM_COMMAND/WM_NOTIFY/WM_HSCROLL/
//     WM_VSCROLL) to the control HWND as WM_PARASITE_CALLBACK. This runs always, so a
//     child frame reflects its own controls even while a parent runs listen().
//   - WindowProc handles live DPI changes: WM_DPICHANGED (top-level/popup) resizes the
//     frame to the OS-suggested rect; WM_DPICHANGED_AFTERPARENT (WS_CHILD zone) reacts to
//     the parent's DPI change. Both update `dpi`, rebuild `sys_font`, then call
//     rescale_children() to reposition every WS_CHILD entry from its logical rect and
//     reapply the font. Owned popup child frames (AddFrame) get their own WM_DPICHANGED.
//   - PreTranslateMessage intercepts ESC (post WM_CLOSE) and Enter (click focused
//     push button) in both normal and edit mode, matching CDialog. Exceptions:
//       * focused window returns DLGC_WANTALLKEYS -> both keys pass through
//         (e.g. a terminal/ConBox widget that needs raw Esc/Enter),
//       * message target == editing_hwnd() -> returns FALSE so Parasite commits
//         (Enter) / cancels (ESC) the repositioned control.
//     TRAP: the editing_hwnd() guard does NOT restrict ESC/Enter to edit mode;
//     in normal mode they are ALWAYS consumed (except DLGC_WANTALLKEYS).
//
// USAGE  (see MINIMAL USAGE in the file header for a full loop)
//     FrameBox Top;  Top.OpenFrame(&theApp, 560,275, 1360,875);  // stack local
//     CButton* b = Top.AddButton(10,10,100,30, "Close");
//     CWnd* ev = Top.listen(b);
//     ...
//     // ~Top at scope exit: reverse order remove subclass -> DestroyWindow -> delete
class FrameBox : public CWnd {
public:
    FrameBox();
    ~FrameBox();

    // Create the window, position it, and attach a self-editing Parasite. Two
    // overloads selected by the first argument type (pass a concrete typed
    // pointer, never a bare 0/nullptr -- the overloads would be ambiguous):
    //  - CWinApp* form: main window. attach(app) + top-level WS_OVERLAPPEDWINDOW.
    //  - CWnd*    form: modal sub-dialog. owned popup (WS_POPUP|WS_CAPTION|
    //    WS_SYSMENU) then owner->EnableWindow(FALSE); close() re-enables it.
    // Driving construction through a member call (not a ctor) lets a FrameBox
    // subclass be used. Use the OpenFrame(...) macro so file/line are injected.
    // Idempotent: a second call repositions.
    bool open(CWinApp* app,   int x0, int y0, int x1, int y1, const char* file, int line);
    bool open(CWnd*    owner, int x0, int y0, int x1, int y1, const char* file, int line);

    // Bind to the app as its main window (app->m_pMainWnd = this). Cleared in
    // close()/~FrameBox so CFrameWnd-style WM_QUIT posting can never apply.
    void attach(CWinApp* app);

    // Tear down owned children (reverse order), the self-edit subclass, and the
    // window. Called by ~FrameBox; idempotent.
    void close();

    // Set a repeating timer. period > 0: fire every period ms, listen() returns
    // this. period <= 0: cancel. Returns 0 on success or GetLastError() on failure.
    int timer(int period);

    // Auto-fit FrameBox size to its children after every rescale_children().
    // margin_96: extra padding (96 DPI logical px, zoom-scaled) beyond the
    // rightmost/bottommost child edge. Call before or after open().
    // Default -1 = disabled (FrameBox keeps the size set by open/zoom/DPI change).
    void set_margin(int margin_96) { snap_margin = margin_96; }

    // Modal wait until one of the listed controls signals. Returns its CWnd*
    // (or 0 if the window was closed, or this if the timer fired).
    template<class... Args>
    CWnd* listen(Args*... controls) {
        CWnd* list[] = { static_cast<CWnd*>(controls)... };
        return listen_core(static_cast<int>(sizeof...(controls)), list);
    }

    // ---- child-control factories (use the Add* macros, which inject file/line)
    // Each creates the control as a child of THIS frame and registers it.
    CButton*         add_button   (int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
    CEdit*           add_edit     (int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
    CStatic*         add_static   (int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
    CComboBox*       add_combo    (int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
    CListBox*        add_list     (int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
    CRichEditCtrl*   add_richedit (int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
    CListCtrl*       add_listctrl (int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
    CTreeCtrl*       add_treectrl (int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
    CTabCtrl*        add_tabctrl  (int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
    CDateTimeCtrl*   add_datetime (int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
    CMonthCalCtrl*   add_monthcal (int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
    CSpinButtonCtrl* add_spin     (int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
    CSliderCtrl*     add_slider   (int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
    CScrollBar*      add_scrollbar(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
    CIPAddressCtrl*  add_ipaddress(int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
    CHotKeyCtrl*     add_hotkey   (int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
    CProgressCtrl*   add_progress (int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
    CStatusBarCtrl*  add_status   (int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);
    CHeaderCtrl*     add_header   (int x0,int y0,int x1,int y1,const char* f,int ln,const char* init=nullptr);

    // ---- container child frames (owned by this frame's registry) ----
    FrameBox*        add_zone     (int x0,int y0,int x1,int y1,const char* f,int ln); // WS_CHILD area (ex-ParasiteZone)
    FrameBox*        add_frame    (int x0,int y0,int x1,int y1,const char* f,int ln); // WS_POPUP separate window

    // ---- external already-created window attach (coords-first; wnd trailing) ----
    // add_new OWNS wnd (must be heap `new`'d; registry DestroyWindow+deletes it).
    // add_asitis BORROWS wnd (registry removes the subclass only).
    // (0,0,0,0): attach-only -- do not move; read current rect for ESC restore.
    CWnd*            add_new      (int x0,int y0,int x1,int y1,const char* f,int ln, CWnd* wnd);
    CWnd*            add_asitis   (int x0,int y0,int x1,int y1,const char* f,int ln, CWnd* wnd);

protected:
    virtual LRESULT WindowProc(UINT msg, WPARAM wParam, LPARAM lParam);
    virtual BOOL    PreTranslateMessage(MSG* pMsg) override;
    // Lifetime is managed by the owning registry / the creating function's
    // delete; PostNcDestroy must NOT self-delete.
    virtual void    PostNcDestroy() override {}
    DECLARE_MESSAGE_MAP()

protected:
    // Subclass-accessible: read-only DPI/zoom state, and apply_zoom for override hooks.
    int       dpi;      // real monitor DPI; updated on WM_DPICHANGED
    int       zoom_pm;  // zoom x1000 (1000=1.0x); range [500,3000]; Ctrl+Wheel adjusts
    int       eff_dpi() const { return max(1, ::MulDiv(dpi, zoom_pm, 1000)); }
    virtual void apply_zoom(int new_pm, bool cursor_anchor); // Ctrl+Wheel zoom: update zoom_pm, resize, rescale

private:
    // Registry entry: wnd==nullptr means borrowed (add_asitis) -- destroy nothing.
    struct ChildEntry { CWnd* wnd; Parasite* layout; };

    bool      create_window(DWORD exStyle, DWORD style, CWnd* parent, const CRect& rc);
    bool      open_core(CWnd* owner, int x0, int y0, int x1, int y1, const char* file, int line);
    FrameBox* make_child(DWORD exStyle, DWORD style, int x0,int y0,int x1,int y1,const char* f,int ln);
    int       alloc_id() { return next_id++; }
    template<class T> T* finish_child(T* wnd, BOOL ok, int x0,int y0,int x1,int y1,
                                      const char* f,int ln,const char* init);
    CWnd*     attach_external(CWnd* wnd, bool owned, int x0,int y0,int x1,int y1,const char* f,int ln);
    CWnd*     listen_core(int count, CWnd** list);
    void      surveil(int count, CWnd** list, bool on);
    void      rescale_children();   // reposition WS_CHILD entries + reapply sys_font at eff_dpi()
    void      fit_to_children();   // if snap_margin>=0: resize FrameBox to wrap all child rects + margin

    friend class Parasite;   // Parasite::eff_dpi() calls protected FrameBox::eff_dpi()
    std::vector<ChildEntry> registry;     // children this frame owns
    int       next_id;                    // per-instance control id counter
    CWinApp*  app;                        // set by attach(); non-null only for the main window
    CWnd*     parent;                     // owner to re-enable in close() (modal sub-frame only)
    Parasite*  self_layout;                // root self-edit subclass (null for child frames)
    bool      waiting;
    bool      timer_fired;                // set by WM_TIMER while waiting, consumed by listen_core
    CWnd*     event;
    UINT_PTR  timer_id;                   // 0 = no active timer
    int       snap_margin;                // set_margin() value (96 DPI logical px); -1 = disabled
    HFONT     sys_font;                   // DPI-correct message font at eff_dpi(); created in open()/make_child(), freed in close()
};

// ---- Factory macros: member-call form that injects __FILE__/__LINE__ ----
// Usage at call sites: `Top->AddStatic(x0,y0,x1,y1,"text")`. The macro is a
// function-like, case-sensitive textual substitution, so `Top.AddStatic(...)`
// and `this->AddStatic(...)` work identically. Coordinates are ALWAYS the first
// four arguments (the source rewriter relies on this); the optional trailing
// argument is an init string (controls) or the window pointer (AddNew/AddAsItIs).
// EXCEPTION: OpenFrame(p, x0,y0,x1,y1) takes the pointer FIRST, so its coords
// start at the 2nd argument (the rewriter special-cases the "OpenFrame" name).
// _DEBUG: pass __FILE__/__LINE__ so live editing can rewrite the call site.
// Release: pass nullptr/0 -- keeps source paths OUT of the binary (edit/rewrite
// machinery is compiled out; only signal interop remains).
#if defined(_DEBUG)
#define LAYOUT_SRC __FILE__, __LINE__
#else
#define LAYOUT_SRC nullptr, 0
#endif

#define AddStatic(x0,y0,x1,y1,...)  add_static((x0),(y0),(x1),(y1), LAYOUT_SRC, ##__VA_ARGS__)
#define AddButton(x0,y0,x1,y1,...)  add_button((x0),(y0),(x1),(y1), LAYOUT_SRC, ##__VA_ARGS__)
#define AddEdit(x0,y0,x1,y1,...)    add_edit((x0),(y0),(x1),(y1), LAYOUT_SRC, ##__VA_ARGS__)
#define AddCombo(x0,y0,x1,y1,...)   add_combo((x0),(y0),(x1),(y1), LAYOUT_SRC, ##__VA_ARGS__)
#define AddList(x0,y0,x1,y1,...)    add_list((x0),(y0),(x1),(y1), LAYOUT_SRC, ##__VA_ARGS__)
#define AddRichEdit(x0,y0,x1,y1,...) add_richedit((x0),(y0),(x1),(y1), LAYOUT_SRC, ##__VA_ARGS__)
#define AddListCtrl(x0,y0,x1,y1,...) add_listctrl((x0),(y0),(x1),(y1), LAYOUT_SRC, ##__VA_ARGS__)
#define AddTreeCtrl(x0,y0,x1,y1,...) add_treectrl((x0),(y0),(x1),(y1), LAYOUT_SRC, ##__VA_ARGS__)
#define AddTabCtrl(x0,y0,x1,y1,...) add_tabctrl((x0),(y0),(x1),(y1), LAYOUT_SRC, ##__VA_ARGS__)
#define AddDateTime(x0,y0,x1,y1,...) add_datetime((x0),(y0),(x1),(y1), LAYOUT_SRC, ##__VA_ARGS__)
#define AddMonthCal(x0,y0,x1,y1,...) add_monthcal((x0),(y0),(x1),(y1), LAYOUT_SRC, ##__VA_ARGS__)
#define AddSpin(x0,y0,x1,y1,...)    add_spin((x0),(y0),(x1),(y1), LAYOUT_SRC, ##__VA_ARGS__)
#define AddSlider(x0,y0,x1,y1,...)  add_slider((x0),(y0),(x1),(y1), LAYOUT_SRC, ##__VA_ARGS__)
#define AddScrollBar(x0,y0,x1,y1,...) add_scrollbar((x0),(y0),(x1),(y1), LAYOUT_SRC, ##__VA_ARGS__)
#define AddIPAddress(x0,y0,x1,y1,...) add_ipaddress((x0),(y0),(x1),(y1), LAYOUT_SRC, ##__VA_ARGS__)
#define AddHotKey(x0,y0,x1,y1,...)  add_hotkey((x0),(y0),(x1),(y1), LAYOUT_SRC, ##__VA_ARGS__)
#define AddProgress(x0,y0,x1,y1,...) add_progress((x0),(y0),(x1),(y1), LAYOUT_SRC, ##__VA_ARGS__)
#define AddStatus(x0,y0,x1,y1,...)  add_status((x0),(y0),(x1),(y1), LAYOUT_SRC, ##__VA_ARGS__)
#define AddHeader(x0,y0,x1,y1,...)  add_header((x0),(y0),(x1),(y1), LAYOUT_SRC, ##__VA_ARGS__)
#define AddZone(x0,y0,x1,y1)        add_zone((x0),(y0),(x1),(y1), LAYOUT_SRC)
#define AddFrame(x0,y0,x1,y1)       add_frame((x0),(y0),(x1),(y1), LAYOUT_SRC)
#define AddNew(x0,y0,x1,y1,wnd)     add_new((x0),(y0),(x1),(y1), LAYOUT_SRC, (wnd))
#define AddAsItIs(x0,y0,x1,y1,wnd)  add_asitis((x0),(y0),(x1),(y1), LAYOUT_SRC, (wnd))

// Frame open (member-call form). Pointer-first: p is &theApp (main window) or
// &parentFrame (modal sub-dialog); coords follow. The source rewriter treats
// OpenFrame coords as starting at the 2nd argument.
//   FrameBox Top;  Top.OpenFrame(&theApp, 393, -955, 1168, -280);
//   FrameBox Sub;  Sub.OpenFrame(&Top,    10, 10, 100, 100);
#define OpenFrame(p,x0,y0,x1,y1)    open((p), (x0),(y0),(x1),(y1), LAYOUT_SRC)
