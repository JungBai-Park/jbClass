// ConBox.h
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
//   - MFC (Unicode or MBCS), Windows only. Depends on afxwin.h. All Win32/MFC calls in the
//     portability unit use explicit W-suffix functions so both character-set settings compile.
//   - start() (ConPTY child) needs Windows 10 1809 (build 17763)+. Pure-view use does not.
//   - imm32.lib (Korean IME) is auto-linked by ConBox.cpp via #pragma comment (no host setup).
//   - For crisp glyphs the HOST app calls SetProcessDPIAware() at startup. DPI awareness is a
//     process-wide property, so the host enables it, not this control.
//   - Every string API takes UTF-8 (const char*) so C++ string literals pass directly.
//   - Self-contained: includes its own headers, does not depend on a precompiled header.
//   - Save .h/.cpp as ASCII (comments are ASCII-only) so encoding is unambiguous.
//   - SGR text attributes: bold/italic use font variants (efont/kfont _bold/_italic/_bold_italic),
//     underline/strike are drawn as 1px decoration lines, blink toggled by BLINK_TIMER (500ms).
//     SGR 8/28 = double-size (CELL_DOUBLE): 2x glyph, always bold, bleeds up+right; cursor auto-
//     advances 2x so no manual spaces needed between big chars. Stored in CharInfo::flags.
//
// --- Usage (inside the host parent window) ---
//     cConBox box;                            // usually held as a member
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

#pragma once

// ConPTY (CreatePseudoConsole/HPCON/ResizePseudoConsole) is declared only on Win10 1809
// (build 17763, RS5)+. afxwin.h pulls in windows.h, so set the minimum version before it.
// #ifndef so a host that picked a higher target (via targetver) is respected.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00            // Windows 10
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006       // NTDDI_WIN10_RS5 (ConPTY minimum)
#endif

#include <afxwin.h>   // MFC core (CWnd, CDC, CFont); also pulls in windows.h (ConPTY HPCON etc.)
#include <vector>
#include <deque>
#include <string>

// One screen cell. A double-width glyph (Korean/CJK) occupies a lead cell (CELL_WIDE set)
// and the next cell is its trail (ch=0, skipped by the renderer). An empty cell is ch=L' '.
// Layout: ch(2)+flags(1)+pad(1)+fg(4)+bg(4) = 12 bytes (was 16 with bool wide at the end).
struct CharInfo {
    wchar_t  ch;     // UTF-16 char; 0 = trail cell of a wide glyph (render skips it)
    uint8_t  flags;  // bit field: CELL_WIDE | CELL_BOLD | CELL_ITALIC | CELL_UNDERLINE | CELL_STRIKE | CELL_BLINK | CELL_DOUBLE
    // 1 byte compiler padding here (aligns fg to offset 4)
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
};

typedef std::vector<CharInfo> Row;

class cConBox : public CWnd
{
public:
    // Returned by grid_size(). rows, cols in ROW, COL order.
    struct GridSize { int rows, cols; };

    cConBox();
    virtual ~cConBox();

    // Create the child window at (left, top) inside parent. Pixel size is computed from the
    // cfg_rows/cfg_cols set by setup_from_ini() (defaults: 96 cols x 32 rows). If cfg_cmdline is
    // non-empty, start() is called automatically after the window is created. Call after
    // setup_from_ini() (fonts/margins must be set before the first layout). The host can then
    // call GetClientRect() on the ConBox CWnd to get the actual pixel size for its own layout.
    void open(CWnd* parent, int left = 0, int top = 0);

    // Set the English font. size is in points. opts is an attribute string (see set_builtin_glyphs
    // note below and ConBox.cpp ParseFontOpts for the grammar).
    void set_efont(const char* name, float size, const char* opts);

    // Set the Korean font (same arg rules as set_efont). If size <= 0, "match mode" turns on:
    // Korean is rasterized to the same pixel (em) height as English (the size value is ignored) and
    // line height tracks English, which keeps box-drawing vertical lines connecting. If size > 0,
    // match mode is off: Korean keeps that size and line height is the max of the two fonts.
    // Default (nothing set) is match mode on.
    void set_kfont(const char* name, float size, const char* opts);

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

    // Load settings from an INI file (section-agnostic key matching). path is UTF-8; a relative
    // path is resolved against the EXE directory (not the working directory). nullptr defaults to
    // "ConBox.ini". If the file does not exist, it is created with compiled-in defaults and a
    // notification is stored in ini_msg (printed by open() once the window exists). Settings stay
    // at constructor defaults. If the file exists but cannot be opened, the same deferred print()
    // path applies. Call before open() so fonts/margins are set before the first layout.
    void setup_from_ini(const char* path = nullptr);

    // Apply INI-format settings from a string. contents is UTF-8 with \n line endings (no BOM).
    // Useful for programmatic injection of settings without a file.
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

    // === ConPTY child runner (optional) ===
    // Using this group makes ConBox spawn a child and auto-wire its I/O. If start() is never called,
    // the ConPTY members stay dormant and ConBox is a pure terminal view.

    // Spawn cmdline (UTF-8, e.g. "python.exe", "cmd /c dir") under ConPTY. Console size is taken from
    // grid_size(). Internally wires the input/resize sinks to itself and starts an output
    // polling timer. Restarts if already running. Returns true on success. Call after open() (the
    // polling timer needs the window).
    // The no-arg overload uses cfg_cmdline set by setup_from_ini() (also called automatically by
    // open() when cfg_cmdline is non-empty).
    bool start();
    bool start(const char* cmdline);

    // Send bytes (UTF-8/VT) to the child stdin. Normally called by the input-sink path.
    void write(const char* data, int len);

    // ResizePseudoConsole to rows x cols. Normally called by the resize-sink path.
    void resize(int rows, int cols);

    // Tear down child/PTY/pipes/polling timer. Idempotent.
    void stop();

    bool is_running() const;

    // Register a callback fired once on the child's natural exit (e.g. shell exit). At callback time
    // cleanup is done (is_running()==false, so the callback may start() again). Not fired for an
    // explicit stop().
    void set_exit_callback(void (*cb)(void* user), void* user);

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
    // Drag-and-drop: sends dropped file paths to child stdin (quoted if the path contains spaces).
    afx_msg void OnDropFiles(HDROP hdrop);
    afx_msg void OnTimer(UINT_PTR id);
    // Korean IME. In terminal mode only committed text is sent to the child; the in-progress
    // composition is held in comp_str and drawn by ConBox in the cursor cell (system inline is
    // suppressed by OnImeComp returning 0).
    afx_msg LRESULT OnImeStart(WPARAM w, LPARAM l);
    afx_msg LRESULT OnImeComp(WPARAM w, LPARAM l);
    afx_msg LRESULT OnImeEnd(WPARAM w, LPARAM l);
    // IME state change (e.g. Korean/English toggle); updates the cursor width immediately.
    afx_msg LRESULT OnImeNotify(WPARAM w, LPARAM l);
    DECLARE_MESSAGE_MAP()

private:
    // Build one font into font, leaving the resulting LOGFONT in lf_out and width ratio in width_pct.
    void make_font(CFont& font, LOGFONTW& lf_out, int& width_pct, const char* name, float size, const char* opts);

    // Fill any unset font with the defaults (English Cascadia Mono, Korean Malgun Gothic).
    void apply_default_fonts();

    // Measure the fixed-grid cell width/height/baseline from the current fonts.
    // (Algorithm detail is in calc_cell_size's own comment.)
    void calc_cell_size();

    // Derive cols/rows that fit the client area.
    void update_metrics();

    void scroll_to_bottom();

    // Clamp view_top and pin it on the alt screen / when there is no scrollback. (No native scrollbar;
    // the overlay bar below is what the user sees.) Kept as update_scrollbar() since many sites call it.
    void update_scrollbar();

    // Overlay scrollbar helpers (auto-hide/fade). geometry: compute the gutter (track) and thumb rects
    // from the current view; false when nothing to scroll (no scrollback or alt screen). show: make the
    // bar fully opaque and (re)start the fade timer. draw: AlphaBlend it into the back buffer in OnPaint.
    bool sbar_geometry(CRect& track, CRect& thumb) const;
    void sbar_show();
    void draw_overlay_scrollbar(CDC& dc);

    // VT parser: feed one char (or control byte) into the state machine. Interprets ESC/CSI/OSC and
    // routes other chars to put_char. State is a member, so it persists across print() chunks.
    void vt_feed(wchar_t wc);

    // Dispatch a completed CSI sequence (final byte fin): cursor moves / erases / SGR etc.
    void dispatch_csi(wchar_t fin);

    // Write one glyph at the cursor (autowrapping first if past cols). A wide glyph fills the lead
    // cell and sets the next to trail (ch=0). Applies the current SGR color/attributes.
    void put_char(wchar_t wc);

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

    // === ConPTY child state (all dormant unless start() is used) ===
    HPCON h_pc;                      // pseudo-console handle
    HANDLE in_write;                 // write end of child stdin
    HANDLE out_read;                 // read end of child output (polled)
    PROCESS_INFORMATION child_proc;
    bool child_running;
    void (*exit_cb)(void* user);     // child natural-exit callback (nullptr if none)
    void* exit_cb_user;
    HANDLE log_file;                 // raw child-output log; INVALID_HANDLE_VALUE = not logging

    // On detecting child exit, clean up then fire the exit callback. stop() runs first so the callback
    // may safely restart (start()) immediately.
    void handle_child_exit();
    // Static thunks start() registers on the input/resize sinks; route to this->write/resize.
    static void child_input_thunk(const char* bytes, int len, void* user);
    static void child_resize_thunk(int rows, int cols, void* user);

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

    bool kfont_match_efont; // on: match Korean height to English and base line height on English
                            // (turned on by set_kfont size<=0, default true)

    int efont_width_pct;   // English width ratio (%) (default 100)
    int kfont_width_pct;   // Korean width ratio (%) (default 100)

    int glyph_level;       // built-in glyph level (0=off, 1=blocks (default), 2=+box lines)

    int cell_w;        // px width of one English cell (Korean is 2x)
    int cell_h;        // px height of one line cell
    int cell_base;     // px from cell top to glyph baseline (baseline alignment)

    int adjust_left;   // per-side cell padding in px (see adjust()); left/top also shift the glyph
    int adjust_top;
    int adjust_right;
    int adjust_bottom;

    int cols;
    int rows;

    int margin_top;    // inner padding (px); glyphs draw only inside it
    int margin_bottom;
    int margin_left;
    int margin_right;

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

    // Current SGR attributes applied by put_char (colors use cur_fg/cur_bg).
    bool cur_bold;
    bool cur_italic;
    bool cur_underline;
    bool cur_strike;
    bool cur_blink;
    bool cur_reverse;             // swap fg/bg
    bool cur_double;              // SGR 8/28; 2x-size glyph (always bold)

    bool blink_on;                // blink visibility state, toggled by BLINK_TIMER

    // Input modes the child turns on via DEC private modes; change key encoding / paste.
    bool app_cursor_keys;         // DECCKM (?1): arrows as ESC O x instead of ESC [ x
    bool bracketed_paste;         // ?2004: wrap pastes in ESC[200~ ... ESC[201~

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
    // fixed. It is shown on user scroll (wheel/PageUp-Down/Ctrl+Home-End) or gutter hover, then fades.
    // Geometry is derived on demand from view_top/scrollback/rows (no stored metrics).
    int sbar_alpha;          // current opacity 0..255; 0 = hidden (not drawn)
    DWORD sbar_hold_until;   // GetTickCount() deadline; fade starts once past it (unless hover/drag)
    bool sbar_hover;         // mouse is over the gutter (held visible while true)
    bool sbar_dragging;      // dragging the thumb (held visible; SetCapture active)
    int sbar_drag_off;       // px from thumb top to the grab point (so the thumb does not jump)

    // config() result cache: grid size, cmdline, and export settings stored for the host to query.
    int         cfg_cols;
    int         cfg_rows;
    std::string cfg_cmdline;
    int         cfg_lines_per_paper; // EMF export: rows per page (lines_per_paper INI key; default 50)
    std::string ini_msg;             // deferred message from setup_from_ini(); printed by open() once the window exists

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
