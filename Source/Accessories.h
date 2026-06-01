// Accessories.h
//
// ConBox 가 사용하는 보조(액세서리) 함수 모음.
// 지금은 폰트 추가설정 문자열 파서만 들어 있으며,
// 이후 다른 보조 성격의 함수들도 이 파일에 함께 모은다.

#pragma once

#include <afxwin.h>   // LOGFONTW 등 GDI 자료형

// 폰트 추가설정 문자열(opts)과 이름/크기를 LOGFONTW 로 변환한다.
//
// name    : 폰트 이름 (UTF-8)
// size_pt : 글자 크기 (포인트)
// opts    : 추가설정 문자열. 다음 기호를 인식한다.
//             B = Bold, I = Italic, U = Underline, S = Strikeout
//             숫자+W = Weight (예 "700W" 면 Weight 를 700 으로)
//           숫자는 바로 뒤에 오는 속성의 값(정도)으로 쓰인다.
// dpi_y   : 세로 방향 DPI (포인트를 픽셀 높이로 환산할 때 사용, 보통 96)
LOGFONTW ParseFontOpts(const char* name, float size_pt, const char* opts, int dpi_y);
