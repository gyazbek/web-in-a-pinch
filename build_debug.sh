#!/bin/bash

echo "Building wip (debug build)..."

# Debug symbols and no optimization. On Linux/macOS also AddressSanitizer and
# UndefinedBehaviorSanitizer, which MinGW on Windows doesn't support.
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" || "$OSTYPE" == "win32" ]]; then
    gcc -g3 -O0 -Wall -Wextra -std=c99 -o wip_debug wip.c -lws2_32
else
    gcc -g3 -O0 -Wall -Wextra -std=c99 -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -o wip_debug wip.c
fi

if [ $? -eq 0 ]; then
    echo ""
    echo "Debug build successful: wip_debug"
    echo "Starting it on http://127.0.0.1:8080 - press Ctrl+C to stop"
    echo ""
    ./wip_debug
else
    echo ""
    echo "Debug build failed!"
    echo "Make sure GCC is installed (with sanitizer support on Linux/macOS)"
    exit 1
fi
