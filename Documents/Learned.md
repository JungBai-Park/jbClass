# Development Notes

This file records project-specific pitfalls and implementation details that are easy to forget. General project rules, coding conventions, encoding rules, folder layout, and default build policy are defined in `CLAUDE.md` and are not repeated here.

## 1. Build and Toolchain Pitfalls

### 1.1 Visual Studio Platform Names

- At solution level, the 32-bit platform name is `x86`.
- `Win32` is the platform name used inside `.vcxproj` files. Passing `Win32` to solution-level MSBuild can produce `MSB4126`.
- Known x64 Debug build command:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" "Project\jbClass.sln" /t:Build /p:Configuration=Debug /p:Platform=x64
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

### 2.4 Factory Macro Hazards (FrameBox Add*/Open)

- The `Add*`/`Open` factory macros are global, function-like, case-sensitive textual substitutions defined at the bottom of `FrameBox.h`.
- `Open` collides with MFC's own `Open(...)` members (e.g. `CFile::Open` in `afxpriv.h`): a function-like `Open` macro rewrites those declarations and breaks the header with cascading parse errors. Include any MFC header that declares an `Open(...)` member (notably `afxpriv.h`, needed for `AfxHookWindowCreate`) BEFORE `FrameBox.h`, so it is parsed before the macro exists. Consumers that pull no such header after `FrameBox.h` are unaffected.
- Do NOT write `Add*/Open` (or any `...*/...`) inside a `/* */` block comment: the `*/` closes the comment early and everything after it compiles as code. Use `Add... / Open` or a `//` line comment instead. (Hit this in both `FrameBox.h` and `FrameBox.cpp` headers this session.)

## 3. FrameBox

### 3.1 Host Notification Reflection

- Notifications such as `BN_CLICKED`, `EN_CHANGE`, `CBN_SELCHANGE`, and scroll request codes are delivered to the parent window, not directly to the control.
- To let the `Parasite` subclass receive a reflected notification, the host must send the callback to the control HWND:

```cpp
::SendMessage(ctrlHwnd, WM_PARASITE_CALLBACK, 0, (LPARAM)(int)code);
```

- Do not call `wnd->WindowProc(...)` through a C++ object pointer for this path. It bypasses the `SetWindowSubclass` chain and loses the notification.
- Preserve signed notification codes. Do not truncate them with `LOWORD`.

### 3.2 Source Rewriter Limits

- The `Add*`/`Open` macros inject `__FILE__` and `__LINE__`; coordinates are ALWAYS the first four arguments.
- The rewriter anchors on a member-call on the target line: `.`/`->` followed by an uppercase-initial identifier and `(` whose first argument is an integer literal (covers `AddStatic`/`AddNew`/`Open`/...). It then rewrites the first four comma/paren-delimited integer literals, with NO leading-argument skip (the old `"LayOut("` + skip-2 logic is gone).
- The trailing argument (init string for controls, window pointer for `AddNew`/`AddAsItIs`) must be preserved; it is.
- The rewriter supports only single-line calls with integer literals.
- UTF-16 BOM source files are not supported. ANSI and UTF-8 files are supported.
- Create a `.bak` file before rewriting.

### 3.3 Lifetime and Ownership

- `Parasite` stores a borrowed reference and does not own the target control.
- Each `FrameBox` OWNS its children through a per-instance registry (the global `new_window_registry()` / `DeleteLayOutWindows()` are gone). `~FrameBox()` (= `close()`) processes the registry in reverse-registration order: `delete Parasite` (removes subclass) -> `DestroyWindow` -> `delete wnd`. A child-frame entry's `delete wnd` recurses into that child's `~FrameBox`, tearing down grandchildren. Do NOT call `DestroyWindow()` separately on registered controls -- it is redundant.
- `AddAsItIs(..., wnd)` registers `{ nullptr, Parasite }` (borrowed): teardown removes the subclass only. `AddNew(..., wnd)` registers `{ wnd, Parasite }` (owned): teardown also `DestroyWindow`s + `delete`s wnd, so wnd must be a heap (`new`) object. Both attach the editing subclass, so the window stays live-editable.
- The root `FrameBox` is created with `new`, bound via `attach(theApp)`, opened via `Open(...)`, and `delete`d by the function that created it (the modal-loop driver) before `InitInstance` returns. `attach()` sets `m_pMainWnd = this`; `close()` clears it FIRST so destruction never leaves a dangling main window.
- Because `FrameBox` is `CWnd`-based (not `CFrameWnd`), `OnDestroy` does not post `WM_QUIT`; the old `m_pMainWnd = nullptr` + `DeleteLayOutWindows()` dance in `InitInstance` is no longer needed.
- A child frame (`AddZone`/`AddFrame`) closing tears down only its OWN registry, so the former footgun -- a child calling the global `DeleteLayOutWindows()` and wiping the main window -- cannot happen anymore.
- If the parent window is destroyed first (`WM_NCDESTROY`), `~Parasite` skips `RemoveWindowSubclass` (HWND already gone) and never deletes the target, so teardown is safe in either order.
- `FrameBox::PostNcDestroy` must be a no-op (lifetime is owner-managed; a self-delete would double-free).
- `FrameBox` creates its window via the `AfxHookWindowCreate` + `CreateWindowExW` pattern (see 2.2), exactly like `cConBox`: it registers one shared `"FrameBox"` class and selects top-level / `WS_CHILD` zone / `WS_POPUP` purely by the style passed at creation.

### 3.4 Edit-Mode Key Commit and WM_CHAR Leak

- When `PreTranslateMessage` returns `FALSE` for a `WM_KEYDOWN` in edit mode, the MFC pump calls `TranslateMessage` **before** `DispatchMessage`. `TranslateMessage` posts `WM_CHAR` to the queue immediately -- before the subclass proc runs and sets `editing = false`.
- After `leave_edit()` returns, `editing` is false, so the queued `WM_CHAR` (`'\r'` for Enter, `'\x1b'` for ESC) passes through the subclass proc and reaches the child window as input.
- Fix: call `::PeekMessageW(&dummy, h, WM_CHAR, WM_CHAR, PM_REMOVE)` immediately after `leave_edit()` in both paths (Enter commit in `on_key`, ESC cancel in the `WM_KEYDOWN` dispatch). `TranslateMessage` has already run at that point so the char is in the queue and can be removed.
- Double-click commit does not have this issue -- mouse messages do not generate `WM_CHAR`.

### 3.5 CEdit Vertical Alignment

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
- `FrameBox::PreTranslateMessage` unconditionally consumes `VK_RETURN` and `VK_ESCAPE` even in normal (non-edit) mode. A window that needs these keys (e.g. ConBox returning `DLGC_WANTALLKEYS`) will never receive them unless a guard is added.
- Fix: check `focus->SendMessage(WM_GETDLGCODE) & DLGC_WANTALLKEYS` before the ESC/Enter intercept and yield to `CWnd::PreTranslateMessage` when true.
- TRAP: the `editing_hwnd()` guard in `PreTranslateMessage` only yields to the Parasite subclass proc during control repositioning. It does NOT mean ESC/Enter are consumed only in edit mode -- they are always consumed in normal mode too (except for `DLGC_WANTALLKEYS` windows).

### 4.8 DemoApp Exit Code and MFC Memory Leak Message

- `CWinApp::ExitInstance()` returns `AfxGetCurrentMessage()->wParam` -- the last pumped message's wParam. When `InitInstance` drives its own modal loop and returns `FALSE`, this yields a meaningless non-zero exit code. Override `ExitInstance()` to `return 0` explicitly.
- "Detected memory leaks! Dumping objects -> Object dump complete." with an EMPTY dump:
  - The header fires when the `_NORMAL_BLOCK` count differs between the initial snapshot and shutdown, but `_CrtMemDumpAllObjectsSince` only prints `_CLIENT_BLOCK` (`DEBUG_NEW`) objects. An empty dump means no user-code leaks -- the difference is in `_NORMAL_BLOCK` (raw `::operator new`, e.g. STL allocators).
  - ROOT CAUSE (confirmed, not an unfixable MFC quirk): a global/static `CWnd`-derived object (e.g. a global `cConBox`) holds STL members (`std::deque`/`vector`/`string`: scrollback, screen grid, buffers). A global object's destructor runs at STATIC DESTRUCTION, which is AFTER the CRT leak snapshot. So those STL allocations are still live at snapshot time -> `_NORMAL_BLOCK` count differs -> header. They are not `_CLIENT_BLOCK`, so the dump is empty. Symptom matches exactly.
  - Calling `DestroyWindow()` on the global does NOT fix it: that frees only the HWND, not the C++ object's STL members (which live until static destruction).
  - FIX (implemented): do not keep such objects global. `new` them and let `~FrameBox()` `delete` them inside the host loop -- attach external windows via `AddNew(...)` and the root frame via `new` + `delete Top` before `InitInstance` returns (see 3.3) -- so all heap members are freed BEFORE the snapshot. This removes the warning entirely (verified: clean build + run, exit code 0, no leak dump). Prefer this over suppressing the check (`_CrtSetDbgFlag`), which would also hide real leaks.
