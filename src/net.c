/*
 * net.c — listen 소켓 생성, 논블로킹 설정
 *
 * CS:APP 대응: open_listenfd() (csapp.c)
 *   Tiny는 getaddrinfo 결과를 돌며 socket → setsockopt(SO_REUSEADDR) → bind → listen.
 *   이 흐름은 그대로 쓰고, 다른 점은:
 *   - 실패 시 unix_error()로 exit 하는 대신 -1 을 돌려준다 (errno 유지)
 *   - IPv6 소켓 하나로 IPv4 까지 받는다 (IPV6_V6ONLY=0 명시).
 *     Tiny 는 getaddrinfo 가 처음 돌려준 주소 하나(보통 0.0.0.0)만 bind 해서
 *     localhost 가 ::1 로 풀리는 환경에서 접속이 거부된다 (이슈 #3 관찰).
 *     IPV6_V6ONLY 기본값은 OS 마다 달라서 (Linux 0, 일부 BSD 1) 직접 정한다.
 *   - IPv6 를 쓸 수 없는 환경(커널·컨테이너에서 꺼짐)이면 IPv4 로 물러난다.
 *     단 포트 충돌(EADDRINUSE)·권한(EACCES)은 IPv4 로 해도 같은 문제라 그대로 실패한다.
 *     (물러나면 "IPv6 는 다른 프로그램이 쓰고 있는데 IPv4 로 떠 버리는" 혼란이 생긴다)
 *   - listen 소켓도 논블로킹으로 만든다. poll 이 읽기 가능이라고 알려 줘도 그 사이
 *     클라이언트가 연결을 취소하면 블로킹 accept 는 다음 연결이 올 때까지 멈춘다.
 * 담당: 트랙 A · 이슈 #7
 */
#include "net.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* listen() 대기열 길이. Tiny 의 LISTENQ(1024)와 같은 역할, 시스템 최대값을 쓴다 */
#define NET_BACKLOG SOMAXCONN

/* family(AF_INET6 / AF_INET) 의 와일드카드 주소로 listen 소켓 하나를 만든다 */
static int listen_family(int family, const char *port)
{
    struct addrinfo hints, *res;
    int fd, rc, saved, one = 1, zero = 0;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;   /* 모든 주소, 숫자 포트만 */

    rc = getaddrinfo(NULL, port, &hints, &res);
    if (rc != 0) {
        /* getaddrinfo 는 errno 대신 EAI_* 를 돌려준다. 호출자는 errno 만 보므로 옮겨 둔다 */
        if (rc == EAI_MEMORY)
            errno = ENOMEM;
        else if (rc != EAI_SYSTEM)
            errno = (rc == EAI_FAMILY) ? EAFNOSUPPORT : EINVAL;
        return -1;
    }

    /* 와일드카드 주소는 family 마다 하나라서 첫 결과만 쓴다 */
    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0)
        goto fail;

    /* 서버를 재시작할 때 TIME_WAIT 연결 때문에 bind 가 EADDRINUSE 로 실패하는 것을 막는다 */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0)
        goto fail;
    if (family == AF_INET6 &&
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero) < 0)
        goto fail;
    if (bind(fd, res->ai_addr, res->ai_addrlen) < 0)
        goto fail;
    if (listen(fd, NET_BACKLOG) < 0)
        goto fail;
    if (net_set_nonblocking(fd) < 0)
        goto fail;

    freeaddrinfo(res);
    return fd;

fail:
    saved = errno;              /* close·freeaddrinfo 가 errno 를 덮어쓸 수 있다 */
    if (fd >= 0)
        close(fd);
    freeaddrinfo(res);
    errno = saved;
    return -1;
}

int net_listen(const char *port)
{
    int fd = listen_family(AF_INET6, port);

    if (fd >= 0 || errno == EADDRINUSE || errno == EACCES)
        return fd;
    return listen_family(AF_INET, port);
}

int net_set_nonblocking(int fd)
{
    /* SOCK_NONBLOCK, accept4 는 Linux 전용이라 쓰지 않는다 (CLAUDE.md 이식성 규칙) */
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0)
        return -1;
    if (flags & O_NONBLOCK)
        return 0;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
}
