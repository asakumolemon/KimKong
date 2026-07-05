#include "proxy_core.h"
#include "lb.h"
#include "ui.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_CONNS 64

static Connection *find_free_slot(ProxyEngine *engine)
{
    for (int i = 0; i < engine->max_conns; i++)
        if (engine->connections[i].state == CONN_CLOSED)
            return &engine->connections[i];
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

    uint64_t total = c->bytes_sent + c->bytes_recv;
    engine->total_bytes += total;
    engine->total_conns--;

    ui_on_forward_end(c->id, c->upstream_index, total);

    if (c->upstream_index >= 0)
        upstream_conn_dec(engine->pool, c->upstream_index);

    c->state = CONN_CLOSED;
    c->client_fd = INVALID_SOCKET_VALUE;
    c->upstream_fd = INVALID_SOCKET_VALUE;
    c->id = 0;
    c->bytes_sent = 0;
    c->bytes_recv = 0;
    c->buf_client_len = 0;
    c->buf_upstream_len = 0;
}

ProxyEngine *proxy_create(uint16_t listen_port, UpstreamPool *pool, HealthChecker *hc)
{
    ProxyEngine *engine = (ProxyEngine *)calloc(1, sizeof(ProxyEngine));
    if (!engine) return NULL;

    engine->listen_port = listen_port;
    engine->pool = pool;
    engine->hc = hc;
    engine->listen_fd = INVALID_SOCKET_VALUE;
    engine->max_conns = MAX_CONNS;
    engine->next_id = 1;

    engine->connections = (Connection *)calloc((size_t)MAX_CONNS, sizeof(Connection));
    if (!engine->connections) {
        free(engine);
        return NULL;
    }

    for (int i = 0; i < MAX_CONNS; i++)
        engine->connections[i].state = CONN_CLOSED;

    return engine;
}

void proxy_destroy(ProxyEngine *engine)
{
    if (!engine) return;
    free(engine->connections);
    free(engine);
}

int proxy_start(ProxyEngine *engine)
{
    if (!engine || engine->running) return -1;

    engine->listen_fd = sock_create_tcp();
    if (engine->listen_fd == INVALID_SOCKET_VALUE) return -1;

    int opt = 1;
    setsockopt(engine->listen_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&opt, sizeof(opt));

    if (sock_bind(engine->listen_fd, "0.0.0.0", engine->listen_port) != 0) {
        sock_close(engine->listen_fd);
        engine->listen_fd = INVALID_SOCKET_VALUE;
        return -1;
    }
    if (sock_listen(engine->listen_fd, 10) != 0) {
        sock_close(engine->listen_fd);
        engine->listen_fd = INVALID_SOCKET_VALUE;
        return -1;
    }
    sock_set_nonblock(engine->listen_fd);

    engine->running = 1;
    engine->total_conns = 0;
    engine->total_bytes = 0;
    ui_on_proxy_started(engine->listen_port);

    uint64_t last_ui_update_ms = 0;

    while (engine->running) {
        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        socket_t maxfd = engine->listen_fd;
        FD_SET(engine->listen_fd, &rfds);

        for (int i = 0; i < engine->max_conns; i++) {
            Connection *c = &engine->connections[i];
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

        for (int i = 0; engine->hc && i < engine->hc->max_probes; i++) {
            if (engine->hc->probes[i].in_progress) {
                FD_SET(engine->hc->probes[i].fd, &wfds);
                FD_SET(engine->hc->probes[i].fd, &rfds);
                if (engine->hc->probes[i].fd > maxfd)
                    maxfd = engine->hc->probes[i].fd;
            }
        }

        struct timeval tv = { 1, 0 };
        int nready = select((int)maxfd + 1, &rfds, &wfds, NULL, &tv);
        if (nready < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            break;
        }

        uint64_t now_ms = GetTickCount64();

        if (FD_ISSET(engine->listen_fd, &rfds)) {
            char ip[64] = {0};
            int port = 0;
            socket_t cfd = sock_accept(engine->listen_fd, ip, sizeof(ip), &port);
            if (cfd != INVALID_SOCKET_VALUE) {
                Connection *c = find_free_slot(engine);
                if (c) {
                    sock_set_nonblock(cfd);
                    c->id = engine->next_id++;
                    c->state = CONN_CLIENT_ACCEPTED;
                    c->client_fd = cfd;
                    c->upstream_fd = INVALID_SOCKET_VALUE;
                    c->start_time_ms = now_ms;

                    int up_idx = -1;
                    if (engine->pool)
                        up_idx = lb_least_conn(engine->pool);

                    c->upstream_index = up_idx;

                    if (up_idx < 0) {
                        ui_on_log(LOG_WARN, "No alive upstream, rejecting");
                        sock_close(cfd);
                        c->state = CONN_CLOSED;
                        continue;
                    }

                    engine->total_conns++;
                    upstream_conn_inc(engine->pool, up_idx);

                    socket_t ufd = sock_create_tcp();
                    if (ufd == INVALID_SOCKET_VALUE) {
                        upstream_conn_dec(engine->pool, up_idx);
                        close_connection(engine, c);
                        continue;
                    }
                    sock_set_nonblock(ufd);

                    struct sockaddr_in uaddr;
                    memset(&uaddr, 0, sizeof(uaddr));
                    uaddr.sin_family = AF_INET;
                    uaddr.sin_port = htons((unsigned short)engine->pool->items[up_idx].port);
                    inet_pton(AF_INET, engine->pool->items[up_idx].host, &uaddr.sin_addr);

                    int ret = connect(ufd, (struct sockaddr *)&uaddr, sizeof(uaddr));
                    if (ret == 0) {
                        c->upstream_fd = ufd;
                        c->state = CONN_FORWARDING;
                        ui_on_forward_start(c->id, up_idx);
                    } else {
#ifdef _WIN32
                        int err = WSAGetLastError();
                        if (err == WSAEWOULDBLOCK) {
                            c->upstream_fd = ufd;
                            c->state = CONN_UPSTREAM_CONNECTING;
                            ui_on_forward_start(c->id, up_idx);
                        } else {
                            ui_on_log(LOG_WARN, "Upstream %s:%d connect error",
                                      engine->pool->items[up_idx].host,
                                      engine->pool->items[up_idx].port);
                            if (engine->hc)
                                hc_on_request_failure(engine->hc, up_idx, now_ms);
                            sock_close(ufd);
                            close_connection(engine, c);
                        }
#else
                        if (errno == EINPROGRESS) {
                            c->upstream_fd = ufd;
                            c->state = CONN_UPSTREAM_CONNECTING;
                            ui_on_forward_start(c->id, up_idx);
                        } else {
                            if (engine->hc)
                                hc_on_request_failure(engine->hc, up_idx, now_ms);
                            sock_close(ufd);
                            close_connection(engine, c);
                        }
#endif
                    }
                } else {
                    sock_close(cfd);
                }
            }
        }

        for (int i = 0; i < engine->max_conns; i++) {
            Connection *c = &engine->connections[i];
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
                    if (engine->hc)
                        hc_on_request_success(engine->hc, c->upstream_index);
                } else {
                    if (engine->hc)
                        hc_on_request_failure(engine->hc, c->upstream_index, now_ms);
                    close_connection(engine, c);
                }
                continue;
            }

            if (c->state != CONN_FORWARDING) continue;

            if (FD_ISSET(c->client_fd, &rfds)) {
                char buf[4096];
                int n = sock_recv(c->client_fd, buf, sizeof(buf));
                if (n > 0) {
                    int s = sock_send(c->upstream_fd, buf, n);
                    if (s > 0) {
                        c->bytes_sent += s;
                        engine->total_bytes += s;
                    }
                    if (s < 0) { close_connection(engine, c); continue; }
                } else if (n == 0) {
                    close_connection(engine, c); continue;
                } else {
#ifdef _WIN32
                    if (WSAGetLastError() != WSAEWOULDBLOCK) {
                        close_connection(engine, c); continue;
                    }
#else
                    if (errno != EAGAIN) { close_connection(engine, c); continue; }
#endif
                }
            }

            if (FD_ISSET(c->upstream_fd, &rfds)) {
                char buf[4096];
                int n = sock_recv(c->upstream_fd, buf, sizeof(buf));
                if (n > 0) {
                    int s = sock_send(c->client_fd, buf, n);
                    if (s > 0) {
                        c->bytes_recv += s;
                        engine->total_bytes += s;
                    }
                    if (s < 0) { close_connection(engine, c); continue; }
                } else if (n == 0) {
                    close_connection(engine, c); continue;
                } else {
#ifdef _WIN32
                    if (WSAGetLastError() != WSAEWOULDBLOCK) {
                        close_connection(engine, c); continue;
                    }
#else
                    if (errno != EAGAIN) { close_connection(engine, c); continue; }
#endif
                }
            }
        }

        if (engine->hc) {
            for (int i = 0; i < engine->hc->max_probes; i++) {
                HealthProbe *p = &engine->hc->probes[i];
                if (!p->in_progress) continue;

                if (FD_ISSET(p->fd, &wfds) || FD_ISSET(p->fd, &rfds)) {
                    int err = 0;
#ifdef _WIN32
                    int len = sizeof(err);
                    getsockopt(p->fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
#else
                    socklen_t len = sizeof(err);
                    getsockopt(p->fd, SOL_SOCKET, SO_ERROR, &err, &len);
#endif
                    hc_on_probe_result(engine->hc, p->upstream_index, err == 0, now_ms);
                    if (err == 0) {
                        int idx = p->upstream_index;
                        if (idx >= 0 && idx < engine->pool->count) {
                            ui_on_log(LOG_INFO, "HC success %s:%d, revived",
                                      engine->pool->items[idx].host,
                                      engine->pool->items[idx].port);
                        }
                    } else {
                        int idx = p->upstream_index;
                        if (idx >= 0 && idx < engine->pool->count) {
                            ui_on_log(LOG_WARN, "HC timeout %s:%d, dead",
                                      engine->pool->items[idx].host,
                                      engine->pool->items[idx].port);
                        }
                    }
                    ui_on_upstream_change(p->upstream_index);
                    sock_close(p->fd);
                    p->fd = INVALID_SOCKET_VALUE;
                    p->in_progress = 0;
                } else if (now_ms - p->start_ms > (uint64_t)engine->hc->timeout_ms) {
                    hc_on_probe_result(engine->hc, p->upstream_index, 0, now_ms);
                    int idx = p->upstream_index;
                    if (idx >= 0 && idx < engine->pool->count) {
                        ui_on_log(LOG_WARN, "HC timeout %s:%d, dead",
                                  engine->pool->items[idx].host,
                                  engine->pool->items[idx].port);
                    }
                    ui_on_upstream_change(p->upstream_index);
                    sock_close(p->fd);
                    p->fd = INVALID_SOCKET_VALUE;
                    p->in_progress = 0;
                }
            }

            if (now_ms - engine->hc->last_check_ms >= (uint64_t)engine->hc->interval_ms) {
                hc_tick(engine->hc, now_ms);
            }
        }

        if (now_ms - last_ui_update_ms >= 200) {
            ui_on_stats(engine->total_conns, engine->total_bytes);
            last_ui_update_ms = now_ms;
        }
    }

    sock_close(engine->listen_fd);
    engine->listen_fd = INVALID_SOCKET_VALUE;

    for (int i = 0; i < engine->max_conns; i++) {
        if (engine->connections[i].state != CONN_CLOSED)
            close_connection(engine, &engine->connections[i]);
    }

    ui_on_proxy_stopped();
    return 0;
}

void proxy_stop(ProxyEngine *engine)
{
    if (engine) engine->running = 0;
}
