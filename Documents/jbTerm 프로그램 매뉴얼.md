# jbTerm 프로그램 매뉴얼

## 목차

1. [개요](#1-개요)
2. [실행 방법](#2-실행-방법)
3. [설정(INI) 항목](#3-설정ini-항목)
4. [창 조작](#4-창-조작)
5. [내장 기본 설정을 rcedit로 교체하기](#5-내장-기본-설정을-rcedit로-교체하기)

---

## 1. 개요

jbTerm은 `FrameBox` + `ConBox`(jbBox 클래스 라이브러리)로 만들어진 VT100 호환 터미널 프로그램입니다.
ConPTY로 자식 프로세스(PowerShell, cmd.exe, WSL 등)를 실행하고, 한글 입력/조합 처리를 개선한 것이 특징입니다.

`jbTerm.exe` 하나로 배포되는 단일 파일 프로그램입니다. 기본 설정 값(폰트, 색상, 창 크기 등)이 실행 파일
안에 내장되어 있어서, 별도의 설정 파일 없이 바로 실행할 수 있습니다.

## 2. 실행 방법

인수 없이 실행하면 내장된 기본 설정 그대로 시작합니다. 명령줄 인수는 다음 세 종류를 조합해서 줄 수
있습니다 (여러 개를 동시에 지정 가능).

| 인수 형태 | 의미 | 예시 |
|---|---|---|
| `@경로` | 그 경로의 INI 파일을 읽어 설정 위에 덮어씁니다. 여러 개 지정하면 준 순서대로 차례로 적용됩니다. 경로는 현재 작업 디렉토리 기준으로 해석됩니다. | `jbTerm.exe @D:\myset.ini` |
| `키=값` | 설정 항목 하나를 그 자리에서 바로 지정합니다. | `jbTerm.exe grid_cols=120` |
| 그 외(= 도 @ 도 아닌 인수) | 자식 프로세스로 실행할 명령줄로 취급합니다 (내부적으로 `cmdline=` 이 자동으로 붙습니다). | `jbTerm.exe powershell.exe` |

```
jbTerm.exe @D:\profile-work.ini efont_size=14 "ssh myuser@myhost"
```

위 예시는 `D:\profile-work.ini`를 먼저 적용한 뒤, 영문 폰트 크기를 14pt로 덮어쓰고, 자식 프로세스로
`ssh myuser@myhost`를 실행합니다.

> `@`로 지정한 INI 파일이 없으면 그 경로에 기본값으로 새로 만들어 줍니다(수정해서 쓰라는 용도). 단,
> 인수 없이 실행할 때 쓰이는 "내장 기본 설정"은 파일이 아니므로 이 자동 생성 대상이 아닙니다.

## 3. 설정(INI) 항목

`@파일` 이나 새로 만들어지는 INI 파일은 아래와 같은 형태입니다 (섹션 이름은 참고용이며 실제로는
키 이름만으로 인식됩니다).

```ini
[font]
efont_name    = Cascadia Mono   ; 영문 폰트 이름
efont_size    = 12              ; 영문 폰트 크기(pt)
efont_opts    =                 ; B=굵게, I=기울임, 90W=너비 90% 등
kfont_name    = Malgun Gothic   ; 한글 폰트 이름
kfont_size    = 0               ; 0 이하면 영문 폰트 높이에 맞춤
kfont_opts    = B
fallback_font_name = Segoe UI Symbol  ; 글리프가 없는 기호의 대체 폰트

[layout]
margin_top/left/bottom/right = 10   ; 창 가장자리 여백(px)
grid_cols = 96                      ; 가로 칸 수
grid_rows = 32                      ; 세로 줄 수

[window]
start_x / start_y                   ; 시작 위치(px, 가상 화면 좌표). 비워두면 시스템 기본 위치

[screen]
screen_text / screen_back           ; 기본 글자색 / 배경색 (#RRGGBB)
screen_palette00 ~ screen_palette15 ; ANSI 16색 팔레트

[cursor]
cursor_type      = 0   ; 0=기본, 1~6=깜빡/고정 블록·밑줄·I빔
cursor_blink_ms  = 0   ; 0=시스템 설정 따름

[titlebar]
titlebar_caption / titlebar_text / titlebar_border   ; Windows 11 이상에서만 적용

[child]
cmdline         = cmd.exe   ; 실행할 자식 프로세스 명령줄 (비우면 자동 실행 안 함)
work_directory  =           ; 자식 프로세스 작업 디렉토리

[macros]
F1 = 강누리 만세\n     ; F1~F12(F10 제외)를 누르면 그대로 자식에 입력됨

[triggers]
match = password:\x20  ; 화면에 이 문자열이 나타나면
send  = ********\n     ; 자식에게 자동으로 이 문자열을 입력
```

값에 쓰는 이스케이프(`\n`, `\t`, `\xHH`, 경로의 `\\` 등)와 각 항목의 자세한 의미는
`Documents\jbBox Class Library 매뉴얼.md`의 ConBox 절(3.9 ~ 3.13)을 참고하십시오.

## 4. 창 조작

**타이틀바 시스템 메뉴** (타이틀바 아이콘 클릭 또는 Alt+스페이스)

| 메뉴 | 동작 |
|---|---|
| Text로 저장... | 화면 스크롤백 내용을 텍스트 파일로 저장 |
| PDF로 저장... | 화면 내용을 PDF로 저장 (시스템 PDF 프린터 필요) |
| EMF로 저장... | 화면 내용을 페이지 단위 EMF 벡터 파일로 저장 |
| 기록 시작... / 기록 중지 | 자식 프로세스 원시 출력을 파일로 로깅 시작/중지 |

**마우스**

| 조작 | 동작 |
|---|---|
| 드래그 | 텍스트 선택 → 클립보드 자동 복사 |
| 더블클릭 | 단어 선택 |
| Alt + 드래그 | 직사각형 블록 선택 |
| 파일 드래그 앤 드롭 | 파일 경로를 자식 프로세스로 전송 |
| 휠 | 스크롤 |
| Ctrl + 휠 | 전체 화면 확대/축소 (50%~300%) |

**키보드**

- Enter / Shift+Enter / Ctrl+Enter / Ctrl+J: 각각 CR / LF / LF / LF로 자식에 전달되어, "제출"과
  "줄바꿈"을 구분하는 프로그램(Claude Code 등)에서 다르게 동작합니다.
- F1~F12(F10 제외): `[macros]`에 등록된 문자열을 그대로 입력합니다.
- 한글 IME 조합 중 방향키/Enter를 눌러도 조합이 깨지지 않고 순서대로 처리됩니다.

**종료**: 닫기 버튼을 누르면 창은 바로 사라지지만, 뒤에서 자식 프로세스가 정상 종료될 때까지 짧게
기다렸다가(`close_kill_timeout_ms`, 기본 250ms) 완전히 정리됩니다.

## 5. 내장 기본 설정을 rcedit로 교체하기

jbTerm.exe는 전역(기본) 설정을 디스크의 어떤 파일도 참조하지 않고, 실행 파일 안에 내장된 리소스에서만
읽어옵니다(위 2장의 `@파일` / `키=값` 인수는 이 기본값 위에 별도로 얹히는 것이라 영향받지 않습니다).
따라서 exe 옆에 `jbTerm.ini`를 놓아도 아무 효과가 없으며, 기본값 자체를 바꾸려면 실행 파일에 내장된
리소스를 직접 교체해야 합니다. 재컴파일 없이 이 작업을 해 주는 도구가
[rcedit](https://github.com/electron/rcedit)(v2.0.0 이상)입니다.

**절차**

1. 원하는 설정을 담은 INI 파일을 준비합니다. 3장의 형식을 참고해 새로 작성하거나, 기존 INI를 복사해
   고치는 편이 쉽습니다.
2. 패치 대상 `jbTerm.exe`가 실행 중이면 먼저 종료합니다 (실행 중인 파일은 rcedit가 열 수 없습니다).
3. rcedit로 리소스를 교체합니다.
   ```
   rcedit.exe "D:\path\to\jbTerm.exe" --set-rcdata 129 "D:\path\to\new.ini"
   ```
   - `129`는 jbTerm.rc에 정의된 `IDR_DEFAULT_INI` 리소스 ID로 고정값입니다 (혹시 바뀌었다면
     `Build\jbTerm\jbTerm.rc`의 `RCDATA` 항목에서 실제 번호를 확인하십시오).
   - `--set-rcdata`는 **이미 그 ID로 존재하는 리소스만 교체**할 수 있습니다. `jbTerm.exe`는 빌드 시
     `jbTerm.rc`의 `129 RCDATA "jbTerm.ini"`로 항상 이 리소스를 내장한 채로 만들어지므로, 정상적으로
     빌드된 `jbTerm.exe`라면 이 조건은 항상 충족됩니다.
4. 패치된 `jbTerm.exe`를 실행하면, 새 INI의 내용이 기본값으로 적용됩니다.

**주의**

- 이 패치는 실행 파일 자체를 직접 수정하는 것입니다. 소스(`jbTerm.rc`가 가리키는 `jbTerm.ini`)는
  건드리지 않으므로, 나중에 다시 빌드하면 그 exe는 원래 내장값으로 되돌아갑니다. 즉 rcedit 패치는
  "이미 빌드된 특정 exe 파일 한 벌"에만 적용되고, 소스에는 반영되지 않습니다.
- 기본값 자체를 프로젝트 차원에서 영구적으로 바꾸려면, `Build\jbTerm\jbTerm.ini`(빌드 시 리소스로
  구워지는 원본)를 수정하고 다시 빌드해야 합니다.

---

*jbTerm - jbBox 클래스 라이브러리 기반 터미널 프로그램*
