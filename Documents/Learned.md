# Learned — pitfalls & conventions (so Claude doesn't repeat mistakes)

Concise notes Claude should recall each session. Implementation truth = `Source/` code; usage =
`Source/ConBox.h`; design map = the "DESIGN OVERVIEW" block atop `ConBox.cpp`. Paths from project root.

## Build
- Solution `Project/ConBox.sln`. Build (x64 Debug):
  `& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\Work\Study\ConBox\Project\ConBox.sln" /t:Build /p:Configuration=Debug /p:Platform=x64`
- 32-bit at the **solution** level is `x86`, NOT `Win32` (vcxproj level is Win32). Wrong → MSB4126.
- Output `Project/Debug.64/ConBox.exe` (per-config `.64`/`.32`).
- Portable unit is **ConBox.h + ConBox.cpp** only. `CConExe` was merged into `CConBox`; `ConExe.h/.cpp` deleted.
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

## Demo & automated verification (GUI)
- GUI app: run via PowerShell, then **capture screen** to verify.
- Keys: `SendInput` (KEYEVENTF_UNICODE; INPUT is 40 bytes on x64) or WScript.Shell `SendKeys`.
  **`PostMessage(WM_CHAR)` is eaten** by the dialog pump — use the input-queue path.
- SendKeys gotcha: `+ ^ % ~ ( ) { } [ ]` are special; send literal `+`/`(` as `{+}`/`{(}`
  (`7*7` is fine, but `1+2` → `1@`). The ConBox child window class starts with "Afx...".
- Demo child = **`powershell.exe`** (interactive stdio shell; representative target). Verify:
  `7*7`+Enter → `49`; colored prompt (PSReadLine = limited VT100); `exit`+Enter → child exits →
  `set_exit_callback` closes the window, no crash.
- Self-dump beats external screenshot: `PrintWindow`/`GetWindowDC` come out **black** (double-buffer
  + DWM). Use the harness PNG dump; for a real WT window, `AttachThreadInput`+`CopyFromScreen`.

## DPI
- Demo calls `SetProcessDPIAware()` as the first line of `CConBoxApp::InitInstance()` (before any
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
  `blink_on`, invalidates whole screen (blink cells may be anywhere). SGR blink cells render glyph
  only when `blink_on=true`; decoration lines (underline/strike) always drawn.
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
- **2-pass deferred render in OnPaint**: double glyphs bleed up (2x ascent) + right (2x width) past
  their cell. Drawing inline lets a later neighbor's `FillSolidRect(bg)` erase the bleed (looks 1x /
  clipped). Fix: collect double cells (DeferredDouble: px,py,w,ch,fg,cw,flags) during the main loop
  (background still fills inline, 1-cell), then draw them in a second pass after the whole grid. Their
  underline/strike are deferred too, spanning the doubled width (`2*w*cell_w`): underline at
  `cell_base+2`, strike at `cell_base-cell_h/2`.
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
- color = `blend_cursor_color()` mixing `default_bg`:`default_fg` at `cursor_bg_weight:cursor_fg_weight` (default **4:6**, leans fg; `set_cursor_blend`). The default colors are stored in `default_fg` and `default_bg` (updated via `set_color`/`set_bg_color`), keeping the cursor blend independent of the SGR colors of the last printed text. Block/underline use the blend; **I-beam uses pure `default_fg`**. Width 2 cells if `is_hangul_mode()` else 1.
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
- Lifetime: `CConBox::OnDestroy` (and the dtor, and demo's OnDestroy) call `stop()` first so polling
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
- `ansi_colors[16]` is a private member initialized in the constructor to the same values as the
  former hardcoded `base16[]`. Configurable via `color0..color15` in the INI file.
- `ParseColor` parses `#RRGGBB` strings; on failure returns the `def` argument unchanged.
- **`ParseIni` must strip inline comments**: `key = value ; comment` — the `;` and everything after
  it must be truncated before storing the value (`strchr(v, ';')` + null-terminate). Without this,
  auto-generated INI files (which have Korean inline comments) corrupt string values like `efont_name`
  on the second run, causing GDI to fall back to a substitute font with different metrics.
- `CreateDefaultIni()` writes Korean UTF-8 comments as `\xNN` hex escapes to keep `.cpp` pure ASCII.
  Default INI filename is `ConBox.ini` (formerly `Config.ini`).

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
