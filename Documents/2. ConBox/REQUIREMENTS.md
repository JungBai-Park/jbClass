## ConBox

### 1. Purpose

- `ConBox` is a portable, single-class, `CWnd`-derived terminal control for MFC.
- `ConBox` supports two operating modes in the same class:
  - Pure terminal view: output through `print()` and input routing through `set_input_sink()`. If no input sink is installed, it can be used as a diagnostic log display.
  - ConPTY child runner: `start(cmdline)` creates the required pipes and pseudo console, then runs a console child process.
- Target programs are interactive stdio shells such as PowerShell and line-oriented stdio programs that emit a limited VT100 stream.
- Full-screen TUI programs are supported only on a best-effort basis.

### 2. Window and Grid Control

- `open(parent, left, top)` registers and creates the window using the `"ConBox"` class name.
- The initial pixel size is calculated from the configured row and column counts.
- Runtime resizing does not perform automatic line reflow.
- When resized, existing rows are padded or truncated to fit the new width.
- `ConBox` is per-monitor DPI aware: fonts/cell metrics use the window's own DPI and rebuild when it changes. A DPI change must rescale only the cell pixels and KEEP the grid (rows/cols) fixed, so the child pseudo-console is not resized (a resize would make the shell re-emit its screen and corrupt scrollback). Font sizes are stored as DPI-independent specs so they can be rebuilt at the new DPI; overlay scrollbar pixel metrics scale with DPI.
- Margins are 96 DPI LOGICAL padding used ONLY to compute the initial window size and to derive rows/cols on resize (scaled to physical via the window DPI); `adjust` padding stays in raw physical pixels. The grid is NOT drawn at the margin offset: it is centered in the client area, and when it overflows it is drawn from the top-left and clipped at the bottom/right.
- Because cell pixels do not scale by the exact DPI ratio (integer font rounding), a preserved grid may not fit the linearly-scaled window after a DPI change. The `snap_mode` ini option controls the response: 0 = centering only (clip if too small); 1 = grow the window only when the grid would be clipped; 2 (default) = always snap the window to the exact grid+margin size. Snapping keeps the upper-left corner fixed and moves the right/bottom edges, and never resizes the pseudo-console.
- **Zoom via `WM_JBZOOM`**: On receipt, ConBox sets `zoom_pm` and `zoom_resize = true`. The next `OnSize` (triggered by FrameBox `rescale_children` → `MoveWindow`) detects `zoom_resize` and takes the `relayout_for_dpi()` + `snap_to_grid()` path — identical to a real DPI change — preserving the logical grid without calling `update_metrics` or `resize_sink` (no PTY resize). `build_font()` applies `zoom_pm` through `eff_dpi()` so fonts scale with zoom exactly as they do with DPI changes.

### 3. Font and Cell Metrics

- English font configuration is handled by `set_efont`.
- Korean font configuration is handled by `set_kfont`.
- If the Korean font size value is `<= 0`, Korean font height matching is enabled by default. In this mode, the Korean font's physical pixel height is forced to match the English font height so line heights remain consistent.
- Cell width calculation:
  - Measure English width as `w_e`.
  - Measure Korean width as `w_k`.
  - Apply the configured ratio option.
  - Calculate `cell_w` as `(max(2 * w_e, w_k) + 1) / 2` to round up and avoid glyph clipping.
- `adjust(left, top, right, bottom)` keeps the font size unchanged, adjusts per-side cell margins, and compensates the internal glyph drawing position so block and box-drawing characters can render without gaps.

### 4. Rendering and Viewport

- The cell grid uses a fully fixed-width unit.
- CJK characters, including Korean characters, occupy two cell columns: lead and trail.
- Instead of the default `WS_VSCROLL`, which consumes client pixels, `ConBox` renders a custom rounded overlay scrollbar at the edge of the view.
- The overlay scrollbar fades out during normal idle state.
- The overlay scrollbar's rounded thumb and arrow buttons are antialiased, and its gutter strip is filled with a background-contrasting color (not just a faint hover-only groove) for the full time the bar is shown. Visual style matches TableBox's overlay scrollbar (see 3.4); ConBox has only the vertical axis.
- The mouse cursor remains `IDC_ARROW`, matching Windows Terminal behavior.
- The text cursor (block/underline/I-beam) is hidden when the window does not have keyboard focus (`WM_KILLFOCUS`). It reappears immediately on `WM_SETFOCUS` with the blink timer restarted.

### 5. Output and VT Escape Parsing

- C0 control support includes `\r`, `\n`, `\b`, and `\t`.
- Tabs advance to 8-column tab stops.
- CSI support includes cursor control such as CUU, CUD, and CUP.
- CSI support includes erase operations such as ED, EL, and ECH.
- CSI support includes scroll region control.
- SGR attributes include Bold, Italic, Underline, Strikethrough, Blink, and Reverse.
- Blink uses a 500 ms cycle.
- SGR 8 enables double-size rendering, and SGR 28 disables it.
- Double-size rendering draws text on the logical grid at a 2x horizontal and vertical scale, overlapping upward and rightward. This uses the standard SGR 8 code because conhost does not discard it in the middle of the stream.
- 256-color and True Color output are supported by default.
- Two-byte ESC support includes ESC 7/8 (DECSC/DECRC), RI (`M`), and RIS (`c`).
- Private sequence extensions that are likely to cause incorrect behavior are excluded.

### 6. Input Mapping and IME Commit Handling

- Standard virtual keys and character combinations are translated into the corresponding VT escape sequence strings encoded as UTF-8 and sent to the input sink.
- If a special key event, such as an arrow key or Enter, arrives while IME composition is active, `ImmNotifyIME` must commit the IME text first.
- After IME commit, the special key sequence is sent to the child process.
- The child process must always receive data in this order: committed Korean text first, then the special key code.
- Horizontal arrow correction:
  - After IME commit, the child process automatically moves the cursor one cell to the right.
  - To keep mouse and manual cursor tracking aligned, the input module may swallow the right arrow or send the left-move sequence twice for a left-arrow input.

### 7. Clipboard and Drag-and-Drop

- Drag selection stores the selected region in the clipboard as Unicode plain text.
- Double click selects a word.
- Alt-drag selects a rectangular block.
- On `WM_DROPFILES`, the full path of each dropped file is typed into the child process through standard input.
- Paths containing spaces are wrapped in double quotes.

### 8. Export and Logging

- `save_emf(dir)` creates a series of page-based EMF vector files in the target directory, using `cfg_lines_per_paper` as the page line count.
- EMF export maps colors through `remap_paper_color` so output is converted to print-oriented paper colors.
- `save_pdf(path)` searches system devices for a PDF conversion driver and exports the grid as a high-quality PDF using paper-oriented inverse color mapping.
- `get_text_lines()` returns the scrollback and current output as a list of UTF-8 text lines after trimming horizontal trailing spaces.
- `save_log(file_name)` writes the raw stream bytes received from the child process to a log file without CRLF conversion, encoding conversion, or other transformation.
- When a logging session restarts, four newline characters and a unique date timestamp delimiter are inserted.
