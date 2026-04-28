CC      := gcc
PROJECT := server
CFLAGS  := -std=c17 -Wall -Wextra -Wpedantic -O2 -D_POSIX_C_SOURCE=200809L

all: $(PROJECT)

$(PROJECT): main.c state.h
	$(CC) $(CFLAGS) -o $(PROJECT) main.c

clean:
	rm -f $(PROJECT)
