
// ConBoxDlg.h: 헤더 파일
//

#pragma once

#include "../Source/ConBox.h"   // 데모에서 사용할 ConBox 컨트롤


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

	CConBox con_box;   // 데모용 ConBox 컨트롤 인스턴스

	// 생성된 메시지 맵 함수
	virtual BOOL OnInitDialog();
	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
	afx_msg void OnSize(UINT type, int cx, int cy);
	DECLARE_MESSAGE_MAP()

	// ConBox 와 확인/취소 버튼을 현재 창 크기에 맞춰 다시 배치한다.
	void layout_children();
};
