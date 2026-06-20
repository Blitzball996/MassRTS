@echo off
echo Starting HOST instance...
start "MassRTS Host" build\Release\MassRTS_GPU.exe --host
timeout /t 2 /nobreak >nul
echo Starting CLIENT instance...
start "MassRTS Client" build\Release\MassRTS_GPU.exe --join 127.0.0.1
echo Both instances launched. Close this window after testing.
