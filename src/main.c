#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "tcp_socket.h"
#include "upstream.h"
#include "health_check.h"
#include "proxy_core.h"
#include "ui_win32.h"
#include "resource.h"

static void add_default_backends(UpstreamPool *pool)
{
    upstream_add(pool, "127.0.0.1", 8080, 1);
    upstream_add(pool, "127.0.0.1", 8081, 1);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;

    if (sock_init() != 0) {
        MessageBoxA(NULL, "Failed to initialize Winsock",
                    "Error", MB_ICONERROR);
        return 1;
    }

    UpstreamPool *pool = upstream_pool_create(16);
    if (!pool) {
        sock_cleanup();
        return 1;
    }
    add_default_backends(pool);

    HealthChecker *hc = hc_create(pool, 5000, 2000);
    if (!hc) {
        upstream_pool_free(pool);
        sock_cleanup();
        return 1;
    }

    ProxyEngine *engine = proxy_create(8888, pool, hc);
    if (!engine) {
        hc_destroy(hc);
        upstream_pool_free(pool);
        sock_cleanup();
        return 1;
    }

    ui_win32_set_engine(engine);
    ui_win32_set_pool(pool);
    ui_win32_set_hc(hc);

    HWND hwnd = ui_win32_create(hInstance, nCmdShow);
    if (!hwnd) {
        proxy_destroy(engine);
        hc_destroy(hc);
        upstream_pool_free(pool);
        sock_cleanup();
        return 1;
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (engine) proxy_stop(engine);

    Sleep(100);

    proxy_destroy(engine);
    hc_destroy(hc);
    upstream_pool_free(pool);
    sock_cleanup();

    return 0;
}
