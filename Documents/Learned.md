# Learned — pitfalls & conventions (so Claude doesn't repeat mistakes)

Concise notes Claude should recall each session. Implementation truth = `Source/` code; usage =
`Source/ConBox.h`; design map = the "설계 개요 (DESIGN)" block atop `ConBox.cpp`. Paths from project root.

## Build
- Solution `Project/ConBox.sln`. Build (x64 Debug):
  `& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\Work\Study\ConBox\Project\ConBox.sln" /t:Build /p:Configuration=Debug /p:Platform=x64`
- 32-bit at the **solution** level is `x86`, NOT `Win32` (vcxproj level is Win32). Wrong → MSB4126.
- Output `Project/Debug.64/ConBox.exe` (per-config `.64`/`.32`).
- Portable unit is **ConBox.h + ConBox.cpp** only. `CConExe` was merged into `CConBox`; `ConExe.h/.cpp` deleted.
- In Bash, MSBuild `/t:` flags get mangled into POSIX paths — build via the **PowerShell** tool instead.

## Source/ file conventions
- Save `Source/*.h`/`*.cpp` as **UTF-8 BOM**. Without BOM, MSVC reads CP949 (C4819) and a Korean
  comment's trailing byte can swallow the next line. (Edit tool preserves BOM; a clean build with
  Korean comments confirms it.)
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
- Demo child = **`python.exe`** (interactive REPL; representative target). Verify: `7*7`+Enter →
  `49`; colored prompt (Python 3.14 REPL color = limited VT100); Ctrl+Z then Enter (EOF) → child
  exits → `set_exit_callback` closes the window, no crash.
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
- Default font (Cascadia Mono 12pt, DPI 96): `natw`=9, `kwid`=16, `cw=round(16/1.84)=9`, clamp `[9,9]` → `cell_w=9`.
- Glyph coverage: `GetGlyphIndicesW` + `GGI_MARK_NONEXISTING_GLYPHS` → `0xFFFF` for missing glyphs.
  Default is **Cascadia Mono** (has rounded corners U+256D.., quadrant blocks U+2598..); **Consolas
  lacks** those — don't default to Consolas.

## Cursor rendering
- Block cursor; color = `blend_cursor_color()` mixing bg:fg at `cursor_bg_weight:cursor_fg_weight`
  (default 6:4; `set_cursor_blend`). Width 2 cells if `is_hangul_mode()` else 1.
- Blink `CURSOR_TIMER`; `OnTimer` toggles `cursor_on`, invalidates only the cursor cell; `bump_cursor()`
  forces visible after input/move/output. Child `?25l` → `cursor_visible=false` → no block.
- **`child_caret` (double-cursor fix)**: some TUIs (claude) paint the cursor cell as a reverse cell
  AND keep the hardware cursor there; drawing the ConBox block too looks like two cursors. `OnPaint`
  skips the block when the cursor cell's bg differs from its neighbors (= child-painted caret).

## ConPTY child runner (now inside ConBox)
- `start`/`write`/`resize`/`stop`/`pump`/`handle_child_exit` are in `ConBox.cpp`'s "자식 실행 (ConPTY)"
  section (design `[PTY]`).
- Close the PTY-side pipe ends right after spawn (else `out_read` never sees EOF).
- **Child-exit detection can't use pipe EOF** (ConPTY's conhost holds the write end open). `pump`
  polls `WaitForSingleObject(child_proc.hProcess, 0)`; after draining output → `handle_child_exit`
  (`stop` then `exit_cb`; callback may `start()` again safely).
- Output polling = ConBox's own window timer `PUMP_TIMER`(=3; distinct from CURSOR=1/DBG=2). Needs a
  running GUI message pump. The old message-only window is gone.
- Lifetime: `CConBox::OnDestroy` (and the dtor, and demo's OnDestroy) call `stop()` first so polling
  never touches a dying window. Idempotent.
- `start()` wires input/resize by registering itself on `set_input_sink`/`set_resize_sink` (same path
  a pure-view host would use).
- **`vt_gtlt` lesson**: claude sends `ESC[<u` (kitty) / `ESC[>4;2m` (XTMODKEYS) — `<`/`=`/`>`-prefixed
  private CSI. Their final byte (`u`/`m`) must NOT be parsed as a standard command (`CSI u` =
  restore-cursor → jumped to 0,0). `dispatch_csi` drops the whole sequence when `vt_gtlt` is set.

## Manual-only verification (Korean IME) — needs a Korean keyboard, can't auto-capture
- **Compose-finalize order** (Requirements.md §10): composing then pressing arrow/Home/End must
  complete the syllable **in its composing cell** then move (`[completed][trigger]`). Done by
  `finalize_composition` (force `ImmNotifyIME(CPS_COMPLETE)` before the trigger key). `OnImeComp`'s
  `n==0` filter + `return 0` prevent double-send / WM_CHAR dup. Third-party IMEs may differ in CPS_COMPLETE timing.
- **IME cursor width**: hangul → 2-cell block, English → 1-cell, switching **immediately** on the
  한/영 key (before composing). `is_hangul_mode()` reads `IME_CMODE_NATIVE`; `OnImeNotify`/
  `IMN_SETCONVERSIONMODE` reflects it. (At a TUI prompt where the child paints its own caret,
  `child_caret` may hide the ConBox block, so verify by composing, not by the empty-prompt width.)
- **Composition self-draw** (design `[IME]`): hollow box in the cursor cell; mid-line compose shifts
  the line right (ScrollDC) to preview insertion — correct for **insert-mode** children (Python REPL).
  Verify: each syllable forms in the cursor cell with prior text in place, following text not hidden,
  restored on commit.

## claude input-line "eaten space / orphan reverse cell" = NON-BUG — do NOT re-investigate
- Symptom (once misdiagnosed as a ConBox bug): the first typed char ate the `❯ ` prompt space (`❯r`)
  and left an orphan reverse cell.
- Real cause: claude (Ink TUI) emits a leading `<08>` (backspace) before the first char echo **only at
  narrow grids** — an artifact of the debug **60×20** grid. At spec **96×32** it isn't sent → `❯ r`.
  **Not a ConBox VT bug** (the same bytes render the same on any terminal).
- Check: in `conexe_io.log` a clean first echo is `?2026lr`; the narrow-grid symptom is `?2026l<08>r`
  (don't confuse with the normal caret pattern `[30m[47m <08>`).
- Keep the demo grid at **96×32**; never shrink it to make captures small (the harness crops instead).
  This is **independent of terminal identity** — env vars (`WT_SESSION`/`TERM_PROGRAM`) and XTVERSION
  responses were all tried and reverted. Don't chase that path again.

## Debug harness (toggleable, default OFF; keep until told to remove)
- Captures the cursor-row strip of ConBox's back buffer to PNG + logs state, to eye-verify Korean
  IME / cursor. All debug code is marked `// [DEBUG]` (child I/O as `[임시 진단]`); grep to remove together.
- Switches (compile-time, both default 0, in `ConBox.cpp`): `#define DBG_HARNESS 0` (captures `cap_*` +
  `condbg.log`; drives `dbg_record`); `#define DBG_IO 0` (child I/O dump `conexe_io.log`; gates
  `DbgDumpIo` body via `#if`). Set to 1 + rebuild to enable.
- When on: `dbg_dump(tag)` saves `Temp/cap_NNN_<tag>.png` (cursor row, full width, ±1 row — ~2KB at
  96×32; full-buffer fallback if no cursor) + a `condbg.log` line (cursor (row,col), on/visible,
  hangul, comp_str). `DbgDumpIo` dumps child IN/OUT bytes (ESC unescaped) to `conexe_io.log`. Captures
  fire on input events + an 80ms output-settle debounce (`DBG_TIMER`).
- Read captures directly (PNG); zoom with `System.Drawing` NearestNeighbor for detail. `condbg.log`
  `seq` ↔ `cap_NNN` is 1:1.
