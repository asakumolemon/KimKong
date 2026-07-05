#include "lb.h"
#include <limits.h>

int lb_least_conn(UpstreamPool *pool)
{
    if (!pool || pool->count == 0) return -1;

    int best = -1;
    int min_conns = INT_MAX;
    static int rr = 0;

    for (int i = 0; i < pool->count; i++) {
        if (!pool->items[i].alive) continue;
        if (pool->items[i].active_conns < min_conns) {
            min_conns = pool->items[i].active_conns;
            best = i;
        }
    }
    if (best < 0) return -1;

    int candidates[256];
    int n = 0;
    for (int i = 0; i < pool->count; i++) {
        if (pool->items[i].alive && pool->items[i].active_conns == min_conns)
            candidates[n++] = i;
    }
    if (n == 1) return candidates[0];

    int pick = candidates[rr % n];
    rr = (rr + 1) % n;
    return pick;
}
