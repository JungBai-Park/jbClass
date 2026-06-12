# Learned — pitfalls & conventions (so Claude doesn't repeat mistakes)

Concise notes Claude should recall each session. Implementation truth = `Source/` code; usage =
`Source/ConBox.h`; design map = the "DESIGN OVERVIEW" block atop `ConBox.cpp`. Paths from project root.

## Build
- Solution `Project/ConBox.sln`. Build (x64 Debug):
  `& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\Work\Study\ConBox\Project\ConBox.sln" /t:Build /p:Configuration=Debug /p:Platform=x64`
- 32-bit at the **solution** level is `x86`, NOT `Win32` (vcxproj level is Win32). Wrong → MSB4126.
- Output `Project/Debug.64/ConBox.exe` (per-config `.64`/`.32`).
- Portable unit is **ConBox.h + ConBox.cpp** only. `CConExe` was merged into `cConBox`; `ConExe.h/.cpp` deleted.
- In Bash, MSBuild `/t:` flags get mangled into POSIX paths — build via the **PowerShell** tool instead.

## Source/ file conventions
- `Source/*.h`/`*.cpp` comments are **English, ASCII-only (no byte >= 128)** per CLAUDE.md. Because
  the files are pure ASCII they need **no BOM** (ASCII is identical in UTF-8/CP949, so no C4819). The
  one non-ASCII code literal (Korean glyph width probe) is written as the escape `L"\xac00"` (U+AC00)
  to keep the file pure ASCII. Verify: `0` non-ASCII bytes via `[IO.File]::ReadAllBytes`.
- `ConBox.cpp` is **PrecompiledHeader=NotUsing** in the vcxproj. Don't add `..\Source` to global
  include paths (demo uses `#include "../Source/ConBox.h"`).
- Source skips the host `targetver.h`, so `ConBox.h` sets `_WIN32_WINNT`/`NTDDI_VERSION` (RS5) via
  `#ifndef` **before** `#include <afxwin.h>` (afxwin pulls windows.h → ConPTY HPCON). The
  `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE_HANDLE` fallback `0x00020016` is in `ConBox.cpp` if missing.
- vcxproj **filters** use one `소스 파일` filter (IDE-only; unrelated to disk/build).
- String-input APIs (`set_efont`/`set_kfont`/`print`) take `const char*` UTF-8 (C++ literals pass directly).

## MBCS build compatibility
- ConBox.h/ConBox.cpp compile under both Unicode and MBCS project settings. All MFC/Win32 calls
  use explicit W-suffix variants to avoid TCHAR-mapped ambiguity. Key rules:
  - **Font creation**: `CFont::CreateFontIndirectW` does NOT exist in MBCS builds. Use
    `CFont::Attach(::CreateFontIndirectW(&lf))` instead (always calls the W Win32 function).
  - **Font retrieval**: `CFont::GetLogFont(LOGFONTW*)` in MBCS calls `GetObjectA` → struct layout
    mismatch (LOGFONTA 60 bytes vs LOGFONTW 92 bytes). Use `::GetObjectW(hFont, sizeof(LOGFONTW), &lf)`.
  - **Text output**: `CDC::TextOutW` does NOT exist in MBCS builds. Use `::TextOutW(dc.GetSafeHdc(), ...)`.
  - **EMF creation**: `::CreateEnhMetaFile` in MBCS = `CreateEnhMetaFileA`. Use `::CreateEnhMetaFileW` explicitly.
  - **Demo paths**: `TCHAR path[]` + `SHGetPathFromIDList` → use `wchar_t[]` + `SHGetPathFromIDListW`.
    `CString` with `WideCharToMultiByte` → use `CStringW wpath(str.GetString())` (direct-init; explicit ctor).
- **`replace_all` ordering pitfall**: when doing bulk replacements, a shorter pattern can be a
  substring of a longer one (e.g. `dc.TextOutW(` inside `cdc.TextOutW(`). Replace the longer pattern
  FIRST, or fix up the garbled results afterward.

## Demo & automated verification (GUI)
- GUI app: run via PowerShell, then **capture screen** to verify.
- Keys: `SendInput` (KEYEVENTF_UNICODE; INPUT is 40 bytes on x64) or WScript.Shell `SendKeys`.
  **`PostMessage(WM_CHAR)` is eaten** by the dialog pump — use the input-queue path.
- SendKeys gotcha: `+ ^ % ~ ( ) { } [ ]` are special; send literal `+`/`(` as `{+}`/`{(}`
  (`7*7` is fine, but `1+2` → `1@`). The ConBox window class is `"ConBox"` (fixed name);
  use `FindWindowEx(parent, NULL, L"ConBox", NULL)` to locate it from host/test scripts.
  Multiple instances in one process are safe — `GetClassInfoExW` guards duplicate registration.
- Demo child = **`cmd.exe`** (default cmdline in INI; was powershell.exe). To test PSReadLine
  colors, change `cmdline=powershell.exe` in ConBox.ini. Verify: `7*7`+Enter → `49`; `exit`+Enter →
  child exits → `set_exit_callback` closes the window, no crash.
- Self-dump beats external screenshot: `PrintWindow`/`GetWindowDC` come out **black** (double-buffer
  + DWM). Use the harness PNG dump; for a real WT window, `AttachThreadInput`+`CopyFromScreen`.

## DPI
- Demo calls `SetProcessDPIAware()` as the first line of `cConBoxApp::InitInstance()` (before any
  window/DC) → System DPI Aware (crisp at non-100%). DPI awareness is **process-wide** → the **host**
  sets it, not the control. Then `make_font`'s `GetDeviceCaps(LOGPIXELSY)` returns real DPI.
- Verify at 125%/150%: text not blurry, layout/cursor/click aligned.

## Font-metric verification (PowerShell + GDI, no build)
To reproduce `calc_cell_size` values (`cell_w`/`natw`/`kwid`):
- Call `SetProcessDPIAware()` in the measuring process too (else 96-virtualized values mismatch).
- Do struct work in C# via `Add-Type` (PowerShell `New-Object LOGFONT` + `[ref]` to
  `CreateFontIndirect` silently fails → measures the old font). Return only result arrays.
- PowerShell 5.1 reads a **BOM-less UTF-8 .ps1 as CP949** → Korean literals break. Save .ps1 UTF-8
  BOM, or pass the char as a codepoint (U+AC00).
- Match C rounding: pt→px `-(LONG)(pt*dpi/72+0.5)` and `cw=(int)(…+0.5)` = round-half-up
  (`[Math]::Floor(x+0.5)`); height scale `(LONG)(…)` truncates toward 0 (`[Math]::Truncate`).
  PowerShell `[int]` is banker's rounding (wrong).
- Default font (Cascadia Mono 12pt, DPI 96): `natw_e`=9, `natw_k`=16, `w_e`=9, `w_k`=16, `cell_w`=9.
- Glyph coverage: `GetGlyphIndicesW` + `GGI_MARK_NONEXISTING_GLYPHS` → `0xFFFF` for missing glyphs.
  Default is **Cascadia Mono** (has rounded corners U+256D.., quadrant blocks U+2598..); **Consolas
  lacks** those — don't default to Consolas.

## SGR text attributes (bold/italic/underline/strike/blink)
- **CharInfo structure**: `bool wide` → `uint8_t flags` (saves 4 bytes/cell: 16→12 bytes; struct is
  ch(2)+flags(1)+pad(1)+fg(4)+bg(4)). Bits: CELL_WIDE(1) | CELL_BOLD(2) | CELL_ITALIC(4) |
  CELL_UNDERLINE(8) | CELL_STRIKE(16) | CELL_BLINK(32).
- **Font variants**: `efont_bold`, `efont_italic`, `efont_bold_italic` (kfont variants too) created
  once in `set_efont`/`set_kfont`/`apply_default_fonts`. Bold weight selection: if base `lfWeight >= 600`
  (already semi-bold/bold), use FW_EXTRABOLD(800) for visual enhancement; else FW_BOLD(700). Allows
  already-bold fonts (e.g. Malgun Gothic B) to become thicker on SGR bold.
- **BLINK_TIMER** (ID=2, 500ms): separate from CURSOR_TIMER(1), PUMP_TIMER(3), SBAR_TIMER(4). Toggles
  `blink_on`; calls `Invalidate(FALSE)` only when CELL_BLINK cells are visible. SGR blink cells render
  glyph only when `blink_on=true`; decoration lines (underline/strike) always drawn.
  **Must scan visible viewport** via `line_at(view_top + row)` (0..rows-1), NOT `screen[]` alone —
  when the user has scrolled back, visible rows come from scrollback; scanning only `screen[]` misses
  them and the blink state change never reaches the screen (blink appears frozen).
- **OnPaint rendering**: for each cell, select font based on (CELL_WIDE, CELL_BOLD, CELL_ITALIC) → 4-way
  branch to one of {efont, efont_bold, efont_italic, efont_bold_italic, kfont, ...}. After glyph,
  draw underline (1px at cell bottom) / strike (1px at middle) if flags set, in cell's fg color.
  Blink glyphs are omitted (cell background filled, glyph skipped) when `!blink_on`.
- **SGR parsing** (`dispatch_csi`): SGR 1/22 (bold), 3/23 (italic), 4/24 (underline), 5,6/25 (blink),
  9/29 (strike), 7/27 (reverse) all stored as separate `cur_*` flags. SGR 0 resets all.
  **Removed bold-as-bright**: old SGR 30-37 color selection no longer adds 8 when `cur_bold`. Bold
  now only affects font, not color.
- **Verification**: run `Temp/verify_sgr.ps1` in PowerShell (within ConBox) to see English/Korean
  bold/italic/underline/strike/blink rendered side-by-side.

## SGR double-size (2x glyph, CELL_DOUBLE 0x40)
- **SGR code choice**: a child triggers double-size via **SGR 8 (on) / SGR 28 (off)**, NOT a custom
  code. Tried SGR 133/134 first: it works via direct `print()` but is **stripped by the inbox conhost**
  on the ConPTY child path (same mechanism as IND/NEL -- conhost re-encodes its screen buffer with only
  standard SGR, dropping unknown params). Proven by the direct-print diagnostic: 133 rendered 2x with no
  child, but a PowerShell child's 133 arrived as plain text. SGR 8 (conceal)/28 (reveal) are *standard*
  codes ConBox doesn't otherwise implement, so conhost passes them through. Confirmed by capture.
- **Always bold**: double cells render with `efont_double`/`kfont_double`, created by `MakeDoubleFont`
  (lfHeight*2, lfWidth*2, lfWeight forced FW_BOLD/EXTRABOLD). Bold/italic SGR is ignored on double cells.
- **Font build location = `calc_cell_size` (end), NOT set_efont/set_kfont**. Critical for Korean: in
  match mode kfont's final height is only settled inside calc_cell_size; building kfont_double in
  set_kfont captures the pre-match (size 0) LOGFONT -> Korean rendered ~1x. Symptom seen: English 2x OK
  but Korean stayed 1x until the build was moved to calc_cell_size.
- **Right-to-left column rendering solves CELL_DOUBLE bleed** (no 2nd pass): double glyphs bleed
  right (2x width) over neighboring cells. By rendering columns right-to-left within each row, the
  right neighbors are already painted before the double glyph draws over them — so the bleed is never
  erased by a later cell's background fill. Upward bleed (2x ascent) is safe because rows are rendered
  top-to-bottom (the row above is already complete). Underline/strike span the doubled width inline.
- **Child must leave blank line above** (bleed goes upward, NOT downward). No manual spaces needed
  between big chars — `put_char` auto-advances `cur_col` by `w*2` in double mode (English: +2,
  Korean: +4), so the child writes `"가나다"` directly and ConBox leaves the correct gap.
- **Verification**: `Temp/verify_double.py` (run as the child) + `Temp/capture.ps1` (CopyFromScreen PNG;
  the python child must `time.sleep` so the window stays open for capture, else child-exit closes it).

## Cursor rendering
- **`cursor_type`** (1..6) picks the shape; `set_cursor(type)` maps `0`→the default (3=blinking
  underline, like classic conhost.exe). **Odd=blink, even=fixed** — `set_cursor`/`bump_cursor`/
  `OnTimer` all gate the blink timer on `(cursor_type & 1)`. The shape itself is chosen in `OnPaint`'s
  cursor branch: `<=2` block, `<=4` underline, else I-beam.
- color = `blend_cursor_color()` mixing `default_bg`:`default_fg` at `cursor_bg_weight:cursor_fg_weight` (default **4:6**, leans fg; `set_cursor_blend`). The default colors are stored in `default_fg` and `default_bg` (updated via `set_fg_color`/`set_bg_color`), keeping the cursor blend independent of the SGR colors of the last printed text. Block/underline use the blend; **I-beam uses pure `default_fg`**. Width 2 cells if `is_hangul_mode()` else 1.
- **`set_cursor_blink(0)` = system rate** (NOT "always on"). When 0 is passed, `GetCaretBlinkTime()`
  is called and the result is stored as `cursor_blink_ms`. If the system has blink disabled (INFINITE),
  then cursor_blink_ms stays 0 → always on. Root cause of the original bug: the INI default is
  `cursor_blink_ms = 0` (comment: "시스템 설정 따름"), but old implementation set cursor_blink_ms=0
  and never started CURSOR_TIMER, so cursor_type=0 (→3 blinking underline) never blinked.
- **I-beam** sits at the cell's **left edge** (insertion point): English 1px, Korean 3px, all `default_fg`.
  During IME composition the I-beam is the one shape that stays visible — drawn (3px) to the **right** of
  the hollow box (after the composing glyph, so it is on top). Block/underline are hidden while composing.
- IME composing hollow box outline is **1px** (`bw=1` in OnPaint), blend color.
- Blink `CURSOR_TIMER`; `OnTimer` toggles `cursor_on`, invalidates only the cursor cell; `bump_cursor()`
  forces visible after input/move/output. Child `?25l` → `cursor_visible=false` → no cursor.
- **`child_caret` (double-cursor fix)**: some TUIs (claude) paint the cursor cell as a reverse cell
  AND keep the hardware cursor there; drawing the ConBox block too looks like two cursors. `OnPaint`
  skips the block when the cursor cell's bg differs from its neighbors (= child-painted caret).

## ConPTY child runner (now inside ConBox)
- `start`/`write`/`resize`/`stop`/`pump`/`handle_child_exit` are in `ConBox.cpp`'s "Child runner
  (ConPTY)" section (design `[PTY]`).
- Close the PTY-side pipe ends right after spawn (else `out_read` never sees EOF).
- **Child-exit detection can't use pipe EOF** (ConPTY's conhost holds the write end open). `pump`
  polls `WaitForSingleObject(child_proc.hProcess, 0)`; after draining output → `handle_child_exit`
  (`stop` then `exit_cb`; callback may `start()` again safely).
- Output polling = ConBox's own window timer `PUMP_TIMER`(=3; distinct from CURSOR=1). Needs a
  running GUI message pump. The old message-only window is gone.
- Lifetime: `cConBox::OnDestroy` (and the dtor, and demo's OnDestroy) call `stop()` first so polling
  never touches a dying window. Idempotent.
- `start()` wires input/resize by registering itself on `set_input_sink`/`set_resize_sink` (same path
  a pure-view host would use).
- **`vt_gtlt` lesson**: claude sends `ESC[<u` (kitty) / `ESC[>4;2m` (XTMODKEYS) — `<`/`=`/`>`-prefixed
  private CSI. Their final byte (`u`/`m`) must NOT be parsed as a standard command (`CSI u` =
  restore-cursor → jumped to 0,0). `dispatch_csi` drops the whole sequence when `vt_gtlt` is set.
- **DECSCUSR (`vt_space`)**: cursor-style `CSI Ps SP q` needs the SP (0x20) intermediate to tell it
  from a bare `CSI q`. The CSI parser tracks only `' '` (sets `vt_space`); `dispatch_csi`'s `q` case
  acts only when `vt_space`. `Ps` 0..6 maps 1:1 to `set_cursor` (0→1). Test from the child:
  `$e=[char]27; Write-Host "$e[5 q" -NoNewline` then Enter (note the space before `q`).

## UTF-8 chunk-boundary fix in print()
- `pump()` delivers child output in arbitrary-sized chunks. A 3-byte UTF-8 sequence (e.g. ─ = U+2500 =
  `E2 94 80`) can be split across two consecutive `print()` calls. Without carry-over, `MultiByteToWideChar`
  produces U+FFFD for the incomplete fragment — stored in the cell grid → 3 garbled glyphs on screen +
  `���` in the clipboard when dragging.
- Fix: `utf8_tail[3]` / `utf8_tail_len` members (initialized to 0 in the ctor). Each `print()` call
  prepends saved tail bytes, then trims any new trailing incomplete sequence into `utf8_tail` before
  calling `MultiByteToWideChar`. The VT parser loop now iterates `wlen` (not `wlen-1`) because explicit
  byte-length conversion omits the null terminator from the output count.
- U+0000 (NUL byte) **never** appears inside a multi-byte UTF-8 sequence, so `strlen()` is safe for
  measuring the input in `print()`.

## Input edge cases
- **Ctrl+V / Ctrl+C double-send**: both `WM_KEYDOWN` and `WM_CHAR` fire. `terminal_keydown`
  special-cases these (paste / copy-or-interrupt), so `OnChar` must **drop** their WM_CHAR control
  bytes `0x16`/`0x03` — else the child gets them too (Ctrl+V shows twice: PSReadLine treats `0x16` as
  its own paste; right-click pastes once because it has no WM_CHAR path). Other Ctrl+letters still go
  through OnChar.

## Manual-only verification (Korean IME) — needs a Korean keyboard, can't auto-capture
- **Compose-finalize order** (Requirements.md §10): composing then pressing arrow/Home/End must
  complete the syllable **in its composing cell** then move (`[completed][trigger]`). Done by
  `finalize_composition` (force `ImmNotifyIME(CPS_COMPLETE)` before the trigger key). `OnImeComp`'s
  `n==0` filter + `return 0` prevent double-send / WM_CHAR dup. Third-party IMEs may differ in CPS_COMPLETE timing.
- **Horizontal-arrow correction** (Requirements.md §10): a commit advances the child cursor one glyph
  right, so a plain Left/Right lands one glyph off (observed: Left = stay, Right = move two). Fix in
  `OnKeyDown`: on Right **swallow** the arrow, on Left send Left **twice**. Key gotcha — can't gate on
  `finalize_composition()`'s return: the MS Korean IME **pre-commits on the arrow's WM_IME_COMPOSITION
  before the WM_KEYDOWN**, so `finalize` is then a no-op (returns false). Use the `ime_committed` flag
  set by `OnImeComp` instead; `OnKeyDown` captures+clears it, and `OnChar`/mouse-down clear it so only
  a commit *immediately* before an arrow triggers the fix. Assumes the child moves one glyph per arrow.
- **IME cursor width**: hangul → 2-cell block, English → 1-cell, switching **immediately** on the
  한/영 key (before composing). `is_hangul_mode()` reads `IME_CMODE_NATIVE`; `OnImeNotify`/
  `IMN_SETCONVERSIONMODE` reflects it. (At a TUI prompt where the child paints its own caret,
  `child_caret` may hide the ConBox block, so verify by composing, not by the empty-prompt width.)
- **Composition self-draw** (design `[IME]`): hollow box in the cursor cell; mid-line compose shifts
  the line right (ScrollDC) to preview insertion — correct for **insert-mode** children (e.g. PowerShell).
  Verify: each syllable forms in the cursor cell with prior text in place, following text not hidden,
  restored on commit.

## save_log (raw child output logging)
- `save_log(file_name)` opens/creates in append mode (`OPEN_ALWAYS` + `SetFilePointerEx(FILE_END)`).
  Each session prepends a separator: 4 blank lines + `--- YYYY-MM-DD HH:MM:SS ---...` (100 cols).
- **CWD matters**: `save_log("Temp\\ConBox.log")` uses a relative path. Call it AFTER `SetCurrentDirectory`
  in the demo, or the path resolves to the wrong directory. Order in `OnInitDialog`:
  `SetCurrentDirectory(...)` → `save_log(...)` → `start(...)`.
- `log_file` member is `HANDLE` (INVALID_HANDLE_VALUE = not logging); auto-closed in `OnDestroy`.

## config / ANSI palette
- `config()` was renamed to `setup_from_ini()`; a new `setup(const char* contents)` was added that
  accepts INI-format text from a string (useful for embedding config in the host). `ParseIni` was
  refactored to accept a `const char*` string instead of a file path (file reading moved to
  `setup_from_ini`). `ConBoxDlg.cpp` updated accordingly. All three methods have the same key rules.
- `setup_from_ini()` is implemented in `ConBox.cpp` ("config" section). Section headers in the INI
  are cosmetic; all keys matched by name only. Relative path resolved against EXE dir (not CWD).
- `Xterm256ToRgb` signature changed to `static COLORREF Xterm256ToRgb(int n, const COLORREF* pal)`.
  The old `static const COLORREF base16[16]` local was removed; callers pass `ansi_colors` (member).
  All 5 call sites in `dispatch_csi` pass `ansi_colors`.
- `ansi_colors[16]` is a private member initialized in the constructor to xterm base16 defaults.
  Configurable via `screen_palette00..15` in the INI file (**0-indexed**, matching ANSI indices directly).
  **Key names**: `screen_text`/`screen_back`/`screen_palette00..15` (screen), `paper_text`/`paper_back`/
  `paper_palette00..15` (paper). Old names `fg`/`bg`/`color0..15` and the 1-indexed `..01..16` variants
  are gone.
- `ParseColor` parses `#RRGGBB` strings; on failure returns the `def` argument unchanged.
- **`ParseIni` must strip inline comments**: `key = value ; comment` — the `;` and everything after
  it must be truncated before storing the value (`strchr(v, ';')` + null-terminate). Without this,
  auto-generated INI files (which have Korean inline comments) corrupt string values like `efont_name`
  on the second run, causing GDI to fall back to a substitute font with different metrics.
- `CreateDefaultIni()` writes Korean UTF-8 comments as `\xNN` hex escapes to keep `.cpp` pure ASCII.
  Default INI filename is `ConBox.ini` (formerly `Config.ini`).
- **Current INI defaults**: `cmdline=` (empty — no auto-start; was `cmd.exe`);
  `adjust_top=-2` (was 0); `builtin_glyphs=2` (was 1); `lines_per_page` renamed to `lines_per_paper`.
- **Paper palette** (`paper_default_fg/bg`, `paper_ansi_colors[16]`): independent second color set for
  export (save_emf/save_pdf). Defaults use semantic inversion for white-bg export (see below).
  Configurable via `paper_text`/`paper_back`/`paper_palette00..15` (0-indexed). `remap_paper_color()`
  maps screen COLORREFs at render time (matches default_fg/bg and ansi_colors[0..15]; truecolor passes
  through unchanged).
- **Palette key naming**: `screen_palette00..15` / `paper_palette00..15` are 0-indexed (matching ANSI
  color indices 0-15 directly). Older sessions used 1-indexed keys (01..16); those are now gone.
- **`setup_from_ini` error → deferred `print()`**: `ini_msg` (std::string private member) stores the
  status message when the INI is missing or unreadable; `open()` calls `print(ini_msg.c_str())` and
  clears it after the window is created. This avoids `MessageBoxW` (window doesn't exist yet at setup
  time). The `w2u` lambda inside `setup_from_ini` converts `wide_path` (wstring) to UTF-8 inline.

## open() / grid API (refactored)
- **`open(parent, left=0, top=0)`** (was `open(parent, x0, y0, x1, y1)`): pixel size computed
  internally from `cfg_rows`/`cfg_cols`; auto-starts if `cfg_cmdline` non-empty. Host uses
  `GetClientRect()` on the ConBox `CWnd*` to get the pixel size afterward.
- **Removed APIs**: `client_size_for_grid()`, `grid_cols()`, `grid_rows()`, `config_cols()`,
  `config_rows()`, `config_cmdline()`. All replaced by `grid_size()` + `GetClientRect()` pattern.
- **`struct GridSize { int rows, cols; }`** nested inside `cConBox` (public). Returned by `grid_size()`.
- **`start()` no-arg** added (uses `cfg_cmdline`). The explicit `start(cmdline)` overload kept.
- **ROW, COL order**: `resize(int rows, int cols)` and the `set_resize_sink` callback
  `void(*)(int rows, int cols, void*)` are now ROW-first. (Was COL, ROW.)
- **`set_exit_callback`** is NOT called automatically by `open()`. Default on child exit = do nothing.
  The host must call it explicitly if it wants the window to close.
- **INI path bug** (fixed): demo used `"Documents\\ConBox.ini"` (EXE-relative →
  `Project/Debug.64/Documents/ConBox.ini`, nonexistent) → corrected to `"..\\..\\Documents\\ConBox.ini"`.
  The old code was hidden because the ctor defaulted `cfg_cmdline = "cmd.exe"` and start() was called
  explicitly — so the broken path went unnoticed.
- **`CreatePtyPipes` local-var rename pitfall**: when `start(cmdline)` renamed locals from `cols`/`rows`
  to `c`/`r`, the `CreatePtyPipes(cols, rows, ...)` call was not updated → runtime passed stale zeros.
  Fixed to `CreatePtyPipes(c, r, ...)`. A symptom: child PTY sized 0x0, no output.
- **Demo `resize_to_grid()`** takes no args; reads `con_box.GetClientRect()` directly (was
  `client_size_for_grid(cols, rows)` with args read from `config_cols()/config_rows()`).

## EMF export (save_emf / get_text_lines)
- **BeginPath/FillPath does NOT work on EMF DCs**: `TextOutW` on an EMF DC renders immediately
  regardless of the path bracket; `FillPath` fills an empty path. Correct approach: `SetTextColor(fg)` +
  `TextOutW`. Using path for vector text on EMF DCs silently produces wrong output (black text, no fill).
- **EMF canvas whitespace**: passing a manually computed HIMETRIC `bounds` rect to `CreateEnhMetaFile`
  can produce extra whitespace on right/bottom (reference device mismatch). Fix: pass `NULL` for `lpRect`
  so GDI auto-computes the tight frame from actual drawing operations.
- **Trail cell flag**: `put_char` writes trail with `ch=0` but sets `flags = attr` (no `CELL_WIDE` on
  trail; only the lead has `CELL_WIDE`). Detecting trail by `ch==0 && CELL_WIDE` always fails → trail
  output as extra space. Correct detection: when the lead has `CELL_WIDE`, unconditionally skip lead+1
  (the trail), regardless of the trail's flags. This also affects `export_text` for single-size Korean.
- **`get_text_lines` cell consumption per char**:
  - Normal English: 1 cell.
  - Normal Korean (`CELL_WIDE`): 2 cells (lead + skip trail at lead+1).
  - Double English (`CELL_DOUBLE`): 2 cells (lead + skip 1 blank from 2x advance).
  - Double Korean (`CELL_WIDE|CELL_DOUBLE`): 4 cells (lead + skip trail + skip 2 blanks from 4x advance).
- **CDC::Detach() before CloseEnhMetaFile**: if CDC owns the HDC, its destructor calls `DeleteDC` after
  `CloseEnhMetaFile`, which is invalid. Always `cdc.Detach()` before calling `CloseEnhMetaFile`.
- **Paper color remapping**: `remap_paper_color()` does an 18-entry table lookup (default_fg, default_bg,
  ansi_colors[0..15]). Both save_emf and save_pdf pass every cell's c.fg/c.bg through this at render time.
  Screen and paper palettes are independent; screen colors never contaminate paper output.
- **Paper palette semantic inversion**: on a dark terminal, index 0 = bg (black), index 7 = fg (white),
  index 8 = bright-bg, index 15 = bright-fg (most emphasized). For white-paper export the roles invert:
  paper[0]=#FFFFFF (bg), paper[7]=#000000 (fg), paper[8]=#EEEEEE (near-white), paper[15]=#000000 (black).
  Chromatic pairs (1-6 / 9-14) collapsed to the same dark Tango color so bright yellow/cyan/green don't
  become invisible on white. Do NOT revert to Tango Light defaults (those had near-white at index 7/15).
- **Korean UTF-8 hex encoding in CreateDefaultIni**: double-check hex escapes against a UTF-8 table.
  Example mistake: U+C7A5 (장) encodes as `\xEC\x9E\xA5` (correct), NOT `\xEC\xA5\xBF` (wrong).
  Wrong bytes produce a different character in the rendered INI comment. Use a UTF-8 table or encode with
  Python: `'장'.encode('utf-8')` → `b'\xec\x9e\xa5`.

## scrollback container
- `scrollback` is `std::deque<Row>` (not `std::vector`) so front removal when the cap is exceeded
  is O(1) (`pop_front()`) rather than O(n) (`erase(begin, begin+over)`). Cap default = 5000 lines,
  configurable via `scrollback_cap` in INI.

## claude input-line "eaten space / orphan reverse cell" = NON-BUG — do NOT re-investigate
- Symptom (once misdiagnosed as a ConBox bug): the first typed char ate the `❯ ` prompt space (`❯r`)
  and left an orphan reverse cell.
- Real cause: claude (Ink TUI) emits a leading `<08>` (backspace) before the first char echo **only at
  narrow grids** — an artifact of the debug **60×20** grid. At spec **96×32** it isn't sent → `❯ r`.
  **Not a ConBox VT bug** (the same bytes render the same on any terminal).
- Keep the demo grid at **96×32**; never shrink it. This is **independent of terminal identity** — env
  vars (`WT_SESSION`/`TERM_PROGRAM`) and XTVERSION responses were all tried and reverted. Don't chase
  that path again.

## VT100 IND/NEL "not working" = NON-BUG (ConPTY artifact) — do NOT re-investigate
- Earlier sessions marked IND (ESC D) / NEL (ESC E) as "NOT WORKING": a PowerShell child printed
  ESC D / ESC E via `[System.Console]::Write` and the result stayed on one line. This was
  misdiagnosed as a ConBox parser bug.
- **The parser is correct.** Verified by feeding `"Row1\x1bDRow2"` etc. straight to `print()` (no
  child): IND moved cur_row down keeping the column, NEL did CR+LF, RI/RIS/CNL/CPL all correct.
  IND/NEL/RI live in vt_feed VT_ESC; CNL/CPL in dispatch_csi.
- **Real cause**: the system conhost behind `CreatePseudoConsole` does NOT forward the child's
  ESC D / ESC E. It absorbs them into its own screen buffer and re-encodes the result as CUP +
  text (logged ConBox-side raw input had ZERO `ESC D` bytes; it arrived as `ESC[r;cH ...text`).
  So ConBox never receives IND/NEL through the ConPTY path; nothing to fix in ConBox.
- **Why wt.exe differs**: wt.exe ships a newer bundled `OpenConsole.exe` (v1.24, ...
  `Microsoft.WindowsTerminal_*\OpenConsole.exe`) as its ConPTY backend; ConBox's
  `CreatePseudoConsole` hard-calls the inbox `\\?\C:\Windows\system32\conhost.exe`
  (v10.0.19041, older), confirmed via the child process tree. The two backends re-encode
  differently. This is a known limitation (microsoft/terminal #5334): kernel32
  CreatePseudoConsole cannot be pointed at a custom conhost path.
- **Using OpenConsole would require** either a redistributable conpty.dll (not present on this
  box; WindowsTerminal ships only `OpenConsoleProxy.dll`, a COM proxy without CreatePseudoConsole),
  or hand-spawning OpenConsole.exe as a ConPTY server (low-level ConDrv handle creation via ntdll
  NtCreateFile on `\Device\ConDrv\Server` + `\Reference`, per winconpty.cpp). Large, version-
  sensitive work; deferred as low priority. Don't retry the "fix IND in ConBox" path.
- **Debugging method that worked** (better than screen capture): a compile-gated file logger
  (`dbg_log` -> `Temp/debug.log`) tracing each char into vt_feed / put_char / line_feed plus the
  raw bytes into `print()`, driven by direct `print()` test strings (no child) for determinism.
  Removed after use; re-add the same pattern if a future VT issue needs tracing.

## Runtime resize (WM_SIZE cols/rows change) — does NOT reflow wrapped lines — INTENTIONAL LIMITATION
- **Current behavior**: When cols/rows change (e.g. user resizes the window), `reset_screen()` pads or
  truncates each existing Row to the new cols width. Autowrapped lines are NOT merged/split; they stay
  as separate Rows.
- **Example**: cols=20, "1234567890123456789ABC" is stored as [Row0(20 chars), Row1("ABC")].
  Resize to cols=40 → [Row0(20 chars + 20 blanks), Row1("ABC" + 37 blanks)]. Not merged into one Row.
- **Why**: Full reflow (unwrapping all lines, rewrapping to new width, then re-splitting scrollback+screen)
  is complex and expensive. ConBox prioritizes simplicity.
- **Workaround**: Keep window size fixed during child execution, or `stop()` and `start()` the child
  after resizing (child will see the new PTY size and re-render accordingly).
- **Difference from wt.exe**: Windows Terminal does reflow on resize; ConBox does not. This is a
  documented limitation, not a bug to fix.
- **Do NOT revisit**: This decision was made consciously; don't reopen the "add reflow" discussion.
