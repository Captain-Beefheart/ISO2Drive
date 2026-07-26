# ISO2Drive build. Auto-selects the host backend.
# Build on MSYS2/MinGW (Windows) -> iso2drive.exe, or on Linux -> iso2drive.

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -Iinclude
LDFLAGS ?=

ifeq ($(OS),Windows_NT)
    BACKEND := src/backend/backend_windows.c src/backend/winutil.c src/backend/winusb.c
    BIN     := iso2drive.exe
else
    BACKEND := src/backend/backend_linux.c
    BIN     := iso2drive
endif

CORE := $(wildcard src/core/*.c)
SRCS := $(CORE) src/main.c $(BACKEND)
OBJS := $(SRCS:.c=.o)

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) iso2drive iso2drive.exe
