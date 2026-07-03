# Phase 1 — TCP Socket Abstraction + Event Loop

## 功能

单线程 `select()` 反代引擎，监听一个端口，将收到的所有 TCP 连接转发到固定的目标地址。

## 文件清单

| 文件 | 用途 |
|------|------|
| `src/tcp_socket.h` | 跨平台 socket 抽象头文件（`#ifdef _WIN32`） |
| `src/tcp_socket_win.c` | Winsock 实现 |
| `src/http_parse.h` | HTTP 头部解析（Phase 1 先写好，尚未集成到事件循环） |
| `src/http_parse.c` | HTTP 解析实现 |
| `src/proxy_core.h` | 代理引擎头文件 |
| `src/proxy_core.c` | select() 事件循环 + 双向转发 |
| `src/main.c` | 入口点 |

## 编译

当前工作目录为 `kimkong/`（放在与 `src/` 平级的位置）。

### MinGW-w64

```bash
gcc -O2 -Wall -Wextra -o proxy.exe \
    src/main.c src/proxy_core.c src/tcp_socket_win.c src/http_parse.c \
    -lws2_32
```

### MSVC (cl.exe)

```cmd
cl /O2 /W3 /Fe:proxy.exe ^
    src/main.c src/proxy_core.c src/tcp_socket_win.c src/http_parse.c ^
    ws2_32.lib
```

## 使用

### 启动

```bash
# 默认监听 :8888，转发到 127.0.0.1:8080
./proxy.exe
```

### 验证

1. 在另一个终端启动一个测试 HTTP 服务（如 `python -m http.server 8080`）
2. 访问 `http://localhost:8888`，应该看到 Python HTTP Server 的内容
3. 观察 proxy 终端的日志输出，可以看到 连接建立/关闭 信息

### 停止

按 `Ctrl+C` 触发优雅关闭。

## 默认参数

编辑 `src/main.c` 开头的三行即可修改：

```c
uint16_t listen_port = 8888;     // 代理监听端口
const char *target_host = "127.0.0.1";  // 目标地址
int target_port = 8080;          // 目标端口
```
