CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?= -lm

TARGET = rgssdecrypter
SRCS = main.c rgss_lib.c
OBJS = $(SRCS:.c=.o)

ifeq ($(OS),Windows_NT)
    TARGET := $(TARGET).exe
    LDFLAGS :=
endif

.PHONY: all clean debug

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c rgss_lib.h
	$(CC) $(CFLAGS) -c -o $@ $<

debug: CFLAGS = -std=c11 -g -O0 -Wall -Wextra -Wpedantic -fsanitize=address,undefined
debug: LDFLAGS += -fsanitize=address,undefined
debug: clean $(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
