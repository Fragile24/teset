CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -Wpedantic -g $(shell pkg-config --cflags sdl2)
LDFLAGS = $(shell pkg-config --libs sdl2)
SRCS = src/main.c src/chip8.c
TARGET = chip8

$(TARGET): $(SRCS) src/chip8.h
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: clean
