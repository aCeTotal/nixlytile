CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS = -lpthread -lsqlite3 -lcurl

# FFmpeg libraries for media probing
FFMPEG_CFLAGS = $(shell pkg-config --cflags libavformat libavcodec libavutil 2>/dev/null)
FFMPEG_LIBS = $(shell pkg-config --libs libavformat libavcodec libavutil 2>/dev/null)

# cJSON for parsing TMDB responses
CJSON_CFLAGS = $(shell pkg-config --cflags libcjson 2>/dev/null)
CJSON_LIBS = $(shell pkg-config --libs libcjson 2>/dev/null)

CFLAGS += $(FFMPEG_CFLAGS) $(CJSON_CFLAGS)
LDFLAGS += $(FFMPEG_LIBS) $(CJSON_LIBS)

SRC = server.c database.c scanner.c config.c tmdb.c watcher.c transcoder.c
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
