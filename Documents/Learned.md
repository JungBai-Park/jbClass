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

## claude input-line "eaten space / orphan reverse cell" = NON-BUG — do NOT re-investigate
- Symptom (once misdiagnosed as a ConBox bug): the first typed char ate the `❯ ` prompt space (`❯r`)
  and left an orphan reverse cell.
- Real cause: claude (Ink TUI) emits a leading `<08>` (backspace) before the first char echo **only at
  narrow grids** — an artifact of the debug **60×20** grid. At spec **96×32** it isn't sent → `❯ r`.
  **Not a ConBox VT bug** (the same bytes render the same on any terminal).
- Keep the demo grid at **96×32**; never shrink it. This is **independent of terminal identity** — env
  vars (`WT_SESSION`/`TERM_PROGRAM`) and XTVERSION responses were all tried and reverted. Don't chase
  that path again.
