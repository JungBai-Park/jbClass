# MFC 프로그램 한글 IME 입력 Issue

CWnd 파생 커스텀 컨트롤(그리드, 터미널 등) 안에서 자식 편집 컨트롤(EDIT)을 띄워 한글을
입력받을 때 발생하는 문제와 그 해결 방법을 정리한 문서이다. TableBox의 In-place Cell
Editing(셀 편집)을 구현하면서 단계별로 디버깅하며 알아낸 동작 원리를 기록하였고, 앞으로
유사한 코드를 개발할 때 참고할 수 있도록 구현 가이드와 체크리스트를 덧붙였다.

> 이 프로젝트는 **MBCS(멀티바이트)와 유니코드 양쪽 빌드에서 모두 동작**해야 한다는 요구사항이
> 있다. 아래의 함정 대부분은 **MBCS 빌드**에서만 드러나며, 유니코드 전용 프로젝트라면 신경 쓸
> 필요가 없는 것도 있다. 그러나 "유니코드로 빌드하면 된다"로 끝내지 않고 양쪽 모두에서 정상
> 동작하도록 만드는 것이 목표이다.



## 1. 증상

편집 가능한 셀이 선택된 상태에서 한글을 입력하기 시작하면 다음과 같은 이상 동작이 단계적으로
나타났다. (각 항목은 앞 단계를 고친 뒤 새로 드러난 별개의 증상이다.)

1. **CEdit가 생기지 않고 엉뚱한 곳에 조합 문자가 표시됨** — 'ㄱ'을 눌러도 편집창이 안 뜨고,
   한 음절이 완성된 뒤에야 편집창이 생기는데 앞 글자는 깨져 있다.
2. **첫 글자가 `?`로 표시됨** — 편집창은 바로 뜨지만 'ㄱ'이 `?`로 보이고, 'ㅏ'를 누르면 '가'로
   정상화된다. 즉 첫 낱자만 깨진다.
3. **첫 글자가 이상한 도형 문자로 표시됨** — `?`가 아니라 글자마다 다른 엉뚱한 글리프가 뜬다.
   ('ㄱ', 'ㄴ', 'ㄷ'이 각각 다른 도형) 역시 음절이 완성되면 정상화된다.
4. **첫 글자가 원래 셀 텍스트(전체 선택)로 보이고 조합이 안 보임** — 'ㄱ' 시점엔 조합이 화면에
   안 나타나고 'ㅏ'를 눌러 '가'가 되어서야 보인다.

핵심 관찰: **IME 엔진(HIMC) 안에는 'ㄱ'이 올바르게 들어가 있어서 음절은 결국 정상적으로
완성된다. 화면 표시(첫 낱자 렌더링)만 깨진다.** 한 증상(3번)에서는 깨진 상태에서 강제로
다시 그리면(Invalidate) 'ㄱ'으로 교정되는 것까지 확인되었다.



## 2. 배경 지식: 한글 IME 입력은 WM_CHAR가 아니다

영문/ASCII 입력은 키를 누르면 곧바로 `WM_CHAR`가 온다. 그러나 한글은 **IME 조합(composition)**
과정을 거치므로 키 입력이 다음 메시지 흐름으로 들어온다.

```
WM_IME_STARTCOMPOSITION        조합 시작 알림 (wParam/lParam 의미 없음)
WM_IME_COMPOSITION (GCS_COMPSTR='ㄱ')   조합 중 문자열 = 미완성 낱자
WM_IME_COMPOSITION (GCS_COMPSTR='가')   조합 진행
...
WM_IME_COMPOSITION (GCS_RESULTSTR='강') 확정
```

- 실제 글자는 `WM_IME_COMPOSITION`에 실려 오며, `lParam`의 GCS 플래그가 그 의미를 가른다.
  - `GCS_COMPSTR` : 조합 중(미확정) 문자열. 편집 컨트롤이 밑줄 친 상태로 인라인 표시한다.
  - `GCS_RESULTSTR` : 확정 문자열. 편집 컨트롤의 본문 텍스트에 삽입된다.
  - `CS_INSERTCHAR` (0x2000) : `wParam`에 실린 입력 문자를 조합 시작 글자로 삽입하라는 의미.
    조합의 **첫** `WM_IME_COMPOSITION`에 함께 온다.
- 편집 컨트롤(EDIT)은 이 메시지를 받아 `ImmGetCompositionString(himc, ...)`으로 조합 문자열을
  읽고 자신의 클라이언트 영역에 직접 그린다.

따라서 `WM_CHAR`만 보고 편집을 시작하는 코드는 **한글의 첫 낱자를 놓친다.** 조합이 시작되는
시점에는 `WM_CHAR`가 아직 없고, 음절이 완성되어야 비로소 `WM_CHAR`(또는 GCS_RESULTSTR)가
오기 때문이다. (증상 1)



## 3. 디버깅으로 밝혀낸 동작 원리

### 3.1 커스텀 CWnd는 IME 조합을 받을 위젯이 아니다

그리드/터미널 같은 `CWnd` 파생 커스텀 컨트롤은 텍스트 위젯이 아니므로, 그 위에서 한글 조합이
시작되면 조합 문자열을 표시할 주체가 없다. 그래서 편집을 시작하려면 **조합이 시작되는 첫
신호(`WM_IME_STARTCOMPOSITION`)를 가로채서 그 순간 편집용 EDIT를 만들고 포커스를 넘겨**
조합을 EDIT가 이어받게 해야 한다.

```cpp
LRESULT MyGrid::WindowProc(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_IME_STARTCOMPOSITION && /* 편집 가능한 셀이고 편집 중이 아니면 */) {
        start_text_edit(...);       // 여기서 EDIT 생성 + SetFocus
        if (edit_box) return 0;     // 소비
    }
    ...
    return CWnd::WindowProc(msg, wp, lp);
}
```

> 주의: EDIT에 `WM_IME_STARTCOMPOSITION`을 다시 PostMessage로 재전달하지 말 것. `SetFocus`만으로
> IME가 EDIT에 연결되며, 빈(0,0) 조합 시작을 덧씌우면 오히려 조합 상태가 깨져 첫 낱자가 `?`로
> 표시된다. (모달 루프 기반의 오래된 패턴에서는 재전달이 필요했지만, 비모달 구조에서는 해롭다.)

### 3.2 MFC가 만든 EDIT는 MBCS 빌드에서 ANSI 윈도우가 된다 (증상 2의 원인)

MBCS 빌드에서 `CEdit::Create`는 물론, `AfxHookWindowCreate` + `CreateWindowExW`로 만들어도
MFC가 윈도우를 ANSI `AfxWndProc`에 묶어버린다. 그 결과 `IsWindowUnicode(edit) == 0`이 된다.

**ANSI EDIT는 `WM_IME_COMPOSITION`의 조합 문자열을 CP949로 처리**하는데, 미완성 낱자
'ㄱ'(U+3131)은 완성형 한글이 아니라 CP949로 변환할 수 없어 `?`(0x3F)로 깨진다. 음절이
완성되는 순간 '가'(U+AC00, 완성형)는 CP949 변환이 가능하므로 그때부터 정상화된다 — 증상 2와
정확히 일치한다.

**해결: 편집 컨트롤을 MFC를 거치지 않은 순수 유니코드 EDIT로 만든다.**

```cpp
// AfxHookWindowCreate 없이 -- 그래야 IsWindowUnicode == 1 유지
HWND eh = ::CreateWindowExW(0, L"EDIT", L"",
    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | align_style,
    x, y, w, h, parent_hwnd, nullptr, AfxGetInstanceHandle(), nullptr);
::SetWindowSubclass(eh, EditSubclassProc, 1, (DWORD_PTR)this);  // Win32 서브클래싱
::SendMessageW(eh, WM_SETFONT, (WPARAM)hFont, TRUE);
```

MFC 메시지맵(`CEdit` 파생 + `PreTranslateMessage`/`OnKillFocus`) 대신, 키 처리(Enter/Esc/Tab),
포커스 상실 시 커밋, `WM_GETDLGCODE` 등을 **Win32 서브클래스 프로시저**에서 직접 처리한다.
(아래 4.2 참조)

### 3.3 조합의 첫 메시지만 ANSI 부모 윈도우로 온다 (증상 4의 원인)

추적 결과, 조합은 **셀(커스텀 CWnd)이 포커스를 가진 상태에서 시작**되므로, EDIT를 만들고
`SetFocus`까지 했는데도 **첫 `WM_IME_COMPOSITION` 하나만 부모(CWnd)로 배달**되고, 두 번째부터는
EDIT로 직접 간다. (포커스 전환이 그 첫 메시지의 라우팅에는 아직 반영되지 않은 것이다.)

```
[부모 WndProc] WM_IME_STARTCOMPOSITION   edit_box 아직 없음 -> 생성 + SetFocus
[부모 WndProc] WM_IME_COMPOSITION 'ㄱ'   <- 첫 조합 메시지가 여기로! (focus는 이미 EDIT)
[EDIT proc]    WM_IME_COMPOSITION '가'   <- 두 번째부터 EDIT로 직접
[EDIT proc]    WM_IME_COMPOSITION ...
```

이 첫 메시지를 무시하면 'ㄱ'이 화면에 안 뜬다(HIMC엔 남아 음절은 완성됨 = 증상 4). 따라서
**부모로 온 첫 `WM_IME_COMPOSITION`을 EDIT로 중계(SendMessageW)** 해야 한다.

### 3.4 ANSI 부모로 온 wParam은 CP949이므로 변환해야 한다 (증상 3의 원인)

그런데 부모(CWnd)가 MBCS 빌드에서 ANSI 윈도우이므로, 부모로 배달된 `WM_IME_COMPOSITION`의
`wParam`은 이미 **CP949로 변환된 코드**(예: 'ㄱ' = `0xA4A1`, `CS_INSERTCHAR` 때문에 실려 옴)이다.
이걸 유니코드 EDIT에 그대로 `SendMessageW`로 넘기면 EDIT가 `0xA4A1`을 **UTF-16 코드포인트
U+A4A1**(Yi 음절 영역의 엉뚱한 글자)로 해석한다. 낱자마다 CP949 코드가 달라 각기 다른 도형
글자가 뜬다 — 증상 3과 정확히 일치한다. (HIMC엔 진짜 'ㄱ'이 남아 있어, 강제로 다시 그리면
교정되는 현상도 이 때문이다.)

**해결: 중계 전에 `wParam`을 CP949(CP_ACP) -> UTF-16으로 변환한다.** (`0xA4A1` -> `U+3131`)

```cpp
else if (msg == WM_IME_COMPOSITION && edit_box) {
    WPARAM wp = wParam;
    if (wParam > 0xFF) {        // 2바이트 DBCS
        char mb[2] = { (char)(wParam >> 8), (char)(wParam & 0xFF) };
        wchar_t wc = 0;
        if (::MultiByteToWideChar(CP_ACP, 0, mb, 2, &wc, 1) == 1) wp = wc;
    } else if (wParam >= 0x80) { // 1바이트 비ASCII
        char mb = (char)wParam;
        wchar_t wc = 0;
        if (::MultiByteToWideChar(CP_ACP, 0, &mb, 1, &wc, 1) == 1) wp = wc;
    }
    return ::SendMessageW(edit_box, msg, wp, lParam);
}
```

### 3.5 함정: IsWindowUnicode로 변환 여부를 판단하지 말 것

"유니코드 빌드에서는 wParam이 이미 UTF-16일 테니, ANSI일 때만 변환하자"는 생각으로
`if (!IsWindowUnicode(m_hWnd))` 가드를 넣으면 **MBCS 빌드에서 변환이 건너뛰어져 증상 3이
재발**한다.

이유: 커스텀 윈도우 클래스를 `RegisterClassExW` + `DefWindowProcW`로 등록했다면, MFC가
서브클래싱해도 **`IsWindowUnicode(m_hWnd)`는 MBCS 빌드에서도 TRUE**를 반환한다. 그러나 실제로
메시지를 배달하는 ANSI `AfxWndProc`는 여전히 `wParam`을 CP949로 준다. 즉 윈도우 클래스의
유니코드 속성과 메시지 펌프의 ANSI/유니코드는 별개다.

`wParam`의 인코딩을 결정하는 것은 윈도우 클래스가 아니라 **메시지를 받는 WndProc(AfxWndProc)의
A/W**이고, 그것은 빌드 설정(`_UNICODE`)으로 정해진다. 다음 두 가지가 안전하다.

- **항상 변환(권장):** 유니코드 빌드에서는 첫 조합 메시지가 부모가 아니라 EDIT로 직접 가서
  이 변환 분기 자체를 타지 않으므로, 항상 변환해도 무해하다. (실측으로 MBCS/유니코드 양쪽 검증)
- **`#ifndef _UNICODE`로 분기:** 의미상 가장 정확하지만 빌드 매크로에 의존한다.



## 4. 구현 가이드 (정리된 최종 형태)

### 4.1 편집 시작 (start_text_edit)

```cpp
void MyGrid::start_text_edit(int row, int col, wchar_t initial_char /*=0*/) {
    // ...셀 사각형 계산...
    HWND eh = ::CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | align_style,
        rc.left, rc.top, rc.Width(), rc.Height(),
        m_hWnd, nullptr, AfxGetInstanceHandle(), nullptr);
    if (!eh) return;
    ::SetWindowSubclass(eh, EditSubclassProc, 1, (DWORD_PTR)this);
    ::SendMessageW(eh, WM_SETFONT, (WPARAM)font.GetSafeHandle(), TRUE);

    // 현재 셀 텍스트를 UTF-8 -> UTF-16 변환 후 채우고 전체 선택
    ::SetWindowTextW(eh, wbuf);
    ::SendMessageW(eh, EM_SETSEL, 0, -1);

    edit_box = eh;                       // 멤버는 HWND (CEdit* 아님)
    ::SetFocus(eh);                      // IME가 EDIT에 연결됨
    if (initial_char) ::SendMessageW(eh, WM_CHAR, (WPARAM)initial_char, 1); // 영문 타이프-오버
}
```

### 4.2 EDIT 서브클래스 프로시저 (EditSubclassProc)

`CEdit` 파생 클래스가 하던 일을 Win32 서브클래스에서 처리한다.

```cpp
LRESULT CALLBACK MyGrid::EditSubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                          UINT_PTR id, DWORD_PTR ref) {
    MyGrid* self = (MyGrid*)ref;
    switch (msg) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;     // 부모 프레임의 Enter/Esc 가로채기에 양보받기
    case WM_KEYDOWN:
        if (wp == VK_RETURN) { self->end_text_edit(true, 1, 0);  self->SetFocus(); return 0; }
        if (wp == VK_ESCAPE) { self->end_text_edit(false, 0, 0); self->SetFocus(); return 0; }
        if (wp == VK_TAB)    { self->end_text_edit(true, 0, 1);  self->SetFocus(); return 0; }
        break;
    case WM_CHAR:                    // 위 키들이 남기는 후행 WM_CHAR 삼키기
        if (wp == '\r' || wp == '\x1b' || wp == '\t') return 0;
        break;
    case WM_KILLFOCUS:               // 다른 곳 클릭 등으로 포커스 상실 시 커밋
        if (!self->ending_edit) self->end_text_edit(true, 0, 0);
        break;
    case WM_NCDESTROY:
        ::RemoveWindowSubclass(h, EditSubclassProc, id);
        break;
    }
    return ::DefSubclassProc(h, msg, wp, lp);
}
```

`SetWindowSubclass`/`DefSubclassProc`/`RemoveWindowSubclass`는 `<commctrl.h>` + `comctl32.lib`가
필요하다.

### 4.3 부모 WindowProc의 IME 처리

```cpp
LRESULT MyGrid::WindowProc(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_IME_STARTCOMPOSITION && /* 편집 가능 셀 && 편집 중 아님 */) {
        start_text_edit(cur_row, cur_col, 0);   // EDIT 생성 + SetFocus
        if (edit_box) return 0;                  // 소비 (재전달 금지)
    }
    else if (msg == WM_IME_COMPOSITION && edit_box) {
        // 부모로 온 첫 조합 메시지만 여기로 옴. CP949 wParam -> UTF-16 변환 후 EDIT로 중계.
        WPARAM cwp = /* 3.4의 변환 */;
        return ::SendMessageW(edit_box, msg, cwp, lp);
    }
    return CWnd::WindowProc(msg, wp, lp);
}
```

### 4.4 포커스/커밋 설계 주의

- `end_text_edit`은 **스스로 `SetFocus`를 호출하지 않는다.** `WM_KILLFOCUS` 처리 도중에
  `SetFocus`를 다시 부르는 것은 재진입이라 Win32가 경고한다. 대신 **호출하는 쪽이 포커스를
  결정**한다: 서브클래스의 키 핸들러(Enter/Esc/Tab, 아직 포커스 보유)는 커밋 후 직접
  `self->SetFocus()`를 부르고, `WM_KILLFOCUS` 경로는 부르지 않는다(포커스는 이미 다른 곳으로
  가는 중).
- 단행 EDIT는 텍스트를 항상 위쪽 정렬하며 `EM_SETRECT`가 듣지 않는다. 세로 위치를 조정하려면
  편집창 사각형 자체를 위/아래로 미세 조정(inset)하는 옵션을 둔다.



## 5. 향후 구현 체크리스트

새로 커스텀 CWnd 안에서 한글 입력을 받는 편집창을 만들 때 점검할 항목:

- [ ] 편집창을 **`CreateWindowExW(L"EDIT")` + `SetWindowSubclass`** 로 만들었는가? (MFC 훅 금지)
      `IsWindowUnicode(edit) == 1`을 한 번 확인하라.
- [ ] `WM_IME_STARTCOMPOSITION`을 부모에서 가로채 편집창을 만들고 `SetFocus`했는가?
      (한글 첫 낱자를 놓치지 않으려면 필수)
- [ ] `WM_IME_STARTCOMPOSITION`을 편집창에 **재전달하지 않았는가?** (빈 재시작은 조합을 깬다)
- [ ] 부모로 온 첫 `WM_IME_COMPOSITION`을 편집창으로 **중계**하는가?
- [ ] 중계 시 `wParam`을 **CP_ACP -> UTF-16 변환**하는가? (raw 중계는 도형 글자)
- [ ] 변환 여부를 **`IsWindowUnicode`로 판단하지 않았는가?** (항상 변환 또는 `#ifndef _UNICODE`)
- [ ] 영문 타이프-오버(`WM_CHAR`로 편집 시작)와 한글(IME) 경로가 **충돌 없이 공존**하는가?
- [ ] Enter/Esc/Tab 후행 `WM_CHAR`(`\r`/`\x1b`/`\t`)를 서브클래스에서 **삼키는가?**
- [ ] `WM_GETDLGCODE`에서 `DLGC_WANTALLKEYS`를 반환하는가? (부모 프레임의 Enter/Esc 가로채기 양보)
- [ ] **MBCS와 유니코드 양쪽 빌드에서** 한글 첫 글자부터 정상 표시되는지 실측했는가?



## 6. 핵심 교훈 한 줄 요약

1. 한글은 `WM_CHAR`가 아니라 IME 조합 메시지로 온다 — 첫 낱자는 `WM_IME_STARTCOMPOSITION`
   시점에 편집창을 만들어 받아야 한다.
2. MBCS 빌드에서 MFC가 만든 EDIT는 ANSI라 미완성 낱자를 `?`로 깨뜨린다 — **순수 유니코드 EDIT +
   Win32 서브클래싱**으로 만들어야 한다.
3. 조합의 첫 메시지만 ANSI 부모로 오고 그 `wParam`은 CP949다 — **CP_ACP -> UTF-16 변환 후
   중계**해야 도형 글자가 안 뜬다.
4. `IsWindowUnicode(m_hWnd)`는 `RegisterClassExW`+`DefWindowProcW` 윈도우에서 빌드와 무관하게
   TRUE이므로, IME 인코딩 판단 기준으로 쓰면 안 된다.
