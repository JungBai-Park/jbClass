# ConBox Development To-Do List

## VT100 Escape Sequences Implementation

### ✅ Completed

1. **CNL (CSI Pn E)** - Cursor Next Line
   - Move cursor down N lines, column resets to 0
   - Location: ConBox.cpp:1764
   - Status: Working correctly

2. **CPL (CSI Pn F)** - Cursor Previous Line
   - Move cursor up N lines, column resets to 0
   - Location: ConBox.cpp:1765
   - Status: Working correctly

3. **RIS (ESC c)** - Reset to Initial State
   - Full terminal reset (cursor, SGR, modes, screen)
   - Location: ConBox.cpp:1684-1694
   - Status: Working correctly

### ❌ Pending / Unresolved

1. **IND (ESC D)** - Index
   - Move cursor down one line; scroll if at region bottom
   - Location: ConBox.cpp:1675-1680
   - Status: **NOT WORKING** - Code present but no effect observed
   - Issue: Despite code implementation, cursor does not move down on ESC D
   - Testing shows: "Row1TextRow2Text" on same line (expected: different lines)
   - Compared with RI (ESC M) which works correctly
   - Root cause: Unknown - requires deeper debugging

2. **NEL (ESC E)** - Next Line
   - CR + LF behavior: move to next line, column = 0
   - Location: ConBox.cpp:1682-1688
   - Status: **NOT WORKING** - Same issue as IND
   - Issue: Cursor does not move to next line; column not reset to 0
   - Testing shows: Same line output
   - Root cause: Unknown - likely related to IND issue

### 🔍 Investigation Notes

- RI (ESC M) works correctly ✅
- RIS (ESC c) works correctly ✅  
- CNL/CPL (CSI E/F) work correctly ✅
- LF (line feed, \n) works correctly ✅
- line_feed() function itself is operational

**Mystery:** IND/NEL code is present and syntactically correct, but:
- Code compiles without errors
- Code appears to be in correct location (VT_ESC case)
- Similar structure to working code (RI, RIS)
- But has zero effect on cursor behavior

**Possible causes (not yet confirmed):**
1. Code never executes (vt_feed not called with D/E in VT_ESC state)
2. Code executes but conditions prevent action (cur_row == rows-1?)
3. Unknown compiler/linker issue
4. Subtle syntax or logic error
5. Variable state corrupted by earlier operations

**Testing approach for next session:**
- Add debug output to vt_feed for IND (OutputDebugString)
- Test with alternative character mapping
- Check vt_state transitions
- Verify cur_row/cur_col values at time of IND

---

## Session Summary (2026-06-07)

- Implemented 5 VT100 sequences (IND, NEL, CNL, CPL, RIS)
- Created test scripts (PowerShell, UTF-8 BOM)
- 3 of 5 fully working
- 2 of 5 have blocking issue requiring investigation
- All code pushed to Source/ConBox.cpp, no uncommitted changes
