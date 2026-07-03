#include "tcp_socket.h"

#ifdef _WIN32

int sock_init(void)
{
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa);
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
    addr.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    return bind(fd, (struct sockaddr *)&addr, sizeof(addr));
}

int sock_listen(socket_t fd, int backlog)
{
    return listen(fd, backlog);
}

socket_t sock_accept(socket_t fd, char *client_ip, int ip_len, int *client_port)
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
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr = *(struct in_addr *)he->h_addr_list[0];

    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);

    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            mode = 0; ioctlsocket(fd, FIONBIO, &mode);
            return -1;
        }
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ret = select((int)fd + 1, NULL, &wset, NULL, &tv);
        if (ret <= 0) {
            mode = 0; ioctlsocket(fd, FIONBIO, &mode);
            return -1;
        }
        int error = 0;
        int len = sizeof(error);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&error, &len);
        if (error != 0) {
            mode = 0; ioctlsocket(fd, FIONBIO, &mode);
            return -1;
        }
    }

    mode = 0;
    ioctlsocket(fd, FIONBIO, &mode);
    return 0;
}

int sock_set_nonblock(socket_t fd)
{
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode);
}

int sock_recv(socket_t fd, char *buf, int len)
{
    return recv(fd, buf, len, 0);
}

int sock_send(socket_t fd, const char *buf, int len)
{
    return send(fd, buf, len, 0);
}

void sock_close(socket_t fd)
{
    closesocket(fd);
}

int sock_get_error(void)
{
    return WSAGetLastError();
}

#endif /* _WIN32 */
