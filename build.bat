@echo off
REM ============================================================
REM  MassRTS - Build script (compiles the game)
REM  Requires: CMake + Visual Studio 2022 (Community is fine)
REM ============================================================
cd /d "%~dp0"

if not exist build (
    echo [*] First-time setup: configuring CMake...
    cmake -B build -G "Visual Studio 17 2022" -A x64
    if errorlevel 1 (
        echo [X] CMake configure failed. Is CMake + VS2022 installed?
        pause
        exit /b 1
    )
)

echo [*] Building Release (this can take a minute)...
cmake --build build --config Release --target MassRTS_GPU

if errorlevel 1 (
    echo [X] Build failed. See errors above.
    pause
    exit /b 1
)

echo.
echo [OK] Build complete: build\Release\MassRTS_GPU.exe
echo      Run PLAY.bat to start the game.
pause
