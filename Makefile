CC = gcc
CFLAGS = -Wall -Wextra -g

server: main.c state.h
	$(CC) $(CFLAGS) -o server main.c

clean:
	rm -f server
