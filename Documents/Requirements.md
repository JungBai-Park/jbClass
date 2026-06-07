# ConBox — Software Requirements (SRS)

Terse spec so Claude can grasp the work each session (not human-facing prose).
Paths are relative to the project root. When a detail is unspecified, follow **Windows Terminal (wt.exe)**.

---

## 1. Overview

- `CConBox` (`Source/ConBox.h` / `ConBox.cpp`): a portable MFC `CWnd`-derived terminal control.
  **Single class** — the former ConPTY runner `CConExe` is merged in; `ConExe.h/.cpp` were removed.
- Two usage modes (both supported by the one class):
  - **Pure terminal view**: host feeds output bytes via `print()` and optionally reads keystroke
    bytes via `set_input_sink()`. Usable with any byte source (file, socket, host-managed process).
    Can be used with or without input sink: without it (no `set_input_sink` or with `nullptr`),
    ConBox acts as a **log/diagnostic message display** (output only, all window messages and timers
    work normally, child process not required).
  - **ConPTY child runner**: `start(cmdline)` launches a console child and auto-wires its I/O,
    sizing the child console to match the current grid. It internally reuses the input/resize sinks,
    so the runner is a built-in driver layered on the generic view interface. If `start()` is never
    called, the ConPTY members stay dormant and the control functions as a pure view.
- **Target programs**: line-oriented **stdio** programs that occasionally emit *limited VT100*
  (SGR colors, cursor moves, EL/ED, CR/LF) — the representative demo child is **PowerShell** (an
  interactive stdio shell). Full-screen TUIs (claude.exe, vim) are **best-effort, not the design
  target** (they work via the same path but their app-specific cursor/redraw quirks are out of scope).
- All string inputs are **UTF-8 `const char*`** (so C++ literals pass directly).
- Portability unit: **ConBox.h + ConBox.cpp** (2 files). Requirements: MFC (Unicode); Win10 1809+
  (ConPTY API) **only if `start()` is called** — the control functions as a pure view (log/diagnostic
  display) on any Win10 without ConPTY if `start()` is never invoked. Both files are **ASCII**
  (comments ASCII-only, so no BOM needed).

---

## 2. Window / Grid API

```cpp
void open(CWnd* parent, int x0, int y0, int x1, int y1);   // create child window by coords (no .rc)
void client_size_for_grid(int cols, int rows, int& w, int& h) const;  // px size to fit cols x rows (+margins)
int  grid_cols() const;   // current screen grid columns (English cells)
int  grid_rows() const;   // current screen grid rows
```

- `client_size_for_grid` returns the client pixel size to hold `cols` x `rows` cells including
  `set_margin` padding; cell px depends on font/DPI so call after `open()`.
- `grid_cols`/`grid_rows` give the current grid; `start()` internally reads them to size the child
  console to match the view (no explicit cols/rows parameters needed).

---

## 3. Font API

```cpp
void set_efont(const char* name, float size, const char* opts);  // English font
void set_kfont(const char* name, float size, const char* opts);  // Korean font
void adjust(int left, int top, int right, int bottom);            // per-side cell padding in px (>0 add, <0 trim)
```

- Size unit = **points** (fractional allowed, but rasterized to integer pixel height; some
  fractional sizes collapse to the same px at a given DPI).
- Defaults (match wt.exe Claude profile): English = `Cascadia Mono, 12pt, normal`;
  Korean = `Malgun Gothic ("맑은 고딕"), size 0` (= match English height; see below).
- `opts` string (LOGFONT attrs; a number before a letter is that attr's value):
  `B`=Bold (if a number is prefixed, e.g. `"700B"`, sets `lfWeight`; if no number, e.g. `"B"`, sets `FW_BOLD`),
  `I`=Italic, `U`=Underline, `S`=Strikeout,
  `Q`=Quality (0~6, default 0 if invalid or empty),
  `W`=Width ratio (percentage, e.g. `"90W"`, default 100).
- **Korean height-match mode** (set via `set_kfont` size):
  - size **<= 0**: match mode ON (default). Korean rasterized to the **same pixel height** as
    English (size value ignored). Line height tracks English so box-drawing rows abut vertically.
  - size **> 0**: match mode OFF. Korean at the given size; English natural width; line height =
    max of the two fonts.
- **Grid cell width (`cell_w`) calculation**:
  - `w_e` = English natural character width $\times$ `efont_width_pct` / 100
  - `w_k` = Korean natural character width $\times$ `kfont_width_pct` / 100
  - `max_w` = `max(2 * w_e, w_k)`
  - `cell_w` = `(max_w + 1) / 2` (rounded up to prevent truncation)
  - English font width is set to w_e, and Korean font width is set to w_k / 2 (base character width) for final GDI creation, preserving their individual width ratios inside the cell grid. However, if the width ratio (`opts` 'W') is 100% (default), `lfWidth` is left at 0 (natural width) to let GDI render the font at its natural width without scaling artifacts.
- **`adjust(left, top, right, bottom)`**: pad (or trim) each cell side by a pixel amount after font
  setup. Font size is unchanged; only the cell box grows/shrinks and the glyph shifts inside it.
  - `>0` adds margin on that side, `<0` eats into it. `cell_w += left+right`, `cell_h += top+bottom`.
  - `left` also shifts the glyph right (applied at the font `TextOutW` x); `top` also shifts it down
    (via `cell_base += top`). `right`/`bottom` only enlarge the cell (no glyph shift). Equal `top`/
    `bottom` (or `left`/`right`) centers the glyph in the padded cell.
  - Built-in block/box glyphs ignore the glyph offset and still fill the whole (padded) cell (gap-free).
  - Negative padding clamps `cell_w`/`cell_h` to >= 1px.
  - Applied after font metrics; call after `set_efont`/`set_kfont`. If called after `open()`,
    `cols`/`rows` are recomputed to fit the window. Persists until the next font change.

---

## 4. Rendering

- Screen is a fixed **`cols` x `rows` cell grid**; every glyph is drawn at a cell (no variable-width).
- Korean/CJK glyphs are **double width** (2 cells: lead + empty trail).
- Recompute `cols`/`rows` on font or window-size change. **Autowrap** at right edge.
- **Overlay scrollbar** (no native WS_VSCROLL — that would shrink the client and reflow the grid on
  first appearance): a rounded-thumb bar is drawn over the right edge of the content when scrollback
  exists; fades out when idle. Up/down triangle buttons at the top/bottom of the gutter scroll by
  one line each. Thumb drag and gutter-click (page scroll) are also supported.
- **Mouse cursor**: always `IDC_ARROW` (like wt.exe). The window does not switch to `IDC_IBEAM` even
  when hovering over text.

```cpp
void set_margin(int top, int left = -1, int bottom = -1, int right = -1);  // default 10px all sides
void set_builtin_glyphs(int level);  // 0=off, 1=block elements (default), 2=+box lines
```

- `set_margin`: inner padding (px); glyphs drawn inside it. CSS-shorthand omission: `left<0`←`top`,
  `bottom<0`←`top`, `right<0`←`left`. Calling after `open()` recomputes the grid.
- `set_builtin_glyphs`: draw box/block characters as **shapes directly** (gap-free, font-independent;
  like wt's "Built-in Glyphs") instead of via the font.
  - `1`: block elements U+2580..259F (halves/quadrants/full).
  - `2`: also box lines — single orthogonal/junctions (`─│┌┐└┘├┤┬┴┼`), rounded corners
    (`╭╮╰╯`, drawn as square), diagonals (`╱╲╳`), pure double lines (`═║╔╗╚╝╠╣╦╩╬`).
  - Mixed single/double lines, dashed, heavy, shaded are always font-drawn.

---

## 5. Cell Grid (internal buffer)

- Fixed `cols` x `rows` grid; each cell = one screen column holding a UTF-16 char + fg/bg colors.
  A double-width glyph occupies a lead cell + an empty trail cell.
- Cursor is an absolute **`(row, col)`** position (VT sequences move it directly).
- Lines scrolled off the top go to **scrollback** (oldest dropped past a cap). View position is a
  unified scrollback+screen index.

---

## 6. Output: `print()` and VT parsing

```cpp
void print(const char* text);   // UTF-8 bytes, interpreted as VT/ANSI escape sequences
```

- Parser state is **kept across calls** (chunked output from `pump()` must not break sequences).
- Handles:
  - Glyph output + **autowrap** at `cols`. A line feed at the bottom scrolls (top line → scrollback).
  - **C0**: `\r`, `\n`, `\b`, `\t` (next multiple-of-8 tab stop), BEL ignored.
  - **CSI**: cursor moves (CUU/CUD/CUF/CUB/CUP/HVP/CHA/VPA/**CNL/CPL**), erase (ED/EL), SGR, cursor save/restore,
    show/hide (DECTCEM `?25`), cursor style (**DECSCUSR** `CSI Ps SP q`; the space intermediate is
    required, so a bare `CSI q` is not it). `Ps` 0..6 maps 1:1 to `set_cursor` types (0 → 1 = blinking
    block), so a child can switch the cursor shape (§8).
  - **SGR**: 16 base colors + bold/italic/underline/strikethrough/blink (see §8 for rendering) + reverse
    + **256-color & truecolor**; `0`/`39`/`49` reset to defaults. OSC (title etc.) ignored.
  - **Full-screen (best-effort)**: alt screen (`?1049`/`?1047`/`?47`, `?1048`), scroll region
    (DECSTBM `CSI r`), IL/DL (`CSI L`/`M`), ICH/DCH (`CSI @`/`P`), ECH (`CSI X`), SU/SD (`CSI S`/`T`).
  - **Queries → child stdin**: DSR cursor pos (`CSI 6 n` → `ESC[row;colR`), terminal status
    (`CSI 5 n`), DA (`CSI c` → VT102-compatible).
  - **2-char ESC**: DECSC/DECRC (`7`/`8`), RI (`M`), **RIS (`c`)** (resets terminal), ~~IND (`D`), NEL (`E`)~~
    (implemented but not working; see §6 notes).
  - **Ignored (to avoid misparse)**: `<`/`=`/`>`-prefixed private sequences (2nd DA, kitty keyboard,
    XTMODKEYS, XTVERSION). Their final byte must **not** be mistaken for a standard command
    (e.g. `ESC[<u` must not act as cursor-restore).
- New output forces the view **scrolled to the bottom**.

---

## 6. Output: Implementation Status

**Fully working:**
- C0 controls: CR, LF, BS, TAB, BEL
- CSI cursor moves: A, B, C, D (CUU/CUD/CUF/CUB), G, ` (CHA), H, f (CUP/HVP), d (VPA), **E, F (CNL/CPL)**
- CSI erase: J, K, X (ED/EL/ECH)
- Full-screen: L, M, @, P, S, T (IL/DL/ICH/DCH/SU/SD), r (DECSTBM)
- SGR, modes (h/l), cursor style (q), save/restore (s/u), queries (n, c)
- 2-char ESC: 7, 8 (DECSC/DECRC), M (RI), **c (RIS)**

**Not working (code present but ineffective):**
- **2-char ESC D (IND)** — code at ConBox.cpp:1675–1680; cursor does not move
- **2-char ESC E (NEL)** — code at ConBox.cpp:1682–1688; cursor does not move
- Root cause unknown; requires debugging with OutputDebugString

---

## 7. Color API

```cpp
void set_color(COLORREF fg);      // foreground
void set_bg_color(COLORREF bg);   // background
```

- Applied to subsequent `print()`; child SGR sequences may override (SGR `0`/`39`/`49` reset to default).
- Defaults: fg = RGB(200,200,200), bg = RGB(32,32,32) (wt Campbell-on-acrylic look).

---

## 8. Text Attributes (SGR) & Cursor & IME

### Text Attributes (SGR)

SGR supports text styling attributes applied per cell:
- **Bold** (SGR 1/22): font weight. If base font weight >= 600 (semi-bold or heavier), bold becomes
  FW_EXTRABOLD (800) for visible enhancement; otherwise FW_BOLD (700). This allows already-bold fonts
  (e.g. Malgun Gothic B) to get noticeably thicker on SGR bold.
- **Italic** (SGR 3/23): font slant (lfItalic = 1).
- **Underline** (SGR 4/24): drawn as a 1px solid line at the cell bottom, in the cell's foreground color.
- **Strikethrough** (SGR 9/29): drawn as a 1px solid line at the cell's vertical middle, in the cell's
  foreground color.
- **Blink** (SGR 5, 6/25): text toggles visibility at 500ms interval (BLINK_TIMER). Cell glyphs are
  omitted when blink is "off" (500ms toggle rate); decoration lines (underline/strike) are always drawn.
- **Reverse** (SGR 7/27): swaps foreground ↔ background (no change to bold/italic/underline/strike/blink).
- **Reset** (SGR 0): clears all attributes and colors.

Attributes are stored per cell in the grid (CharInfo::flags) so they persist across scrollback and
survive viewport changes. Bold/italic use pre-created font variants to avoid GDI font construction
per character. Underline/strike are rasterized each frame (inexpensive 1px fills).

### Cursor & IME

```cpp
void set_cursor(int type);                            // shape; 0 = default (see below)
void set_cursor_blend(int bg_weight, int fg_weight);  // block/underline color mix, default 4:6 (bg:fg)
void set_cursor_blink(int interval_ms);               // toggle ms; 0 = always on
```

- **`set_cursor(type)`** — cursor shape. `0` = default = **blinking underline (3)**, matching the
  classic conhost.exe cursor. Explicit types:
  `1`=blinking block, `2`=fixed block, `3`=blinking underline, `4`=fixed underline,
  `5`=blinking I-beam, `6`=fixed I-beam. **Odd = blinking, even = fixed.** Fixed types kill the blink
  timer and stay solid; odd types blink per `set_cursor_blink`.
- **Block** (1/2): fills the cell with the blend color; any glyph under it is redrawn in default fg on
  top so it shows (a 2-cell Korean block redraws both covered glyphs).
- **Underline** (3/4): a **3px-high bar at the cell bottom** in the blend color; the glyph stays
  visible above it (no redraw).
- **I-beam** (5/6): a vertical bar at the cell's **left edge** (the insertion point). Drawn in **pure
  default fg** (not the blend). English = **1px** wide; Korean (2-cell) = **3px** wide.
- Cursor color **blend** = mix of default bg and fg (set via `set_bg_color`/`set_color`, defaulting to
  RGB(32,32,32)/RGB(200,200,200)) at `bg_weight:fg_weight`, default **4:6 (leans fg)**. Independent of
  the SGR colors of the last printed text. Used by block/underline and the IME composing box outline;
  the I-beam ignores it (pure fg).
- **Blink** at the system caret rate (`GetCaretBlinkTime`); typing/move/output bumps it visible for a
  beat. `set_cursor_blink` overrides; if the system disables blink, stays always on.
- Position is driven by the child (CUP etc.). If the child hides the cursor (DECTCEM `?25l`), no cursor.
- **`child_caret` suppression**: if the child paints the cursor cell itself as a highlight (reverse
  cell whose bg differs from its neighbors), ConBox does **not** draw its own cursor there (avoids a
  double cursor). Full-surface themes (all cells same non-default bg) are not treated as a caret.
- **IME composition (self-draw)**: the uncommitted composing glyph is drawn by ConBox in the cursor
  cell (system inline suppressed) as a **hollow rectangle with a 1px outline** (blend color) + the
  glyph in fg, no blink while composing. When composing mid-line over existing text, the line pixels
  from the cursor to the right edge are shifted right by the composition width to **preview the
  insertion** — correct for **insert-mode** line editors (the target, e.g. PowerShell PSReadLine /
  readline); restored on commit. Block/underline cursors are **hidden** during composition (only the
  hollow box shows); the **I-beam stays visible**, drawn (3px, pure fg) to the **right** of the box
  (composing syllable inside the box, cursor just past it).
- **IME hangul cursor width**: when the IME is in Korean (jamo) mode the cursor is a **2-cell** block
  (English mode = **1-cell**); reflected **immediately** on the 한/영 toggle (no keystroke needed).

---

## 9. Input (terminal raw mode)

ConBox runs in **raw/terminal mode** when an input sink is set (no local line editing/echo). Keys are
encoded to VT sequences / UTF-8 and sent to the sink (child stdin); the child draws the screen via its
VT output. The key → byte mapping is **§10**.

- `Ctrl+Home`/`Ctrl+End`/`PageUp`/`PageDown` scroll the scrollback view (local). All other keys go to
  the child.
- **Korean IME**: only committed text is sent (UTF-8). Before an end-of-composition trigger key
  (arrows, Home, End, Delete, Enter, Tab, Esc, mouse click), the composition is **force-committed
  first**, guaranteeing the order `[completed glyph][trigger key]` (the in-place-then-move criterion).
  See §10 IME for the pass criterion. The uncommitted glyph is drawn locally (§8), never sent to the child.

---

## 10. ConPTY Child Runner (merged from former ConExe)

Launches a console child via ConPTY (pseudo console) and connects its I/O to this same ConBox.
The child sees a real console, so line tools through full-screen TUIs run by one path.

```cpp
bool start(const char* cmdline);                       // launch child (cmdline UTF-8); call after open()
void write(const char* data, int len);                // bytes -> child stdin (input sink calls this)
void resize(int cols, int rows);                       // ResizePseudoConsole (resize sink calls this)
void stop();                                           // tear down child/PTY/pipes/timer (idempotent)
bool is_running() const;
void set_exit_callback(void (*cb)(void* user), void* user);  // fired once on child's natural exit
void pump();                                           // read pending child output -> print()
// generic hooks (also usable for pure-view mode without start()):
void set_input_sink(void (*sink)(const char* bytes, int len, void* user), void* user);
void set_resize_sink(void (*sink)(int cols, int rows, void* user), void* user);
```

- `start` reads the current grid size (`grid_cols()`, `grid_rows()`), then makes input/output pipes +
  a pseudo console sized to match, spawns the child (STARTUPINFOEX +
  `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE_HANDLE`), and **internally registers itself** on the input
  sink (→`write`) and resize sink (→`resize`); an internal timer polls output into `print()`. Requires
  the window to exist (call after `open()`). If already running, restarts.
- `resize` calls `ResizePseudoConsole`; the resize sink fires it when the grid changes.
- `set_exit_callback`: on the child's **natural exit** (e.g. shell `exit`, EOF), called once after
  cleanup (so `is_running()` is false and the callback may `start()` again). Not called for explicit
  `stop()`. ConPTY keeps the output pipe open after child exit, so exit is detected by **polling the
  child process handle**, not pipe EOF.

### Key → child byte mapping (raw mode)

| Input | Bytes to child |
|------|------|
| printable / committed Korean | UTF-8 |
| Enter | `\r` (0x0D) |
| Backspace | DEL (0x7F) |
| Tab / Esc / Ctrl+letter | control code (0x09 / 0x1B / 0x01..0x1A) |
| Arrows ←↑→↓ | normal `ESC[D/A/C/B`; app cursor keys (DECCKM `?1h`) `ESC O D/A/C/B` |
| modifier + arrow/Home/End | `ESC[1;{mod}{letter}` (mod = 1 + Shift·1 + Ctrl·4) |
| Home / End / Delete / Insert | `ESC[H` / `ESC[F` / `ESC[3~` / `ESC[2~` |
| PageUp / PageDown | `ESC[5~` / `ESC[6~` |
| Shift+Tab | `ESC[Z` |
| F1..F4 / F5..F12 | `ESC O P`..`S` / `ESC[15~`,`17~`..`24~` |
| Ctrl+C | if selection active → copy to clipboard + clear selection; else interrupt `0x03` |
| Ctrl+V / paste | clipboard text to child stdin; if bracketed paste (`?2004h`) wrap in `ESC[200~`..`ESC[201~` |

- Child mode sequences change input encoding: app cursor keys (DECCKM `?1`) switch arrow prefix
  `ESC[`→`ESC O`; bracketed paste (`?2004`) wraps pastes.

### IME compose-finalize order [required]

- While a Korean composition is in progress, an end-of-composition trigger (arrows, Home, End,
  Delete, Enter, Tab, Esc, mouse click) must cause ConBox to **first** force-commit
  (`ImmNotifyIME(NI_COMPOSITIONSTR, CPS_COMPLETE)`) — sending the completed UTF-8 to the child — and
  **then** send the trigger's VT sequence. Child byte order must always be `[completed][trigger]`.
  - **Pass criterion**: composing Korean then pressing an arrow must complete the glyph **in its
    composing cell** and *then* move (completing in the moved cell = fail). Verified by real typing
    (manual; needs a Korean keyboard).
- **Horizontal-arrow correction**: committing the glyph inserts it and advances the child cursor one
  glyph to the right, so a plain Left/Right would land one glyph off from the composing cell. ConBox
  makes the arrow act relative to that cell: on **Right** it swallows the arrow (the commit already
  moved right one glyph); on **Left** it sends Left **twice** (one cancels the commit's advance). So
  after committing, Left lands on the previous glyph and Right on the next — net one-glyph moves.
  Only for unmodified arrows (Ctrl/Shift+arrow keep raw behavior).

---

## 11. Clipboard & Selection

```cpp
// (no new public API — selection is entirely mouse/keyboard driven)
```

- **Mouse drag select**: left-button down starts a selection anchor (cell coordinates); dragging
  extends the selection end in real-time with inverted (swapped fg/bg) rendering. On button-up the
  selected text is **auto-copied** to the clipboard (CF_UNICODETEXT). Clicking without dragging (same
  cell) clears the selection. `SetCapture`/`ReleaseCapture` ensure tracking outside the window.
- **Double-click word select**: double-clicking a cell selects the word under the cursor
  (contiguous non-space run). Wide (Korean/CJK) chars are treated as whole units (trail cell snaps
  to the lead). The word is auto-copied to the clipboard.
- **Alt+drag block (rectangular) select**: holding Alt while dragging selects a rectangle instead
  of a linear span. Copy extracts a fixed-width slice per row (no trailing-space trim per row).
  Alt is sampled on button-down; releasing Alt mid-drag does not change the mode.
- **Selection text extraction** (`copy_selection`): linear — walks rows from anchor to end, collects
  non-trail (`ch!=0`) characters, trims trailing spaces per line, joins lines with CR+LF.
  Block — extracts a fixed-width rectangle per row.
- **Selection clear**: new output (`print`), typing, or any paste operation clears the selection.
- **Ctrl+C** (terminal mode): if a selection is active, copies it to the clipboard and clears the
  selection. If no selection, sends interrupt `0x03` to the child (standard terminal behavior).
- **Ctrl+V**: pastes clipboard text to child stdin (bracketed paste if `?2004h`). Same as before.
- **Right-click**: pastes clipboard text to child stdin (same as Ctrl+V). Selection is cleared first.
- **Hit-test** (`hit_test`): converts client pixel coordinates to unified (scrollback+screen) row
  index and cell column. Clamps to grid edges. Wide-char trail cells snap to their lead cell.
- **IME interaction**: mouse-down and right-click force-finalize any active IME composition before
  selection/paste (compose-finalize order maintained).
- **Drag-and-drop files**: dropping files onto the window types their paths to child stdin (wt.exe
  behavior). A path with no spaces is sent bare; a path containing spaces is wrapped in `"..."`.
  Multiple files are space-separated. Any active IME composition is finalized and the selection is
  cleared before the paths are typed. Implemented via `WM_DROPFILES` / `DragAcceptFiles`; no new
  public API.

---

## 12. Scrolling

- Mouse wheel scrolls **scrollback** (past output) up; new output returns to the bottom.
- `print()` of new output forces scroll-to-bottom.
- Vertical scrollbar shown only when scrollback exists.

---

## 13. Demo (`Project/`)

- An MFC dialog app (`CConBoxDlg`) hosts ConBox as a child window for portability testing
  (ConBox source stays demo-free).
- No OK/Cancel; ConBox fills the client area with a **5px** bezel margin. Titlebar has min/max;
  resizing the window resizes ConBox.
- On startup `con_box.config("../../Documents/Config.ini")` is called before `open()`; all font,
  color, cursor, margin, grid-size, and cmdline settings come from the INI file. Individual
  `set_efont`/`set_kfont`/`adjust`/`set_cursor`/`start("powershell.exe")` calls were removed from
  the demo; use the config file instead.
- Startup sizes the main window so ConBox is **`config_cols()` x `config_rows()` cells** (default
  96 x 32) via `client_size_for_grid`, centered on the work area.
- `set_exit_callback` closes the window when the child exits.
- The app is **System DPI Aware** (crisp at non-100% scaling) — set by the host
  (`SetProcessDPIAware()`), not the control, since DPI awareness is process-wide.

---

## 14. Config File (`config()`)

```cpp
void config(const char* path = nullptr);  // load INI; nullptr -> "ConBox.ini" next to EXE
int         config_cols()    const;        // grid_cols from last config() (default 96)
int         config_rows()    const;        // grid_rows from last config() (default 32)
const char* config_cmdline() const;        // cmdline from last config() (default "powershell.exe")
```

- Sections are cosmetic; keys are matched by **name only** (section-agnostic).
- Relative path resolved against **EXE directory**, not the working directory.
- If the file does not exist it is **auto-created** with compiled-in defaults; this call then
  returns without applying anything (settings stay at constructor defaults).
- Call **before `open()`** so fonts/margins are set before the first layout.
- `config_cols`/`config_rows`/`config_cmdline` return the values read; the host uses them to call
  `resize_to_grid` and `start()` after `open()`.

### Configurable keys

| Key | API | Default |
|-----|-----|---------|
| `efont_name`, `efont_size`, `efont_opts` | `set_efont` | Cascadia Mono, 12, "" |
| `kfont_name`, `kfont_size`, `kfont_opts` | `set_kfont` | Malgun Gothic, 0 (match), "B" |
| `margin_top/left/bottom/right` | `set_margin` | 10 each |
| `adjust_left/top/right/bottom` | `adjust` | 0 each |
| `fg`, `bg` | `set_color`, `set_bg_color` | #C8C8C8, #202020 |
| `color0`..`color15` | `ansi_colors[i]` | xterm default base16 (see below) |
| `cursor_type` | `set_cursor` | 0 (→ 3 blinking underline) |
| `cursor_blend_bg`, `cursor_blend_fg` | `set_cursor_blend` | 4, 6 |
| `cursor_blink_ms` | `set_cursor_blink` | 0 (system rate) |
| `builtin_glyphs` | `set_builtin_glyphs` | 1 |
| `scrollback_cap` | `max_scrollback` | 5000 |
| `grid_cols`, `grid_rows` | stored in `cfg_cols`/`cfg_rows` | 96, 32 |
| `cmdline` | stored in `cfg_cmdline` | powershell.exe |

- **`color0..color15`**: xterm 256-color index 0-15 (the ANSI 16-color base palette). Format `#RRGGBB`.
  Defaults match the classic xterm colors (black/red/green/yellow/blue/magenta/cyan/white ×2 bright).
  Changing these recolors all SGR 30-37/40-47/90-97/100-107 output.

---

## 15. Reference

- For any behavior not specified here, follow **Windows Terminal (wt.exe)**.
