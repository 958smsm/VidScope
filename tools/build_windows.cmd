@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "PYTHON_BIN="

where python >nul 2>nul
if %ERRORLEVEL% equ 0 (
    set "PYTHON_BIN=python"
) else (
    where py >nul 2>nul
    if %ERRORLEVEL% equ 0 (
        set "PYTHON_BIN=py"
    )
)

if "%PYTHON_BIN%"=="" (
    echo [ERROR] Python executable not found in PATH. Please install Python 3.8+.
    exit /b 1
)

"%PYTHON_BIN%" "%SCRIPT_DIR%build.py" %*
set EXIT_CODE=%ERRORLEVEL%

exit /b %EXIT_CODE%
