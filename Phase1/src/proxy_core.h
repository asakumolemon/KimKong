#ifndef PROXY_CORE_H
#define PROXY_CORE_H

#include <stdint.h>

typedef struct ProxyEngine ProxyEngine;

ProxyEngine *proxy_create(uint16_t listen_port, const char *target_host, int target_port);
void         proxy_destroy(ProxyEngine *engine);
int          proxy_start(ProxyEngine *engine);
void         proxy_stop(ProxyEngine *engine);

#endif /* PROXY_CORE_H */
