#include "http_parse.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int http_parse_request(const char *raw, int len, HttpRequest *req) 
{
    memset(req, 0, sizeof(HttpRequest));

    req->port = 80;

    const char *p = raw;
    const char *end = raw + len;

    const char *crlf = memchr(p, '\r', (size_t)(end - p));
    if (!crlf || crlf + 1 >= end || crlf[1] != '\n')
        return 0;

    const char *sp = memchr(p, ' ', (size_t)(crlf - p));
    if (!sp) { req->parse_error = 1; return -1; }
    int ml = (int)(sp - p);
    if (ml >= (int) sizeof(req->method)) ml = (int) sizeof(req->method) -1;
    memcpy(req->method, p, ml);
    req->method[ml] = '\0';

    p = sp +1;
    sp = memchr(p, ' ', (size_t)(crlf - p));
    if (!sp) { req->parse_error = 1; return -1;}
    int pl = (int)(sp - p);
    if (pl >= (int)sizeof(req->path)) pl = (int)sizeof(req->path) - 1;
    memcpy(req->path, p, pl);
    req->path[pl] = '\0';

    p = crlf + 2;
    int found_end = 0;
    while (p < end) {
        if (*p == '\r' && p + 1 < end && *(p + 1) == '\n') {
            found_end = 1;
            break;
        }
        if ((p[0] == 'H' || p[0] =='h') &&
            (p[1] == 'O' || p[1] =='o') &&
            (p[2] == 'S' || p[2] =='s') &&
            (p[3] == 'T' || p[3] =='t') &&
            p[4] == ':') 
        {
            p += 5;
            while (p < end && (*p == ' ' || *p == '\t')) 
                p++;
            const char *hs = p;
            const char *he = memchr(p, '\r', (size_t)(end-p));
            if (!he) he = memchr(p, '\n', (size_t)(end-p));
            if (!he) he = end;
            int hl = (int)(he - hs);
            if (hl >= (int)sizeof(req->host)) hl = (int)sizeof(req->host) - 1;
            memcpy(req->host, hs, hl);
            req->host[hl] = '\0';

            char *colon = strrchr(req->host, ':');
            if (colon) {
                *colon = '\0';
                req->port = atoi(colon + 1);
            }
        }
        const char *nl = memchr(p, '\n', (size_t)(end-p));
        if (!nl) break;
        p = nl +1;
    }

    if (found_end) req->complete = 1;
    return (int)(p-raw);
}