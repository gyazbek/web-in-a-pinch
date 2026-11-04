@echo off
echo Building WIP (Web In a Pinch) Server...
echo Detected Windows environment

REM Portable optimized build (runs on any x64 Windows machine)
gcc -O2 -Wall -Wextra -std=c99 -o wip.exe wip.c -lws2_32

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Build successful: wip.exe
    echo.
    echo Starting WIP server on http://127.0.0.1:8080
    echo Press Ctrl+C to stop the server
    echo.
    .\wip.exe
) else (
    echo.
    echo Build failed!
    echo Make sure GCC is installed and in your PATH
    pause
)
