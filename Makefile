CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS ?= -lws2_32

SRC_DIR = src
TARGET  = proxy.exe

SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/proxy_core.c \
       $(SRC_DIR)/tcp_socket_win.c \
       $(SRC_DIR)/http_parse.c

OBJS = $(SRCS:.c=.o)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

run: $(TARGET)
	./$(TARGET)
