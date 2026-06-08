
// ConBoxDlg.h: 헤더 파일
//

#pragma once

#include "../Source/ConBox.h"   // 데모에서 사용할 ConBox 컨트롤 (자식 실행 ConPTY 기능 포함)


// CConBoxDlg 대화 상자
class CConBoxDlg : public CDialog
{
// 생성입니다.
public:
	CConBoxDlg(CWnd* pParent = nullptr);	// 표준 생성자입니다.

// 대화 상자 데이터입니다.
#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_CONBOX_DIALOG };
#endif

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);	// DDX/DDV 지원입니다.


// 구현입니다.
protected:
	HICON m_hIcon;

	CConBox con_box;   // 데모용 ConBox 컨트롤 인스턴스 (start() 로 자식도 직접 실행)

	// 생성된 메시지 맵 함수
	virtual BOOL OnInitDialog();
	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
	afx_msg void OnSize(UINT type, int cx, int cy);
	afx_msg void OnDestroy();
	afx_msg void OnSysCommand(UINT id, LPARAM lparam);
	DECLARE_MESSAGE_MAP()

	// ConBox 와 확인/취소 버튼을 현재 창 크기에 맞춰 다시 배치한다.
	void layout_children();

	// ConBox 가 가로 cols 칸 x 세로 rows 줄(영문 기준)이 되도록 메인 창 크기를
	// 다시 잡고, 작업 영역 중앙으로 옮긴다. open() 이후(셀 크기 확정 후) 호출한다.
	void resize_to_grid(int cols, int rows);

	// ConExe 자식(powershell)이 종료되면 호출되는 콜백(set_exit_callback 으로 등록).
	// user 로 받은 다이얼로그 인스턴스의 ConBox 에 종료 안내를 출력한다.
	static void on_child_exit(void* user);
};
