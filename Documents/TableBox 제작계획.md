# TableBox 제작 계획 #1



- 본 문서는 CLAUDE.md에서 언급한 TableBox 모듈을 제작하기 위한 계획을 AI와 협의하기 위해 작성한다.
  - TableBox 모듈은 Source/ 디렉토리에 하나의 소스 파일과 하나의 헤더파일로 작성된다.
    (TableBox.cpp, TableBox.h : UTF-8 with BOM)
- 제작 계획을 읽고, 구현 상 문제가 되는 부분이나 구현 방법에 대해 추가적인 설계 결정이 필요한 사항이 있으면
  AI가 나에게 질문을 하여 답변해 나아가는 과정을 통해 설계 요구사항 및 단계별 구현/검증 계획을 확정한다.

- 본 협의 결과 AI가 설계 내용이 명확해 졌다고 말해 주면, 
  내가 Requirement.md에 요구사항을 반영하고 ToDoList.md에 단계별 계획을 작성해 달라고 요청할 예정이다.



## 아이디어

- TableBox는 내용이 많은 자료를 전시하기 위한 컨트롤로서, Excel과 유사한 모양으로 자료를 전시하는 컨트롤이다.
- TableBox는 특정 프로젝트 전용으로 개발하는 것이 아니며, 
  불특정 다수가 당시 상황에 맞게 커스터마이즈 하여 쓸수 있도록 최대한의 유연성을 갖게하는 컨셉이다.
- TableBox는 자료를 모두 갖고 있는 구조가 아니라, 
  열과 행을 인수로 하는 콜백함수를 통해 자료를 얻어와서 전시하는 구조를 갖는다.

- TableBox는 CWnd 또는 CWnd의 파생클래스를 기저클래스로 갖는 클래스 정의이다.

- 다음과 같은 퍼블릭 멤버함수를 제공한다.

  - 열의 화면넓이 및 갯수 지정 방법 :
    - `set_cols(int width, int limit)` - 일정한 폭(width:96dpi를 기준으로한 논리적 픽셀간격)으로 limit 갯수만큼
    - `set_cols(vector\<int\>)` - 벡터에 각 열의 폭이 지정되어 있으며, 벡터의 크기 만큼의 칼럼 갯수를 가짐
  - 행의 화면높이 및 갯수 지정 방법 :
    - `set_rows(int height, int limit)` - 일정한 폭(width:96dpi를 기준으로한 논리적 픽셀간격)으로 limit 갯수만큼
    - `set_rows(vector<int>)` - 벡터에 각 열의 폭이 지정되어 있으며, 벡터의 크기 만큼의 칼럼 갯수를 가짐
  -  고정된 행/열 지정 방법
    - `set_fixed(int rows, int cols)` - 고정된 열과 행의 갯수를 지정 (기본값 1,1) : 엑셀의 행/열 헤더 처럼 고정시킬 행/열 갯수
  - 셀에 표시할 텍스트를 얻어오는 call_back 함수 지정
    - `set_callback(const char *(*call_back)(int row, col))` - call_back은 그릴 열과 행 값을 인수로 호출한다.
    - 인수로 사용되는 열과 행의 값은 0부터 시작되는 정수이다.
  - 기본 폰트를 설정하는 함수
    - set_font(const char* name, float size, const char* opts = 0)
      - ConBox에서 구현된 방식과 동일하게 인수를 받아 처리한다.
      - ConBox에서 구현된 변수/함수를 참조하지 말고, 중복된 코드가 있더라도 Table 모듈 내에서 동일하게 구현한다.

- 윈도우 기본 스크롤 바를 사용하지 말고 ConBox에서 구현한 클라이언트 영역내 동적 스크롤바를 수직/수평에 자동으로 표시한다.

  - ConBox와 마찬가지로 DPI에 따라 크기가 변동해야 하며, 배경색에 따라 색상이 조절되어야 한다.
  - ConBox에서 구현된 변수/함수를 참조하지 말고, 중복된 코드가 있더라도 Table 모듈 내에서 동일하게 구현한다.

  



## 1단계 

더블 버퍼링 방식으로 그림을 그려 두었다가 OnPaint에서 화면에 표시하는 CWnd(또는 CWind의 파생클래스)기반 클래스를 제작한다.

일단은 흰색 배경으로 채운다.

FrameBox의 데모프로그램에 자리를 잡아둔다.



## 2단계

set_cols(100, 15)

set_rows(25, 60)

set_fixed(1, 1)



char buff[80];

const char* DefaultText(int row, int col) {

​	sprintf(buff, [%d:%d], row, col)

}

set_callback(DefaultText)

set_font("맑은 고딕", 10)



위 설정을 이용하여 화면에 엑셀처럼 그려주는 테이블박스를 구현한다.



## 3단계

2단계 수행 후 구체화 또는 AI의 제안에 따라 결정



## 문서 정리

#### Requrement.md는 다음과 같은 내용을 담아서 정리한다.

- 협의 결과 구체화된 요구사항을 TableBox 모듈용 챕터를 하나 추가하여 작성한다.



#### Learned.md는 다음과 같은 내용을 담아서 정리한다.

- ToDoList.md의 단계를 수행하며 디버깅하는 과정에서 기록해야 할 메모
- ableBox 모듈용 챕터를 하나 추가하여 작성한다.



#### ToDoList.md는 다음과 같은 내용을 담아서 정리한다.

- 단계별 진행할 내용 여러 세션에 걸쳐 진행하더라도, 매 세션의 처음에 어떤 작업을 수행할 차례인지 알 수 있도록

