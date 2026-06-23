/*
    DemoApp.cpp - CWinApp entry + DemoMain() exercising FrameBox.
*/

#include "..\Source\FrameBox.h"
#include "..\Source\ConBox.h"
#include "..\Source\TableBox.h"
#include "resource.h"  // IDR_BACKGROUND


class cDemoApp : public CWinApp {
    int exit_code = 0;
public:
    BOOL InitInstance() override {
        int main(int argc, const char *argv[]);

        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES };
        InitCommonControlsEx(&icc);
        CWinApp::InitInstance();

        exit_code = main(__argc, const_cast<const char**>(__argv));
        return FALSE;
    }

    int ExitInstance() override {
        return(exit_code);
    }
};

class cDemoBox : public FrameBox {
public:
    int dpi_changed_count = 0;

    void update_title() {
        wchar_t buf[128];
        swprintf_s(buf, _countof(buf), L"cParasite Demo [DPI: %d  zoom: %d%%  DPICHANGED: %d]",
                   dpi, zoom_pm / 10, dpi_changed_count);
        ::SetWindowTextW(m_hWnd, buf);
    }

    void apply_zoom(int new_pm, bool cursor_anchor) override {
        FrameBox::apply_zoom(new_pm, cursor_anchor);
        update_title();
    }

    LRESULT WindowProc(UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg == WM_DPICHANGED) {
            TRACE(L"[cDemoBox] WM_DPICHANGED received: DPI=%d\n", (int)LOWORD(wp));
            ++dpi_changed_count;
            LRESULT r = FrameBox::WindowProc(msg, wp, lp);
            update_title();
            return r;
        }
        return FrameBox::WindowProc(msg, wp, lp);
    }
};

class cDemoSub : public FrameBox {
public:
    int dpi_changed_count = 0;

    void update_title() {
        wchar_t buf[128];
        swprintf_s(buf, _countof(buf), L"Modal Sub [DPI: %d  zoom: %d%%  DPICHANGED: %d]",
                   dpi, zoom_pm / 10, dpi_changed_count);
        ::SetWindowTextW(m_hWnd, buf);
    }

    void apply_zoom(int new_pm, bool cursor_anchor) override {
        FrameBox::apply_zoom(new_pm, cursor_anchor);
        update_title();
    }

    LRESULT WindowProc(UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg == WM_DPICHANGED) {
            TRACE(L"[cDemoSub] WM_DPICHANGED received: DPI=%d\n", (int)LOWORD(wp));
            ++dpi_changed_count;
            LRESULT r = FrameBox::WindowProc(msg, wp, lp);
            update_title();
            return r;
        }
        return FrameBox::WindowProc(msg, wp, lp);
    }
};

cDemoApp App;

// TableBox edit callback: dual-purpose (query and commit). The table runs in OWNED data mode
// (alloc_text), so no text callback is needed. context defaults to the TableBox* (set_edit_callback
// stores `this` when context is omitted), so the commit path writes back via cell().
// Query (text == nullptr): return cell type.
//   [6][2]               -> combo "하나, 둘, 셋, 넷, 다섯, 여섯"
//   even col (except 0)  -> (const char*)(-1) = CEdit-editable
//   otherwise            -> nullptr = read-only
// Commit (text != nullptr): store the new value into the owned buffer via cell().
//   For combo cells, text is the selected index string ("0","1",...).
//   For CEdit cells, text is the user-typed UTF-8 string.
static const char* DemoTableEdit(int row, int col, const char* text, void* context) {
    if (text == nullptr) {
        if (row == 6 && col == 2) return "하나, 둘, 셋, 넷, 다섯, 여섯";
        if (col > 0 && col % 2 == 0) return (const char*)(-1);
        return nullptr;
    }
    static_cast<TableBox*>(context)->cell(row, col) = text;
    return nullptr;
}

cDemoBox Top;
void DemoSub(CWnd *owner);

int main(int argc, const char* argv[]) {
    Top.OpenFrame(&App, 502, -942, 1402, -142);
    Top.set_image(IDR_BACKGROUND);
    Top.frameless(2);                                    // borderless + close/min/max caption buttons
    Top.CenterWindow();
    Top.set_margin(5);
    Top.update_title();   // show initial DPI (count=0)

    ConBox* conBox = new ConBox;
    conBox->setup_from_ini("..\\..\\Documents\\ConBox.ini");
    conBox->open(&Top, 5, 5);
    Top.AddNew(5, 340, 601, 735, conBox);   // coords-first; Top owns conBox

    TableBox* table = new TableBox;
    table->set_cols({125, 75}, 25);   // 50 cols: 125/75 pattern x25
    table->set_rows({25}, 50);        // 50 rows: uniform 25px
    table->set_fixed(1, 1);
    table->alloc_text();              // owned mode: A..Z / 1.. headers auto-filled, body blank
    table->cell(6, 2) = "2";          // combo-cell demo: preselect item index 2
    table->set_edit_callback(DemoTableEdit);
    table->set_font("Malgun Gothic", 10);
    table->set_align(5);
    table->set_edit_adjust(1, 4, 0, 0);
    table->open(&Top, 5, 5, 10, 8);
    Top.AddNew(20, 45, 820, 322, table);    // attach-only; keep the size/position open() computed

    CEdit*     edit   = Top.AddEdit  (685, 405, 874, 445);
    CButton*   button = Top.AddButton(710, 370, 790, 395, "Close");
    CButton*   subBtn = Top.AddButton(795, 370, 875, 395, "Sub");
    CStatic*   label  = Top.AddStatic(665, 455, 875, 509, "강누리 만세");
    CComboBox* combo  = Top.AddCombo (685, 340, 875, 363, "Left,Center,Right");

    AlignText(edit, 5);
    AlignText(label, 3);

    Top.listen(edit, button, subBtn, label, combo);
    while (::IsWindow(Top)) {
        CWnd* ev = Top.wait();
        if (ev == 0)      break;          // window closed
        if (ev == button) break;          // Close button -> quit
        if (ev == subBtn) DemoSub(&Top);  // open the modal sub-dialog
    }
    return 0;
}

// Modal sub-dialog: Sub.OpenFrame(owner,...) opens an owned popup and disables
// owner (CWnd* overload); ~Sub at scope exit re-enables owner via close().
void DemoSub(CWnd* owner) {
    cDemoSub Sub;
    Sub.OpenFrame(owner, 929, 100, 1229, 520);   // centered on the top (secondary) monitor
    Sub.set_margin(5);
    Sub.update_title();   // show initial DPI (count=0)
    CStatic* msg = Sub.AddStatic(22, 124, 262, 148, "모달 서브 프레임");
    CButton* ok  = Sub.AddButton(155, 9, 275, 41, "OK");
    AlignText(msg, 5);
    Sub.listen(ok);
    while (::IsWindow(Sub)) {
        CWnd* ev = Sub.wait();
        if (ev == 0 || ev == ok) break;   // closed or OK -> leave (re-enables owner)
    }
}
