#include "tcp_socket.h"

#ifdef _WIN32

int sock_init(void)
{
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2,2), &wsa);
}

void sock_cleanup(void)
{
    WSACleanup();
}

socket_t sock_create_tcp(void)
{
    return socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
}

int sock_bind(socket_t fd, const char *ip, int port)
{
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short) port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    return bind(fd, (struct sockaddr *)&addr, sizeof(addr));
}

int sock_listen(socket_t fd, int backlog)
{
    return listen(fd, backlog);
}

socket_t sock_accept(socket_t fd, char *client_ip, int ip_len, char *client_port)
{
    struct sockaddr_in addr;
    int addr_len = sizeof(addr);
    socket_t client = accept(fd, (struct sockaddr *)&addr, &addr_len);
    if (client != INVALID_SOCKET_VALUE) {
        inet_ntop(AF_INET, &addr.sin_addr, client_ip, (unsigned)ip_len);
        if (client_port) *client_port = ntohs(addr.sin_port);
    }
    return client;
}

int sock_connect(socket_t fd, const char *host, int port, int timeout_ms)
{
    (void)timeout_ms;
    struct sockaddr_in addr;
    struct hostent *he;

    he = gethostbyname(host);
    if (!he) return -1;

    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short) port);
    addr.sin_addr = *(struct in_addr *)he->h_addr_list[0];

    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);

    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == SOCKET_ERROR)
    {
        
    }
}

#endif