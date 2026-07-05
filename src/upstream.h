#ifndef UPSTREAM_H
#define UPSTREAM_H

#include <stdint.h>
#include <ws2tcpip.h>

typedef struct {
    char     host[NI_MAXHOST];
    int      port;
    int      weight;
    int      active_conns;
    int      fail_count;
    int      alive;
    uint64_t last_hc_ms;
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

#endif
