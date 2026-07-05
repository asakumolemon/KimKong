#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <stdio.h>
#include <stdarg.h>
#include "resource.h"
#include "ui.h"
#include "proxy_core.h"
#include "upstream.h"
#include "health_check.h"
#include "lb.h"

#pragma comment(lib, "comctl32.lib")

#define WM_USER_LOG              (WM_USER + 1)
#define WM_USER_UPSTREAM_CHANGE  (WM_USER + 2)
#define WM_USER_STATS            (WM_USER + 3)
#define WM_USER_PROXY_STARTED    (WM_USER + 4)
#define WM_USER_PROXY_STOPPED    (WM_USER + 5)
#define WM_USER_FORWARD_START    (WM_USER + 6)
#define WM_USER_FORWARD_END      (WM_USER + 7)

static HWND g_hwnd = NULL;
static HWND g_hToolbar = NULL;
static HWND g_hListView = NULL;
static HWND g_hLog = NULL;
static HWND g_hStatus = NULL;
static HWND g_hStats = NULL;
static HWND g_hwndPort = NULL;
static HINSTANCE g_hInst = NULL;
static int g_proxy_running = 0;
static int g_show_toolbar = 1;
static int g_show_statusbar = 1;

static ProxyEngine *g_engine = NULL;
static UpstreamPool *g_pool = NULL;
static HealthChecker *g_hc = NULL;
static int g_listen_port = 8888;

static const int LV_COL_HOST = 0;
static const int LV_COL_PORT = 1;
static const int LV_COL_WEIGHT = 2;
static const int LV_COL_CONNS = 3;
static const int LV_COL_STATUS = 4;

static DWORD WINAPI proxy_thread(LPVOID param)
{
    (void)param;
    if (g_engine)
        proxy_start(g_engine);
    return 0;
}

void ui_on_log(LogLevel level, const char *fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (g_hwnd) {
        COPYDATASTRUCT cds;
        cds.dwData = (DWORD)level;
        cds.cbData = (DWORD)(strlen(buf) + 1);
        cds.lpData = buf;
        SendMessageA(g_hwnd, WM_USER_LOG, (WPARAM)level, (LPARAM)&cds);
    }
}

void ui_on_upstream_change(int index)
{
    if (g_hwnd)
        PostMessageA(g_hwnd, WM_USER_UPSTREAM_CHANGE, (WPARAM)index, 0);
}

void ui_on_stats(int total_conns, uint64_t total_bytes)
{
    if (g_hwnd) {
        uint64_t *data = (uint64_t *)malloc(2 * sizeof(uint64_t));
        if (data) {
            data[0] = (uint64_t)total_conns;
            data[1] = total_bytes;
            PostMessageA(g_hwnd, WM_USER_STATS, 0, (LPARAM)data);
        }
    }
}

void ui_on_proxy_started(uint16_t port)
{
    if (g_hwnd)
        PostMessageA(g_hwnd, WM_USER_PROXY_STARTED, (WPARAM)port, 0);
}

void ui_on_proxy_stopped(void)
{
    if (g_hwnd)
        PostMessageA(g_hwnd, WM_USER_PROXY_STOPPED, 0, 0);
}

void ui_on_forward_start(int client_index, int upstream_index)
{
    if (g_hwnd) {
        int *data = (int *)malloc(2 * sizeof(int));
        if (data) {
            data[0] = client_index;
            data[1] = upstream_index;
            PostMessageA(g_hwnd, WM_USER_FORWARD_START, 0, (LPARAM)data);
        }
    }
}

void ui_on_forward_end(int client_index, int upstream_index, uint64_t bytes)
{
    if (g_hwnd) {
        uint64_t *data = (uint64_t *)malloc(3 * sizeof(uint64_t));
        if (data) {
            data[0] = (uint64_t)client_index;
            data[1] = (uint64_t)upstream_index;
            data[2] = bytes;
            PostMessageA(g_hwnd, WM_USER_FORWARD_END, 0, (LPARAM)data);
        }
    }
}

static void append_log(HWND hwndLog, LogLevel level, const char *msg)
{
    if (!hwndLog) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char prefix[64];
    const char *level_str = "";
    switch (level) {
        case LOG_INFO:  level_str = "INFO";  break;
        case LOG_WARN:  level_str = "WARN";  break;
        case LOG_ERROR: level_str = "ERROR"; break;
    }
    _snprintf(prefix, sizeof(prefix), "%02d:%02d:%02d [%s] ",
              st.wHour, st.wMinute, st.wSecond, level_str);

    int tlen = GetWindowTextLengthA(hwndLog);

    CHARFORMATA cf;
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    switch (level) {
        case LOG_INFO:  cf.crTextColor = RGB(0, 0, 0);     break;
        case LOG_WARN:  cf.crTextColor = RGB(200, 100, 0); break;
        case LOG_ERROR: cf.crTextColor = RGB(200, 0, 0);   break;
    }

    SendMessageA(hwndLog, EM_SETSEL, (WPARAM)tlen, (LPARAM)tlen);
    SendMessageA(hwndLog, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageA(hwndLog, EM_REPLACESEL, FALSE, (LPARAM)prefix);

    tlen = GetWindowTextLengthA(hwndLog);
    cf.crTextColor = RGB(0, 0, 0);
    SendMessageA(hwndLog, EM_SETSEL, (WPARAM)tlen, (LPARAM)tlen);
    SendMessageA(hwndLog, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    SendMessageA(hwndLog, EM_REPLACESEL, FALSE, (LPARAM)msg);

    tlen = GetWindowTextLengthA(hwndLog);
    SendMessageA(hwndLog, EM_REPLACESEL, FALSE, (LPARAM)"\r\n");

    SendMessageA(hwndLog, EM_SCROLLCARET, 0, 0);
}

static void rebuild_listview(void)
{
    if (!g_hListView || !g_pool) return;

    ListView_DeleteAllItems(g_hListView);
    LV_ITEMA lvi;
    memset(&lvi, 0, sizeof(lvi));

    for (int i = 0; i < g_pool->count; i++) {
        Upstream *u = &g_pool->items[i];
        char buf[64];

        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = i;
        lvi.lParam = i;

        lvi.iSubItem = LV_COL_HOST;
        lvi.pszText = u->host;
        ListView_InsertItem(g_hListView, &lvi);

        _snprintf(buf, sizeof(buf), "%d", u->port);
        ListView_SetItemText(g_hListView, i, LV_COL_PORT, buf);

        _snprintf(buf, sizeof(buf), "%d", u->weight);
        ListView_SetItemText(g_hListView, i, LV_COL_WEIGHT, buf);

        _snprintf(buf, sizeof(buf), "%d", u->active_conns);
        ListView_SetItemText(g_hListView, i, LV_COL_CONNS, buf);

        ListView_SetItemText(g_hListView, i, LV_COL_STATUS,
                             u->alive ? "Alive" : "Dead");
    }
}

static void update_listview_row(int index)
{
    if (!g_hListView || !g_pool || index < 0 || index >= g_pool->count)
        return;

    Upstream *u = &g_pool->items[index];
    char buf[64];

    _snprintf(buf, sizeof(buf), "%d", u->active_conns);
    ListView_SetItemText(g_hListView, index, LV_COL_CONNS, buf);

    ListView_SetItemText(g_hListView, index, LV_COL_STATUS,
                         u->alive ? "Alive" : "Dead");
}

static void update_stats_text(void)
{
    if (!g_hStats || !g_engine) return;

    char buf[512];
    uint64_t total = g_engine->total_bytes;
    const char *unit = "B";
    double val = (double)total;
    if (total >= 1073741824) { val /= 1073741824; unit = "GB"; }
    else if (total >= 1048576) { val /= 1048576; unit = "MB"; }
    else if (total >= 1024) { val /= 1024; unit = "KB"; }

    DWORD uptime = 0;
    if (g_engine->start_time_ms > 0) {
        uptime = (DWORD)((GetTickCount64() - g_engine->start_time_ms) / 1000);
    }
    int h = uptime / 3600;
    int m = (uptime % 3600) / 60;
    int s = uptime % 60;

    _snprintf(buf, sizeof(buf),
              "Active Connections: %d\r\n"
              "Total Forwarded: %.2f %s\r\n"
              "Uptime: %02d:%02d:%02d",
              g_engine->total_conns, val, unit, h, m, s);

    SetWindowTextA(g_hStats, buf);
}

static void update_status_bar(void)
{
    if (!g_hStatus) return;

    char buf[256];
    if (g_engine) {
        uint64_t total = g_engine->total_bytes;
        const char *unit = "B";
        double val = (double)total;
        if (total >= 1073741824) { val /= 1073741824; unit = "GB"; }
        else if (total >= 1048576) { val /= 1048576; unit = "MB"; }
        else if (total >= 1024) { val /= 1024; unit = "KB"; }

        _snprintf(buf, sizeof(buf),
                  "Listen: :%d  |  Conns: %d  |  Fwd: %.2f %s",
                  g_listen_port, g_engine->total_conns, val, unit);
    } else {
        _snprintf(buf, sizeof(buf), "Proxy stopped");
    }

    SendMessageA(g_hStatus, SB_SETTEXT, 0, (LPARAM)buf);
}

static void create_toolbar(HWND hwndParent)
{
    const int numButtons = 5;
    TBBUTTON tbb[5];
    memset(tbb, 0, sizeof(tbb));

    tbb[0].iBitmap = 0;
    tbb[0].fsState = TBSTATE_ENABLED;
    tbb[0].fsStyle = BTNS_BUTTON;
    tbb[0].idCommand = ID_TOOLBAR_START;

    tbb[1].iBitmap = 1;
    tbb[1].fsState = TBSTATE_ENABLED;
    tbb[1].fsStyle = BTNS_BUTTON;
    tbb[1].idCommand = ID_TOOLBAR_STOP;

    tbb[2].iBitmap = 0;
    tbb[2].fsState = TBSTATE_ENABLED;
    tbb[2].fsStyle = BTNS_SEP;
    tbb[2].idCommand = 0;

    tbb[3].iBitmap = 2;
    tbb[3].fsState = TBSTATE_ENABLED;
    tbb[3].fsStyle = BTNS_BUTTON;
    tbb[3].idCommand = ID_TOOLBAR_CONFIG;

    tbb[4].iBitmap = 0;
    tbb[4].fsState = TBSTATE_ENABLED;
    tbb[4].fsStyle = BTNS_SEP;
    tbb[4].idCommand = 0;

    g_hToolbar = CreateWindowExA(0, TOOLBARCLASSNAME, NULL,
                                 WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT |
                                 TBSTYLE_TOOLTIPS | CCS_NORESIZE,
                                 0, 0, 0, 0,
                                 hwndParent, (HMENU)0, g_hInst, NULL);
    if (!g_hToolbar) return;

    HIMAGELIST himl = ImageList_Create(24, 24, ILC_COLOR32, 3, 0);

    HDC hdc = GetDC(hwndParent);
    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbm = CreateCompatibleBitmap(hdc, 72, 24);
    SelectObject(hdcMem, hbm);

    RECT rc;
    rc.left = 0; rc.top = 0; rc.right = 24; rc.bottom = 24;
    HBRUSH hbrPlay = CreateSolidBrush(RGB(0, 150, 0));
    FillRect(hdcMem, &rc, hbrPlay);
    DeleteObject(hbrPlay);
    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, RGB(255, 255, 255));
    DrawTextA(hdcMem, ">", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    rc.left = 24; rc.top = 0; rc.right = 48; rc.bottom = 24;
    HBRUSH hbrStop = CreateSolidBrush(RGB(200, 0, 0));
    FillRect(hdcMem, &rc, hbrStop);
    DeleteObject(hbrStop);
    DrawTextA(hdcMem, "#", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    rc.left = 48; rc.top = 0; rc.right = 72; rc.bottom = 24;
    HBRUSH hbrCfg = CreateSolidBrush(RGB(80, 80, 80));
    FillRect(hdcMem, &rc, hbrCfg);
    DeleteObject(hbrCfg);
    DrawTextA(hdcMem, "C", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    ImageList_AddMasked(himl, hbm, RGB(0, 0, 0));
    DeleteDC(hdcMem);
    DeleteObject(hbm);
    ReleaseDC(hwndParent, hdc);

    SendMessageA(g_hToolbar, TB_SETIMAGELIST, 0, (LPARAM)himl);
    SendMessageA(g_hToolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageA(g_hToolbar, TB_ADDBUTTONS, numButtons, (LPARAM)&tbb);
    SendMessageA(g_hToolbar, TB_AUTOSIZE, 0, 0);

    RECT r;
    GetWindowRect(g_hToolbar, &r);
    int tbHeight = r.bottom - r.top;

    CreateWindowExA(0, "STATIC", "Listen:",
                    WS_CHILD | WS_VISIBLE,
                    400, 2, 50, tbHeight - 4,
                    hwndParent, NULL, g_hInst, NULL);

    g_hwndPort = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "8888",
                                 WS_CHILD | WS_VISIBLE | ES_NUMBER |
                                 ES_CENTER,
                                 450, 3, 60, tbHeight - 6,
                                 hwndParent, (HMENU)ID_EDIT_PORT,
                                 g_hInst, NULL);

    SendMessageA(g_hwndPort, EM_SETLIMITTEXT, 5, 0);

    HWND hwndUpDown = CreateWindowExA(0, UPDOWN_CLASS, NULL,
                                      WS_CHILD | WS_VISIBLE |
                                      UDS_ALIGNRIGHT | UDS_ARROWKEYS,
                                      0, 0, 0, 0,
                                      hwndParent, NULL, g_hInst, NULL);
    SendMessageA(hwndUpDown, UDM_SETRANGE, 0, MAKELPARAM(1, 65535));
    SendMessageA(hwndUpDown, UDM_SETPOS, 0, MAKELPARAM(8888, TRUE));
    SendMessageA(hwndUpDown, UDM_SETBUDDY, (WPARAM)g_hwndPort, 0);
}

static void create_listview(HWND hwndParent)
{
    g_hListView = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEW, NULL,
                                  WS_CHILD | WS_VISIBLE |
                                  LVS_REPORT | LVS_SINGLESEL |
                                  LVS_NOSORTHEADER,
                                  0, 0, 200, 100,
                                  hwndParent, (HMENU)ID_UPSTREAM_LIST,
                                  g_hInst, NULL);

    LV_COLUMNA lvc;
    memset(&lvc, 0, sizeof(lvc));
    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    lvc.cx = 80;  lvc.pszText = "Host";
    ListView_InsertColumn(g_hListView, LV_COL_HOST, &lvc);

    lvc.cx = 50;  lvc.pszText = "Port";
    ListView_InsertColumn(g_hListView, LV_COL_PORT, &lvc);

    lvc.cx = 50;  lvc.pszText = "Weight";
    ListView_InsertColumn(g_hListView, LV_COL_WEIGHT, &lvc);

    lvc.cx = 60;  lvc.pszText = "Conns";
    ListView_InsertColumn(g_hListView, LV_COL_CONNS, &lvc);

    lvc.cx = 60;  lvc.pszText = "Status";
    ListView_InsertColumn(g_hListView, LV_COL_STATUS, &lvc);

    ListView_SetExtendedListViewStyle(g_hListView,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
}

static void create_stats_panel(HWND hwndParent)
{
    g_hStats = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", NULL,
                               WS_CHILD | WS_VISIBLE | ES_READONLY |
                               ES_MULTILINE | ES_LEFT,
                               210, 0, 400, 120,
                               hwndParent, (HMENU)ID_STATS_TEXT,
                               g_hInst, NULL);
}

static HWND create_log_window(HWND hwndParent)
{
    LoadLibraryA("riched20.dll");

    g_hLog = CreateWindowExA(WS_EX_CLIENTEDGE, "RichEdit20A", NULL,
                             WS_CHILD | WS_VISIBLE | ES_READONLY |
                             ES_MULTILINE | ES_AUTOVSCROLL |
                             WS_VSCROLL | WS_HSCROLL,
                             0, 0, 100, 100,
                             hwndParent, (HMENU)ID_LOG_EDIT,
                             g_hInst, NULL);

    return g_hLog;
}

static void create_status_bar(HWND hwndParent)
{
    g_hStatus = CreateWindowExA(0, STATUSCLASSNAME, NULL,
                                WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                0, 0, 0, 0,
                                hwndParent, NULL, g_hInst, NULL);

    int parts[] = { -1 };
    SendMessageA(g_hStatus, SB_SETPARTS, 1, (LPARAM)parts);
    SendMessageA(g_hStatus, SB_SETTEXT, 0, (LPARAM)"Ready");
}

static INT_PTR CALLBACK config_dlg_proc(HWND hDlg, UINT msg,
                                        WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (msg) {
        case WM_INITDIALOG: {
            SendDlgItemMessageA(hDlg, ID_EDIT_HC_INTERVAL, EM_SETLIMITTEXT, 5, 0);
            SendDlgItemMessageA(hDlg, ID_EDIT_HC_TIMEOUT, EM_SETLIMITTEXT, 5, 0);
            SendDlgItemMessageA(hDlg, ID_EDIT_HC_FAIL_THRESH, EM_SETLIMITTEXT, 3, 0);
            SendDlgItemMessageA(hDlg, ID_EDIT_HC_REVIVE_THRESH, EM_SETLIMITTEXT, 3, 0);
            SendDlgItemMessageA(hDlg, ID_EDIT_HOST, EM_SETLIMITTEXT, 255, 0);
            SendDlgItemMessageA(hDlg, ID_EDIT_UPORT, EM_SETLIMITTEXT, 5, 0);
            SendDlgItemMessageA(hDlg, ID_EDIT_WEIGHT, EM_SETLIMITTEXT, 5, 0);

            if (g_pool) {
                for (int i = 0; i < g_pool->count; i++) {
                    char buf[512];
                    _snprintf(buf, sizeof(buf), "%s:%d  w:%d  %s",
                              g_pool->items[i].host,
                              g_pool->items[i].port,
                              g_pool->items[i].weight,
                              g_pool->items[i].alive ? "Alive" : "Dead");
                    SendDlgItemMessageA(hDlg, ID_LIST_BACKENDS, LB_ADDSTRING,
                                        0, (LPARAM)buf);
                }
            }
            if (g_hc) {
                char buf[32];
                _snprintf(buf, sizeof(buf), "%d", g_hc->interval_ms / 1000);
                SetDlgItemTextA(hDlg, ID_EDIT_HC_INTERVAL, buf);
                _snprintf(buf, sizeof(buf), "%d", g_hc->timeout_ms / 1000);
                SetDlgItemTextA(hDlg, ID_EDIT_HC_TIMEOUT, buf);
                _snprintf(buf, sizeof(buf), "%d", g_hc->passive_threshold);
                SetDlgItemTextA(hDlg, ID_EDIT_HC_FAIL_THRESH, buf);
                _snprintf(buf, sizeof(buf), "%d", g_hc->revive_threshold);
                SetDlgItemTextA(hDlg, ID_EDIT_HC_REVIVE_THRESH, buf);
            }
            return TRUE;
        }

        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case ID_BUTTON_ADD: {
                    char host[256] = {0};
                    char port_str[16] = {0};
                    char weight_str[16] = {0};
                    GetDlgItemTextA(hDlg, ID_EDIT_HOST, host, sizeof(host));
                    GetDlgItemTextA(hDlg, ID_EDIT_UPORT, port_str, sizeof(port_str));
                    GetDlgItemTextA(hDlg, ID_EDIT_WEIGHT, weight_str, sizeof(weight_str));

                    if (host[0] && port_str[0]) {
                        int port = atoi(port_str);
                        int weight = weight_str[0] ? atoi(weight_str) : 1;
                        if (g_pool && upstream_add(g_pool, host, port, weight) >= 0) {
                            char buf[512];
                            _snprintf(buf, sizeof(buf), "%s:%d  w:%d  Alive",
                                      host, port, weight);
                            SendDlgItemMessageA(hDlg, ID_LIST_BACKENDS, LB_ADDSTRING,
                                                0, (LPARAM)buf);
                        }
                    }
                    break;
                }
                case ID_BUTTON_REMOVE: {
                    int sel = (int)SendDlgItemMessageA(hDlg, ID_LIST_BACKENDS,
                                                       LB_GETCURSEL, 0, 0);
                    if (sel != LB_ERR && g_pool && sel >= 0 && sel < g_pool->count) {
                        SendDlgItemMessageA(hDlg, ID_LIST_BACKENDS, LB_DELETESTRING,
                                            (WPARAM)sel, 0);
                        int remaining = g_pool->count - sel - 1;
                        if (remaining > 0)
                            memmove(&g_pool->items[sel], &g_pool->items[sel + 1],
                                    (size_t)remaining * sizeof(Upstream));
                        g_pool->count--;
                    }
                    break;
                }
                case IDOK: {
                    if (g_hc) {
                        char buf[32];
                        GetDlgItemTextA(hDlg, ID_EDIT_HC_INTERVAL, buf, sizeof(buf));
                        if (buf[0]) g_hc->interval_ms = atoi(buf) * 1000;
                        GetDlgItemTextA(hDlg, ID_EDIT_HC_TIMEOUT, buf, sizeof(buf));
                        if (buf[0]) g_hc->timeout_ms = atoi(buf) * 1000;
                        GetDlgItemTextA(hDlg, ID_EDIT_HC_FAIL_THRESH, buf, sizeof(buf));
                        if (buf[0]) g_hc->passive_threshold = atoi(buf);
                        GetDlgItemTextA(hDlg, ID_EDIT_HC_REVIVE_THRESH, buf, sizeof(buf));
                        if (buf[0]) g_hc->revive_threshold = atoi(buf);
                    }
                    EndDialog(hDlg, IDOK);
                    rebuild_listview();
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;
            }
            break;
        }
    }
    return FALSE;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg,
                                 WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_CREATE: {
            HMENU hMenu = CreateMenu();

            HMENU hFileMenu = CreatePopupMenu();
            AppendMenuA(hFileMenu, MF_STRING, ID_MENU_FILE_START, "Start\tCtrl+R");
            AppendMenuA(hFileMenu, MF_STRING, ID_MENU_FILE_STOP, "Stop\tCtrl+.");
            AppendMenuA(hFileMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuA(hFileMenu, MF_STRING, ID_MENU_FILE_EXIT, "Exit\tAlt+F4");
            AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hFileMenu, "File");

            HMENU hConfigMenu = CreatePopupMenu();
            AppendMenuA(hConfigMenu, MF_STRING, ID_MENU_CONFIG_BACKENDS, "Backends && Health...");
            AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hConfigMenu, "Config");

            HMENU hViewMenu = CreatePopupMenu();
            AppendMenuA(hViewMenu, MF_STRING | MF_CHECKED, ID_MENU_VIEW_TOOLBAR, "Toolbar");
            AppendMenuA(hViewMenu, MF_STRING | MF_CHECKED, ID_MENU_VIEW_STATUSBAR, "Status Bar");
            AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hViewMenu, "View");

            HMENU hHelpMenu = CreatePopupMenu();
            AppendMenuA(hHelpMenu, MF_STRING, ID_MENU_HELP_ABOUT, "About");
            AppendMenuA(hMenu, MF_POPUP, (UINT_PTR)hHelpMenu, "Help");

            SetMenu(hwnd, hMenu);

            create_toolbar(hwnd);
            create_listview(hwnd);
            create_stats_panel(hwnd);
            create_log_window(hwnd);
            create_status_bar(hwnd);

            SetTimer(hwnd, ID_TIMER_UI, 200, NULL);
            break;
        }

        case WM_SIZE: {
            RECT rc;
            GetClientRect(hwnd, &rc);

            int tbHeight = 0;
            if (g_hToolbar && g_show_toolbar) {
                RECT r;
                GetWindowRect(g_hToolbar, &r);
                tbHeight = r.bottom - r.top;
                SetWindowPos(g_hToolbar, NULL, 0, 0, rc.right, tbHeight, SWP_NOZORDER);
            }

            int sbHeight = 0;
            if (g_hStatus && g_show_statusbar) {
                SendMessageA(g_hStatus, WM_SIZE, 0, 0);
                RECT r;
                GetWindowRect(g_hStatus, &r);
                sbHeight = r.bottom - r.top;
            }

            int lvHeight = (rc.bottom - tbHeight - sbHeight) * 2 / 5;
            int logTop = tbHeight + lvHeight;
            int logHeight = rc.bottom - tbHeight - lvHeight - sbHeight;

            if (g_hListView)
                SetWindowPos(g_hListView, NULL, 0, tbHeight, 200, lvHeight, SWP_NOZORDER);
            if (g_hStats)
                SetWindowPos(g_hStats, NULL, 210, tbHeight, rc.right - 210, lvHeight, SWP_NOZORDER);
            if (g_hLog)
                SetWindowPos(g_hLog, NULL, 0, logTop, rc.right, logHeight, SWP_NOZORDER);
            if (g_hStatus)
                SetWindowPos(g_hStatus, NULL, 0, rc.bottom - sbHeight, rc.right, sbHeight, SWP_NOZORDER);

            break;
        }

        case WM_TIMER: {
            if (wParam == ID_TIMER_UI) {
                update_stats_text();
                update_status_bar();
            }
            break;
        }

        case WM_USER_LOG: {
            COPYDATASTRUCT *cds = (COPYDATASTRUCT *)lParam;
            if (cds && g_hLog)
                append_log(g_hLog, (LogLevel)cds->dwData, (const char *)cds->lpData);
            break;
        }

        case WM_USER_UPSTREAM_CHANGE: {
            update_listview_row((int)wParam);
            break;
        }

        case WM_USER_STATS: {
            uint64_t *data = (uint64_t *)lParam;
            if (data) {
                if (g_engine) {
                    g_engine->total_conns = (int)data[0];
                    g_engine->total_bytes = data[1];
                }
                free(data);
                update_stats_text();
                update_status_bar();
            }
            break;
        }

        case WM_USER_PROXY_STARTED: {
            g_proxy_running = 1;
            SetWindowTextA(hwnd, "KimKong - Reverse Proxy [Running]");
            char buf[256];
            _snprintf(buf, sizeof(buf), "Proxy started on :%d", (int)wParam);
            append_log(g_hLog, LOG_INFO, buf);
            update_status_bar();
            break;
        }

        case WM_USER_PROXY_STOPPED: {
            g_proxy_running = 0;
            SetWindowTextA(hwnd, "KimKong - Reverse Proxy [Stopped]");
            append_log(g_hLog, LOG_INFO, "Proxy stopped");
            update_status_bar();
            break;
        }

        case WM_USER_FORWARD_START: {
            int *data = (int *)lParam;
            if (data && g_pool && data[1] >= 0 && data[1] < g_pool->count) {
                char buf[256];
                _snprintf(buf, sizeof(buf), "Forward [%d] -> %s:%d",
                          data[0],
                          g_pool->items[data[1]].host,
                          g_pool->items[data[1]].port);
                append_log(g_hLog, LOG_INFO, buf);
                free(data);
            }
            break;
        }

        case WM_USER_FORWARD_END: {
            uint64_t *data = (uint64_t *)lParam;
            if (data && g_pool) {
                int up_index = (int)data[1];
                const char *unit = "B";
                double val = (double)data[2];
                if (data[2] >= 1073741824) { val /= 1073741824; unit = "GB"; }
                else if (data[2] >= 1048576) { val /= 1048576; unit = "MB"; }
                else if (data[2] >= 1024) { val /= 1024; unit = "KB"; }

                char buf[256];
                if (up_index >= 0 && up_index < g_pool->count) {
                    _snprintf(buf, sizeof(buf), "Forward [%d] <- %s:%d  (%.2f %s)",
                              (int)data[0],
                              g_pool->items[up_index].host,
                              g_pool->items[up_index].port,
                              val, unit);
                } else {
                    _snprintf(buf, sizeof(buf), "Forward [%d] closed (%.2f %s)",
                              (int)data[0], val, unit);
                }
                append_log(g_hLog, LOG_INFO, buf);
                free(data);
            }
            break;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            switch (id) {
                case ID_TOOLBAR_START:
                case ID_MENU_FILE_START: {
                    if (!g_proxy_running && g_engine) {
                        char buf[16];
                        GetWindowTextA(g_hwndPort, buf, sizeof(buf));
                        int port = atoi(buf);
                        if (port > 0 && port < 65536) {
                            g_listen_port = port;
                            g_engine->listen_port = (uint16_t)port;
                            g_engine->start_time_ms = GetTickCount64();
                        }
                        CreateThread(NULL, 0, proxy_thread, NULL, 0, NULL);
                    }
                    break;
                }
                case ID_TOOLBAR_STOP:
                case ID_MENU_FILE_STOP: {
                    if (g_proxy_running && g_engine)
                        proxy_stop(g_engine);
                    break;
                }
                case ID_TOOLBAR_CONFIG:
                case ID_MENU_CONFIG_BACKENDS: {
                    DialogBoxA(g_hInst, MAKEINTRESOURCEA(ID_DIALOG_CONFIG),
                               hwnd, config_dlg_proc);
                    rebuild_listview();
                    break;
                }
                case ID_MENU_FILE_EXIT:
                    DestroyWindow(hwnd);
                    break;
                case ID_MENU_VIEW_TOOLBAR: {
                    g_show_toolbar = !g_show_toolbar;
                    CheckMenuItem(GetMenu(hwnd), ID_MENU_VIEW_TOOLBAR,
                                  g_show_toolbar ? MF_CHECKED : MF_UNCHECKED);
                    ShowWindow(g_hToolbar, g_show_toolbar ? SW_SHOW : SW_HIDE);
                    SendMessageA(hwnd, WM_SIZE, 0, 0);
                    break;
                }
                case ID_MENU_VIEW_STATUSBAR: {
                    g_show_statusbar = !g_show_statusbar;
                    CheckMenuItem(GetMenu(hwnd), ID_MENU_VIEW_STATUSBAR,
                                  g_show_statusbar ? MF_CHECKED : MF_UNCHECKED);
                    ShowWindow(g_hStatus, g_show_statusbar ? SW_SHOW : SW_HIDE);
                    SendMessageA(hwnd, WM_SIZE, 0, 0);
                    break;
                }
                case ID_MENU_HELP_ABOUT:
                    MessageBoxA(hwnd, "KimKong v1.0\nReverse Proxy with GUI",
                                "About KimKong", MB_OK);
                    break;
            }
            break;
        }

        case WM_DESTROY:
            if (g_proxy_running && g_engine)
                proxy_stop(g_engine);
            KillTimer(hwnd, ID_TIMER_UI);
            PostQuitMessage(0);
            break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void ui_win32_set_engine(void *engine)
{
    g_engine = (ProxyEngine *)engine;
}

void ui_win32_set_pool(void *pool)
{
    g_pool = (UpstreamPool *)pool;
    if (g_hListView) rebuild_listview();
}

void ui_win32_set_hc(void *hc)
{
    g_hc = (HealthChecker *)hc;
}

HWND ui_win32_create(HINSTANCE hInstance, int nCmdShow)
{
    g_hInst = hInstance;

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES |
                 ICC_UPDOWN_CLASS | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    wc.lpszClassName = "KimKongWindow";

    if (!RegisterClassA(&wc)) return NULL;

    g_hwnd = CreateWindowExA(WS_EX_APPWINDOW, "KimKongWindow",
                             "KimKong - Reverse Proxy",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             800, 600,
                             NULL, NULL, hInstance, NULL);

    if (!g_hwnd) return NULL;

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    return g_hwnd;
}
