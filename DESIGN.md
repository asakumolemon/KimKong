# KimKong — Reverse Proxy Design Document

> A lightweight reverse proxy with Win32 native GUI, least-connections load balancing, and TCP health checking.
> Written in C, designed for cross-platform migration (Windows → macOS).

---

## Table of Contents

1. [Overview](#1-overview)
2. [Architecture](#2-architecture)
3. [Module Design](#3-module-design)
4. [Data Structures](#4-data-structures)
5. [Event Loop](#5-event-loop)
6. [Load Balancing](#6-load-balancing)
7. [Health Check](#7-health-check)
8. [GUI Layout (Win32 VS Style)](#8-gui-layout-win32-vs-style)
9. [Platform Abstraction Strategy](#9-platform-abstraction-strategy)
10. [File Structure](#10-file-structure)
11. [Phase Plan](#11-phase-plan)

---

## 1. Overview

### Goals

- **Function**: A TCP-layer reverse proxy that forwards incoming connections to one of several backend servers.
- **Load Balancing**: Least-connections algorithm — pick the backend with the fewest active connections.
- **Health Check**: Proactive TCP connectivity probes + passive failure detection; auto-remove/recover backends.
- **UI**: Native Windows application with Visual-Studio-style layout (toolbar, listview, log pane, status bar).
- **Cross-platform**: Core engine written in portable C; GUI layer isolated behind an interface for future macOS Cocoa port.

### Constraints

- Single-threaded event loop using `select()` (portable foundation, sufficient for toy-level throughput).
- No external dependencies beyond Windows SDK (for GUI) and C standard library + Winsock2.
- Toy-level: not aiming for high concurrency or production-grade performance.

---

## 2. Architecture

```
┌──────────────────────────────────────────────────┐
│                   GUI Layer                       │
│           (platform-specific, isolated)           │
│                                                   │
│  ┌────────────────┐  ┌────────────────────────┐  │
│  │   Win32 GUI    │  │   macOS Cocoa (future)  │  │
│  │  ui_win32.c    │  │   ui_cocoa.m            │  │
│  └──────┬─────────┘  └────────┬───────────────┘  │
│         │                      │                  │
│         └──────────┬───────────┘                  │
│                    │  ui.h (abstraction)          │
├────────────────────┼─────────────────────────────┤
│                    │  callbacks                   │
│  ┌─────────────────┴──────────────────────────┐  │
│  │            Proxy Core Engine               │  │
│  │              proxy_core.c                  │  │
│  │  single-threaded select() event loop       │  │
│  └───┬──────────┬──────────┬─────────┬───────┘  │
│      │          │          │         │           │
│  ┌───┴───┐ ┌───┴───┐ ┌───┴────┐ ┌──┴────────┐  │
│  │Socket │ │HTTP   │ │Upstream│ │Health     │  │
│  │Abstrac│ │Parse  │ │Pool    │ │Check      │  │
│  │tion   │ │       │ │   +    │ │           │  │
│  │       │ │       │ │LB      │ │           │  │
│  └───────┘ └───────┘ └────────┘ └───────────┘  │
│                      Core Layer                  │
└──────────────────────────────────────────────────┘
```

### Design Principles

- **GUI never touches sockets directly.** All network operations are in the core layer; GUI only observes via callbacks.
- **Core never depends on Windows headers.** The core is pure C + POSIX/Winsock abstracted behind `tcp_socket.h`.
- **Callback-driven UI updates.** Core fires `ui_on_*` notifications; GUI decides how to render.

---

## 3. Module Design

### 3.1 `tcp_socket.h` / `tcp_socket_win.c`

Cross-platform socket abstraction. Hides the difference between Winsock and POSIX socket APIs.

```c
// tcp_socket.h

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef SOCKET socket_t;
    #define INVALID_SOCKET_VALUE INVALID_SOCKET
    #define SOCKET_ERROR_RETURN  SOCKET_ERROR
#else
    typedef int socket_t;
    #define INVALID_SOCKET_VALUE -1
    #define SOCKET_ERROR_RETURN  -1
#endif

int      sock_init(void);                    // WSAStartup on Windows
void     sock_cleanup(void);                 // WSACleanup on Windows
socket_t sock_create_tcp(void);
int      sock_bind(socket_t fd, const char *ip, int port);
int      sock_listen(socket_t fd, int backlog);
socket_t sock_accept(socket_t fd, char *client_ip, int ip_len, int *client_port);
int      sock_connect(socket_t fd, const char *host, int port, int timeout_ms);
int      sock_set_nonblock(socket_t fd);
int      sock_recv(socket_t fd, char *buf, int len);
int      sock_send(socket_t fd, const char *buf, int len);
void     sock_close(socket_t fd);
int      sock_get_error(void);              // portable errno
```

### 3.2 `http_parse.h` / `http_parse.c`

Minimal HTTP request line + header parser. Only extracts what's needed for routing.

```c
// http_parse.h

typedef struct {
    char method[16];         // GET, POST, etc.
    char path[1024];         // /foo/bar
    char host[256];          // Host header value
    int  port;               // parsed from Host (or default 80)
    int  complete;           // 1 = full headers received
    int  parse_error;        // 1 = malformed request
} HttpRequest;

// Returns bytes consumed, or 0 if incomplete, or -1 on error.
int http_parse_request(const char *raw, int len, HttpRequest *req);
```

### 3.3 `upstream.h` / `upstream.c`

Manages the backend server pool. Tracks active connections per backend.

```c
// upstream.h

typedef struct {
    char     host[NI_MAXHOST];
    int      port;
    int      weight;              // reserved for future weighted algorithms
    int      active_conns;        // current active connections
    int      fail_count;          // consecutive failures (passive detection)
    int      alive;               // 1 = healthy, 0 = dead
    uint64_t last_hc_ms;          // timestamp of last health check
} Upstream;

typedef struct {
    Upstream *items;
    int       count;
    int       capacity;
} UpstreamPool;

UpstreamPool *upstream_pool_create(int capacity);
void          upstream_pool_free(UpstreamPool *pool);
int           upstream_add(UpstreamPool *pool, const char *host, int port, int weight);
void          upstream_conn_inc(UpstreamPool *pool, int index);
void          upstream_conn_dec(UpstreamPool *pool, int index);
void          upstream_mark_alive(UpstreamPool *pool, int index);
void          upstream_mark_dead(UpstreamPool *pool, int index);
```

### 3.4 `lb.h` / `lb.c`

Load balancing — least-connections algorithm.

```c
// lb.h

// Select the alive backend with the fewest active_conns.
// Returns index into pool, or -1 if no backend is alive.
int lb_least_conn(UpstreamPool *pool);
```

### 3.5 `health_check.h` / `health_check.c`

TCP-based health probe engine.

```c
// health_check.h

typedef struct {
    UpstreamPool *pool;
    int           interval_ms;       // default 5000
    int           timeout_ms;        // default 2000
    int           passive_threshold; // consecutive failures to mark dead, default 3
    int           revive_threshold;  // consecutive success to revive, default 2
    uint64_t      last_check_ms;
} HealthChecker;

HealthChecker *hc_create(UpstreamPool *pool, int interval_ms, int timeout_ms);
void           hc_destroy(HealthChecker *hc);

// Called from the event loop when it's time to run a probe cycle.
// Returns the number of probes initiated.
int hc_tick(HealthChecker *hc, uint64_t now_ms);

// Called when a health check connection completes (success or failure).
void hc_on_probe_result(HealthChecker *hc, int upstream_index, int success, uint64_t now_ms);

// Called when a real request to a backend fails (passive detection).
void hc_on_request_failure(HealthChecker *hc, int upstream_index, uint64_t now_ms);
void hc_on_request_success(HealthChecker *hc, int upstream_index);
```

**Active probe flow:**
1. Open a TCP connection to the backend (non-blocking).
2. Wait up to `timeout_ms` for connect to complete.
3. If connected → mark success; if timeout/refused → mark failure.
4. Close the probe socket.

**Passive detection flow:**
1. Real proxy request fails (connect error, read error) → `fail_count++`.
2. `fail_count >= passive_threshold` → mark dead.
3. Real request succeeds on an alive backend → `fail_count = 0` (reset).

**Recovery flow:**
1. Dead backends still get active probes periodically.
2. On `revive_threshold` consecutive successful probes → mark alive.

### 3.6 `proxy_core.h` / `proxy_core.c`

The heart of the program — single-threaded `select()` event loop.

```c
// proxy_core.h

typedef struct {
    uint16_t       listen_port;      // proxy listen port
    UpstreamPool  *pool;
    HealthChecker *hc;
    int            running;          // 0 = stop requested
    int            total_conns;      // total active client connections
    uint64_t       total_bytes;      // total bytes forwarded
    uint64_t       start_time_ms;
    // internal: fd_sets, connection tracking, etc.
} ProxyEngine;

ProxyEngine *proxy_create(uint16_t listen_port, UpstreamPool *pool, HealthChecker *hc);
void         proxy_destroy(ProxyEngine *engine);
int          proxy_start(ProxyEngine *engine);  // enters event loop
void         proxy_stop(ProxyEngine *engine);   // signals loop to exit
```

### 3.7 `ui.h` — UI Abstraction Interface

```c
// ui.h

// Called by the core engine to update the UI.
// The GUI layer implements these.

typedef enum {
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} LogLevel;

void ui_on_log(LogLevel level, const char *fmt, ...);        // log message
void ui_on_upstream_change(int index);                       // backend status changed
void ui_on_stats(int total_conns, uint64_t total_bytes);     // periodic stats update
void ui_on_proxy_started(uint16_t port);                     // proxy running
void ui_on_proxy_stopped(void);                              // proxy stopped
void ui_on_forward_start(int client_index, int upstream_index); // new forward
void ui_on_forward_end(int client_index, int upstream_index, uint64_t bytes); // forward done
```

---

## 4. Data Structures

### 4.1 Connection State Machine

Each proxied client connection has an associated state:

```
CLIENT_ACCEPTED → UPSTREAM_CONNECTING → FORWARDING → CLOSED
                                                    ↑
                                              ERROR →┘
```

```c
// proxy_core.h (Connection states and struct)

typedef enum {
    CONN_CLIENT_ACCEPTED,       // client connected, awaiting upstream connect
    CONN_UPSTREAM_CONNECTING,   // connecting to upstream (non-blocking)
    CONN_FORWARDING,            // bidirectional forwarding active
    CONN_CLOSED                 // done, slot can be reused
} ConnState;

typedef struct {
    int       id;               // connection ID
    ConnState state;

    socket_t  client_fd;        // client side socket
    socket_t  upstream_fd;      // upstream side socket

    int       upstream_index;   // which backend this is paired with
    uint64_t  bytes_sent;       // bytes forwarded client→upstream
    uint64_t  bytes_recv;       // bytes forwarded upstream→client
    uint64_t  start_time_ms;    // when this connection started

    // Buffers for partial reads (simplified: single static buffer per direction)
    char      buf_client[4096]; // buffer for client→upstream data
    int       buf_client_len;
    char      buf_upstream[4096]; // buffer for upstream→client data
    int       buf_upstream_len;
} Connection;
```

### 4.2 ProxyEngine Internal State

```c
// proxy_core.c (internal struct definition)

struct ProxyEngine {
    uint16_t       listen_port;
    socket_t       listen_fd;
    UpstreamPool  *pool;
    HealthChecker *hc;

    Connection    *connections;
    int            max_conns;     // max concurrent connections (e.g. 64)
    int            total_conns;   // current active connections count

    uint64_t       total_bytes;
    int            running;

    // Health check probe sockets also tracked in the same fd_set
    // (simplified approach: one probe at a time per backend)

    uint64_t       hc_interval_ms;
};
```

---

## 5. Event Loop

```
while (engine->running) {
    // ── 1. Build fd_sets ──
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    max_fd = engine->listen_fd;

    FD_SET(engine->listen_fd, &read_fds);   // new connections

    for each connection in engine->connections:
        if state == CLIENT_ACCEPTED:
            FD_SET(client_fd, &read_fds)    // client sends request
        if state == UPSTREAM_CONNECTING:
            FD_SET(upstream_fd, &write_fds) // connect completion
            FD_SET(upstream_fd, &read_fds)  // connect error
        if state == FORWARDING:
            FD_SET(client_fd, &read_fds)
            FD_SET(upstream_fd, &read_fds)

    for each health check probe socket:
        FD_SET(hc_fd, &write_fds)           // connect completion
        FD_SET(hc_fd, &read_fds)            // connect error

    // ── 2. Calculate timeout ──
    timeout = min(
        HC_INTERVAL - (now - last_hc_time),
        1000  // cap at 1s to keep UI responsive
    );

    // ── 3. select() ──
    nready = select(max_fd + 1, &read_fds, &write_fds, NULL, &timeout);

    // ── 4. Handle events ──
    if (listen_fd readable) {
        accept → create Connection(CLIENT_ACCEPTED)
        lb_least_conn() → pick backend → non-blocking connect → state = UPSTREAM_CONNECTING
        ui_on_forward_start()
    }

    for each connection:
        if (client_fd readable) {
            recv → append to buf_client
            if buf not empty → send to upstream_fd
        }
        if (upstream_fd readable) {
            recv → append to buf_upstream
            if buf not empty → send to client_fd
        }
        if (upstream_fd writable && state == UPSTREAM_CONNECTING) {
            check connect result
            if ok → state = FORWARDING, passive success
            if fail → state = CLOSED, passive failure, upstream_conn_dec()
        }
        if (client_fd closed / error) → state = CLOSED, upstream_conn_dec()
        if (upstream_fd closed / error) → state = CLOSED, upstream_conn_dec()

    // ── 5. Health check tick ──
    now = get_time_ms()
    if (now - hc->last_check_ms >= hc->interval_ms) {
        hc_tick(hc, now)
    }

    // ── 6. Health check probe completion ──
    for each HC socket:
        if writable/readable → connect completed
        hc_on_probe_result(success/fail)
        ui_on_upstream_change(index)

    // ── 7. UI refresh (throttled) ──
    if (now - last_ui_update_ms >= 200) {
        ui_on_stats(engine->total_conns, engine->total_bytes)
        last_ui_update_ms = now
    }

    // ── 8. Cleanup closed connections ──
    sweep closed slots for reuse
}
```

---

## 6. Load Balancing

### Least-Connections Algorithm

```
function lb_least_conn(pool):
    best = -1
    min_conns = INF

    for i = 0 to pool.count-1:
        if pool.items[i].alive AND pool.items[i].active_conns < min_conns:
            min_conns = pool.items[i].active_conns
            best = i

    return best   // -1 means all dead -> 502
```

When a new client arrives:
1. `proxy_core` calls `lb_least_conn(pool)`.
2. Gets backend index `n`.
3. Calls `upstream_conn_inc(pool, n)`.
4. Initiates non-blocking TCP connect to `pool.items[n]`.
5. On connection close (client or upstream): `upstream_conn_dec(pool, n)`.

---

## 7. Health Check

### Timing Diagram

```
Time ──────────────────────────────────────────────>
      │      │      │      │      │      │
      HC     HC     HC     HC     HC     HC
     tick   tick   tick   tick   tick   tick

Each tick:
  For each backend (alive or dead):
    - If alive: send TCP probe; on timeout/fail → fail_count++
    - If dead:  send TCP probe; on success → success_count++
    - Skip if already probing (one outstanding probe per backend)
```

### State Transitions

```
┌──────────┐  passive_threshold failures   ┌──────────┐
│          │ ─────────────────────────────→ │          │
│  ALIVE   │                                │   DEAD   │
│          │ ←───────────────────────────── │          │
└──────────┘  revive_threshold success      └──────────┘
     ↑                                            │
     │  probe success, reset fail_count           │
     └────────────────────────────────────────────┘
                    (passive counter reset)
```

### Probe Implementation

```c
// health_check.h (HealthProbe struct) / health_check.c (probe logic)

typedef struct {
    socket_t  fd;
    int       upstream_index;
    int       in_progress;   // 1 = probe outstanding
    uint64_t  start_ms;
} HealthProbe;

// On hc_tick():
//   for each upstream that isn't currently being probed:
//     fd = socket(AF_INET, SOCK_STREAM, 0)
//     set_nonblock(fd)
//     connect(fd, &upstream_addr)
//     store probe info
//     track fd in next select()

// On select() return with HC fd writable:
//   check SO_ERROR for connection result
//   close probe socket
//   call hc_on_probe_result(index, success)
//   mark probe slot as free
```

---

## 8. GUI Layout (Win32 VS Style)

### Window Structure

```
┌──────────────────────────────────────────────────────────────┐
│  Title: "KimKong — Reverse Proxy [Running]"                │
├──────────────────────────────────────────────────────────────┤
│  [Menu Bar]                                                  │
│   File:  Start (Ctrl+R) | Stop (Ctrl+.) | Separator | Exit  │
│   Config: Backends | Health Check                            │
│   View:  Toolbar | Status Bar                                │
│   Help:  About                                               │
├──────────────────────────────────────────────────────────────┤
│  [Toolbar]                                                   │
│  ┌─────┐ ┌─────┐ ┌─────┐ ┌───────────┐ ┌──────────────┐   │
│  │  ▶  │ │  ■  │ │  ⚙  │ │  Listen:  │ │  8080  ▲  ▼  │   │
│  └─────┘ └─────┘ └─────┘ └───────────┘ └──────────────┘   │
│  [Start] [Stop] [Config]  Port spinner/edit                  │
├───────────────┬──────────────────────────────────────────────┤
│               │  [Stats Panel]                               │
│ [Backend List]│  Active Conns: 12        Total Forwarded:    │
│ (ListView)    │  2.34 MB                                     │
│               │  Uptime: 00:12:34                            │
│  ● 1.2.3.4    │                                              │
│    :8080      │  [Placeholder for future detail view]        │
│    conns: 5   │                                              │
│  ○ 1.2.3.5    │                                              │
│    :8081      │                                              │
│    conns: 7   │                                              │
│               │                                              │
├───────────────┴──────────────────────────────────────────────┤
│  [Log Output] (RichEdit, color-coded)                        │
│  12:00:01 [INFO]  Proxy started on :8080                     │
│  12:00:02 [INFO]  → 1.2.3.4:8080 (conn=5)                   │
│  12:00:05 [WARN]  HC timeout 1.2.3.5:8081, dead             │
│  12:00:07 [INFO]  ← 1.2.3.4:8080 (conn=4)                   │
│  12:00:10 [INFO]  HC success 1.2.3.5:8081, revived          │
├──────────────────────────────────────────────────────────────┤
│  [Status Bar]                                                │
│  Listen: :8080  |  Conns: 12  |  Fwd: 2.34 MB  |  502: 0   │
└──────────────────────────────────────────────────────────────┘
```

### Controls Used

| Control | Windows Class | Purpose |
|---------|---------------|---------|
| Main Window | `#32770` or custom `WC` | Frame |
| Toolbar | `ToolbarWindow32` | Start/Stop/Config |
| Backend List | `SysListView32` (Report) | Backend table |
| Status indicator | Owner-draw via `NM_CUSTOMDRAW` | Green/red dot per row |
| Log Output | `RichEdit20W` | Colored log |
| Stats | Static + `UpdateData` | Connection/bytes counters |
| Status Bar | `msctls_statusbar32` | Bottom status line |
| Timer | `SetTimer(hwnd, ID_TIMER, 200, NULL)` | UI refresh |

### Timer-Driven Refresh

- GUI does NOT poll the core engine.
- Core engine calls `ui_on_*` callbacks when events happen.
- GUI timer (200ms) is used ONLY for:
  - Flashing the "dead" indicators.
  - Updating the uptime clock.

### Backend Config Dialog

A separate modal dialog (VS-style property sheet or simple dialog):

```
┌─────────────────────────────────┐
│  Backend Configuration    [X]  │
├─────────────────────────────────┤
│  ┌───────────────────────────┐  │
│  │  Add New Backend          │  │
│  │  Host:  [______________]  │  │
│  │  Port:  [____]            │  │
│  │  Weight:[____]            │  │
│  │  [Add]                    │  │
│  └───────────────────────────┘  │
│  ┌───────────────────────────┐  │
│  │ Existing Backends:        │  │
│  │  ☑ 1.2.3.4:8080  w:1     │  │
│  │  ☑ 1.2.3.5:8081  w:1     │  │
│  │  [Remove] [Edit]          │  │
│  └───────────────────────────┘  │
│  Health Check Settings:         │
│    Interval: [5]s               │
│    Timeout:  [2]s               │
│    Fail Threshold: [3]          │
│    Revive Threshold: [2]        │
│                                 │
│  [ OK ]           [ Cancel ]    │
└─────────────────────────────────┘
```

---

## 9. Platform Abstraction Strategy

### Separation Boundary

```
                    ┌──────────────────────┐
                    │   Application Entry  │
                    │    main.c / main.m   │
                    └──────────┬───────────┘
                               │
          ┌────────────────────┼────────────────────┐
          │                    │                    │
    ┌─────┴─────┐     ┌───────┴────────┐   ┌──────┴──────┐
    │ Win32 GUI │     │  Core Engine   │   │ Cocoa GUI  │
    │ ui_win32  │     │  (pure C)      │   │ ui_cocoa   │
    └───────────┘     └───────┬────────┘   └─────────────┘
                              │
                    ┌─────────┴─────────┐
                    │  tcp_socket.h     │
                    │  #ifdef _WIN32    │
                    │  vs POSIX         │
                    └───────────────────┘
```

### What Changes Per Platform

| Component | Windows | macOS |
|-----------|---------|-------|
| Socket init | `WSAStartup` | (none) |
| Socket API | `WSASocket`/`closesocket` | `socket`/`close` |
| Socket error | `WSAGetLastError` | `errno` |
| GUI | Win32 API (`CreateWindowEx`, etc.) | Cocoa (`NSApplication`, etc.) |
| Event loop addon | (GUI messages integrated via `GetMessage` + `select()`) | `NSRunLoop` integration |
| Build | `Makefile` + MinGW/MSVC | `Makefile` + Clang |

### What Stays Identical

- `upstream.c` — pure C, no system deps
- `lb.c` — pure C, no system deps
- `health_check.c` — uses `tcp_socket.h` only
- `proxy_core.c` — uses `tcp_socket.h`, `upstream`, `lb`, `health_check`
- `http_parse.c` — pure C string handling

### Future macOS Migration Steps

1. Compile `tcp_socket_posix.c` (POSIX implementation, same header).
2. Create `ui_cocoa.m` — implement the 6 `ui_on_*` callbacks using Cocoa controls.
3. Create `main.m` — `NSApplicationMain` entry point.
4. Link with `Foundation` + `AppKit`.
5. Zero changes to `proxy_core.c`, `upstream.c`, `lb.c`, `health_check.c`.

---

## 10. File Structure

```
kimkong/
├── DESIGN.md                   # this document
├── Makefile                    # build script
│
├── src/
│   ├── main.c                  # WinMain / entry point
│   │
│   ├── tcp_socket.h            # inline cross-platform socket abstraction
│   ├── tcp_socket_win.c        # Winsock implementation
│   │
│   ├── http_parse.h
│   ├── http_parse.c
│   │
│   ├── upstream.h
│   ├── upstream.c
│   │
│   ├── lb.h
│   ├── lb.c
│   │
│   ├── health_check.h
│   ├── health_check.c
│   │
│   ├── proxy_core.h
│   ├── proxy_core.c
│   │
│   ├── ui.h                    # callback interface
│   ├── ui_win32.h
│   ├── ui_win32.c              # Win32 GUI implementation
│   │
│   └── resource.h              # resource IDs
│       resource.rc             # icon, manifest
│
└── assets/
    └── app.ico                 # application icon
```

---

## 11. Phase Plan

| Phase | Deliverable | Files | Verification |
|-------|-------------|-------|-------------|
| **1** | TCP socket abstraction + select() event loop, fixed forward to `127.0.0.1:8080` | `tcp_socket.h`, `tcp_socket_win.c`, `proxy_core.c`, `proxy_core.h`, `http_parse.c`, `http_parse.h` | curl `localhost:8888` → data forwarded to local web server |
| **2** | Upstream pool + least-connections LB | `upstream.c`, `upstream.h`, `lb.c`, `lb.h` | Multiple backend processes; verify connections distributed to least-loaded one |
| **3** | TCP health check (active + passive) | `health_check.c`, `health_check.h` | Kill a backend → it's marked dead; restart → revived |
| **4** | Win32 VS-style GUI | `main.c`, `ui_win32.c`, `ui_win32.h`, `resource.*` | Full UI: start/stop, backend list, stats, log |
| **5** | macOS Cocoa GUI (future) | `main.m`, `ui_cocoa.m`, `tcp_socket_posix.c` | Same app, native macOS look |
