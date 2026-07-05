#ifndef HEALTH_CHECK_H
#define HEALTH_CHECK_H

#include "upstream.h"
#include "tcp_socket.h"
#include <stdint.h>

typedef struct {
    socket_t  fd;
    int       upstream_index;
    int       in_progress;
    uint64_t  start_ms;
} HealthProbe;

typedef struct {
    UpstreamPool *pool;
    int           interval_ms;
    int           timeout_ms;
    int           passive_threshold;
    int           revive_threshold;
    uint64_t      last_check_ms;
    HealthProbe  *probes;
    int           max_probes;
} HealthChecker;

HealthChecker *hc_create(UpstreamPool *pool, int interval_ms, int timeout_ms);
void           hc_destroy(HealthChecker *hc);
int            hc_tick(HealthChecker *hc, uint64_t now_ms);
void           hc_on_probe_result(HealthChecker *hc, int upstream_index, int success, uint64_t now_ms);
void           hc_on_request_failure(HealthChecker *hc, int upstream_index, uint64_t now_ms);
void           hc_on_request_success(HealthChecker *hc, int upstream_index);

#endif
