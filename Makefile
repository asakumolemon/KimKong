CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra -Isrc
LDFLAGS ?= -lws2_32 -lcomctl32 -lgdi32 -luser32 -lkernel32 -lcomdlg32 -lole32 -luuid

SRC_DIR = src
TARGET  = KimKong.exe

SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/proxy_core.c \
       $(SRC_DIR)/tcp_socket_win.c \
       $(SRC_DIR)/http_parse.c \
       $(SRC_DIR)/upstream.c \
       $(SRC_DIR)/lb.c \
       $(SRC_DIR)/health_check.c \
       $(SRC_DIR)/ui_win32.c

OBJS = $(SRCS:.c=.o)
RES  = $(SRC_DIR)/resource.res

.PHONY: all clean run

all: $(TARGET)

$(SRC_DIR)/resource.res: $(SRC_DIR)/resource.rc $(SRC_DIR)/resource.h assets/app.ico
	llvm-rc $(SRC_DIR)/resource.rc

$(TARGET): $(OBJS) $(RES)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	del /Q $(OBJS) $(TARGET) $(SRC_DIR)/resource.res 2>nul

run: $(TARGET)
	$(TARGET)
