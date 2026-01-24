CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS = -lpthread -lsqlite3 -lssl -lcrypto

# FFmpeg libraries for media probing
FFMPEG_CFLAGS = $(shell pkg-config --cflags libavformat libavcodec libavutil)
FFMPEG_LIBS = $(shell pkg-config --libs libavformat libavcodec libavutil)

CFLAGS += $(FFMPEG_CFLAGS)
LDFLAGS += $(FFMPEG_LIBS)

SRC = server.c database.c scanner.c
OBJ = $(SRC:.c=.o)
TARGET = nixly-server

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
