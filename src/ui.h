#ifndef UI_H
#define UI_H

#include <stdint.h>
#include <stdarg.h>

typedef enum {
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} LogLevel;

void ui_on_log(LogLevel level, const char *fmt, ...);
void ui_on_upstream_change(int index);
void ui_on_stats(int total_conns, uint64_t total_bytes);
void ui_on_proxy_started(uint16_t port);
void ui_on_proxy_stopped(void);
void ui_on_forward_start(int client_index, int upstream_index);
void ui_on_forward_end(int client_index, int upstream_index, uint64_t bytes);

#endif
