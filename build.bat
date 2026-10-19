@echo off
setlocal
title Building LMMS

echo ===================================================
echo               LMMS Build Script
echo ===================================================
echo.

set "MSYS2_ROOT=C:\msys64"

if not exist "%MSYS2_ROOT%\msys2_shell.cmd" (
    echo [ERROR] MSYS2 was not found at %MSYS2_ROOT%
    echo Please make sure MSYS2 is installed at C:\msys64
    pause
    exit /b 1
)

cd /d "%~dp0"

echo [INFO] Resuming LMMS compilation using UCRT64 toolchain...
echo [INFO] CPU Cores: %NUMBER_OF_PROCESSORS%
echo.

call "%MSYS2_ROOT%\msys2_shell.cmd" -ucrt64 -defterm -no-start -here -c "cmake --build build -j %NUMBER_OF_PROCESSORS%"

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ===================================================
    echo [SUCCESS] LMMS build completed successfully!
    echo Binary executable is located in: build\lmms.exe
    echo ===================================================
) else (
    echo.
    echo ===================================================
    echo [ERROR] Build failed or was interrupted with code %ERRORLEVEL%.
    echo You can run this script again to resume.
    echo ===================================================
)

echo.
pause
