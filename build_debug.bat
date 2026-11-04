@echo off
echo Building wip (debug build)...

REM Debug symbols, no optimization. MinGW doesn't support -fsanitize=address, so it isn't used here.
gcc -g3 -O0 -Wall -Wextra -std=c99 -o wip_debug.exe wip.c -lws2_32

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Debug build successful: wip_debug.exe
    echo Starting it on http://127.0.0.1:8080 - press Ctrl+C to stop
    echo.
    .\wip_debug.exe
) else (
    echo.
    echo Debug build failed!
    echo Make sure GCC is installed and in your PATH
    pause
)
