@echo off
REM ============================================================
REM  SDFCraft - Dedicated Server (headless, no window/GL)
REM  Runs the authoritative simulation at 20Hz. Clients connect
REM  with:  SDFCraft.exe --connect <server-ip> --port 55001
REM  (locally:  SDFCraft.exe --connect 127.0.0.1 --port 55001)
REM ============================================================
cd /d "%~dp0"

set EXE=build\Release\SDFCraft.exe
set PORT=55001
set SEED=1337

if not exist "%EXE%" (
    echo [*] SDFCraft not built yet. Building...
    cmake --build build --config Release --target SDFCraft
    if not exist "%EXE%" (
        echo [X] Build failed. See messages above.
        pause
        exit /b 1
    )
)

cd build\Release
echo [*] Starting DEDICATED SERVER on port %PORT% (seed %SEED%).
echo     Clients connect with:  SDFCraft.exe --connect ^<this-pc-ip^> --port %PORT%
echo     Press Ctrl-C in this window to stop the server.
echo.
SDFCraft.exe %SEED% --server --port %PORT%
cd ..\..
pause
