// Accessories.cpp
//
// ConBox 보조 함수들의 구현.

#include "Accessories.h"

LOGFONTW ParseFontOpts(const char* name, int size_pt, const char* opts, int dpi_y)
{
	LOGFONTW lf;
	ZeroMemory(&lf, sizeof(lf));

	// 포인트 크기를 픽셀 높이로 환산한다.
	// 음수 높이는 글자 셀(em) 높이를 기준으로 한다는 의미이다.
	lf.lfHeight = -MulDiv(size_pt, dpi_y, 72);
	lf.lfWeight = FW_NORMAL;
	lf.lfCharSet = DEFAULT_CHARSET;

	// 폰트 이름(UTF-8)을 와이드 문자로 변환해 채운다.
	if (name != nullptr)
		::MultiByteToWideChar(CP_UTF8, 0, name, -1, lf.lfFaceName, LF_FACESIZE);

	// 추가설정 문자열을 왼쪽부터 한 글자씩 해석한다.
	// 숫자가 나오면 누적해 두었다가 바로 뒤 속성의 값으로 사용한다.
	// 현재 값을 사용하는 속성은 W(Weight) 뿐이다.
	int num = 0;
	bool has_num = false;
	for (const char* p = opts; p != nullptr && *p != '\0'; ++p) {
		char c = *p;

		if (c >= '0' && c <= '9') {
			num = num * 10 + (c - '0');
			has_num = true;
			continue;
		}

		switch (c) {
		case 'B': case 'b': lf.lfWeight = FW_BOLD; break;
		case 'I': case 'i': lf.lfItalic = 1; break;
		case 'U': case 'u': lf.lfUnderline = 1; break;
		case 'S': case 's': lf.lfStrikeOut = 1; break;
		case 'W': case 'w': if (has_num) lf.lfWeight = num; break;
		default: break;   // 알 수 없는 기호는 무시한다.
		}

		// 한 속성을 처리했으면 누적 숫자를 비운다.
		num = 0;
		has_num = false;
	}

	return lf;
}
