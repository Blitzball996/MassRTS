@echo off
REM ============================================================
REM  MassRTS - One-click launcher (just double-click this file)
REM ============================================================
cd /d "%~dp0"

set EXE=build\Release\MassRTS_GPU.exe

if not exist "%EXE%" (
    echo [!] Game not built yet. Building now...
    echo.
    call build.bat
    if not exist "%EXE%" (
        echo.
        echo [X] Build failed. See messages above.
        pause
        exit /b 1
    )
)

echo Starting MassRTS...
cd build\Release
MassRTS_GPU.exe
cd ..\..
echo.
echo Game exited.
pause
