# Development Notes

This file records project-specific pitfalls and implementation details that are easy to forget. General project rules, coding conventions, encoding rules, folder layout, and default build policy are defined in `CLAUDE.md` and are not repeated here.

## 1. Build and Toolchain Pitfalls

### 1.1 Visual Studio Platform Names

- At solution level, the 32-bit platform name is `x86`.
- `Win32` is the platform name used inside `.vcxproj` files. Passing `Win32` to solution-level MSBuild can produce `MSB4126`.
- Known x64 Debug build command:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" "Project\ConBox.sln" /t:Build /p:Configuration=Debug /p:Platform=x64
```

- Build output uses configuration-specific suffix directories such as `Project\Debug.64\` and `Project\Debug.32\`.

### 1.2 Bash MSBuild Option Issue

- Running MSBuild from Bash can corrupt slash-prefixed options such as `/t:` by treating them as POSIX-style paths.
- Use PowerShell for MSBuild commands that pass Visual Studio properties or targets.

### 1.3 Temp Directory Handling

- The project `Temp\` directory can be automatically reset at session boundaries.
- Do not recursively delete `Temp\` during a live development session because harness outputs may still depend on files under it.

## 2. Source Compatibility Pitfalls

### 2.1 Comment Encoding and Korean Literals

- Non-ASCII comments in `Source/*.h` and `Source/*.cpp` can trigger `C4819`.
- For wide literals used as fixed code-point probes, prefer explicit escapes such as `L"\xac00"` so the value does not depend on source decoding.
- For user-visible narrow string literals that are intentionally UTF-8, direct Korean text is allowed when the source file is saved as UTF-8 with BOM.

### 2.2 Explicit Win32 APIs in MBCS Builds

- `CFont::CreateFontIndirectW` is not available in MBCS builds. Use `CFont::Attach(::CreateFontIndirectW(&lf))`.
- `CFont::GetLogFont(LOGFONTW*)` can map internally to `GetObjectA`, causing a mismatch between 60-byte and 92-byte structures. Use `::GetObjectW(hFont, sizeof(LOGFONTW), &lf)`.
- Use `::TextOutW(dc.GetSafeHdc(), ...)` instead of `CDC::TextOutW`.
- Use `::CreateEnhMetaFileW` explicitly because `::CreateEnhMetaFile` maps to the `A` variant in MBCS builds.
- When converting paths from `CString`, force the wide constructor with `CStringW wpath(str.GetString())` before calling `WideCharToMultiByte`.
- `IDC_ARROW` and other `IDC_*` cursor constants expand to `MAKEINTRESOURCEA` (= `LPCSTR`) in MBCS. Cast to `(LPCWSTR)` when passing to `LoadCursorW`.
- `CDC::GetTextMetricsW` does not exist in MBCS builds. Use `::GetTextMetricsW(dc.GetSafeHdc(), &tm)`.
- `CWnd::CreateEx` accepts `LPCTSTR` (= `LPCSTR` in MBCS), so a `const wchar_t*` class-name literal cannot be passed directly. Use `AfxHookWindowCreate(this)` + `::CreateWindowExW(...)` + `AfxUnhookWindowCreate()` to create a CWnd-derived window with a wide class name while keeping full MFC message-map support. `Attach(hwnd)` alone stores only the HWND with no WndProc replacement (message map never fires). `SubclassWindow(hwnd)` works but registers an "external subclassing" entry that can produce a phantom leak for global CWnd objects.

### 2.3 Bulk Replacement Hazards

- Short replacement patterns can accidentally modify longer identifiers or expressions.
- Example: replacing `dc.TextOutW(` can damage `cdc.TextOutW(`.
- Replace the longest and most specific patterns first, then manually inspect the result.

## 3. LayOut

### 3.1 Host Notification Reflection

- Notifications such as `BN_CLICKED`, `EN_CHANGE`, `CBN_SELCHANGE`, and scroll request codes are delivered to the parent window, not directly to the control.
- To let the `cLayOut` subclass receive a reflected notification, the host must send the callback to the control HWND:

```cpp
::SendMessage(ctrlHwnd, WM_EVOL_CALLBACK, 0, (LPARAM)(int)code);
```

- Do not call `wnd->WindowProc(...)` through a C++ object pointer for this path. It bypasses the `SetWindowSubclass` chain and loses the notification.
- Preserve signed notification codes. Do not truncate them with `LOWORD`.

### 3.2 Source Rewriter Limits

- `LayOut(Type, parent, x0, y0, x1, y1 [,init])` injects `__FILE__` and `__LINE__`.
- The rewriter searches for `"LayOut("`, skips the first two arguments, then updates only the four coordinate literals.
- Extra arguments such as initialization text must be preserved.
- The rewriter supports only single-line calls with integer literals.
- UTF-16 BOM source files are not supported. ANSI and UTF-8 files are supported.
- Create a `.bak` file before rewriting.

### 3.3 Lifetime and Ownership

- `cLayOut` stores a borrowed reference and does not own the target control.
- Declare variables so each `cLayOut` instance is destroyed before the related `CWnd` member or stack variable.
- `DeleteLayOutWindows()` processes the registry in reverse-registration order: `delete cLayOut` (removes subclass) → `DestroyWindow` → `delete wnd`. It handles everything; do NOT call `DestroyWindow()` separately on registered controls -- it is redundant and creates confusion.
- `LayOut(this, wnd, ...)` registers with `wnd = nullptr`. `DeleteLayOutWindows()` removes the subclass only and does not call `DestroyWindow` or `delete` on that window.
- If the parent window is destroyed first and `WM_NCDESTROY` arrives, immediately clear the target pointer to avoid double release.
- `cModalFrame` lifetime is manually controlled by `DeleteLayOutWindows()`. Its `PostNcDestroy` must be a no-op and must not call `delete this`.
- When opening a modal sub-frame inside an event loop, do not call `DeleteLayOutWindows()` from the child unexpectedly. It can erase global registry resources that include the main window. Reactivate the parent after the dialog closes, then clean up outside the loop.
- Global `CWnd`-derived objects (e.g. `cConBox conBox`) that are NOT registered via the LayOut factory must have `DestroyWindow()` called explicitly BEFORE `DeleteLayOutWindows()`. Without this, the global destructor fires after MFC's cleanup, leaving a stale HWND in the handle map and triggering a phantom "Detected memory leaks!" message.

### 3.4 CEdit Vertical Alignment

- `EM_SETRECT` is ignored by single-line edit controls.
- To adjust vertical margins, the edit control must include `ES_MULTILINE`.
- When calling `SetRect()`, first call `GetRect()` and change only the Y-axis top offset. This avoids corrupting horizontal inset margins and border rendering.
- If `PreTranslateMessage` intercepts Enter, always return `TRUE` after handling it. Otherwise an unintended `'\r'` can enter the edit view.

## 4. ConBox

### 4.1 Font Measurement and Korean Matching

- The final measured height for the Korean matching font must be handled at the end of `calc_cell_size`.
- Do not build the Korean matching font early in `set_kfont`.
- If it is built before font match information is available, the GDI object is created with a default height of 0 and Korean glyphs render too small compared with English 2x output.

### 4.2 SGR Double-Size Rendering

- Use standard SGR 8 for double-size on and SGR 28 for double-size off.
- Do not invent a private control code for this feature because conhost may drop unknown private sequences.
- Double-size glyphs bleed pixels into cells on the right.
- Paint each row from right to left so adjacent clear operations do not erase the bleed pixels and leave visual artifacts.

### 4.3 Cursor and Korean IME Commit Order

- If a navigation key or Enter is received while Korean IME composition is active, call `ImmNotifyIME(CPS_COMPLETE)` first.
- Send the special key only after the committed text is delivered.
- The child process must receive input in this order: committed text, then movement or control key.
- After Korean syllable commit, the emulation path advances the cursor one cell to the right.
- To compensate, swallow right-arrow input when needed and send the left-arrow sequence twice for left-arrow correction.

### 4.4 EMF Export Pitfalls

- In an EMF DC, `BeginPath` and `FillPath` do not reliably wrap `TextOutW`.
- If path-based filling is used, text can appear as black blocks.
- Set the foreground color with `SetTextColor(fg)` and draw text directly.
- Pass `NULL` as the media bounds rectangle pointer to `CreateEnhMetaFile` so GDI fits the output bounds tightly.
- If a `CDC` is attached to a target HDC, call `Detach()` before closing or destroying the HDC. Otherwise the CDC destructor can destroy the handle at the wrong time.

### 4.5 UTF-8 Chunk Boundaries

- Data from an external process pipe can split a multibyte UTF-8 character across chunks.
- Keep the incomplete trailing bytes and their length in temporary members such as `utf8_tail` and `utf8_tail_len`.
- Prefix the saved tail to the next chunk before decoding.
- Without this, display output and copied text can be corrupted.

### 4.6 ConPTY IND/NEL Behavior and Resize Limits

- If ESC D (IND) or ESC E (NEL) appears to do nothing, do not assume a terminal parser bug first.
- Microsoft ConPTY can intercept those control streams and rewrite them into normal cursor positioning sequences such as CUP before delivery.
- Runtime resize intentionally uses truncation and padding instead of reflow to keep the implementation lightweight.
- Avoid manual resize during a running shell session when preserving visual layout matters. Restart the shell if a clean layout is required.

### 4.7 Keyboard Input Not Working in cConBox

- `CWnd`-derived custom windows do not receive focus automatically on click (unlike standard controls such as `CEdit`).
- `OnLButtonDown` must call `SetFocus()` explicitly so the window can receive keyboard input.
- `cModalFrame::PreTranslateMessage` unconditionally consumes `VK_RETURN` and `VK_ESCAPE` even in normal (non-edit) mode. A window that needs these keys (e.g. ConBox returning `DLGC_WANTALLKEYS`) will never receive them unless a guard is added.
- Fix: check `focus->SendMessage(WM_GETDLGCODE) & DLGC_WANTALLKEYS` before the ESC/Enter intercept and yield to `CFrameWnd::PreTranslateMessage` when true.
- TRAP: the `editing_hwnd()` guard in `PreTranslateMessage` only yields to the cLayOut subclass proc during control repositioning. It does NOT mean ESC/Enter are consumed only in edit mode -- they are always consumed in normal mode too (except for `DLGC_WANTALLKEYS` windows).

### 4.8 DemoApp Exit Code and MFC Memory Leak Message

- `CWinApp::ExitInstance()` returns `AfxGetCurrentMessage()->wParam` -- the last pumped message's wParam. When `InitInstance` drives its own modal loop and returns `FALSE`, this yields a meaningless non-zero exit code. Override `ExitInstance()` to `return 0` explicitly.
- "Detected memory leaks! Dumping objects -> Object dump complete." (empty dump) is an MFC false positive. MFC's leak check fires when `_NORMAL_BLOCK` count differs between the initial snapshot and shutdown, but `_CrtMemDumpAllObjectsSince` only prints `_CLIENT_BLOCK` objects. An empty dump means no user-code (`DEBUG_NEW`) leaks exist. The `_NORMAL_BLOCK` difference comes from MFC's internal `CHandleMap` / STL nodes and cannot be eliminated without suppressing the check. It is safe to ignore.
