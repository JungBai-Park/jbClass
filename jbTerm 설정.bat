@echo off
rem SetJbTermIni.bat - jbTerm.exe의 초기설정값 리소스(RCDATA 129)를 지정한 ini 파일로 교체한다.
rem 사용법: SetJbTermIni.bat "적용할.ini"
rem 대상 jbTerm.exe는 이 배치파일과 같은 디렉토리에서 찾는다 (%~dp0jbTerm.exe).
rem 거기 없으면 %~dp0Build\jbTerm\Release.64\jbTerm.exe를 찾아 있으면 %~dp0로 복사해 온다.
rem rcedit는 실행 중인 exe를 직접 덮어쓰지 못할 수 있으므로, 임시 파일에 먼저 적용해
rem 성공한 뒤에만 원본을 교체한다 (실패 시 원본은 그대로 남는다).

setlocal

set "TARGET_EXE=%~dp0jbTerm.exe"
set "RELEASE_EXE=%~dp0Build\jbTerm\Release.64\jbTerm.exe"
set "TEMP_EXE=%~dp0jbTerm.exe.tmp"
set "RCDATA_ID=129"

if "%~1"=="" (
    echo [오류] 적용할 ini 파일 경로를 인자로 지정하십시오.
    echo 사용법: %~nx0 "적용할.ini"
    exit /b 1
)

set "SRC_INI=%~dpnx1"

if not exist "%SRC_INI%" (
    echo [오류] 지정한 ini 파일을 찾을 수 없습니다: %SRC_INI%
    exit /b 1
)

if not exist "%TARGET_EXE%" (
    if exist "%RELEASE_EXE%" (
        rem Release 빌드가 있으면 %~dp0로 복사해 놓고 이후 동일하게 진행한다.
        copy /Y "%RELEASE_EXE%" "%TARGET_EXE%" >nul 2>&1
        if not exist "%TARGET_EXE%" (
            echo [오류] Release 빌드 복사에 실패했습니다: %RELEASE_EXE%
            exit /b 1
        )
    ) else (
        echo [오류] jbTerm.exe를 찾을 수 없습니다: %TARGET_EXE%
        echo 다음 경로에서도 찾지 못했습니다: %RELEASE_EXE%
        exit /b 1
    )
)

where rcedit >nul 2>&1
if errorlevel 1 (
    echo [오류] rcedit 명령을 찾을 수 없습니다. PATH에 rcedit.exe를 추가한 뒤 다시 시도하십시오.
    exit /b 1
)

if exist "%TEMP_EXE%" del /F /Q "%TEMP_EXE%" >nul 2>&1

rem 1) jbTerm.exe를 임시 이름으로 복사한다 (원본은 아직 손대지 않는다).
copy /Y "%TARGET_EXE%" "%TEMP_EXE%" >nul 2>&1
if not exist "%TEMP_EXE%" (
    echo [오류] 임시 파일 생성에 실패했습니다: %TEMP_EXE%
    exit /b 1
)

rem 2) 임시 파일에 rcedit로 리소스를 적용한다. 실패하면 임시 파일만 지우고 원본은 그대로 둔다.
rcedit "%TEMP_EXE%" --set-rcdata %RCDATA_ID% "%SRC_INI%"
if errorlevel 1 (
    echo [오류] rcedit 리소스 적용에 실패했습니다: %SRC_INI%
    del /F /Q "%TEMP_EXE%" >nul 2>&1
    exit /b 1
)

rem 3) 적용이 성공했으므로 원본을 지운다. jbTerm.exe가 실행 중이면 여기서 실패할 수 있다.
del /F /Q "%TARGET_EXE%" >nul 2>&1
if exist "%TARGET_EXE%" (
    echo [오류] 원본 파일 삭제에 실패했습니다. jbTerm.exe가 실행 중인지 확인한 뒤 다시 시도하십시오: %TARGET_EXE%
    del /F /Q "%TEMP_EXE%" >nul 2>&1
    exit /b 1
)

rem 4) 임시 파일을 원래 이름으로 되돌린다.
ren "%TEMP_EXE%" "jbTerm.exe"
if not exist "%TARGET_EXE%" (
    echo [심각한 오류] 원본 삭제 후 이름 변경에 실패해 jbTerm.exe가 없는 상태입니다.
    echo 다음 파일을 수동으로 jbTerm.exe로 이름을 바꾸십시오: %TEMP_EXE%
    exit /b 1
)

echo [완료] "%SRC_INI%" 을(를) jbTerm.exe의 초기설정값 리소스에 적용했습니다.
exit /b 0
