# TODO: make sure the rules for server client and markdown filled!
CC := gcc
CFLAGS := -g -O1 -fsanitize=address -fno-omit-frame-pointer \
          -Wall -Wextra -pthread

SOURCE := source
LIBS := libs

SERVER_C := $(SOURCE)/server.c
CLIENT_C := $(SOURCE)/client.c
MARKDOWN_C := $(SOURCE)/markdown.c

MARKDOWN_H := $(LIBS)/markdown.h

SERVER_O := server.o
CLIENT_O := client.o
MARKDOWN_O := markdown.o

.PHONY: all clean

all: server client

server: $(SERVER_O) $(MARKDOWN_O)
	$(CC) $(CFLAGS) -o $@ $^

client: $(CLIENT_O) $(MARKDOWN_O)
	$(CC) $(CFLAGS) -o $@ $^

$(SERVER_O): $(SERVER_C) $(MARKDOWN_H)
	$(CC) $(CFLAGS) -c $< -o $@

$(CLIENT_O): $(CLIENT_C) $(MARKDOWN_H)
	$(CC) $(CFLAGS) -c $< -o $@

$(MARKDOWN_O): $(MARKDOWN_C) $(MARKDOWN_H)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f server client *.o
	rm -f FIFO_S2C_* FIFO_C2S_*
