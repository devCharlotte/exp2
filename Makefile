# Makefile
CC=gcc
CFLAGS=-O2 -Wall -std=gnu11

all: wl

wl: wl.c
	$(CC) $(CFLAGS) -o wl wl.c

clean:
	rm -f wl *.o *.csv ready_*.log
