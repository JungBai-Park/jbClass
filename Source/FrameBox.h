// FrameBox.h
// Copyright (c) 2026 JungBai Park. All rights reserved.
//
// Parasite: live layout editor via SetWindowSubclass (_DEBUG only).
// FrameBox: CWnd-derived host window; owns child controls via per-instance registry;
//           implements the surveil/listen/report event protocol.
//
// --- Requirements / dependencies ---
//   - MFC (Unicode or MBCS), Windows only. comctl32 SetWindowSubclass. GDI+ for background images.
//   - All Win32/MFC calls use explicit W-suffix; Unicode or MBCS, x64.
//   - Zoom (WM_JBZOOM): zoom_pm (x1000); eff_dpi()=dpi*zoom_pm/1000 drives rescale_children().
//     apply_zoom() is virtual+protected; subclasses override and call base first.
//   - Parasite zoom: requires set_owner(this) from each Add* factory; else falls back to GetDpiForWindow.
//
// --- Ownership / Lifetime ---
//   - FrameBox owns Add*-created controls (per-instance registry).
//     ~FrameBox() tears down in reverse: delete Parasite -> DestroyWindow -> delete wnd.
//   - Root frame: caller `new`s and must `delete` it.
//   - Child frame (add_zone/add_frame): owned by parent registry, destroyed recursively.
//   - Parasite does NOT own its target; attach() borrows a CWnd*.
//
// --- Host contract (message protocol) ---
//   Host reflects control notifications to the control HWND as WM_PARASITE_CALLBACK:
//     WM_COMMAND  -> SendMessage(ctrlHwnd, WM_PARASITE_CALLBACK, 0, (LPARAM)HIWORD(wParam))
//     WM_NOTIFY   -> SendMessage(nmhdr.hwndFrom, WM_PARASITE_CALLBACK, 0, (LPARAM)nmhdr.code)
//     WM_HSCROLL/VSCROLL (lParam=ctrl) -> SendMessage(ctrlHwnd, WM_PARASITE_CALLBACK, 0, LOWORD(wParam))
//   WM_PARASITE_SURVEIL (wParam=CWnd* or 0): register/unregister report target at listen() entry/exit.
//   WM_PARASITE_REPORT  (wParam=signaling CWnd*): signals listen() to return.
//
// --- Usage ---
//     FrameBox Top;  Top.OpenFrame(&App, 560,275, 1360,875);
//     Top.set_margin(5);                          // auto-fit frame to children + 5 px padding
//     CEdit*     e = Top.AddEdit  (10, 10,200, 30);
//     CButton*   b = Top.AddButton(10, 50,100, 30, "OK");
//     CComboBox* c = Top.AddCombo (10, 90,200, 60, "A,B,C");
//     while (::IsWindow(Top)) {
//         CWnd* ev = Top.listen(e, b, c);
//         if (ev == 0 || ev == b) break;
//     }
//     // Background image: Top.set_image(IDR_BG);  or  Top.set_image("bg.png");  (call before or after open)
//     // Solid color:      Top.set_bg_color(RGB(30,30,30));  Top.set_bg_color(); // restore default gray
//     // Modal sub-dialog: FrameBox Sub; Sub.OpenFrame(&Top, ...); disables Top until Sub closes.
//

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
#include <functional>   // std::function for menu action callbacks
#include <string>       // std::string for UTF-8 menu labels

// Forward-declare only; full <gdiplus.h> is in FrameBox.cpp to avoid polluting includers.
namespace Gdiplus { class Image; }

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
//   - add_menu(label, items): appends a popup to the menu bar at runtime (no .rc
//     resource needed). Actions stored in menu_popups; dispatched from WindowProc
//     on WM_COMMAND (lParam==0). modify_menu_label(id, label) renames an item.
//     First add_menu call auto-compensates window height for the new menu bar.
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

    // Show/hide the frame window. open() creates the window hidden; call show()
    // explicitly when the UI is fully built and ready to be presented.
    void show() { if (::IsWindow(m_hWnd)) ShowWindow(SW_SHOW); }
    void hide() { if (::IsWindow(m_hWnd)) ShowWindow(SW_HIDE); }

    // Set a repeating timer. period > 0: fire every period ms, listen() returns
    // this. period <= 0: cancel. Returns 0 on success or GetLastError() on failure.
    int timer(int period);

    // Auto-fit FrameBox size to its children after every rescale_children().
    // margin_96: extra padding (96 DPI logical px, zoom-scaled) beyond the
    // rightmost/bottommost child edge. Call before or after open().
    // Default -1 = disabled (FrameBox keeps the size set by open/zoom/DPI change).
    void set_margin(int margin_96) { snap_margin = margin_96; }

    // Add a top-level popup to the menu bar (no resource file needed). Returns the
    // WM_COMMAND ID assigned to the first non-separator item in this popup, which
    // the caller can use with modify_menu_label() to change the label later.
    // Pass { "", nullptr } as an item to insert a separator.
    int  add_menu(const char* popup_label,
                  std::initializer_list<std::pair<const char*, std::function<void()>>> items);

    // Change the display label of a menu bar item identified by its WM_COMMAND ID.
    void modify_menu_label(int id, const char* label);

    // Set a background image painted stretched to the client area (GDI+ cached resample).
    // Safe to call before or after open(). resource_id: RCDATA (JPEG/PNG/BMP).
    // file: UTF-8 path; image bytes are copied so the file is not locked after return.
    // Returns true on success. On failure (bad id / path / non-image data) the background is
    // set to solid red via set_bg_color so the error is visible, and false is returned.
    bool set_image(int resource_id);
    bool set_image(const char* file);

    // Set a solid background color, replacing any background image.
    // set_bg_color() with no argument restores the default dialog face color (COLOR_BTNFACE).
    void set_bg_color(COLORREF color);
    void set_bg_color();

    // Remove the OS title bar and draw owner-drawn Ubuntu-style round caption
    // buttons in the top-right of the client area. Must be called AFTER open()/
    // open_image() (requires a valid m_hWnd). Empty client area becomes draggable
    // (WM_NCHITTEST -> HTCAPTION). Button geometry scales with eff_dpi() from a
    // 96 DPI baseline, so it tracks Ctrl+Wheel zoom and DPI changes automatically.
    //   option 0: frameless, no buttons
    //   option 1: close only (right slot)
    //   option 2: close + minimize (minimize takes the middle/maximize slot)
    //   option 3: close + minimize + maximize (all three slots)
    //   other   : no-op, returns -1
    // Returns 0 on success, -1 on invalid option, -2 if the window is not created.
    int  frameless(int option);

    // Manage the surveil set. listen() and wait() are decoupled:
    //   listen()            - clear all surveilled controls (sends WM_PARASITE_SURVEIL=0 to each)
    //   listen(a, b, ...)   - ADD controls to the current surveil set (does not replace)
    //   wait()              - block until any surveilled control signals; returns its CWnd*
    //                         (nullptr if window closed, this if timer fired)
    // Typical loop:
    //   Top.listen(edit, button, combo);   // set once before loop
    //   while (::IsWindow(Top)) {
    //       CWnd* ev = Top.wait();
    //       if (!ev || ev == button) break;
    //   }
    // listen() with no args is also called by close() to clean up surveil state.
    void listen() {
        surveil_all(false);
        surveil_list.clear();
    }
    template<class... Args>
    void listen(Args*... controls) {
        CWnd* add[] = { static_cast<CWnd*>(controls)... };
        for (CWnd* w : add) surveil_one(w, true);
        for (CWnd* w : add) surveil_list.push_back(w);
    }
    CWnd* wait();

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
    void      surveil_one(CWnd* w, bool on);   // send WM_PARASITE_SURVEIL to one control
    void      surveil_all(bool on);            // send to all controls in surveil_list
    void      rescale_children();   // reposition WS_CHILD entries + reapply sys_font at eff_dpi()
    void      fit_to_children();   // if snap_margin>=0: resize FrameBox to wrap all child rects + margin

    friend class Parasite;   // Parasite::eff_dpi() calls protected FrameBox::eff_dpi()
    std::vector<ChildEntry> registry;     // children this frame owns
    int       next_id;                    // per-instance control id counter
    CWinApp*  app;                        // set by attach(); non-null only for the main window
    CWnd*     parent;                     // owner to re-enable in close() (modal sub-frame only)
    Parasite*  self_layout;                // root self-edit subclass (null for child frames)
    std::vector<CWnd*> surveil_list;       // controls currently registered for WM_PARASITE_SURVEIL
    bool      waiting;
    bool      timer_fired;                // set by WM_TIMER while waiting, consumed by wait()
    CWnd*     event;
    UINT_PTR  timer_id;                   // 0 = no active timer
    int       snap_margin;                // set_margin() value (96 DPI logical px); -1 = disabled
    HFONT     sys_font;                   // DPI-correct message font at eff_dpi(); created in open()/make_child(), freed in close()

    // Dynamic menu bar. Built by add_menu(); actions dispatched in WindowProc WM_COMMAND.
    // menu_bar ownership: CMenu holds the HMENU. close() calls SetMenu(nullptr) before
    // DestroyWindow so Windows does not double-free the handle on window destruction.
    struct MenuAction {
        int                   id;      // WM_COMMAND id (0 = separator)
        std::string           label;   // UTF-8
        std::function<void()> action;  // null for separator
    };
    struct MenuPopup {
        std::string             label;
        std::vector<MenuAction> items;
    };
    CMenu                   menu_bar;
    std::vector<MenuPopup>  menu_popups;
    int                     next_menu_id;

    // Background image. bg_image is a GDI+ Image* painted stretched in WM_ERASEBKGND.
    // bg_stream keeps the raw bytes alive (GDI+ Image::FromStream does NOT copy them).
    // bg_color/bg_color_set: solid color fallback when bg_image is nullptr.
    // All freed in close(). Default: bg_image=nullptr, bg_color_set=false -> COLOR_BTNFACE.
    bool            load_bg_image(int id);          // resource -> bg_image/bg_stream
    bool            load_bg_file(const char* file); // file -> memory -> bg_image/bg_stream
    void            clear_bg_image();               // free bg_image/bg_stream/bg_cache
    Gdiplus::Image* bg_image;
    IStream*        bg_stream;
    COLORREF        bg_color;
    bool            bg_color_set;

    // Cached, pre-scaled background. WM_ERASEBKGND would otherwise re-stretch the
    // GDI+ image (slow JPEG resample) on every paint; during a zoom that runs many
    // times and stalls the redraw 1-2 s. We stretch once into a screen-compatible
    // bitmap sized to the client, then BitBlt it on every erase. Rebuilt only when
    // the client size changes. Freed in close() / load_bg_image().
    void    rebuild_bg_cache(HDC ref, int w, int h);
    HBITMAP bg_cache;
    int     bg_cache_w;
    int     bg_cache_h;

    // Frameless mode (frameless()). fless_opt: -1 = normal window (OS title bar);
    // 0-3 = frameless with that many caption buttons. All caption-button handling
    // in WindowProc (WM_NCHITTEST/PAINT/MOUSEMOVE/MOUSELEAVE/LBUTTONUP) is gated on
    // fless_opt >= 0, so a normal window is byte-for-byte unaffected.
    // fless_gdip: true if frameless() raised the GDI+ refcount (so close() releases it).
    int   fless_opt;          // -1 = not frameless; 0..3 = button count
    bool  fless_gdip;         // frameless() called gdip_addref()
    bool  fless_hover_close;  // hover state per button (drives the brighten effect)
    bool  fless_hover_max;
    bool  fless_hover_min;
    bool  fless_tracking;     // TrackMouseEvent(TME_LEAVE) is armed
    CRect fless_btn_rect(int from_right) const;  // circle rect in client coords; 0=rightmost(close)
    CRect fless_strip() const;                   // bounding rect of all buttons (+pad) for draw/invalidate
    int   fless_hit(POINT pt) const;             // 1=close,2=minimize,3=maximize,0=none (pt in client coords)
    void  fless_draw(HDC hdc) const;             // double-buffered button paint (WM_PAINT)
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

// Frame open (member-call form). Pointer-first: p is &App (main window) or
// &parentFrame (modal sub-dialog); coords follow. The source rewriter treats
// OpenFrame coords as starting at the 2nd argument.
//   FrameBox Top;  Top.OpenFrame(&App, 393, -955, 1168, -280);
//   FrameBox Sub;  Sub.OpenFrame(&Top, 10, 10, 100, 100);
#define OpenFrame(p,x0,y0,x1,y1)    open((p), (x0),(y0),(x1),(y1), LAYOUT_SRC)

