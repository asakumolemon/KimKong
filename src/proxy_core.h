#ifndef PROXY_CORE_H
#define PROXY_CORE_H

#include <stdint.h>
#include "tcp_socket.h"
#include "upstream.h"
#include "health_check.h"

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
    int       upstream_index;
    uint64_t  bytes_sent;
    uint64_t  bytes_recv;
    uint64_t  start_time_ms;
    char      buf_client[4096];
    int       buf_client_len;
    char      buf_upstream[4096];
    int       buf_upstream_len;
} Connection;

typedef struct {
    uint16_t       listen_port;
    socket_t       listen_fd;
    UpstreamPool  *pool;
    HealthChecker *hc;
    Connection    *connections;
    int            max_conns;
    int            total_conns;
    int            next_id;
    uint64_t       total_bytes;
    uint64_t       start_time_ms;
    int            running;
} ProxyEngine;

ProxyEngine *proxy_create(uint16_t listen_port, UpstreamPool *pool, HealthChecker *hc);
void         proxy_destroy(ProxyEngine *engine);
int          proxy_start(ProxyEngine *engine);
void         proxy_stop(ProxyEngine *engine);

#endif
