# Makefile for rgssdecrypter (Arch Linux, Termux on Android, & cross-platform)

VERSION ?= 1.1

UNAME_O := $(shell uname -o 2>/dev/null)

# Detect Termux / Android vs Desktop Linux (Arch, etc.)
ifeq ($(UNAME_O),Android)
    CC ?= clang
    STRIP ?= strip
    PREFIX ?= /data/data/com.termux/files/usr
else ifneq ($(findstring com.termux,$(PREFIX)),)
    CC ?= clang
    STRIP ?= strip
else
    CC ?= cc
    STRIP ?= strip
    PREFIX ?= /usr/local
endif

BINDIR ?= $(PREFIX)/bin

CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -D_FILE_OFFSET_BITS=64
CPPFLAGS += -DAPP_VERSION=\"$(VERSION)\"
LDFLAGS ?= -lm

TARGET = rgssdecrypter
SRCS = main.c rgss_lib.c
OBJS = $(SRCS:.c=.o)

ifeq ($(OS),Windows_NT)
    TARGET := $(TARGET).exe
    LDFLAGS :=
endif

.PHONY: all clean debug install uninstall

all: $(TARGET)
	$(STRIP) $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c rgss_lib.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

debug: CFLAGS = -std=c11 -g -O0 -Wall -Wextra -Wpedantic -D_FILE_OFFSET_BITS=64 -fsanitize=address,undefined
debug: LDFLAGS += -fsanitize=address,undefined
debug: clean $(TARGET)

install: all
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 $(TARGET) "$(DESTDIR)$(BINDIR)/$(TARGET)"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/$(TARGET)"

clean:
	rm -f $(OBJS) $(TARGET)
