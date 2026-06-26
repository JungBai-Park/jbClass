# jbClass

A C++/MFC class library providing three reusable UI controls for Windows desktop applications.

---

## Modules

### FrameBox

A dialog host that manages child control layout interactively at runtime.

- **Live layout editing**: toggle edit mode with the middle mouse button to drag, resize, and reposition controls visually. Confirmed edits are written back directly into the source code (`__FILE__`/`__LINE__` injection).
- **Procedural input loop**: `listen()` delivers control signals inside a `while` loop, enabling a straightforward procedural coding style instead of message-map callbacks.
- **DPI and zoom awareness**: `Ctrl+Wheel` zooms the entire frame. All child controls scale together using an effective DPI (`eff_dpi = real_dpi × zoom_pm / 1000`).
- **Background image**: load JPEG/PNG/BMP from a resource or file path; transparent statics let the image show through.
- **Frameless mode**: removes the OS title bar and draws owner-drawn Ubuntu-style caption buttons (close / minimize / maximize).
- **Dynamic menu bar**: build a menu at runtime from `initializer_list` without a resource file.

### ConBox

A VT100-compatible terminal control embeddable in any MFC window.

- **Two modes**: pure output display (diagnostic log) or full ConPTY child runner (`start(cmdline)`).
- **Child process I/O**: launches PowerShell, CMD, or any console program and pipes its stdin/stdout through a pseudo console.
- **Korean input**: improves Korean IME handling compared to standard terminal programs.
- **Per-monitor DPI awareness**: fonts and cell metrics rebuild on DPI change without resizing the pseudo console (grid row/col count is preserved).
- **Zoom support**: responds to `WM_JBZOOM` from FrameBox to scale fonts in sync with the parent frame.

### TableBox

An Excel-like grid control for MFC.

- **Virtual mode** (default): no data ownership; a `(row, col)` callback supplies only the visible cells on demand — suitable for large or dynamic datasets.
- **Owned mode** (`alloc_text`): the table stores every cell's UTF-8 string internally, for small fixed grids (< 100 cols × 27 rows). No callback required.
- **In-place editing**: click a cell to edit; combo (dropdown) cells are also supported.
- **Fixed headers**: frozen header rows and columns scroll independently (Excel-style).
- **DPI and zoom aware**: font and cell metrics rebuild on DPI change or `WM_JBZOOM`.

---

## Requirements

| Item | Detail |
|------|--------|
| OS | Windows 10 1809 or later (64-bit) |
| Compiler | Visual Studio 2022 |
| Framework | MFC (Unicode and MBCS compatible) |
| Language | C++17 |

All Win32 API calls use explicit `W`-suffix variants (`CreateFontIndirectW`, `TextOutW`, etc.) so the library compiles correctly under both Unicode and MBCS project configurations.

---

## Build

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  "Project\jbBox.sln" /t:Build /p:Configuration=Debug /p:Platform=x64
```

Build output is placed in `Project\Debug.64\`.

---

## Project Structure

```
jbClass/
├── Source/          # FrameBox.h/.cpp  ConBox.h/.cpp  TableBox.h/.cpp
├── Project/         # Visual Studio solution and project files
├── Documents/       # Requirements and development notes per module
└── Reference/       # Reference materials (not tracked in git)
```

---

## License

Not yet specified.
