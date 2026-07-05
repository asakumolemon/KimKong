#ifndef UI_WIN32_H
#define UI_WIN32_H

#include <windows.h>

HWND ui_win32_create(HINSTANCE hInstance, int nCmdShow);
void ui_win32_set_engine(void *engine);
void ui_win32_set_pool(void *pool);
void ui_win32_set_hc(void *hc);

#endif
