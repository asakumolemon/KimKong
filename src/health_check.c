#include "health_check.h"
#include <stdlib.h>

HealthChecker *hc_create(UpstreamPool *pool, int interval_ms, int timeout_ms)
{
    HealthChecker *hc = (HealthChecker *)calloc(1, sizeof(HealthChecker));
    if (!hc) return NULL;
    hc->pool = pool;
    hc->interval_ms = interval_ms > 0 ? interval_ms : 5000;
    hc->timeout_ms = timeout_ms > 0 ? timeout_ms : 2000;
    hc->passive_threshold = 3;
    hc->revive_threshold = 2;
    hc->last_check_ms = 0;
    hc->max_probes = pool ? pool->capacity : 16;
    hc->probes = (HealthProbe *)calloc((size_t)hc->max_probes, sizeof(HealthProbe));
    if (!hc->probes) {
        free(hc);
        return NULL;
    }
    for (int i = 0; i < hc->max_probes; i++)
        hc->probes[i].fd = INVALID_SOCKET_VALUE;
    return hc;
}

void hc_destroy(HealthChecker *hc)
{
    if (!hc) return;
    for (int i = 0; i < hc->max_probes; i++) {
        if (hc->probes[i].fd != INVALID_SOCKET_VALUE) {
            sock_close(hc->probes[i].fd);
        }
    }
    free(hc->probes);
    free(hc);
}

int hc_tick(HealthChecker *hc, uint64_t now_ms)
{
    if (!hc || !hc->pool) return 0;
    int initiated = 0;

    for (int i = 0; i < hc->pool->count; i++) {
        int already_probing = 0;
        for (int j = 0; j < hc->max_probes; j++) {
            if (hc->probes[j].in_progress && hc->probes[j].upstream_index == i) {
                already_probing = 1;
                break;
            }
        }
        if (already_probing) continue;

        if (now_ms - hc->pool->items[i].last_hc_ms < (uint64_t)hc->interval_ms)
            continue;

        socket_t fd = sock_create_tcp();
        if (fd == INVALID_SOCKET_VALUE) continue;
        sock_set_nonblock(fd);

        int ret = sock_connect(fd, hc->pool->items[i].host,
                               hc->pool->items[i].port, 0);
        if (ret == 0) {
            hc_on_probe_result(hc, i, 1, now_ms);
            sock_close(fd);
            continue;
        }

        int slot = -1;
        for (int j = 0; j < hc->max_probes; j++) {
            if (!hc->probes[j].in_progress) {
                slot = j;
                break;
            }
        }
        if (slot < 0) {
            sock_close(fd);
            continue;
        }

        hc->probes[slot].fd = fd;
        hc->probes[slot].upstream_index = i;
        hc->probes[slot].in_progress = 1;
        hc->probes[slot].start_ms = now_ms;
        hc->pool->items[i].last_hc_ms = now_ms;
        initiated++;
    }

    hc->last_check_ms = now_ms;
    return initiated;
}

void hc_on_probe_result(HealthChecker *hc, int upstream_index, int success, uint64_t now_ms)
{
    if (!hc || !hc->pool || upstream_index < 0 || upstream_index >= hc->pool->count)
        return;
    Upstream *u = &hc->pool->items[upstream_index];

    if (success) {
        if (!u->alive) {
            u->fail_count++;
            if (u->fail_count >= hc->revive_threshold) {
                upstream_mark_alive(hc->pool, upstream_index);
                u->fail_count = 0;
            }
        } else {
            u->fail_count = 0;
        }
    } else {
        u->fail_count++;
        if (u->alive && u->fail_count >= hc->passive_threshold) {
            upstream_mark_dead(hc->pool, upstream_index);
        }
    }
    u->last_hc_ms = now_ms;
}

void hc_on_request_failure(HealthChecker *hc, int upstream_index, uint64_t now_ms)
{
    (void)now_ms;
    if (!hc || !hc->pool || upstream_index < 0 || upstream_index >= hc->pool->count)
        return;
    Upstream *u = &hc->pool->items[upstream_index];
    u->fail_count++;
    if (u->alive && u->fail_count >= hc->passive_threshold)
        upstream_mark_dead(hc->pool, upstream_index);
}

void hc_on_request_success(HealthChecker *hc, int upstream_index)
{
    if (!hc || !hc->pool || upstream_index < 0 || upstream_index >= hc->pool->count)
        return;
    hc->pool->items[upstream_index].fail_count = 0;
}
