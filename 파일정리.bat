@echo on

rem jbBox project structure cleanup: Build\ is the solution directory, and each
rem project lives in its own Build\<ProjectName>\ subfolder (see project CLAUDE.md).
rem OutDir/IntDir per project are $(ProjectDir)$(Configuration).64\ and .32\.
rem Deletion targets under Build\ are limited to files/folders that VS/MSBuild
rem regenerates on its own, i.e. removing them still leaves the *.sln buildable
rem as-is. Temp\ is emptied but the folder itself is kept (see project CLAUDE.md).

setlocal enabledelayedexpansion
chcp 949 > nul

cd /d "%~dp0Build"
if not %errorlevel% == 0 goto :quit
call :clean_solution

for /d %%P in (*) do (
    pushd "%%P"
    call :clean_project
    popd
)

if exist "%~dp0Temp" (
    pushd "%~dp0Temp"
    call :clean_temp
    popd
)

echo 디렉토리 정리를 마쳤습니다 : %cd%
goto :quit

:clean_solution
    call :del   *.suo
    call :del   *.sdf
    call :del   *.opensdf

    call :rmdir .vs
    call :rmdir ipch
    exit /b

:clean_project
    call :del   *.aps
    call :del   *.pdb
    call :del   *.ilk
    call :del   *.idb
    call :del   *.sdf
    call :del   *.opensdf
    call :del   *.user
    call :del   *.log

    call :rmdir ipch
    call :rmdir Debug.32
    call :rmdir Debug.64
    call :rmdir Release.32
    call :rmdir Release.64
    exit /b

:clean_temp
    for %%F in (*) do if not exist "%%F\" call :del "%%F"
    for /d %%D in (*) do call :rmdir "%%D"
    exit /b

:del
    if exist "%~1" (
        attrib /D /S -h -r -s "%~1"
        del /F /S "%~1"
    )
    exit /b

:rmdir
    if exist "%~1" (
        attrib /D -h -r -s "%~1"
        rmdir /Q /S "%~1"
    )
    exit /b

:quit - 더블클릭으로 실행되었을 경우 종료하기 전 pause가 실행됨
    set CMD=%cmdcmdline: =%
    if not defined SUPPRESS_QUIT_PAUSE if not [%CMD:/C=%] == [%CMD%] pause
    exit /b
