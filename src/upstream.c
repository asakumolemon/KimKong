#include "upstream.h"
#include <stdlib.h>
#include <string.h>

UpstreamPool *upstream_pool_create(int capacity)
{
    UpstreamPool *pool = (UpstreamPool *)calloc(1, sizeof(UpstreamPool));
    if (!pool) return NULL;
    pool->items = (Upstream *)calloc((size_t)capacity, sizeof(Upstream));
    if (!pool->items) {
        free(pool);
        return NULL;
    }
    pool->count = 0;
    pool->capacity = capacity;
    return pool;
}

void upstream_pool_free(UpstreamPool *pool)
{
    if (!pool) return;
    free(pool->items);
    free(pool);
}

int upstream_add(UpstreamPool *pool, const char *host, int port, int weight)
{
    if (!pool || pool->count >= pool->capacity) return -1;
    Upstream *u = &pool->items[pool->count];
    strncpy(u->host, host, sizeof(u->host) - 1);
    u->host[sizeof(u->host) - 1] = '\0';
    u->port = port;
    u->weight = weight > 0 ? weight : 1;
    u->active_conns = 0;
    u->fail_count = 0;
    u->alive = 1;
    u->last_hc_ms = 0;
    return pool->count++;
}

void upstream_conn_inc(UpstreamPool *pool, int index)
{
    if (!pool || index < 0 || index >= pool->count) return;
    pool->items[index].active_conns++;
}

void upstream_conn_dec(UpstreamPool *pool, int index)
{
    if (!pool || index < 0 || index >= pool->count) return;
    if (pool->items[index].active_conns > 0)
        pool->items[index].active_conns--;
}

void upstream_mark_alive(UpstreamPool *pool, int index)
{
    if (!pool || index < 0 || index >= pool->count) return;
    pool->items[index].alive = 1;
    pool->items[index].fail_count = 0;
}

void upstream_mark_dead(UpstreamPool *pool, int index)
{
    if (!pool || index < 0 || index >= pool->count) return;
    pool->items[index].alive = 0;
}
