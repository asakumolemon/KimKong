#ifndef HTTP_PARSE_H
#define HTTP_PARSE_H

typedef struct 
{
    char method[16];
    char path[1024];
    char host[256];
    int port;
    int complete;
    int parse_error;
} HttpRequest;

int http_parse_request(const char *raw, int len, HttpRequest *req);

#endif 
