// This file is UTF-8 with BOM
# DPI 인식 설정 및 동작 특성 정리

본 세션에서 확인한 DPI 관련 설정 방법과 동작 특성을 정리한 참고 문서.
최종 결정: **Per-Monitor V2** + `WM_DPICHANGED` 완전 처리 방식 채택.

---

## 1. DPI 인식 수준 비교

| 수준 | 설정 방법 | 동작 |
|---|---|---|
| 비인식 (기본) | 아무것도 선언하지 않음 | OS가 96 DPI 기준으로 렌더링 후 비트맵 스트레칭 → 흐릿하고 크게 표시 |
| System-aware | `InitInstance()` 최상단에서 `SetProcessDPIAware()` 한 번 호출 | 주 모니터 DPI 기준으로 렌더링. 비트맵 스트레칭 없음. 보조 모니터에서 흐릿할 수 있음 |
| Per-Monitor V1 | `SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE)` API 또는 매니페스트 `PerMonitor` | 최상위 창에만 `WM_DPICHANGED` 전달. 비클라이언트 영역은 OS가 비트맵 스트레칭 |
| Per-Monitor V2 | 매니페스트(`PerMonitorV2`) 또는 `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)` (Windows 10 1703+) | 모든 창에 `WM_DPICHANGED` 전달. OS가 비클라이언트 영역(타이틀바 등)을 올바른 DPI로 직접 렌더링 |

`SetProcessDPIAware()`는 **프로세스 전체**에 적용되므로 한 번만 호출하면 된다. 창마다 개별 설정 불필요.

---

## 2. Per-Monitor V2 매니페스트 선언

`.rc` 파일 또는 별도 `.manifest` 파일에 추가:

```xml
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings">
        PerMonitorV2
      </dpiAwareness>
    </windowsSettings>
  </application>
</assembly>
```

`.rc`에 추가한 경우, 링커 자동 매니페스트 생성 비활성화 필수:
**프로젝트 속성 → Linker → Manifest File → Generate Manifest = No**
(활성화 상태면 매니페스트가 두 개 내장되어 빌드 오류 발생)

매니페스트로 선언한 경우 `InitInstance()`에서 별도 API 호출 불필요.
`SetProcessDPIAware()` 등과 혼용하면 충돌하므로 제거해야 한다.

---

## 3. Per-Monitor V2 코드 방식 설정 (이식성 목적)

소스코드를 라이브러리 형태로 배포할 때 매니페스트 선언을 강제하기 어려운 경우,
`InitInstance()` 최상단에서 API로 직접 설정할 수 있다.

`SetProcessDpiAwareness()`(구형)와 달리 `SetProcessDpiAwarenessContext()`(신형, Windows 10 1703+)는
**Per-Monitor V2를 코드로 설정 가능**하다.

구형 OS 안전성을 위한 동적 로드 방식 (권장):

```cpp
BOOL cDemoApp::InitInstance()
{
    // Per-Monitor V2 (Windows 10 1703+). Dynamic load for safe fallback on older OS.
    typedef BOOL(WINAPI* PFN)(DPI_AWARENESS_CONTEXT);
    auto fn = (PFN)::GetProcAddress(::GetModuleHandleW(L"user32.dll"),
                                    "SetProcessDpiAwarenessContext");
    if (fn)
        fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ...
}
```

- 매니페스트와 이 API를 혼용하면 충돌하므로 둘 중 하나만 사용한다.
- API 방식은 매니페스트 방식보다 늦게 적용되므로, 반드시 창 생성 전(즉 `InitInstance()` 최상단)에 호출해야 한다.
- 매니페스트가 없고 이 API도 호출하지 않으면 DPI 비인식 상태가 되어
  `WM_DPICHANGED`가 전달되지 않고 DPI 대응 코드 전체가 사문화된다.

---

## 3. WM_DPICHANGED 동작

- 창이 DPI가 다른 모니터로 이동하면 OS가 해당 창에 전송.
- **V1**: 최상위 창에만 전송.
- **V2**: 최상위 창 + 차일드 창도 `WM_DPICHANGED_BEFOREPARENT` / `WM_DPICHANGED_AFTERPARENT` 수신.
- `wParam`: 새 DPI 값 (`LOWORD` = x DPI, `HIWORD` = y DPI).
- `lParam`: OS가 제안하는 새 창 위치/크기 (`RECT*`). 이 값으로 `SetWindowPos` 호출 시 물리적 크기 유지.

### 일반적인 처리 코드

```cpp
LRESULT OnDpiChanged(WPARAM wp, LPARAM lp) {
    int new_dpi = HIWORD(wp);
    RECT* r = (RECT*)lp;
    SetWindowPos(nullptr, r->left, r->top,
                 r->right - r->left, r->bottom - r->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    rebuild_fonts(new_dpi);  // 새 DPI 기준으로 폰트 재생성
    Invalidate();
    return 0;
}
```

### 차일드 컨트롤 커스텀 폰트 처리

- V2에서 시스템 기본 폰트를 사용하는 컨트롤은 OS가 자동 조정.
- `WM_SETFONT`로 커스텀 폰트를 명시적으로 지정한 컨트롤은 앱이 직접 새 폰트를 재생성하여 다시 `WM_SETFONT`로 전달해야 함.

### WM_DPICHANGED_BEFOREPARENT / WM_DPICHANGED_AFTERPARENT

두 메시지로 나뉜 이유는 부모-자식 간 **처리 순서 의존성**을 모두 지원하기 위해서다.

전달 순서:
```
WM_DPICHANGED_BEFOREPARENT  →  자식 트리를 아래→위(bottom-up) 순으로 전달
          ↓
WM_DPICHANGED               →  최상위 창 처리
          ↓
WM_DPICHANGED_AFTERPARENT   →  자식 트리를 위→아래(top-down) 순으로 전달
```

- `WM_DPICHANGED_BEFOREPARENT`: 부모와 무관하게 독립적으로 처리 가능한 것을 여기서 수행.
  주로 **폰트 재생성**. `wParam`/`lParam` 모두 0이므로 DPI는 `GetDpiForWindow(m_hWnd)`로 직접 조회.
- `WM_DPICHANGED_AFTERPARENT`: 부모의 새 레이아웃/크기에 의존하는 처리를 여기서 수행.
  주로 **위치·크기 재조정** (부모가 이미 새 크기로 확정된 후이므로 부모 기준 좌표 계산이 정확함).

폰트만 바꾸면 되는 단순한 컨트롤은 `WM_DPICHANGED_BEFOREPARENT`에서
폰트 재생성 + `Invalidate()`만으로 충분하다.

두 모니터에 걸쳐 있는 창은 과반수 면적 기준으로 DPI가 결정되며,
나머지 모니터 쪽은 OS가 비트맵 스트레칭을 적용한다. Per-Monitor V2로 완벽히
대응해도 창이 두 모니터에 걸쳐 있는 동안은 피할 수 없는 구조적 한계다.

### 최상위 창의 범위

"최상위 창(top-level window)"은 CWinApp에 등록된 메인 창 1개만이 아니다.
`WS_CHILD`가 없는 창 전체가 최상위 창이며, `WS_POPUP` 스타일의 팝업 다이얼로그도
최상위 창에 해당한다.

- 팝업 다이얼로그는 메인 창과 독립적으로 **자신만의 `WM_DPICHANGED`를 수신**한다.
- 메인 창과 팝업이 서로 다른 모니터에 배치된 경우 DPI가 달라질 수 있으며,
  각자 독립적으로 DPI 변경에 대응해야 한다.
- 팝업의 차일드 컨트롤들은 그 팝업을 기준으로
  `WM_DPICHANGED_BEFOREPARENT` / `WM_DPICHANGED_AFTERPARENT`를 수신한다.

본 프로젝트에서 `FrameBox::AddFrame`으로 생성하는 `WS_POPUP` 서브 다이얼로그는
메인 `FrameBox`와 별개로 자신의 `WM_DPICHANGED`를 처리해야 한다.

---

## 4. GetDeviceCaps(hdc, LOGPIXELSY)의 한계

- 멀티 모니터 환경에서 창의 위치에 무관하게 **항상 주 모니터 DPI를 반환**.
- Per-Monitor 폰트 크기 계산에 사용하면 안 된다.

### 올바른 모니터별 DPI 조회 방법

```cpp
// Windows 10 1607 이상 (권장)
UINT dpi = ::GetDpiForWindow(m_hWnd);

// Windows 8.1 이상
HMONITOR hmon = ::MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
UINT dpiX, dpiY;
::GetDpiForMonitor(hmon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
```

### GetDpiForWindow와 차일드 컨트롤

`GetDpiForWindow`에 차일드 컨트롤의 `HWND`를 넘기면, 해당 컨트롤이 어느 모니터에
위치하든 무관하게 **항상 최상위 부모 창의 DPI를 반환**한다.

창이 DPI가 다른 두 모니터에 걸쳐 있어도, 그 창의 모든 차일드 컨트롤은
`GetDpiForWindow` 호출 시 동일한 DPI 값을 반환한다.

이는 Windows DPI 설계의 근본 원칙으로, **한 창 트리(window tree) 안의 모든 HWND는
동일한 DPI 인식 모드와 DPI 값으로 동작**하도록 설계되어 있기 때문이다.
컨트롤별로 DPI가 달라지는 것은 지원하지 않는다.

`WM_DPICHANGED` 발생 기준도 창 전체 면적의 **과반수 이상**이 다른 DPI 모니터로
넘어갔을 때이며, 그 시점에 최상위 창 기준으로 한 번에 DPI가 전환된다.

---

## 5. 본 프로젝트에서의 영향

### ConBox::make_font()
현재 `::GetDeviceCaps(hdc, LOGPIXELSY)` 사용 → 주 모니터 DPI만 반환.
`::GetDpiForWindow(m_hWnd)`로 변경 필요.
`WM_DPICHANGED` 수신 시 `set_efont` / `set_kfont`를 재호출하여 폰트 재생성.

### ConBox 오버레이 스크롤바
`SBAR_W=14`, `SBAR_THUMB_W=6` 등 모든 치수가 고정 픽셀 상수.
Per-Monitor 대응 시 DPI에 비례하여 스케일링 필요:
`scaled = MulDiv(base_px, new_dpi, 96)`

### FrameBox Add* 컨트롤
`Add*` 팩토리가 각 컨트롤에 `WM_SETFONT`로 시스템 폰트 전파.
`WM_DPICHANGED` 수신 시 시스템 폰트를 새로 가져와 등록된 모든 컨트롤에 재전달 필요.

---

## 6. "물리 픽셀 고정" 방식 (미채택, 참고용)

고해상도 모니터에서 창이 작게 표시되더라도 물리 픽셀 크기를 유지하고 싶은 경우:

- Per-Monitor V2 매니페스트 선언 (비클라이언트 영역 선명하게).
- `WM_DPICHANGED` 수신하되 `SetWindowPos` 호출하지 않고 무시.
- `make_font()`에서 DPI를 96으로 고정.
- 스크롤바 상수는 그대로 유지 (스케일링 불필요).

이 방식은 검토 후 전체 Per-Monitor V2 대응 방식으로 결정하여 채택하지 않음.
