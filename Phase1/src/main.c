#include "tcp_socket.h"
#include "proxy_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

static ProxyEngine *g_engine = NULL;

#ifdef _WIN32
static BOOL WINAPI ctrl_handler(DWORD dwCtrlType)
{
    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT) {
        printf("\nShutting down...\n");
        if (g_engine) proxy_stop(g_engine);
        return TRUE;
    }
    return FALSE;
}
#else
static void sigint_handler(int sig)
{
    (void)sig;
    printf("\nShutting down...\n");
    if (g_engine) proxy_stop(g_engine);
}
#endif

int main(void)
{
    uint16_t listen_port = 8888;
    const char *target_host = "127.0.0.1";
    int target_port = 8080;

    if (sock_init() != 0) {
        printf("Failed to initialize sockets\n");
        return 1;
    }

#ifdef _WIN32
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
#endif

    g_engine = proxy_create(listen_port, target_host, target_port);
    if (!g_engine) {
        printf("Failed to create proxy engine\n");
        sock_cleanup();
        return 1;
    }

    int ret = proxy_start(g_engine);
    proxy_destroy(g_engine);
    g_engine = NULL;

    sock_cleanup();
    return ret;
}
