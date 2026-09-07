# PITFALLS.md

This document captures environmental traps, library conflicts, and external API anomalies encountered during development to prevent recurring trial-and-error across the project.



This project adopts a modular architecture, with the PITFALLS.md files located as follows:

- General/Common Decisions: Refer to the "Common Pitfalls" section of this file below.
- FrameBox: `Documents\1. FrameBox\PITFALLS.md`
- ConBox: `Documents\2. ConBox\PITFALLS.md`
- TableBox: `Documents\3. TableBox\PITFALLS.md`



[CRITICAL FOR TOKEN SAVING] 

- Global guidelines that apply across all modules and are critical enough to be read at the start of every session must be documented in the "Common Pitfalls" section below.

- DO NOT read or reference individual module PITFALLS.md files by default. Access them ONLY when explicitly required for the current task. Any modifications or additions to design decisions made during the session must be updated directly in the corresponding module's file, NOT in this global document.



---

## Common Pitfalls

### 1. Visual Studio Platform Names

- At solution level, the 32-bit platform name is `x86`.
- `Win32` is the platform name used inside `.vcxproj` files. Passing `Win32` to solution-level MSBuild can produce `MSB4126`.
- Known x64 Debug build command:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "Build\jbBox.sln" /t:Build /p:Configuration=Debug /p:Platform=x64
```

- Solution directory is `Build\` (solution `jbBox.sln` at its root); each project lives in its own subdirectory, e.g. `Build\DemoApp\DemoApp.vcxproj`.
- Build output uses per-project, configuration-specific suffix directories (`OutDir`/`IntDir` set to `$(ProjectDir)$(Configuration).64\` etc., not `$(SolutionDir)`), e.g. `Build\DemoApp\Debug.64\`.
- If a previously launched `DemoApp.exe` is still running, the link step fails with `LNK1168` (cannot open exe for writing). Kill it first: `Get-Process DemoApp -ErrorAction SilentlyContinue | Stop-Process -Force` before MSBuild.

### 2. Bash MSBuild Option Issue

- Running MSBuild from Bash can corrupt slash-prefixed options such as `/t:` by treating them as POSIX-style paths.
- Use PowerShell for MSBuild commands that pass Visual Studio properties or targets.

### 3. Temp Directory Handling

- The project `Temp\` directory can be automatically reset at session boundaries.
- Do not recursively delete `Temp\` during a live development session because harness outputs may still depend on files under it.

### 4. Comment Encoding and Korean Literals

- Non-ASCII comments in `Source/*.h` and `Source/*.cpp` can trigger `C4819`.
- For wide literals used as fixed code-point probes, prefer explicit escapes such as `L"\xac00"` so the value does not depend on source decoding.
- For user-visible narrow string literals that are intentionally UTF-8, direct Korean text is allowed when the source file is saved as UTF-8 with BOM.

### 5. Explicit Win32 APIs in MBCS Builds

- `CFont::CreateFontIndirectW` is not available in MBCS builds. Use `CFont::Attach(::CreateFontIndirectW(&lf))`.
- `CFont::GetLogFont(LOGFONTW*)` can map internally to `GetObjectA`, causing a mismatch between 60-byte and 92-byte structures. Use `::GetObjectW(hFont, sizeof(LOGFONTW), &lf)`.
- Use `::TextOutW(dc.GetSafeHdc(), ...)` instead of `CDC::TextOutW`.
- Use `::CreateEnhMetaFileW` explicitly because `::CreateEnhMetaFile` maps to the `A` variant in MBCS builds.
- When converting paths from `CString`, force the wide constructor with `CStringW wpath(str.GetString())` before calling `WideCharToMultiByte`.
- `IDC_ARROW` and other `IDC_*` cursor constants expand to `MAKEINTRESOURCEA` (= `LPCSTR`) in MBCS. Cast to `(LPCWSTR)` when passing to `LoadCursorW`.
- `CDC::GetTextMetricsW` does not exist in MBCS builds. Use `::GetTextMetricsW(dc.GetSafeHdc(), &tm)`.
- `CWnd::CreateEx` accepts `LPCTSTR` (= `LPCSTR` in MBCS), so a `const wchar_t*` class-name literal cannot be passed directly. Use `AfxHookWindowCreate(this)` + `::CreateWindowExW(...)` + `AfxUnhookWindowCreate()` to create a CWnd-derived window with a wide class name while keeping full MFC message-map support. `Attach(hwnd)` alone stores only the HWND with no WndProc replacement (message map never fires). `SubclassWindow(hwnd)` works but registers an "external subclassing" entry that can produce a phantom leak for global CWnd objects.

### 6. Bulk Replacement Hazards

- Short replacement patterns can accidentally modify longer identifiers or expressions.
- Example: replacing `dc.TextOutW(` can damage `cdc.TextOutW(`.
- Replace the longest and most specific patterns first, then manually inspect the result.

### 7. Factory Macro Hazards (FrameBox Add*/OpenFrame)

- The `Add*`/`OpenFrame` factory macros are global, function-like, case-sensitive textual substitutions defined at the bottom of `FrameBox.h`.
- `OpenFrame` takes 5 arguments and does NOT collide with MFC `Open(...)` members, so `afxpriv.h` no longer needs to precede `FrameBox.h`. Always include `"FrameBox.h"` first in `.cpp` files so `_WIN32_WINNT` is defined before `<afxwin.h>` is parsed (avoids the `_WIN32_WINNT not defined` warning).
- Do NOT write `Add*` (or any `...*/...`) inside a `/* */` block comment: the `*/` closes the comment early and everything after it compiles as code. Use `Add...` or a `//` line comment instead.

### 8. C++ Standard Version Check on MSVC

- MSVC pins `__cplusplus` at `199711L` unless `/Zc:__cplusplus` is passed, so a `#if __cplusplus >= 201703L` guard misfires even with `/std:c++17`.
- Use `_MSVC_LANG` (always reflects `/std`) when defined, else `__cplusplus`. (The `FrameBox.h` C++17 `#error` guard was removed when `NewFrame` copy-elision dependency was eliminated; this pitfall still applies to any new guards added.)

### 9. WM_JBZOOM Must Be Sent BEFORE rescale_children() / MoveWindow

- ConBox needs `zoom_pm` set and `zoom_resize = true` BEFORE `MoveWindow` triggers `OnSize`.
  `OnSize` reads `zoom_resize` at entry to decide between `relayout_for_dpi` (zoom/DPI path,
  no PTY resize) and `update_metrics` (user-resize path, may resize PTY). If `WM_JBZOOM` arrives
  after `MoveWindow`, `OnSize` already ran with `zoom_resize = false` and took the wrong path.
- Same principle for TableBox: `build_font()` inside `WM_JBZOOM` uses the new `eff_dpi()` so the
  font is correct when `OnSize` → `Invalidate` runs.
- Rule: always send `WM_JBZOOM` to all children BEFORE calling `rescale_children()`.

### 10. snap_to_grid Is Synchronous -- fit_to_children Reads the Final Size Immediately

- ConBox's `snap_to_grid()` calls `SetWindowPos(m_hWnd, ...)` which, for a same-thread window,
  runs synchronously and delivers `WM_SIZE` → `OnSize` before returning. This entire chain
  executes INSIDE the single `MoveWindow(ConBox)` call in `rescale_children()`.
- When `rescale_children()` returns, ConBox already has its final post-snap window size.
  `fit_to_children()` (called at the end of `rescale_children`) can therefore read the actual
  `GetWindowRect` of each child and compute the correct FrameBox size immediately -- no deferred
  message, timer, or callback is needed.

### 11. ConBox zoom_resize Flag: Why It Is Needed

- A zoom-triggered `OnSize` has `cur == box_dpi` (no real monitor DPI change), so the existing
  `cur != box_dpi` guard in `OnSize` would fall through to `update_metrics()` → potential PTY
  resize and grid recompute.
- `WM_JBZOOM` handler sets `zoom_resize = true` before the `MoveWindow` that fires `OnSize`.
  `OnSize` checks `dpi_changed || zoom_resize`; if either is true, clears `zoom_resize` and
  takes the `relayout_for_dpi` + `snap_to_grid` path (no PTY resize).

### 12. apply_zoom Must Be virtual+protected for Subclass Title-Bar Hooks

- Ctrl+Wheel calls `apply_zoom()` directly (not via a message), so subclasses cannot intercept
  it through `WindowProc`. Making `apply_zoom` `virtual protected` lets a subclass override it:
  call `FrameBox::apply_zoom(new_pm, anchor)` then update a title bar or status indicator.
- `dpi` and `zoom_pm` are also `protected` (not `private`) so the override can read them.

### 13. Naming Unification: ConBox build_font / to_px

- ConBox previously used `make_font` (TableBox uses `build_font`) and `sbar_px` (TableBox uses
  `to_px`) for the same concepts. Renamed to `build_font` / `to_px` in ConBox to match TableBox.
- If divergence reappears in future sessions, the canonical names are `build_font` and `to_px`.

### 14. CBM (codebase-memory-mcp) Cannot Parse MFC Message-Map Macros

- `BEGIN_MESSAGE_MAP(...)`/`ON_WM_*()`/`END_MESSAGE_MAP()` are not real C++ syntax before macro
  expansion, and CBM's indexer (tree-sitter, no preprocessor) can choke on them. At a top-level
  occurrence (translation-unit scope) it is usually a handful of 1-line parse blips that resync
  immediately next line. But one occurrence (a `BEGIN_MESSAGE_MAP` for a locally-defined class,
  deep inside `TableBox.cpp`) made the parser fail to resync AT ALL for the rest of the file
  (~1250 lines silently missing from the graph) -- confirmed by comparing the indexer's reported
  error-range format: many separate `N-N` (1-line) entries vs. one giant `1000-2276` (to EOF) entry.
- Fix applied project-wide: move every `BEGIN_MESSAGE_MAP`/`END_MESSAGE_MAP` block to the end of
  its `.cpp` file (see project `CLAUDE.md` coding rule #4). Verified by re-indexing: the giant
  range collapsed back to small per-macro-line blips, node/edge counts increased (920->961,
  3170->3517). `DECLARE_MESSAGE_MAP()` cannot move (must stay inside the class body in the header)
  but only produces a harmless 1-line blip on its own.
- This is a CBM/tree-sitter parsing limitation, not a project code defect (all affected files
  compile cleanly).

### 15. Command-Line Arguments: `__argv` Is Not UTF-8, Use `__wargv`

- `main(int argc, const char* argv[])` fed from `__argc`/`__argv` (narrow) receives command-line text
  in the process's ANSI codepage (e.g. CP949 on Korean Windows), NOT UTF-8, even though every other
  string boundary in this project is UTF-8. Passing `argv[i]` straight into a UTF-8-expecting function
  garbles any non-ASCII (Korean) argument.
- Use the parallel CRT global `__wargv` (wide, correctly Unicode-decoded regardless of codepage) and
  convert each argument to UTF-8 with `WideCharToMultiByte(CP_UTF8, ...)` before use.
- `__wargv` is populated automatically by the CRT startup alongside `__argv`/`__argc` -- no extra init
  needed, even in an MFC app entered via `WinMain`/`CWinApp` rather than a `wmain`.

### 16. `git add` Rejects LF-Only Files (core.safecrlf) -- Not a File Corruption

- The global git config here is `core.autocrlf=true` + `core.safecrlf=true`, and every tracked text file
  in this repo is CRLF in the working tree. A file that is LF-only instead is refused on staging with
  `fatal: LF would be replaced by CRLF in <path>` (plain `git diff` only warns), because the
  store-then-checkout round trip would not reproduce the current bytes.
- This is a config/line-ending mismatch, NOT damage to the file: check with `file <path>` (reports
  "with CRLF line terminators" or not) rather than assuming the content is broken.
- Known case: `.claude/skills/*/SKILL.md` were committed LF-only and no-BOM (they are Claude Code
  config, outside the project's UTF-8-with-BOM source/document convention), so editing one and staging
  it hits this every time.
- Workaround that keeps both the file and the user's global config untouched:
  `git -c core.safecrlf=false add <path>`. Do not "fix" it by converting the file to CRLF -- that
  rewrites every line and buries the real diff.

### 17. Never Kill jbTerm.exe by Process Name Before Building

- This Claude Code session commonly runs hosted inside jbTerm.exe itself (ConPTY child process),
  so the DemoApp-style pre-build workaround in pitfall #1 (`Get-Process X | Stop-Process -Force`
  to dodge `LNK1168`) is unsafe for jbTerm: it can kill the very process running the current
  session, not just a stray test copy.
- `Stop-Process`/`taskkill` match by process **name** only, not by path or working directory.
  Running a test copy of jbTerm.exe from a different folder does NOT make killing-by-name safe --
  it still matches (and kills) any other jbTerm.exe, including the session's own host process.
- Do not preemptively kill jbTerm.exe before MSBuild. If the link step fails with `LNK1168`, that
  means the file at the actual build output path (`Build\jbTerm\Debug.64\jbTerm.exe`) is locked by
  a running instance at that exact path -- identify that specific process before killing anything.
