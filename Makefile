#CROSS_COMPILE := aarch64-linux-gnu-
CFLAGS += -W -Wall -Werror -pedantic -std=c11
CC := $(CROSS_COMPILE)gcc
targets := client server

.PHONY: all clean

all: client server

client: client.o

server: server.o

clean:
	rm -rf *.o
