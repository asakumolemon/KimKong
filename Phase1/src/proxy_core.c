#include "proxy_core.h"
#include "tcp_socket.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_CONNS  64
#define BUF_SIZE  4096

typedef enum {
    CONN_CLIENT_ACCEPTED,
    CONN_UPSTREAM_CONNECTING,
    CONN_FORWARDING,
    CONN_CLOSED
} ConnState;

typedef struct {
    int       id;
    ConnState state;
    socket_t  client_fd;
    socket_t  upstream_fd;
    uint64_t  bytes_sent;
    uint64_t  bytes_recv;
    char      buf_client[BUF_SIZE];
    int       buf_client_len;
    char      buf_upstream[BUF_SIZE];
    int       buf_upstream_len;
} Connection;

struct ProxyEngine {
    uint16_t    listen_port;
    char        target_host[256];
    int         target_port;
    socket_t    listen_fd;
    Connection  conns[MAX_CONNS];
    int         total_conns;
    uint64_t    total_bytes;
    int         running;
    int         next_id;
};

ProxyEngine *proxy_create(uint16_t listen_port, const char *target_host, int target_port)
{
    ProxyEngine *engine = (ProxyEngine *)calloc(1, sizeof(ProxyEngine));
    if (!engine) return NULL;

    engine->listen_port = listen_port;
    engine->target_port = target_port;
    snprintf(engine->target_host, sizeof(engine->target_host), "%s", target_host);
    engine->listen_fd = INVALID_SOCKET_VALUE;
    engine->next_id = 1;

    return engine;
}

void proxy_destroy(ProxyEngine *engine)
{
    if (!engine) return;
    if (engine->listen_fd != INVALID_SOCKET_VALUE)
        sock_close(engine->listen_fd);
    for (int i = 0; i < MAX_CONNS; i++) {
        if (engine->conns[i].state != CONN_CLOSED) {
            if (engine->conns[i].client_fd != INVALID_SOCKET_VALUE)
                sock_close(engine->conns[i].client_fd);
            if (engine->conns[i].upstream_fd != INVALID_SOCKET_VALUE)
                sock_close(engine->conns[i].upstream_fd);
        }
    }
    free(engine);
}

static Connection *find_free_slot(ProxyEngine *engine)
{
    for (int i = 0; i < MAX_CONNS; i++)
        if (engine->conns[i].state == CONN_CLOSED)
            return &engine->conns[i];
    return NULL;
}

static void close_connection(ProxyEngine *engine, Connection *c)
{
    if (c->client_fd != INVALID_SOCKET_VALUE) {
        sock_close(c->client_fd);
        c->client_fd = INVALID_SOCKET_VALUE;
    }
    if (c->upstream_fd != INVALID_SOCKET_VALUE) {
        sock_close(c->upstream_fd);
        c->upstream_fd = INVALID_SOCKET_VALUE;
    }
    printf("[%d] closed, sent=%llu recv=%llu\n",
           c->id,
           (unsigned long long)c->bytes_sent,
           (unsigned long long)c->bytes_recv);
    engine->total_bytes += c->bytes_sent + c->bytes_recv;
    engine->total_conns--;
    memset(c, 0, sizeof(Connection));
}

int proxy_start(ProxyEngine *engine)
{
    if (engine->running) return -1;

    engine->listen_fd = sock_create_tcp();
    if (engine->listen_fd == INVALID_SOCKET_VALUE) {
        printf("Failed to create listen socket\n"); return -1;
    }
    int opt = 1;
    setsockopt(engine->listen_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&opt, sizeof(opt));
    if (sock_bind(engine->listen_fd, "0.0.0.0", engine->listen_port) != 0) {
        printf("Failed to bind :%d\n", engine->listen_port);
        sock_close(engine->listen_fd);
        engine->listen_fd = INVALID_SOCKET_VALUE;
        return -1;
    }
    if (sock_listen(engine->listen_fd, 10) != 0) {
        printf("Failed to listen\n");
        sock_close(engine->listen_fd);
        engine->listen_fd = INVALID_SOCKET_VALUE;
        return -1;
    }
    sock_set_nonblock(engine->listen_fd);

    engine->running = 1;
    printf("Proxy listening on :%d, forwarding to %s:%d\n",
           engine->listen_port, engine->target_host, engine->target_port);

    while (engine->running) {
        fd_set rfds, wfds;
        FD_ZERO(&rfds); FD_ZERO(&wfds);
        socket_t maxfd = engine->listen_fd;
        FD_SET(engine->listen_fd, &rfds);

        for (int i = 0; i < MAX_CONNS; i++) {
            Connection *c = &engine->conns[i];
            if (c->state == CONN_CLOSED) continue;
            if (c->state == CONN_CLIENT_ACCEPTED) {
                FD_SET(c->client_fd, &rfds);
                if (c->client_fd > maxfd) maxfd = c->client_fd;
            }
            if (c->state == CONN_UPSTREAM_CONNECTING) {
                FD_SET(c->upstream_fd, &wfds);
                FD_SET(c->upstream_fd, &rfds);
                if (c->upstream_fd > maxfd) maxfd = c->upstream_fd;
            }
            if (c->state == CONN_FORWARDING) {
                FD_SET(c->client_fd, &rfds);
                FD_SET(c->upstream_fd, &rfds);
                if (c->client_fd > maxfd) maxfd = c->client_fd;
                if (c->upstream_fd > maxfd) maxfd = c->upstream_fd;
            }
        }

        struct timeval tv = {1, 0};
        int nready = select((int)maxfd + 1, &rfds, &wfds, NULL, &tv);
        if (nready < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            printf("select() error\n"); break;
        }

        if (FD_ISSET(engine->listen_fd, &rfds)) {
            char ip[64] = {0};
            int  port = 0;
            socket_t cfd = sock_accept(engine->listen_fd, ip, sizeof(ip), &port);
            if (cfd != INVALID_SOCKET_VALUE) {
                Connection *c = find_free_slot(engine);
                if (c) {
                    sock_set_nonblock(cfd);
                    c->id = engine->next_id++;
                    c->state = CONN_CLIENT_ACCEPTED;
                    c->client_fd = cfd;
                    c->upstream_fd = INVALID_SOCKET_VALUE;
                    engine->total_conns++;
                    printf("[%d] accept %s:%d\n", c->id, ip, port);

                    socket_t ufd = sock_create_tcp();
                    if (ufd == INVALID_SOCKET_VALUE) {
                        close_connection(engine, c); continue;
                    }
                    sock_set_nonblock(ufd);
                    struct sockaddr_in uaddr;
                    memset(&uaddr, 0, sizeof(uaddr));
                    uaddr.sin_family = AF_INET;
                    uaddr.sin_port = htons((unsigned short)engine->target_port);
#ifdef _WIN32
                    inet_pton(AF_INET, engine->target_host, &uaddr.sin_addr);
#else
                    inet_pton(AF_INET, engine->target_host, &uaddr.sin_addr);
#endif
                    int ret = connect(ufd, (struct sockaddr *)&uaddr, sizeof(uaddr));
                    if (ret == 0) {
                        c->upstream_fd = ufd;
                        c->state = CONN_FORWARDING;
                        printf("[%d] upstream connected immediately\n", c->id);
                    } else {
#ifdef _WIN32
                        int err = WSAGetLastError();
                        if (err == WSAEWOULDBLOCK) {
                            c->upstream_fd = ufd;
                            c->state = CONN_UPSTREAM_CONNECTING;
                            printf("[%d] connecting upstream %s:%d\n",
                                   c->id, engine->target_host, engine->target_port);
                        } else {
                            printf("[%d] upstream connect err %d\n", c->id, err);
                            sock_close(ufd); close_connection(engine, c);
                        }
#else
                        if (errno == EINPROGRESS) {
                            c->upstream_fd = ufd;
                            c->state = CONN_UPSTREAM_CONNECTING;
                            printf("[%d] connecting upstream %s:%d\n",
                                   c->id, engine->target_host, engine->target_port);
                        } else {
                            printf("[%d] upstream connect err %d\n", c->id, errno);
                            sock_close(ufd); close_connection(engine, c);
                        }
#endif
                    }
                } else {
                    printf("max conns, reject\n");
                    sock_close(cfd);
                }
            }
        }

        for (int i = 0; i < MAX_CONNS; i++) {
            Connection *c = &engine->conns[i];
            if (c->state == CONN_CLOSED) continue;

            if (c->state == CONN_UPSTREAM_CONNECTING &&
                FD_ISSET(c->upstream_fd, &wfds))
            {
                int err = 0;
#ifdef _WIN32
                int len = sizeof(err);
                getsockopt(c->upstream_fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
#else
                socklen_t len = sizeof(err);
                getsockopt(c->upstream_fd, SOL_SOCKET, SO_ERROR, &err, &len);
#endif
                if (err == 0) {
                    c->state = CONN_FORWARDING;
                    printf("[%d] upstream connected\n", c->id);
                } else {
                    printf("[%d] upstream connect err %d\n", c->id, err);
                    close_connection(engine, c);
                }
                continue;
            }

            if (c->state != CONN_FORWARDING) continue;

            if (FD_ISSET(c->client_fd, &rfds)) {
                char buf[BUF_SIZE];
                int n = sock_recv(c->client_fd, buf, BUF_SIZE);
                if (n > 0) {
                    int s = sock_send(c->upstream_fd, buf, n);
                    if (s > 0) { c->bytes_sent += s; engine->total_bytes += s; }
                    if (s < 0) { close_connection(engine, c); continue; }
                } else if (n == 0) {
                    printf("[%d] client closed\n", c->id);
                    close_connection(engine, c); continue;
                } else {
#ifdef _WIN32
                    if (WSAGetLastError() != WSAEWOULDBLOCK) {
                        close_connection(engine, c); continue;
                    }
#else
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        close_connection(engine, c); continue;
                    }
#endif
                }
            }

            if (FD_ISSET(c->upstream_fd, &rfds)) {
                char buf[BUF_SIZE];
                int n = sock_recv(c->upstream_fd, buf, BUF_SIZE);
                if (n > 0) {
                    int s = sock_send(c->client_fd, buf, n);
                    if (s > 0) { c->bytes_recv += s; engine->total_bytes += s; }
                    if (s < 0) { close_connection(engine, c); continue; }
                } else if (n == 0) {
                    printf("[%d] upstream closed\n", c->id);
                    close_connection(engine, c); continue;
                } else {
#ifdef _WIN32
                    if (WSAGetLastError() != WSAEWOULDBLOCK) {
                        close_connection(engine, c); continue;
                    }
#else
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        close_connection(engine, c); continue;
                    }
#endif
                }
            }
        }
    }

    sock_close(engine->listen_fd);
    engine->listen_fd = INVALID_SOCKET_VALUE;
    for (int i = 0; i < MAX_CONNS; i++)
        if (engine->conns[i].state != CONN_CLOSED)
            close_connection(engine, &engine->conns[i]);
    printf("Proxy stopped. total bytes: %llu\n",
           (unsigned long long)engine->total_bytes);
    return 0;
}

void proxy_stop(ProxyEngine *engine)
{
    if (engine) engine->running = 0;
}
