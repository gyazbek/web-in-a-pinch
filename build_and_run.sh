#!/bin/bash

echo "Building WIP (Web In a Pinch) Server..."

# Portable optimized build (no CPU-specific flags, so the binary runs on other machines)
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" || "$OSTYPE" == "win32" ]]; then
    # Windows (MSYS2/MinGW)
    echo "Detected Windows environment"
    gcc -O2 -Wall -Wextra -std=c99 -o wip wip.c -lws2_32
else
    # Linux/WSL/macOS
    echo "Detected Unix-like environment (Linux/WSL/macOS)"
    gcc -O2 -Wall -Wextra -std=c99 -pthread -o wip wip.c
fi

if [ $? -eq 0 ]; then
    echo ""
    echo "Build successful: wip"
    echo ""
    echo "Starting WIP server on http://127.0.0.1:8080"
    echo "Press Ctrl+C to stop the server"
    echo ""
    ./wip
else
    echo ""
    echo "Build failed!"
    echo "Make sure GCC is installed"
    exit 1
fi
