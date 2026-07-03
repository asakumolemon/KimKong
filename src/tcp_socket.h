#ifndef TCP_SOCKET_H
#define TCP_SOCKET_H

#include <stdint.h>
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    typedef SOCKET socket_t;
    #define INVALID_SOCKET_VALUE INVALID_SOCKET
    #define SOCKET_ERROR_RETURN  SOCKET_ERROR
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    typedef int socket_t;
    #define INVALID_SOCKET_VALUE (-1)
    #define SOCKET_ERROR_RETURN  (-1)
#endif

int         sock_init(void);
void        sock_cleanup(void);
socket_t    sock_create_tcp(void);
int         sock_bind(socket_t fd, const char *ip, int port);
int         sock_listen(socket_t fd, int backlog);
socket_t    sock_accept(socket_t fd, char *client_ip, int ip_len, int *client_port);
int         sock_connect(socket_t fd, const char *host, int port, int timeout_ms);
int         sock_set_nonblock(socket_t fd);
int         sock_recv(socket_t fd, char *buf, int len);
int         sock_send(socket_t fd, const char *buf, int len);
void        sock_close(socket_t fd);
int         sock_get_error(void);


#endif TCP_SOCKET_H