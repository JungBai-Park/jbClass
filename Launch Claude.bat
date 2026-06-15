@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "TEMP=%~dp0\Temp"
set "TMP=%~dp0\Temp"
if not exist "%TEMP%"   mkdir "%TEMP%"

if /i "%~1" == "up" goto :upgrade

:claude
    sudo "D:\ETC\Programs\Node.js\node_modules\@anthropic-ai\claude-code\bin\claude.exe" --remote-control --allow-dangerously-skip-permissions %*
    goto quit

:upgrade
    npm install -g @anthropic-ai/claude-code
    goto quit

:quit
