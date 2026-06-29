@echo off
REM ============================================================
REM  SDFCraft - Multiplayer test (Listen Server + 1 Client)
REM  Builds if needed, starts a HOST window, waits, then opens a
REM  second window that connects to it on localhost. Play across
REM  the two windows: dig/build/mobs/day-night all replicate.
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

echo [*] Starting HOST (listen server) on port %PORT% ...
start "SDFCraft HOST" SDFCraft.exe %SEED% --host --port %PORT%

echo [*] Waiting for the host to come up...
timeout /t 3 /nobreak >nul

echo [*] Starting CLIENT -> 127.0.0.1:%PORT% ...
start "SDFCraft CLIENT" SDFCraft.exe --connect 127.0.0.1 --port %PORT%

cd ..\..
echo.
echo Two windows launched: HOST and CLIENT, sharing one world.
echo Close either window to leave. The host owns the simulation.
