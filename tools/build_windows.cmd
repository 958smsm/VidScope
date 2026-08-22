@echo off
setlocal EnableExtensions
rem VidScope Windows CMD build wrapper.
rem No arguments => Release configure + build + runtime repair + tests.

set "SCRIPT_DIR=%~dp0"
set "BUILD_SCRIPT=%SCRIPT_DIR%build.py"

where python.exe >nul 2>nul
if not errorlevel 1 (
    if "%~1"=="" echo [INFO] VidScope default: Release build + runtime repair + tests
    python.exe "%BUILD_SCRIPT%" %*
    exit /b %ERRORLEVEL%
)

where py.exe >nul 2>nul
if not errorlevel 1 (
    if "%~1"=="" echo [INFO] VidScope default: Release build + runtime repair + tests
    py.exe -3 "%BUILD_SCRIPT%" %*
    exit /b %ERRORLEVEL%
)

echo [ERROR] Python 3.8+ was not found. Install Python or add python.exe/py.exe to PATH. 1>&2
exit /b 1
