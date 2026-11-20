/*
 * net.c — listen 소켓 생성, 논블로킹 설정
 *
 * CS:APP 대응: open_listenfd() (csapp.c)
 *   Tiny는 getaddrinfo 결과를 돌며 socket → setsockopt(SO_REUSEADDR) → bind → listen.
 *   이 흐름은 그대로 쓰고, 실패 시 unix_error()로 exit 하는 대신 -1 을 돌려준다.
 * 담당: 트랙 A · 이슈 #7
 */
#include "net.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>

int net_listen(const char *port)
{
    /* TODO(#7): open_listenfd 와 같은 흐름
     *  - struct addrinfo hints 를 memset 0 (macOS sin_len 주의)
     *  - AI_PASSIVE | AI_ADDRCONFIG, SOCK_STREAM
     *  - SO_REUSEADDR, listen backlog
     *  - net_set_nonblocking() */
    (void)port;
    return -1;
}

int net_set_nonblocking(int fd)
{
    /* TODO(#7): fcntl(F_GETFL) | O_NONBLOCK → fcntl(F_SETFL)
     * (SOCK_NONBLOCK, accept4 는 Linux 전용이라 쓰지 않는다) */
    (void)fd;
    return -1;
}
