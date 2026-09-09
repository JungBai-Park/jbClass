## ConBox

### 1. Font Measurement and Korean Matching

- The final measured height for the Korean matching font must be handled at the end of `calc_cell_size`.
- Do not build the Korean matching font early in `set_kfont`.
- If it is built before font match information is available, the GDI object is created with a default height of 0 and Korean glyphs render too small compared with English 2x output.
- In match mode (`set_kfont` size <= 0), `kfont_lf.lfHeight` is 0 at `set_kfont` call time, so the bold/italic/bold_italic variants built there also have `lfHeight=0` (GDI default = too large, overlaps adjacent lines). `calc_cell_size` corrects `kfont` but does NOT propagate the matched height back to the variants. Fix: at the end of `calc_cell_size`, after all font rebuilds, if `kfont_match_efont`, call `GetObjectW(kfont, &kfont_lf)` then rebuild the three variants via `MakeFontVariant`. The double variant (`kfont_double`) already uses `GetObjectW` internally via `MakeDoubleFont` so it was never affected.

### 2. SGR Double-Size Rendering

- Use standard SGR 8 for double-size on and SGR 28 for double-size off.
- Do not invent a private control code for this feature because conhost may drop unknown private sequences.
- Double-size glyphs bleed pixels into cells on the right.
- Paint each row from right to left so adjacent clear operations do not erase the bleed pixels and leave visual artifacts.

### 3. Cursor and Korean IME Commit Order

- If a navigation key or Enter is received while Korean IME composition is active, call `ImmNotifyIME(CPS_COMPLETE)` first.
- Send the special key only after the committed text is delivered.
- The child process must receive input in this order: committed text, then movement or control key.
- After Korean syllable commit, the emulation path advances the cursor one cell to the right.
- To compensate, swallow right-arrow input when needed and send the left-arrow sequence twice for left-arrow correction.

### 4. EMF Export Pitfalls

- In an EMF DC, `BeginPath` and `FillPath` do not reliably wrap `TextOutW`.
- If path-based filling is used, text can appear as black blocks.
- Set the foreground color with `SetTextColor(fg)` and draw text directly.
- Pass `NULL` as the media bounds rectangle pointer to `CreateEnhMetaFile` so GDI fits the output bounds tightly.
- If a `CDC` is attached to a target HDC, call `Detach()` before closing or destroying the HDC. Otherwise the CDC destructor can destroy the handle at the wrong time.

### 5. UTF-8 Chunk Boundaries

- Data from an external process pipe can split a multibyte UTF-8 character across chunks.
- Keep the incomplete trailing bytes and their length in temporary members such as `utf8_tail` and `utf8_tail_len`.
- Prefix the saved tail to the next chunk before decoding.
- Without this, display output and copied text can be corrupted.

### 6. ConPTY IND/NEL Behavior and Resize Limits

- If ESC D (IND) or ESC E (NEL) appears to do nothing, do not assume a terminal parser bug first.
- Microsoft ConPTY can intercept those control streams and rewrite them into normal cursor positioning sequences such as CUP before delivery.
- Runtime resize intentionally uses truncation and padding instead of reflow to keep the implementation lightweight.
- Avoid manual resize during a running shell session when preserving visual layout matters. Restart the shell if a clean layout is required.

### 7. Keyboard Input Not Working in ConBox

- `CWnd`-derived custom windows do not receive focus automatically on click (unlike standard controls such as `CEdit`).
- `OnLButtonDown` must call `SetFocus()` explicitly so the window can receive keyboard input.
- `FrameBox::PreTranslateMessage` unconditionally consumes `VK_RETURN` and `VK_ESCAPE` even in normal (non-edit) mode. A window that needs these keys (e.g. ConBox returning `DLGC_WANTALLKEYS`) will never receive them unless a guard is added.
- Fix: check `focus->SendMessage(WM_GETDLGCODE) & DLGC_WANTALLKEYS` before the ESC/Enter intercept and yield to `CWnd::PreTranslateMessage` when true.
- TRAP: the `editing_hwnd()` guard in `PreTranslateMessage` only yields to the Parasite subclass proc during control repositioning. It does NOT mean ESC/Enter are consumed only in edit mode -- they are always consumed in normal mode too (except for `DLGC_WANTALLKEYS` windows).

### 8. DemoApp Exit Code and MFC Memory Leak Message

- `CWinApp::ExitInstance()` returns `AfxGetCurrentMessage()->wParam` -- the last pumped message's wParam. When `InitInstance` drives its own modal loop and returns `FALSE`, this yields a meaningless non-zero exit code. Override `ExitInstance()` to `return 0` explicitly.
- "Detected memory leaks! Dumping objects -> Object dump complete." with an EMPTY dump:
  - The header fires when the `_NORMAL_BLOCK` count differs between the initial snapshot and shutdown, but `_CrtMemDumpAllObjectsSince` only prints `_CLIENT_BLOCK` (`DEBUG_NEW`) objects. An empty dump means no user-code leaks -- the difference is in `_NORMAL_BLOCK` (raw `::operator new`, e.g. STL allocators).
  - ROOT CAUSE (confirmed, not an unfixable MFC quirk): a global/static `CWnd`-derived object (e.g. a global `ConBox`) holds STL members (`std::deque`/`vector`/`string`: scrollback, screen grid, buffers). A global object's destructor runs at STATIC DESTRUCTION, which is AFTER the CRT leak snapshot. So those STL allocations are still live at snapshot time -> `_NORMAL_BLOCK` count differs -> header. They are not `_CLIENT_BLOCK`, so the dump is empty. Symptom matches exactly.
  - Calling `DestroyWindow()` on the global does NOT fix it: that frees only the HWND, not the C++ object's STL members (which live until static destruction).
  - FIX (implemented): do not keep such objects global. `new` them and let `~FrameBox()` `delete` them inside the host loop -- attach external windows via `AddNew(...)` and the root frame via `new` + `delete Top` before `InitInstance` returns (see 3.3) -- so all heap members are freed BEFORE the snapshot. This removes the warning entirely (verified: clean build + run, exit code 0, no leak dump). Prefer this over suppressing the check (`_CrtSetDbgFlag`), which would also hide real leaks.

### 9. Per-Monitor DPI: Preserve the Grid, Do Not Resize the PTY

- A DPI change must rescale only the cell pixels (fonts) and KEEP cols/rows fixed. The grid is a
  logical entity; if `update_metrics` recomputes cols/rows from the new window pixels on a DPI change
  and calls `resize_sink` -> `ResizePseudoConsole`, the child shell RE-EMITS its visible screen
  (ConPTY reflow, see 4.6). Our terminal then appended it -> scrollback duplicated/grew on every
  monitor switch; an intermediate wrong-size recompute also truncated content (lost lines).
- `build_font` runs BEFORE the window exists (set_efont/set_kfont via setup_from_ini, and the
  pre-create font build in `open()`), so `GetDpiForWindow(NULL)` returns 0 there. Resolve DPI as:
  `GetDpiForWindow(m_hWnd)` if the window exists, else `box_dpi` (seeded from the parent monitor in
  `open()`), else primary-monitor `GetDeviceCaps`. Naively swapping in `GetDpiForWindow(m_hWnd)`
  alone zeroes `lfHeight` and makes default-size (broken) fonts.
- DO the live DPI work in `OnSize`, NOT in `WM_DPICHANGED_BEFOREPARENT/AFTERPARENT`. Approaches that
  set a "lock grid" flag in those handlers FAILED: the order of AFTERPARENT vs the parent's
  follow-up `MoveWindow`->OnSize is NOT reliable (observed: the grid-recomputing OnSize ran while the
  lock was not yet set and fonts were still old -> grid collapsed to ~19 rows, content truncated).
  `OnSize` is the single point where the grid is computed and `GetDpiForWindow(m_hWnd)` is
  authoritative; compare it to `box_dpi`: if different it is a DPI change -> rebuild fonts + PRESERVE
  the grid (skip `update_metrics`, so no `resize_sink`); if equal it is a user resize -> normal
  recompute. Order-independent, no message-timing assumptions. (For a WS_CHILD ConBox the parent
  FrameBox's `rescale_children` MoveWindow is what fires that OnSize, and the child's DPI is already
  the new value by then.)
- SLACK/CLIP RESIDUAL -- RESOLVED. With cols/rows fixed, the window scales by the exact DPI ratio but
  cell_w/cell_h scale by ROUNDED integer font metrics (plus Korean match-mode leading), so the same
  grid needs a few more (or fewer) pixels than the linearly-scaled window -> deficit clips the last
  column(s)/row, surplus leaves slack. Confirmed: cell_w/cell_h at a higher DPI are NOT exactly the
  DPI-ratio multiple of the 96 DPI metrics (GDI rounds up), and the per-cell error accumulates over
  ~96 cols / ~25 rows into more than one cell (the user's ">1 cell" observation; it is whole cells,
  not pixels). Fix has two parts:
  - `margin` is now 96 DPI LOGICAL padding used ONLY to size the window (`open`) and derive cols/rows
    (`update_metrics`), scaled via `MulDiv(margin, box_dpi, 96)`. Drawing no longer uses margins.
    `adjust` stays raw physical px.
  - `recalc_origin()` CENTERS the grid: `origin = (client - grid) / 2`, clamped to 0 on overflow
    (draw top-left, clip bottom/right). All on-screen draw / hit_test / caret / IME read
    `origin_x/origin_y`; EMF/PDF export keeps its paper margins.
  - `snap_to_grid()` (OnSize DPI branch, after `relayout_for_dpi`) resizes the window to fit the
    preserved grid; ini `snap_mode` (default 2): 0=centering only, 1=grow-only when clipped, 2=always
    exact grid+margin. `SWP_NOMOVE` keeps the upper-left fixed (right/bottom edges move); non-client
    diff added so a bordered popup sizes its CLIENT. The snap's follow-up OnSize has `cur == box_dpi`
    and recomputes the IDENTICAL grid (same size -> no `resize_sink` -> no PTY resize), so even
    mode-2 shrink is corruption-safe.

### 10. ClosePseudoConsole Does Not Guarantee the Child Exits

- `stop()`'s `::ClosePseudoConsole(h_pc)` ends the console session, and most console apps notice
  and exit on their own -- but this is NOT an OS guarantee, only common-case cooperative behavior.
- Measured directly (real run, not just reasoning about the API): closing the host frame while a
  plain, idle `cmd.exe` child was sitting at its prompt did NOT make it exit promptly. It only
  exited once the `terminate()` (`::TerminateProcess`) fallback fired at the `close_kill_timeout_ms`
  grace period then in effect (see REQUIREMENTS #9; default has since changed, 3000 -> 250 ms -- the
  point stands regardless of the exact number). So even the simplest, most well-behaved shell cannot
  be assumed to exit from `ClosePseudoConsole` alone within any short/bounded time.
- Implication: any code path that tears down a `ConBox` with a live child MUST have a forced
  `terminate()` fallback (with a timeout) if it needs to guarantee the child is gone -- `stop()`
  alone can leave an orphaned child process running indefinitely.

### 11. GetGlyphIndicesW Must Use a Throwaway DC, Never the Target Being Recorded

- `HasGlyph()` (glyph-existence check for font fallback) queries via `::GetDC(NULL)`, never the
  `OnPaint` back buffer, an EMF DC (`CreateEnhMetaFileW`), or a printer DC (`save_pdf`). GDI calls on
  an EMF/printer DC are metafile/spool records, not free queries -- running the check there would
  corrupt the recording or the print job, not just waste a call.

### 12. Title-Bar Color: Windows 11 Only, No Per-Window API on Windows 10

- `DWMWA_CAPTION_COLOR`/`DWMWA_TEXT_COLOR`/`DWMWA_BORDER_COLOR` (via `DwmSetWindowAttribute`) require
  Windows 11 build 22000+. On Windows 10 there is no per-window caption background color API at all
  (only `DWMWA_USE_IMMERSIVE_DARK_MODE`, a dark/light toggle) -- a window's title bar there follows
  the system accent-color/theme setting unconditionally.
  `DwmSetWindowAttribute` with an attribute the running OS does not recognize just fails (non-zero
  `HRESULT`), it does not crash -- calling it unconditionally and ignoring the return value is safe
  and needs no OS-version branch.
- The installed SDK (10.0.26100.0) defines all three attribute constants unconditionally in
  `dwmapi.h` (no `NTDDI_VERSION` guard), so `#include <dwmapi.h>` + `#pragma comment(lib,
  "dwmapi.lib")` is enough; no extra version-gated include logic is needed.

### 13. WM_CHAR Does Not Vary Enter's Char Code With Shift -- Only Ctrl Does

- Plain Enter and Shift+Enter both arrive at `OnChar` as `WM_CHAR` with `ch == '\r'` (0x0D): Windows'
  key translation does not give Shift+Enter a different char code, so Shift must be detected
  separately via `GetKeyState(VK_SHIFT)` inside the `'\r'` handler, not inferred from `ch`.
- Ctrl+Enter, in contrast, IS translated differently by Windows itself: it arrives as `ch == '\n'`
  (0x0A), identical to Ctrl+J. This is the OS keyboard-layout translation, not app logic -- do not
  assume `ch` alone identifies the physical key.

### 14. F10 (and Any Alt+key) Is WM_SYSKEYDOWN, Never Reaches WM_KEYDOWN

- Win32 always routes F10-by-itself, and any key held with Alt, through `WM_SYSKEYDOWN`/`WM_SYSCHAR`,
  not `WM_KEYDOWN`/`WM_CHAR` -- this is a fixed OS rule (F10 activates the menu bar; Shift+F10's
  context-menu convention uses the same path), not something app code opts into.
- Neither `ConBox` nor `FrameBox` has an `ON_WM_SYSKEYDOWN`/`OnSysKeyDown` handler anywhere. Result:
  `terminal_keydown()`'s `case VK_F10` (sends the xterm F10 VT sequence) was already unreachable dead
  code before the `[macros]` feature existed -- confirmed by tracing the message path, not by a live
  keypress test. Any future F10 (or Alt+anything) handling needs an explicit `OnSysKeyDown` override
  that consumes the message (else `DefWindowProc` tries to activate the system menu).

### 15. ParseIni's Map Collapses Repeated Keys -- [triggers] Needed a Separate Line-Order Parser

- `ParseIni()` builds a `std::map<key, value>`, so a repeated key (`match=` appearing many times, one
  per `[triggers]` rule) collapses to only the last occurrence -- unusable for a format where the
  same key name intentionally repeats once per group.
- Fix: factored the single-line "key = value" extraction out of `ParseIni()` into `ParseIniLine()`,
  then added `ParseTriggers()` which walks the raw INI text in line order (not through the map) and
  groups `match=`/`send=`/`cool=` runs itself. `ParseIni()`'s map is still used for every other
  (non-repeating) key.

### 16. setup_from_ini() No Longer Auto-Creates a File on a Missing Path

- The behavior this entry used to warn about (a missing path silently auto-created a default INI,
  making a speculative existence probe dangerous) was removed: `setup_from_ini()` on a missing path
  now only appends a status message and leaves settings unchanged -- see
  `Documents/2. ConBox/REQUIREMENTS.md` #13 for the current behavior. Probing a candidate path by
  calling `setup_from_ini()` directly is now safe.
- `CreateDefaultIni()` (the function that used to write that file) was replaced by
  `ConBox::create_current_ini()`, a public member that returns the CURRENT resolved settings as INI
  text instead of writing a fixed default to a file (REQUIREMENTS #13).

### 17. OnPaint Background Fill Used cur_bg (Per-Cell SGR State), Not default_bg

- `OnPaint` filled the whole client rect (margins included) with `cur_bg` -- the per-cell "SGR
  background for the next glyph to be written" state -- instead of `default_bg`. A full-screen TUI
  actively re-coloring a mouse selection (see REQUIREMENTS #12) can leave `cur_bg` transiently holding
  the selection-highlight color, which then leaked into the margin area on the next repaint.
- Fix: fill the client background with `default_bg` unconditionally. `cur_bg` is per-cell state
  consumed by `put_char`, never a stand-in for "the terminal's background color."

### 18. The Mouse-Reporting Path in OnMouseMove Returns Early -- Local Hover Never Runs

- `OnMouseMove` forwards motion to the child and `return`s while `mouse_reporting()` holds, BEFORE the
  gutter-hover block further down. So anything that needs local hover state while the child owns the
  mouse (the thumbless overlay scrollbar needs `sbar_hover` both to appear and to accept clicks) is
  silently dead: the hover flag never turns on, so the bar never shows and its hit-test never arms.
- Same shape in `OnLButtonDown`/`OnLButtonDblClk`: the forward-to-child block sits ahead of the gutter
  handling, so a gutter press reached the child instead of the bar until the gutter check was moved in
  front of it.
- Rule: a local UI element that must stay usable during mouse reporting has to be handled BEFORE the
  forwarding block in every mouse handler, not after it. Grep for `mouse_reporting()` and check the
  ordering at each site when adding one.

### 19. SGR Mouse Reporting Cannot Express a Line Count -- Only Notches

- Forwarded scrolling is quantized to wheel notches (`ESC[<64` up / `<65` down); there is no way to say
  "scroll N lines" or "go to position P". How many lines a notch is worth is decided by the CHILD
  (~3 by the common convention, which is also what ConBox's own local wheel path uses).
- Consequences when translating a scrollbar gesture into notches: an arrow cannot be finer than 1 notch
  (so it moves ~3 lines, not the 1 line the local path gives), and a "page" must be divided down --
  `rows` notches would move ~3 screens, not one. ConBox sends `rows / 4`.
- Do NOT compensate by sending multiple notches per physical wheel click: 1 notch per click already
  matches the local path's 3 lines, and every other terminal (xterm, wt.exe) sends exactly one.

### 20. ParseIniLine Treated a CRLF Blank Line as Malformed

- `ParseIni`/`ParseTriggers` split INI text on `\n` only (`std::getline`), so a blank line in a
  CRLF-encoded file (every INI in this project) arrives as a lone `"\r"`, not an empty string.
- `ParseIniLine`'s leading-whitespace skip only recognized `' '`/`'\t'`, so that lone `\r` was treated
  as real content with no `=` and reported as `"ignored malformed settings line (no '=')"` -- one such
  warning per blank line in the file (12 in `jbTerm.ini`, matching its 12 section-separator blank
  lines), even though nothing was actually wrong.
- Fixed by also skipping `'\r'` in that leading-whitespace loop.

### 21. Testing stdin/stdout Redirection: Two Automation Gotchas

- A `>`-redirected destination file is created EMPTY at process launch, before the child ever writes
  to it -- `Test-Path`/existence alone races the real write. Poll until the file's size stops
  changing, not merely until it exists (worse right after copying+resource-patching a fresh exe, e.g.
  jbTerm's clone-to-exe menu action: that exe's first-ever launch can be slowed by a real-time
  antivirus scan).
- When scripting a launch of `jbTerm.exe` for a test, never kill it by bare process name (see root
  `PITFALLS.md` #17 -- this Claude Code session can itself be hosted inside a `jbTerm.exe` ConPTY
  process). Resolve the exact PID via the process tree of the specific launch the script started
  (e.g. `Win32_Process.ParentProcessId`) and kill only that PID.
- `cmd.exe` does not wait for a launched GUI-subsystem (`/SUBSYSTEM:WINDOWS`) process the way it
  waits for a console one -- `cmd /c "gui.exe ..."` returns almost immediately regardless of whether
  `gui.exe` is still starting up or has already exited. A test that infers success/failure from
  `cmd.exe`'s own exit code or from how long the wrapping `cmd.exe` process stays alive is measuring
  the wrong process; poll for the target process (or its output file) directly instead.

### 22. A Host Launched With Its Own stdio Redirected Broke the ConPTY Child's stdin

- `start()`'s `CreateProcessW` intentionally does not set `STARTF_USESTDHANDLES` (the pseudoconsole
  attribute is what gives the child its console -- the documented ConPTY pattern). Without that flag,
  `CreateProcessW` still copies THIS process's current standard-handle VALUES into the child's
  process parameters. When the host itself was launched with its own stdio redirected (e.g. jbTerm
  run as `jbTerm.exe < settings.ini`), those values are file/pipe handles; the console subsystem does
  not swap them for the pseudoconsole's the way it swaps real console handles, and since
  `bInheritHandles` is `FALSE` they are not valid handles in the child's own table either.
- Symptom looked nothing like a handle bug: the child shell started, ran its startup batch/profile
  fine (proving the process itself launched correctly), then read EOF from its now-broken stdin on
  its very first prompt and exited -- which fired `exit_cb` and closed the HOST window within
  milliseconds of launch, with no crash dialog and no error-log entry (clean `WM_CLOSE`, not a fault).
- Confirmed by making the child's stdin state directly observable: a probe batch that does `set /p
  X=` blocks forever on a real console (the working case, e.g. `@file`) but returns immediately when
  the host's own stdin was redirected (the broken case) -- the same INI content, only the host's
  launch handle state differs.
- Fix: blank `STD_INPUT/OUTPUT/ERROR_HANDLE` to `NULL` on this process for the duration of the
  `CreateProcessW` call, restore them right after. `NULL` is the state a GUI host launched from
  Explorer already has (the case that always worked), so this makes every launch state converge on
  it at the one moment it matters, regardless of how the host itself was started.
