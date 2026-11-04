CC=gcc
CFLAGS=-Wall -Wextra -std=c99 -O2

# Default target
all: wip

# Windows target
windows: wip.exe

# Unix/Linux/macOS target
unix: wip

# Build targets
wip: wip.c
	$(CC) $(CFLAGS) -pthread -o wip wip.c

wip.exe: wip.c
	$(CC) $(CFLAGS) -o wip.exe wip.c -lws2_32

clean:
	rm -f wip wip.exe

.PHONY: all windows unix clean
