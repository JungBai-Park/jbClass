// ConBox.h
// Copyright (c) 2026 JungBai Park. All rights reserved.
//
// Portable terminal control (CWnd-derived). Like wt.exe it displays the console
// output of a terminal child program and forwards keystrokes to it. Internally a
// cols x rows cell-grid screen buffer plus scrollback.
//
// Portability unit is just ConBox.h + ConBox.cpp. To "use" it this header alone
// suffices; read ConBox.cpp only when modifying behavior (its top-of-file table of
// contents lets you find one function without reading the whole file).
//
// --- Requirements / dependencies ---
//   - MFC (Unicode or MBCS), Windows only. Depends on afxwin.h. All Win32/MFC calls use explicit W-suffix.
//   - start() (ConPTY child) needs Windows 10 1809 (build 17763)+. Pure-view use does not.
//   - imm32.lib (Korean IME) is auto-linked by ConBox.cpp via #pragma comment (no host setup).
//   - Host app must enable DPI awareness (PerMonitorV2 via manifest). ConBox is per-monitor aware:
//     fonts/cell metrics use GetDpiForWindow, rebuilt automatically on monitor DPI change.
//   - Zoom (WM_JBZOOM from FrameBox): sets zoom_pm (x1000); eff_dpi()=box_dpi*zoom_pm/1000 drives
//     build_font() and to_px() so fonts/geometry scale with zoom like a DPI change.
//   - Host-close safety (WM_JBCLOSEQUERY, answered internally): if this ConBox is a FrameBox
//     registry child with a live start()'d process, closing the host hides it and waits (up to a
//     few seconds, force-killing via terminate() if needed) before the host actually closes.
//     Automatic; no host code required. See terminate()/is_running() below.
//   - Register callbacks (set_title_cb, set_titlebar_color_cb, set_exit_callback, set_input_sink,
//     set_resize_sink) BEFORE setup()/setup_from_ini(): set_titlebar_color_cb in particular must be
//     registered first, or a freshly created default INI (no file existed yet) omits [titlebar]
//     (see CreateDefaultIni/set_titlebar_color_cb).
//   - Mouse: local drag-selection/overlay scrollbar by default. When the child turns on xterm mouse
//     tracking (?1000/?1002/?1003 with ?1006 SGR encoding, as Claude Code / vim / htop do), clicks,
//     drags and wheel notches go to the CHILD instead, the overlay scrollbar is hidden, and the
//     child scrolls its own view -- holding Shift forces the local behavior back (xterm/wt.exe
//     convention). Right click always pastes locally. OSC 52 lets the child read/write the Windows
//     clipboard (that is how a TUI copies a selection it drew itself). All automatic; no host code.
//   - OSC 8 hyperlinks: text the child wraps in OSC 8 is underlined; Ctrl+Click opens it via
//     ShellExecuteW. BEL (\a): response is bell_style (INI, default 1=audible); see osc8()/vt_feed().
//     Both automatic; no host code required.
//   - Every string API takes UTF-8 (const char*) so C++ string literals pass directly.
//   - Self-contained: includes its own headers, does not depend on a precompiled header.
//   - Save .h/.cpp as ASCII (comments are ASCII-only) so encoding is unambiguous.
//
// --- Usage (inside the host parent window) ---
//     ConBox box;                            // usually held as a member
//     box.set_efont("Consolas", 13, "B");     // (optional) font; omitted = defaults
//     box.open(parent, 0, 0);                  // create child window at (left,top); size from cfg
//     box.print("hello\n");                   // output (UTF-8) flows into the cell grid
//
// Two ways to use ConBox:
//  (1) Pure terminal view: host feeds bytes via print() and takes keystroke bytes via
//      set_input_sink() (raw mode: no local echo; keys encoded to VT/UTF-8). Works with any
//      byte source (file/socket/host-managed process).
//  (2) ConPTY child runner: one start(cmdline) call spawns a child (cmd/powershell/python REPL)
//      and auto-wires its console I/O (console size taken from grid_size(); internally
//      reuses the input/resize sinks). Child output is polled by an internal timer into print().
// With no sink and no start(), it is a read-only viewer of print() output.
//
#pragma once

// Zoom message shared with FrameBox/TableBox (same numeric value in each module header).
#ifndef WM_JBZOOM
#define WM_JBZOOM  (WM_APP + 100)
#endif

// Close-query message shared with FrameBox (same numeric value in each module header, same
// pattern as WM_JBZOOM). Sent by FrameBox to every WS_CHILD registry entry on WM_CLOSE.
// A handler returns non-zero to mean "not ready yet" (FrameBox hides instead of destroying);
// an unhandled default (DefWindowProc, e.g. plain controls) returns 0 = ready. No header
// dependency between FrameBox and ConBox is introduced by this.
#ifndef WM_JBCLOSEQUERY
#define WM_JBCLOSEQUERY  (WM_APP + 101)
#endif

// ConPTY (CreatePseudoConsole/HPCON/ResizePseudoConsole) is declared only on Win10 1809
// (build 17763, RS5)+. afxwin.h pulls in windows.h, so set the minimum version before it.
// #ifndef so a host that picked a higher target (via targetver) is respected.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00            // Windows 10
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006       // NTDDI_WIN10_RS5 (ConPTY minimum)
#endif

// Emulator identity reported to the child in the XTVERSION reply (CSI > 0 q -> DCS > | ... ST).
// Deliberately the real name: impersonating a known emulator (XTerm etc.) would make TUIs assume
// capabilities ConBox lacks (sixel, XTGETTCAP, kitty keyboard) and then draw garbage. Bump on
// behavior changes a child could reasonably branch on.
#define CONBOX_NAME     "ConBox"
#define CONBOX_VERSION  "1.0"

#include <afxwin.h>   // MFC core (CWnd, CDC, CFont); also pulls in windows.h (ConPTY HPCON etc.)
#include <vector>
#include <deque>
#include <string>
#include <unordered_map>

// One screen cell. A double-width glyph (Korean/CJK) occupies a lead cell (CELL_WIDE set)
// and the next cell is its trail (ch=0, skipped by the renderer). An empty cell is ch=L' '.
// Layout: ch(2)+flags(1)+link_id(1)+fg(4)+bg(4) = 12 bytes (was 16 with bool wide at the end).
struct CharInfo {
    wchar_t  ch;     // UTF-16 char; 0 = trail cell of a wide glyph (render skips it)
    uint8_t  flags;  // bit field: CELL_WIDE | CELL_BOLD | CELL_ITALIC | CELL_UNDERLINE | CELL_STRIKE | CELL_BLINK | CELL_DOUBLE
    // 0 = no hyperlink; else a 1-based index into ConBox::link_table (OSC 8, see dispatch_osc/osc8).
    // Occupies the byte that used to be pure alignment padding, so struct size is unchanged.
    uint8_t  link_id = 0;
    COLORREF fg;
    COLORREF bg;
};

// CharInfo::flags bit masks.
enum {
    CELL_WIDE      = 0x01,   // lead cell of a double-width (Korean/CJK) glyph
    CELL_BOLD      = 0x02,   // SGR 1
    CELL_ITALIC    = 0x04,   // SGR 3
    CELL_UNDERLINE = 0x08,   // SGR 4; drawn as a 1px line at cell bottom
    CELL_STRIKE    = 0x10,   // SGR 9; drawn as a 1px line at cell middle
    CELL_BLINK     = 0x20,   // SGR 5/6; glyph toggled by BLINK_TIMER
    CELL_DOUBLE    = 0x40,   // SGR 8/28; 2x-size glyph, always bold (bleeds right/upward)
    // No CELL_DIM: SGR 2 (faint) is baked into CharInfo::fg by put_char (see DimColor), so the
    // renderer has no dim case and dim survives copy/export like any other color.
};

typedef std::vector<CharInfo> Row;

// One [triggers] rule: when the current line's text from column 0 up to the cursor ends with
// `match`, `send` is sent to the child verbatim (see check_triggers()). cool_ms: <0 = fire once
// then stay inactive, 0 = fire every match, >0 = minimum ms between fires. `active`/`last_fire` are
// runtime state, not INI input.
struct Trigger {
    std::string match, send;
    int         cool_ms;
    bool        active = true;    // cool_ms < 0 only: false once it has fired
    ULONGLONG   last_fire = 0;    // cool_ms > 0 only: GetTickCount64() at the last fire
};

class ConBox : public CWnd
{
public:
    // Returned by grid_size(). rows, cols in ROW, COL order.
    struct GridSize { int rows, cols; };

    ConBox();
    virtual ~ConBox();

    // Create the child window at (left, top) inside parent. Pixel size is computed from the
    // cfg_rows/cfg_cols set by setup_from_ini() (defaults: 96 cols x 32 rows). If cfg_cmdline is
    // non-empty, start() is called automatically after the window is created. Call after
    // setup_from_ini() (fonts/margins must be set before the first layout). The host can then
    // call GetClientRect() on the ConBox CWnd to get the actual pixel size for its own layout.
    void open(CWnd* parent, int left = 0, int top = 0);

    // Set the English font. size is in points. option is an attribute string and may be omitted
    // (see set_builtin_glyphs note below and ConBox.cpp ParseFontOpts for the grammar).
    void set_efont(const char* name, float size, const char* option = 0);

    // Set the Korean font (same arg rules as set_efont). If size <= 0, "match mode" turns on:
    // Korean is rasterized to the same pixel (em) height as English (the size value is ignored) and
    // line height tracks English, which keeps box-drawing vertical lines connecting. If size > 0,
    // match mode is off: Korean keeps that size and line height is the max of the two fonts.
    // Default (nothing set) is match mode on.
    void set_kfont(const char* name, float size, const char*option = 0);

    // Pad (or trim) each side of every cell by a pixel amount, applied after font metrics are computed.
    // Positive adds margin on that side; negative eats into it. The font size is unchanged -- only the
    // cell box grows/shrinks and the glyph shifts inside it:
    //   left   : glyph moves right by 'left' (left margin appears before it); right pads the cell's right.
    //   top    : glyph moves down by 'top' (top margin appears above it); bottom pads the cell's bottom.
    // So cell_w += left+right, cell_h += top+bottom. Built-in block/box glyphs ignore the glyph offset
    // and still fill the whole (padded) cell to stay gap-free. Persists until the next font change;
    // calling after open() recomputes cols/rows.
    void adjust(int left, int top, int right, int bottom);

    // Choose how many box/block characters ConBox draws as shapes itself instead of via the font.
    // Font block glyphs often leave gaps between adjacent cells (especially vertically); drawing the
    // cell directly is gap-free and font-independent.
    //   0 = off (everything via the font)
    //   1 = block elements U+2580..259F (halves/quadrants/full) only (default)
    //   2 = also box lines: single orthogonal/junctions, rounded corners (drawn square),
    //       diagonals, and pure double lines.
    // Mixed single/double, dashed, heavy, and shaded lines are always font-drawn.
    void set_builtin_glyphs(int level);

    // Print text at the cursor into the cell grid. Autowraps past cols; scrolls (top line -> scrollback)
    // past the bottom. text is UTF-8 and is parsed as VT/ANSI (escape sequences, \r \n \b \t). New
    // output forces a scroll to the bottom. Parser state persists across calls (chunked output is safe).
    void print(const char* text);

    // Default foreground / background color for subsequent output (child SGR may override; SGR
    // 0/39/49 reset to these). Also the basis for the cursor block blend.
    void set_fg_color(COLORREF fg);
    void set_bg_color(COLORREF bg);

    // Cursor color = blend of default bg:fg at this weight ratio. Default bg 4 : fg 6 (leans fg).
    // Normalized by the sum, so (4,6) == (40,60). Ignored if the sum is 0. (I-beam draws pure fg, not
    // this blend; this blend is the block/underline color and the IME composing box outline.)
    void set_cursor_blend(int bg_weight, int fg_weight);

    // Cursor blink interval (ms); 0 (or negative) = follow the system caret rate (GetCaretBlinkTime);
    // if the system has blink disabled (INFINITE) the cursor stays always on. Positive = fixed rate.
    void set_cursor_blink(int interval_ms);

    // Cursor shape constants for set_cursor(). Odd = blinks, even = fixed.
    enum CursorType {
        CURSOR_DEFAULT        = 0,   // -> CURSOR_BLINKING_UNDER (classic conhost.exe default)
        CURSOR_BLINKING_BLOCK = 1,
        CURSOR_FIXED_BLOCK    = 2,
        CURSOR_BLINKING_UNDER = 3,
        CURSOR_FIXED_UNDER    = 4,
        CURSOR_BLINKING_IBEAM = 5,
        CURSOR_FIXED_IBEAM    = 6,
    };

    // Set the cursor shape. Pass a CursorType constant or its raw int (0-6). Block and underline hide
    // while a Korean IME composition is active; the I-beam stays visible. Korean (2-cell) cursors
    // render 2 cells wide.
    void set_cursor(int type);

    // Inner padding (px); glyphs draw only inside it. CSS-shorthand omission of negative sides:
    // left<0 follows top, bottom<0 follows top, right<0 follows the resolved left.
    // Default 10 on all sides. Calling after open() recomputes the grid.
    void set_margin(int top, int left = -1, int bottom = -1, int right = -1);

    // Current screen grid size (rows, cols). Valid after open().
    GridSize grid_size() const { return { rows, cols }; }

    // Top-level window start position from the start_x/start_y INI keys (host-consumed: ConBox
    // does not position any window itself). CW_USEDEFAULT if the key is absent or empty -- the
    // host should pass that straight through to CreateWindow so the system picks the position.
    // Valid after setup()/setup_from_ini(); call before creating the host's top-level window.
    int config_start_x() const { return cfg_start_x; }
    int config_start_y() const { return cfg_start_y; }

    // Load settings from an INI file (section-agnostic key matching). path is UTF-8; a relative
    // path is resolved against the EXE directory (not the working directory). nullptr defaults to
    // "ConBox.ini". If the file does not exist, it is created with compiled-in defaults and a
    // notification is appended to ini_msg (printed by open() once the window exists; multiple
    // calls accumulate their messages instead of overwriting each other). Settings stay at
    // constructor defaults. If the file exists but cannot be opened, the same deferred print()
    // path applies. Call before open() so fonts/margins are set before the first layout.
    // Calling this (or setup()) more than once layers settings: the first call resolves every key
    // to its compiled-in default unless the file overrides it; each later call only touches keys
    // present in that file and leaves every other setting at whatever the previous call resolved
    // -- e.g. setup_from_ini(global) then setup_from_ini(local) makes local an override layer on
    // top of global, not a second independent reset. A key must be entirely absent from a file to
    // inherit the previous layer; writing it with the same value as the default still pins it.
    void setup_from_ini(const char* path = nullptr);

    // Apply INI-format settings from a string. contents is UTF-8 with \n line endings (no BOM).
    // Useful for programmatic injection of settings without a file. Same layering rule as
    // setup_from_ini(): the first call ever made (to either function) establishes compiled-in
    // defaults for absent keys; subsequent calls keep the previous layer's resolved value for any
    // key the new contents does not mention.
    // Two more problem classes append to ini_msg the same way as setup_from_ini()'s own messages
    // (deferred print() once the window exists): a line with real content but no '=' at all, and a
    // "key=value" line whose key nothing in setup() recognizes (e.g. a typo) -- both previously
    // failed or applied with no feedback.
    void setup(const char* contents);

    // Export all content (scrollback + screen) to a series of EMF vector files in dir (UTF-8 path).
    // Files are named ConBox000.emf, ConBox001.emf, ...
    // Lines per page is read from the INI key lines_per_paper (default 50).
    // All cell attributes are preserved: fg/bg colors, bold, italic, underline, strikethrough,
    // double-size. Blink cells are exported in the visible (on) state (static capture).
    // If the first row of a page has CELL_DOUBLE glyphs, an extra blank row is prepended so the
    // 2x upward bleed is not clipped. Glyphs are stored as text records in the EMF (vector, not
    // bitmap); the original fonts must be installed on the machine where the EMF is opened.
    bool save_emf(const char* dir);   // returns true if at least one EMF file was written

    // Save all content (scrollback+screen) to a PDF file via the system PDF printer.
    // path: UTF-8 output file path (e.g. "C:\\out.pdf"). The output PDF is written directly
    // without showing a Save dialog (DOCINFO.lpszOutput). Returns false if no PDF printer is
    // found (a printer whose name contains "PDF", e.g. "Microsoft Print to PDF") or if the
    // print job fails. Rendering is identical to save_emf (same cell loop, MM_ANISOTROPIC
    // viewport scaling so cell/font sizes match the screen appearance at the correct DPI).
    bool save_pdf(const char* path);

    // Extract all content (scrollback + screen) as plain UTF-8 text lines.
    // Returns one std::string per row (scrollback first); null-terminated, no trailing newline.
    // All colors and attributes stripped; trailing spaces trimmed per line.
    // Trail cells (put_char writes ch=0, flags=attr -- no CELL_WIDE on trail, only on lead) are
    // skipped by position (lead+1), not by flag. Cell consumption per char:
    //   Normal Korean (CELL_WIDE):              2 cells (lead + trail).
    //   Double English (CELL_DOUBLE):           2 cells (lead + 1 blank from 2x advance).
    //   Double Korean  (CELL_WIDE|CELL_DOUBLE): 4 cells (lead + trail + 2 blanks from 4x advance).
    std::vector<std::string> get_text_lines() const;

    // Save get_text_lines() to a plain text file (UTF-8 with BOM, CRLF line endings).
    // path: UTF-8 output file path. Returns false if the file could not be created.
    bool save_text(const char* path);

    // Start or stop raw child-output logging. file_name (UTF-8 path): open/create the file and begin
    // logging; each raw byte from the child (VT codes intact, no CR/LF conversion, no encoding
    // conversion) is written as received by pump(). nullptr or empty string: close the log file.
    // Returns 0 on success, GetLastError() code on failure. May be called before open() (no window
    // needed); actual bytes start flowing once the child is running (after start()).
    int save_log(const char* file_name = 0);
    bool is_logging() const { return log_file != INVALID_HANDLE_VALUE; }

    // Set the input sink (raw/terminal mode). Once set, ConBox does not locally edit/echo; it encodes
    // keys/chars to VT sequences and UTF-8 bytes and pushes them to sink (for the child's stdin). user
    // is an opaque context returned on each call. nullptr reverts to read-only viewer mode.
    // (start() registers itself via this API internally.)
    void set_input_sink(void (*sink)(const char* bytes, int len, void* user), void* user);

    // Set the resize sink. When the grid (rows/cols) changes, the new size is reported here (to match
    // the child's pseudo-console). nullptr = no notification. (start() registers itself here to wire
    // ResizePseudoConsole.)
    void set_resize_sink(void (*sink)(int rows, int cols, void* user), void* user);

    // Set the window-title callback. Fired whenever the child sends an OSC 0/2 "set title" sequence
    // (the xterm convention most shells/CLIs use); title (UTF-8) is the decoded text, with no window
    // of its own applied -- ConBox has no title bar, so applying it (e.g. SetWindowTextW on the
    // host's top-level frame) is entirely up to the host. nullptr = no notification (default).
    void set_title_cb(void (*cb)(const char* title));

    // Set the title-bar color callback. Fired once from open() -- after every setup()/
    // setup_from_ini() call the host made beforehand has resolved titlebar_caption/text/border, so
    // a multi-layer setup (e.g. a global INI then a local override INI) notifies the host only once,
    // with the final colors -- and again immediately on registration, so the callback never misses
    // the current values if registered after open() already ran. Colors are the
    // titlebar_caption/titlebar_text/titlebar_border INI keys. ConBox has no title bar of its own;
    // applying these (DwmSetWindowAttribute with DWMWA_CAPTION_COLOR/DWMWA_TEXT_COLOR/
    // DWMWA_BORDER_COLOR on the host's top-level frame, Windows 11 22000+ only) is entirely up to
    // the host. Any color not set in the INI is CLR_INVALID -- leave that attribute at the system
    // default. nullptr = no notification (default).
    // Register this (and every other set_*_cb/set_*_sink/set_exit_callback) BEFORE calling
    // setup()/setup_from_ini(): CreateDefaultIni only writes the [titlebar] block into a freshly
    // created INI when this callback is already registered at that point (see setup_from_ini).
    void set_titlebar_color_cb(void (*cb)(COLORREF caption, COLORREF text, COLORREF border));

    // === ConPTY child runner (optional) ===
    // Using this group makes ConBox spawn a child and auto-wire its I/O. If start() is never called,
    // the ConPTY members stay dormant and ConBox is a pure terminal view.

    // Spawn cmdline (UTF-8, e.g. "python.exe", "cmd /c dir") under ConPTY. Console size is taken from
    // grid_size(). Internally wires the input/resize sinks to itself and starts an output
    // polling timer. Restarts if already running. Returns true on success. Call after open() (the
    // polling timer needs the window). The child's working directory is cfg_work_dir (work_directory
    // INI key) if set -- a relative value is resolved against the EXE directory, same as an INI
    // path -- otherwise the child inherits this process's current directory (CreateProcessW default).
    // The no-arg overload uses cfg_cmdline set by setup_from_ini() (also called automatically by
    // open() when cfg_cmdline is non-empty). On ANY failure along the way (ConPTY pipe/attribute
    // setup or the final CreateProcessW) prints a diagnostic to the screen before returning false, so
    // a bad cmdline (typo, missing exe, no permission, ...) is visible, not silently inert; where the
    // failing API sets one, the message includes its GetLastError()/HRESULT system text.
    bool start();
    bool start(const char* cmdline);

    // Send bytes (UTF-8/VT) to the child stdin. Normally called by the input-sink path.
    void write(const char* data, int len);

    // ResizePseudoConsole to rows x cols. Normally called by the resize-sink path.
    void resize(int rows, int cols);

    // Tear down child/PTY/pipes/polling timer. Idempotent.
    void stop();

    // Force-kill the child if still alive (::TerminateProcess), then stop(). Idempotent (a no-op
    // if no child is running). Last-resort cleanup for a child that ignores ClosePseudoConsole.
    void terminate();

    bool is_running() const;

    // Register a callback fired once on the child's natural exit (e.g. shell exit). At callback time
    // cleanup is done (is_running()==false, so the callback may start() again). Not fired for an
    // explicit stop().
    void set_exit_callback(void (*cb)());

    // Read pending child output and feed it to print(). Normally driven by the internal timer.
    void pump();

protected:
    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC* dc);
    afx_msg void OnSize(UINT type, int cx, int cy);
    // Tears down child/polling timer first so polling never touches a dying window.
    afx_msg void OnDestroy();
    afx_msg void OnChar(UINT ch, UINT rep, UINT flags);
    afx_msg void OnKeyDown(UINT vk, UINT rep, UINT flags);
    afx_msg UINT OnGetDlgCode();
    afx_msg BOOL OnMouseWheel(UINT flags, short zDelta, CPoint pt);
    // Mouse drag selection: down anchors, drag extends, up copies to clipboard. A button-down inside the
    // overlay-scrollbar gutter is diverted to thumb drag / track paging instead of selection.
    afx_msg void OnLButtonDown(UINT flags, CPoint pt);
    afx_msg void OnLButtonDblClk(UINT flags, CPoint pt);
    afx_msg void OnLButtonUp(UINT flags, CPoint pt);
    afx_msg void OnMouseMove(UINT flags, CPoint pt);
    afx_msg void OnMouseLeave();   // clears gutter hover so the overlay scrollbar can fade
    // Right click pastes the clipboard to the child stdin.
    afx_msg void OnRButtonDown(UINT flags, CPoint pt);
    afx_msg void OnMButtonDown(UINT flags, CPoint pt);   // forwarded to the child while mouse_reporting()
    afx_msg void OnMButtonUp(UINT flags, CPoint pt);
    // Drag-and-drop: sends dropped file paths to child stdin (quoted if the path contains spaces).
    afx_msg void OnDropFiles(HDROP hdrop);
    afx_msg void OnTimer(UINT_PTR id);
    afx_msg void OnSetFocus(CWnd* old_wnd);
    afx_msg void OnKillFocus(CWnd* new_wnd);
    // Korean IME. In terminal mode only committed text is sent to the child; the in-progress
    // composition is held in comp_str and drawn by ConBox in the cursor cell (system inline is
    // suppressed by OnImeComp returning 0).
    afx_msg LRESULT OnImeStart(WPARAM w, LPARAM l);
    afx_msg LRESULT OnImeComp(WPARAM w, LPARAM l);
    afx_msg LRESULT OnImeEnd(WPARAM w, LPARAM l);
    // IME state change (e.g. Korean/English toggle); updates the cursor width immediately.
    afx_msg LRESULT OnImeNotify(WPARAM w, LPARAM l);
    // Live DPI change. The WS_CHILD case (usual AddNew) is handled entirely in OnSize: when the host
    // (FrameBox) resizes us to a new-DPI rect, OnSize sees GetDpiForWindow != box_dpi and rebuilds the
    // fonts while PRESERVING the logical grid (cols/rows) -- a DPI change only rescales cell pixels, so
    // the child PTY must NOT be resized (that makes the shell re-emit -> duplicated/lost content). This
    // is order-independent (no reliance on WM_DPICHANGED_*PARENT delivery timing). OnDpiChanged is only
    // for a top-level/popup ConBox: it applies the OS-suggested rect (lParam), and the resulting OnSize
    // does the font rebuild + grid preservation.
    afx_msg LRESULT OnDpiChanged(WPARAM w, LPARAM l);
    afx_msg LRESULT OnJbZoom(WPARAM w, LPARAM l);  // WM_JBZOOM: update zoom_pm and flag next OnSize
    // WM_JBCLOSEQUERY: non-zero ("not ready") while a child process is running; starts CLOSE_TIMER
    // on the first call. See the "Host-close safety net" block below for the rest of the sequence.
    afx_msg LRESULT OnCloseQuery(WPARAM w, LPARAM l);
    DECLARE_MESSAGE_MAP()

private:
    // Build one font into font, leaving the resulting LOGFONT in lf_out and width ratio in width_pct.
    void build_font(CFont& font, LOGFONTW& lf_out, int& width_pct, const char* name, float size, const char* option);

    // Build the base font + its bold/italic/bold-italic variants from the stored spec members
    // (efont_*/kfont_*). Used by set_efont/set_kfont, open(), and DPI changes. The double-size
    // variant and the kfont match-height sync are done in calc_cell_size (after these run).
    void build_efont();
    void build_kfont();

    // Build efont_fallback (fallback_font_name) at efont_lf's pixel height. Called from build_efont
    // so it tracks efont's DPI/zoom/point-size changes automatically.
    void build_fallback_font();

    // True if (wide ? kfont : efont) has a real (non-.notdef) glyph for ch. Result is cached; the
    // cache is cleared in set_efont/set_kfont.
    bool HasGlyph(bool wide, wchar_t ch);

    // Returns primary if HasGlyph(wide, ch), else the symbol fallback font (the double-size variant
    // if dbl). Single substitution point shared by OnPaint/save_emf/save_pdf.
    CFont* PickFont(CFont* primary, bool wide, bool dbl, wchar_t ch);

    // Rebuild both fonts at the window's current DPI, recompute the grid, and repaint. Called from
    // the WM_DPICHANGED / WM_DPICHANGED_AFTERPARENT handlers.
    void relayout_for_dpi();

    // Effective DPI = real monitor DPI * zoom_pm / 1000. Used for all font/cell/scrollbar sizing.
    int  eff_dpi()  const { int d = box_dpi > 0 ? box_dpi : 96; return max(1, ::MulDiv(d, zoom_pm, 1000)); }
    // Scale a 96 DPI pixel constant to the effective DPI (identity at 96, zoom=1.0).
    int  to_px(int base96) const { return ::MulDiv(base96, eff_dpi(), 96); }

    // Ensure a font spec exists for any font the host never set (English Cascadia Mono 12pt,
    // Korean Malgun Gothic match-mode). Specs only; the GDI build happens in build_efont/build_kfont.
    void apply_default_fonts();

    // Measure the fixed-grid cell width/height/baseline from the current fonts.
    // (Algorithm detail is in calc_cell_size's own comment.)
    void calc_cell_size();

    // Derive cols/rows that fit the client area.
    void update_metrics();

    // Recompute origin_x/origin_y: the drawing offset that centers the grid in the client area.
    void recalc_origin();

    // Grow the window (never shrink) so the preserved grid fits at the current DPI; used on DPI change.
    void snap_to_grid();

    void scroll_to_bottom();

    // Clamp view_top and pin it on the alt screen / when there is no scrollback. (No native scrollbar;
    // the overlay bar below is what the user sees.) Kept as update_scrollbar() since many sites call it.
    void update_scrollbar();

    // Overlay scrollbar helpers (auto-hide/fade). geometry: compute the gutter (track) and thumb rects
    // from the current view; false when nothing to scroll (no scrollback or alt screen), and true with
    // an EMPTY thumb while the child is mouse-reporting -- it owns the scroll position then, so only
    // "scroll by one notch" gestures (arrows / track halves) are meaningful and they get forwarded to
    // the child instead of moving view_top. show: make the bar fully opaque and (re)start the fade
    // timer. draw: AlphaBlend it into the back buffer in OnPaint.
    bool sbar_geometry(CRect& track, CRect& thumb) const;
    CRect sbar_gutter() const;
    bool sbar_hit(CPoint pt, CRect& track, CRect& thumb) const;
    void sbar_track_hover(CPoint pt);
    void sbar_show();

    // Perform one unit of an overlay-scrollbar gesture -- one arrow step or one track page -- exactly
    // as a single press does: local view_top move, or forwarded wheel notches in thumbless mode
    // (see sbar_geometry). Shared by OnLButtonDown (first press) and the SBAR_REPEAT_TIMER tick, so
    // holding an arrow/track keeps moving instead of firing once. A no-op once view_top/notches can no
    // longer move (e.g. already at an end), so a held button harmlessly idles there.
    void sbar_fire(bool up, bool arrow, CPoint pt);
    void draw_overlay_scrollbar(CDC& dc);

    // VT parser: feed one char (or control byte) into the state machine. Interprets ESC/CSI/OSC and
    // routes other chars to put_char. State is a member, so it persists across print() chunks.
    void vt_feed(wchar_t wc);

    // Dispatch a completed CSI sequence (final byte fin): cursor moves / erases / SGR etc.
    void dispatch_csi(wchar_t fin);

    // Parse the accumulated OSC payload (osc_buf, "Ps;Pt") on BEL/ST. Ps 0 or 2 (set window title,
    // the xterm convention Claude Code and most shells use) decodes Pt to UTF-8 and forwards it via
    // title_cb; Ps 52 goes to osc52(). Any other Ps (icon name, color queries, etc.) is dropped --
    // ConBox has no representation for those. A payload with no ';' is ignored.
    void dispatch_osc();

    // OSC 52 (clipboard access by the child), arg = "<targets>;<payload>" (targets c/p/s... ignored;
    // everything maps to the single Windows clipboard).
    //   payload "?"  : query -- reply ESC ] 52 ; c ; <base64> ST with the clipboard's current text.
    //   payload else : base64 of UTF-8 text to put on the clipboard (empty payload clears it).
    // This is how a TUI that owns the mouse (see mouse_report) copies a selection it made itself:
    // it has no OS clipboard access of its own and asks the terminal to do it.
    void osc52(const std::wstring& arg);

    // OSC 8 (hyperlink), arg = "<params>;<URI>" (params, e.g. "id=xxx" for multi-line grouping, is
    // ignored). Empty URI closes the current link (cur_link_id = 0); non-empty opens one, reusing an
    // existing link_table entry with the same URI if there is one. put_char() stamps cur_link_id onto
    // every cell written while a link is open; OnPaint underlines those cells and Ctrl+Click on one
    // opens it via ShellExecuteW (see OnLButtonDown). link_table is capped at 255 entries (CharInfo::
    // link_id is 1 byte); once full, further distinct links print as plain (non-clickable) text.
    void osc8(const std::wstring& arg);

    // Write one glyph at the cursor (autowrapping first if past cols). A wide glyph fills the lead
    // cell and sets the next to trail (ch=0). Applies the current SGR color/attributes.
    void put_char(wchar_t wc);

    // [triggers]: compare the current line's text (column 0..cursor, UTF-8) against cfg_triggers by
    // suffix; fires at most one eligible rule's `send` per call. Called once at the end of print()'s
    // chunk. Short-circuits immediately when active_trigger_count is 0 (no rules, or every one-shot
    // rule already fired) so a quiet terminal with no/exhausted triggers pays no per-chunk cost.
    void check_triggers();

    // Move the cursor down a line. At the scroll region's bottom (scroll_bot) this scrolls the region
    // (scroll_up_region); otherwise it advances one line without leaving the screen.
    void line_feed();

    // Scroll region [scroll_top, scroll_bot] up n lines (top lines lost, blanks at the bottom). Only on
    // the main screen with scroll_top==0 are the displaced lines preserved into scrollback.
    // (Used by LF at the bottom and by SU (CSI S).)
    void scroll_up_region(int n);

    // Scroll region [scroll_top, scroll_bot] down n lines (bottom lines lost, blanks at the top).
    // (Used by RI at the top and by SD (CSI T); does not touch scrollback.)
    void scroll_down_region(int n);

    // Rotate lines within [top, bot] up/down by n (filling blanks). Pure line moves not touching
    // scrollback; shared worker for region scrolls and IL/DL.
    void scroll_lines_up(int top, int bot, int n);
    void scroll_lines_down(int top, int bot, int n);

    // IL/DL: when the cursor is inside the scroll region, insert n blank lines at / delete n lines from
    // the cursor line down to scroll_bot; the cursor moves to column 1.
    void insert_lines(int n);
    void delete_lines(int n);

    // ICH/DCH: insert n blanks at / delete n cells from the cursor within the same line. Line width
    // (cols) is preserved: overflow on the right is dropped, deletes fill blanks at the end.
    void insert_chars(int n);
    void delete_chars(int n);

    // Alt-screen enter/leave. Full-screen TUIs use ?1049 (/?1047/?47). Enter backs up the main screen
    // into main_saved and shows a blank alt screen (scrollback frozen/hidden); leave restores it.
    void enter_alt_screen();
    void leave_alt_screen();

    // A blank row (cols blank cells in the current background color).
    Row blank_row() const;

    // Erase screen[row] cells [c0, c1) to blanks (current bg). Shared by ED/EL.
    void erase_cells(int row, int c0, int c1);

    // Clamp the cursor into [0,rows-1] / [0,cols].
    void clamp_cursor();

    // Rebuild screen to the current cols/rows (row count, line width, cursor clamp).
    void reset_screen();

    // Line at unified (scrollback + screen) index idx. Used by render/scroll.
    const Row& line_at(int idx) const;

    // Convert the cursor (cur_row/cur_col) to screen coords: row_out relative to view_top, vx_out the
    // cell column within the line. false if the buffer is empty (off-screen is judged by the caller).
    bool cursor_screen_pos(int& row_out, int& vx_out) const;

    // The cell-filling rectangle where the cursor draws. false if off-screen.
    bool get_cursor_rect(CRect& rc) const;

    // Cursor block fill color = blend of default_bg:default_fg at the weight ratio (independent of the
    // last printed SGR colors).
    COLORREF blend_cursor_color() const;

    // Map a screen COLORREF to its paper (export) equivalent. Used by save_emf / save_pdf.
    // Matches default_fg/bg and ansi_colors[0..15]; truecolor / 256-color values pass through.
    COLORREF remap_paper_color(COLORREF c) const;

    // Make the cursor visible now and restart the blink timer (so it is solid for a beat after
    // input/move/output).
    void bump_cursor();

    // Whether the IME is currently in Korean (jamo) input mode. Known even before composition starts,
    // so the cursor width can be set ahead of typing.
    bool is_hangul_mode() const;

    // Send key input to the child in raw mode.
    //   send_input_bytes : raw UTF-8 bytes straight to the input sink.
    //   send_input_wide  : UTF-16 chars converted to UTF-8 (typed/committed-IME glyphs).
    //   terminal_keydown : non-char keys (arrows/Home/End/Delete...) as VT sequences; true if sent.
    void send_input_bytes(const char* bytes, int len);
    void send_input_wide(const wchar_t* ws, int n);
    bool terminal_keydown(UINT vk, bool ctrl, bool shift);

    // Paste clipboard text to the child stdin, wrapping in ESC[200~ ... ESC[201~ if bracketed_paste is
    // on (so the child distinguishes paste from typing).
    void paste_clipboard();

    // Selection helpers.
    // Convert client pixel coords to a unified-index row + cell column.
    void hit_test(CPoint pt, int& abs_row, int& col) const;
    void copy_selection();
    void clear_selection();

    // === Mouse reporting (xterm mouse tracking) ===
    // True while the child has asked for mouse events AND for SGR encoding (?1006), the only
    // encoding ConBox emits. Every local mouse gesture (selection, overlay scrollbar, wheel
    // scrollback) defers to the child while this holds, EXCEPT when Shift is down (xterm/wt.exe
    // convention: Shift forces the terminal's own handling) and except the right button, which
    // stays a local paste. The overlay scrollbar stays usable but goes THUMBLESS (sbar_geometry
    // returns an empty thumb): it shows only on hover, draws just the gutter + arrow buttons, and
    // its arrow / track-half presses are forwarded to the child as wheel notches -- same as wt.exe.
    bool mouse_reporting() const;

    // Client pixel coords -> 0-based cell column/row of the VISIBLE screen (clamped to the grid).
    // Unlike hit_test this never indexes scrollback: the child only knows about its own screen.
    // Returns false when the grid metrics are not ready yet (cell_w/cell_h still 0).
    bool mouse_cell(CPoint pt, int& row, int& col) const;

    // Encode one event as SGR (?1006) -- ESC [ < Cb ; col ; row M (press) / m (release) -- and send
    // it to the child. btn: 0=left 1=middle 2=right, 3=none (motion with no button), 64/65=wheel
    // up/down; caller adds 32 for motion. Alt/Ctrl modifier bits are added here; Shift never is
    // (a Shift gesture is handled locally and never reaches this).
    void mouse_report(int btn, bool press, CPoint pt);

    // If an IME composition is in progress, force-commit it so the completed glyph's UTF-8 reaches the
    // child first. Call right before sending a composition-ending trigger (arrows/Home/End/Delete/
    // Enter/Tab/Esc, mouse click) to guarantee the [completed][trigger] order (Requirements.md sec 10).
    // Returns true if a composition was committed.
    bool finalize_composition();

    // Lazily (re)create the double-buffer memory DC/bitmap. The bitmap is rebuilt only when the client
    // size changes, so frequent repaints do not reallocate GDI objects each time.
    void ensure_back_buffer(CDC* ref, int w, int h);

    // Input sink (raw/terminal mode). When set, keys are encoded and pushed here instead of locally edited.
    void (*input_sink)(const char* bytes, int len, void* user);
    void* input_sink_user;

    // Resize sink. Reports a new grid size (rows, cols order) to sync the child PTY size.
    void (*resize_sink)(int rows, int cols, void* user);
    void* resize_sink_user;

    // Title callback. Reports the decoded text of an OSC 0/2 "set title" sequence (see dispatch_osc).
    void (*title_cb)(const char* title);

    // Title-bar color callback + the INI-parsed values it is fired with (see set_titlebar_color_cb).
    // Stored as members (not just passed through at parse time) so registering the callback after
    // setup()/setup_from_ini() already ran still replays the current values immediately.
    void (*titlebar_color_cb)(COLORREF caption, COLORREF text, COLORREF border);
    COLORREF titlebar_caption, titlebar_text, titlebar_border;

    // === ConPTY child state (all dormant unless start() is used) ===
    HPCON h_pc;                      // pseudo-console handle
    HANDLE in_write;                 // write end of child stdin
    HANDLE out_read;                 // read end of child output (polled)
    PROCESS_INFORMATION child_proc;
    bool child_running;
    void (*exit_cb)();               // child natural-exit callback (nullptr if none)
    HANDLE log_file;                 // raw child-output log; INVALID_HANDLE_VALUE = not logging

    // On detecting child exit, clean up then fire the exit callback. stop() runs first so the callback
    // may safely restart (start()) immediately.
    void handle_child_exit();
    // Static thunks start() registers on the input/resize sinks; route to this->write/resize.
    static void child_input_thunk(const char* bytes, int len, void* user);
    static void child_resize_thunk(int rows, int cols, void* user);

    // === Host-close safety net (WM_JBCLOSEQUERY, see OnCloseQuery above) ===
    // While a child is running, OnCloseQuery answers "not ready" so the host frame hides instead
    // of destroying itself; a one-shot CLOSE_TIMER then either sees the child exit on its own
    // (handle_child_exit reposts WM_CLOSE to the parent) or, on timeout, force-terminate()s it
    // before reposting WM_CLOSE so the close finally proceeds.
    bool closing;   // true from the first OnCloseQuery call until the host frame actually closes

    COLORREF default_fg; // default RGB(200,200,200)
    COLORREF default_bg; // default RGB(32,32,32)

    COLORREF ansi_colors[16];     // xterm 256-color index 0-15 (base ANSI palette); configurable via screen_palette01..16

    // Paper (export) colors used by save_emf / save_pdf. Defaults are Tango Light theme.
    // Configurable via paper_text / paper_back / paper_palette01..16 in the [paper] INI section.
    // remap_paper_color() maps any cell COLORREF from the screen palette to its paper equivalent.
    COLORREF paper_default_fg;      // paper fg; default: #000000
    COLORREF paper_default_bg;      // paper bg; default: #FFFFFF
    COLORREF paper_ansi_colors[16]; // paper ANSI palette; Tango Light defaults

    COLORREF cur_fg;     // current SGR foreground
    COLORREF cur_bg;     // current SGR background

    int cursor_bg_weight;  // cursor blend bg weight (default 6)
    int cursor_fg_weight;  // cursor blend fg weight (default 4)

    bool has_focus;        // window has keyboard focus; cursor is hidden when false
    bool cursor_on;        // blink visible state
    int cursor_blink_ms;   // blink toggle interval (ms); 0 = always on
    int cursor_type;       // shape: 1=blink block,2=fixed block,3=blink underline,4=fixed underline,
                           // 5=blink I-beam,6=fixed I-beam. Odd=blinking, even=fixed (see set_cursor).

    CFont efont;              // normal (base)
    CFont efont_bold;         // SGR bold variant
    CFont efont_italic;       // SGR italic variant
    CFont efont_bold_italic;  // SGR bold+italic variant
    CFont efont_double;       // 2x-size variant, always bold (SGR 8/28); built in calc_cell_size
    CFont kfont;
    CFont kfont_bold;
    CFont kfont_italic;
    CFont kfont_bold_italic;
    CFont kfont_double;       // 2x-size variant, always bold (SGR 8/28); built in calc_cell_size
    LOGFONTW efont_lf;
    LOGFONTW kfont_lf; // keeps the user's original size/style

    // Symbol fallback font (fallback_font_name, e.g. Segoe UI Symbol): TextOutW does not do font
    // fallback, so a glyph missing from efont/kfont (e.g. media-control symbols outside typical
    // coding/UI fonts) would show as a hollow box. PickFont substitutes this font (checked via
    // HasGlyph) at every glyph-drawing site (OnPaint/save_emf/save_pdf). Rebuilt in build_efont()
    // at efont's pixel height, so it always matches the current cell size/DPI/zoom.
    CFont efont_fallback;
    CFont efont_fallback_double;  // 2x-size variant for CELL_DOUBLE cells; built in calc_cell_size

    // Original (DPI-independent) font specs, stored by set_efont/set_kfont (and apply_default_fonts
    // for the unset case). Kept so fonts can be rebuilt at a new DPI (build_efont/build_kfont) without
    // the host re-calling. efont_size/kfont_size are in points; kfont_size<=0 means match mode.
    std::string efont_name, efont_opts;
    float       efont_size;
    std::string kfont_name, kfont_opts;
    float       kfont_size;
    std::string fallback_font_name;  // symbol fallback face name (default "Segoe UI Symbol")

    // HasGlyph() result cache, keyed by character; separate per font since efont/kfont are usually
    // different faces with different coverage. Cleared in set_efont/set_kfont (a new face name can
    // change glyph coverage even at the same size).
    std::unordered_map<wchar_t, bool> efont_glyph_cache, kfont_glyph_cache;

    int  box_dpi;           // real monitor DPI; set in open() and updated in OnSize on a DPI change.
                            // build_font uses it pre-window; OnSize compares it to detect a DPI change.
    int  zoom_pm;           // zoom x1000 (1000=1.0x); set via WM_JBZOOM from FrameBox; eff_dpi() applies it
    bool zoom_resize;       // true: next OnSize is zoom-triggered; skip update_metrics to preserve PTY grid

    bool kfont_match_efont; // on: match Korean height to English and base line height on English
                            // (turned on by set_kfont size<=0, default true)

    int efont_width_pct;   // English width ratio (%) (default 100)
    int kfont_width_pct;   // Korean width ratio (%) (default 100)

    int glyph_level;       // built-in glyph level (0=off, 1=blocks (default), 2=+box lines)

    int cell_w;        // px width of one English cell (Korean is 2x)
    int cell_h;        // px height of one line cell
    int cell_base;     // px from cell top to glyph baseline (baseline alignment)

    int origin_x;      // px offset of the grid's left edge: grid is centered in the client (recalc_origin)
    int origin_y;      // px offset of the grid's top edge; clamps to 0 when the grid overflows the client

    int adjust_left;   // per-side cell padding in px (see adjust()); left/top also shift the glyph
    int adjust_top;
    int adjust_right;
    int adjust_bottom;

    int cols;
    int rows;

    // Configured margins are 96 DPI LOGICAL inner padding. They are used ONLY to size the window in
    // open() and to derive cols/rows on resize in update_metrics() (scaled to physical via MulDiv at
    // box_dpi). They are NOT used when drawing: the grid is centered (recalc_origin), so the realized
    // margin can differ from the configured value. adjust() padding stays in raw physical pixels.
    int margin_top;
    int margin_bottom;
    int margin_left;
    int margin_right;

    // DPI-change window snap policy (snap_to_grid). 0 = centering only (window stays at the host's
    // linearly-scaled rect, bottom/right clipped if the grid does not fit); 1 = grow only (snap larger
    // only when the grid would be clipped); 2 = always snap to the exact grid+margin size (grow or
    // shrink). Snapping keeps the upper-left corner fixed and moves the right/bottom edges.
    int snap_mode;

    // Cell grid. screen = current screen (always rows lines x cols cells). scrollback = lines pushed
    // off the top. Cursor (cur_row, cur_col) is a 0-based on-screen cell coord (VT absolute moves).
    int max_scrollback;           // scrollback line cap (configurable via config(); default 5000)
    std::vector<Row> screen;
    std::deque<Row> scrollback;   // oldest lines trimmed when size exceeds max_scrollback; deque for O(1) front removal
    int view_top;                 // top of view = unified (scrollback + screen) index (wheel/scrollbar)
    int cur_row;                  // 0..rows-1
    int cur_col;                  // 0..cols

    bool cursor_visible;          // DECTCEM (?25): cursor shown (default true)
    struct SavedCursor { int row, col; };
    SavedCursor saved_cur;        // saved by DECSC/DECRC (ESC 7/8, CSI s/u)

    // In-progress (uncommitted) IME string. Empty = no composition. When non-empty OnPaint draws it in
    // the cursor cell itself (instead of system inline) to pin it to the cursor.
    std::wstring comp_str;

    // Set by OnImeComp when it commits a glyph (GCS_RESULTSTR sent to the child); consumed by OnKeyDown
    // to fix the horizontal-arrow position after a commit (the commit advances the child cursor one
    // glyph right). The IME often pre-commits on the arrow's WM_IME_COMPOSITION *before* the arrow's
    // WM_KEYDOWN, so finalize_composition() inside OnKeyDown is then a no-op -- this flag catches that.
    // Cleared by other input paths (OnChar, mouse) so it only applies to a commit immediately followed
    // by an arrow.
    bool ime_committed;

    // Scroll region (DECSTBM), respected by LF/RI/IL/DL/SU/SD. Default is the whole screen [0, rows-1].
    int scroll_top;
    int scroll_bot;

    // Alt screen. Full-screen TUIs switch in via ?1049 (/?1047/?47).
    std::vector<Row> main_saved;  // main screen backed up on entry (restored on leave)
    bool alt_active;              // alt screen active (scrollback frozen/hidden while so)
    SavedCursor saved_main_cur;   // main-screen cursor saved on alt entry

    // VT parser state (members so a sequence survives across chunked print() calls).
    char utf8_tail[3];            // trailing incomplete UTF-8 lead bytes carried over from previous print()
    int  utf8_tail_len;           // 0 = none pending; max 3 (a 4-byte sequence needs at most 3 lead bytes)
    int  vt_state;                // 0=GROUND 1=ESC 2=CSI 3=OSC
    int  vt_params[16];           // CSI numeric params
    int  vt_nparam;
    bool vt_priv;                 // CSI '?' (DEC private) marker
    bool vt_gtlt;                 // CSI '<' '=' '>' prefix marker (2nd DA/kitty/XTMODKEYS; all ignored)
    bool vt_space;                // CSI ' ' (0x20) intermediate marker; needed to spot DECSCUSR (CSI Ps SP q)
    std::wstring osc_buf;         // OSC payload accumulated in VT_OSC, consumed by dispatch_osc()

    // Current SGR attributes applied by put_char (colors use cur_fg/cur_bg).
    bool cur_bold;
    bool cur_dim;                 // SGR 2 (faint); put_char blends the cell fg toward its bg
    bool cur_italic;
    bool cur_underline;
    bool cur_strike;
    bool cur_blink;
    bool cur_reverse;             // swap fg/bg
    bool cur_double;              // SGR 8/28; 2x-size glyph (always bold)

    // OSC 8 hyperlink state (not SGR -- survives SGR resets, only changed by another OSC 8). 0 = no
    // link; else an index into link_table stamped onto cells by put_char (see CharInfo::link_id).
    int cur_link_id;
    // index 0 is a reserved "" placeholder (never assigned to a cell); 1..255 map to URIs opened by
    // osc8(). Capped at 256 entries total (CharInfo::link_id is 1 byte) -- see osc8().
    std::vector<std::string> link_table;

    bool blink_on;                // blink visibility state, toggled by BLINK_TIMER

    // Input modes the child turns on via DEC private modes; change key encoding / paste.
    bool app_cursor_keys;         // DECCKM (?1): arrows as ESC O x instead of ESC [ x
    bool bracketed_paste;         // ?2004: wrap pastes in ESC[200~ ... ESC[201~

    // === Mouse reporting state (see mouse_reporting()) ===
    // Full-screen TUIs (Claude Code, vim, htop) turn tracking on to run their own selection and
    // scrolling; without it a wheel notch does nothing on the alt screen, since the terminal's own
    // scrollback is frozen there and the child never hears about the wheel.
    int  mouse_track;             // 0=off, or the active tracking mode: 1000 (clicks only),
                                  // 1002 (clicks + motion while a button is held), 1003 (all motion)
    bool mouse_sgr;               // ?1006: SGR encoding. Only encoding emitted, so reporting needs it.
    int  mouse_btn;               // button currently held and being reported (0/1), -1 = none. Also
                                  // marks "this press was forwarded", so its release is too even if
                                  // the child turned tracking off mid-drag.
    int  mouse_last_row;          // last cell reported by motion; motion is sent only on a cell change
    int  mouse_last_col;          // (a pixel-level report per WM_MOUSEMOVE would flood the child)

    // === Mouse drag selection state ===
    // anchor = drag start cell (fixed on button down), end = current drag cell (live). Stored
    // unordered; sorted when drawing/copying.
    bool sel_active;        // a selection exists (shown + clipboard target)
    bool selecting;         // dragging with the left button down (SetCapture active)
    bool sel_block;         // true = rectangular (Alt+drag); false = linear drag / word select
    int sel_anchor_row;     // unified-index row
    int sel_anchor_col;
    int sel_end_row;
    int sel_end_col;

    // === Overlay scrollbar (auto-hide/fade, wt-style) ===
    // No native WS_VSCROLL (it would shrink the client and reflow the grid on first appearance). The bar
    // is drawn into the back buffer over the right edge and never reserves client space, so cols stays
    // fixed. While scrollable it is always drawn as a slim bar; user scroll (wheel/PageUp-Down/
    // Ctrl+Home-End), gutter hover, or thumb drag EXPANDS it to the full bar until sbar_hold_until.
    // Geometry is derived on demand from view_top/scrollback/rows (no stored metrics).
    int sbar_fade;           // expanded-form opacity 0..255; 0 = only the slim bar shows, 255 = full bar
    DWORD sbar_hold_until;   // GetTickCount() deadline; the expanded form fades out once past it (unless hover/drag)
    bool sbar_hover;         // mouse is over the gutter (held visible while true)
    bool sbar_dragging;      // dragging the thumb (held visible; SetCapture active)
    int sbar_drag_off;       // px from thumb top to the grab point (so the thumb does not jump)
    bool sbar_repeat_active; // holding an arrow/track press (SBAR_REPEAT_TIMER running; SetCapture active)
    bool sbar_repeat_fast;   // false until the first repeat tick switches SBAR_REPEAT_TIMER to the faster rate
    bool sbar_repeat_up;     // direction fixed at the initial press; unaffected by pointer movement while held
    bool sbar_repeat_arrow;  // true = arrow (1 line/1 notch per fire), false = track (1 page/rows-4 notches)
    CPoint sbar_repeat_pt;   // cell target for mouse_report while forwarding (thumbless mode)

    // config() result cache: grid size, cmdline, and export settings stored for the host to query.
    int         cfg_cols;
    int         cfg_rows;
    int         cfg_start_x;   // start_x INI key: top-level window's initial left (px, virtual-screen
                                // coords). CW_USEDEFAULT (Windows' sentinel) if unset -- host lets the
                                // system choose. Not used by ConBox itself; the host reads it via
                                // config_start_x()/config_start_y() before creating its top-level window.
    int         cfg_start_y;   // start_y INI key; see cfg_start_x
    std::string cfg_cmdline;
    std::string cfg_work_dir;  // work_directory INI key (UTF-8); empty = inherit the host process's CWD
    int         cfg_lines_per_paper; // EMF export: rows per page (lines_per_paper INI key; default 50)
    int         cfg_close_kill_timeout_ms; // host-close grace period before terminate() (close_kill_timeout_ms INI key; default 250)
    int         cfg_bell_style;      // BEL (\a) response (bell_style INI key; default 1):
                                      // 0=none, 1=audible (MessageBeep), 2=visual (brief invert flash), 3=both
    ULONGLONG   bell_flash_until;    // GetTickCount64() deadline for the visual bell flash; 0 = not flashing
    std::string cfg_macro_f[12];     // [macros] F1..F12 INI keys, escape-decoded (UTF-8); empty = key
                                      // keeps its normal VT sequence. Modifier state (Ctrl/Shift/Alt)
                                      // is not distinguished. F10 never reaches here (WM_SYSKEYDOWN,
                                      // not WM_KEYDOWN -- see terminal_keydown), so an F10 entry is inert.
    std::vector<Trigger> cfg_triggers;   // [triggers] match=/send=/cool= groups; see check_triggers().
                                          // Append-only: every setup()/setup_from_ini() layer can only
                                          // add rules, never remove or override ones already loaded.
    int         active_trigger_count;    // count of cfg_triggers not yet permanently fired (see Trigger)
    std::string ini_msg;             // deferred message from setup_from_ini()/setup(); printed by open() once the window exists
    bool        setup_ran;           // false until setup() first applies content; see setup()'s layering comment

    // Double-buffer cache reused by OnPaint (not recreated each frame).
    CDC back_dc;              // persistent memory DC
    CBitmap back_bmp;         // back buffer bitmap (matches client size)
    CBitmap* back_bmp_saved;  // bitmap originally selected into back_dc (restored on teardown)
    int back_w;               // current back_bmp size (change detection)
    int back_h;

    // Reusable buffers for print(): avoids per-call heap allocation on the pump() hot path.
    std::string         print_buf;  // UTF-8 working buffer (tail + new bytes)
    std::vector<wchar_t> print_ws;  // UTF-16 conversion output
};
