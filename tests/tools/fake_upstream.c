/*
 * fake_upstream.c — 프록시 스모크 테스트용 가짜 백엔드 (이슈 #9)
 *
 * 실제 백엔드(같은 바이너리)는 항상 올바른 응답만 준다. 깨진 머리, 일찍 끊기는 본문, 대답 없는 서버,
 * 한 바이트씩 오는 응답처럼 프록시가 502·504·연결 종료로 처리해야 하는 응답을 만들어 내는 도구다.
 * 서버 코드와 링크하지 않는 독립 실행 파일이다. 테스트 전용이라 쓰기는 블로킹이고 연결은 하나씩 순서대로 처리한다.
 *
 * 사용법: fake_upstream <port> <mode>
 *   127.0.0.1(IPv4)에서 listen 하고, 준비가 되면 stdout 에 "ready" 한 줄을 찍는다 (테스트가 이것을 기다린다).
 *   연결마다 요청 머리("\r\n\r\n" 까지)를 읽은 뒤 mode 에 따라 답한다. 죽일 때까지 계속 받는다.
 *
 *   ok            200, Content-Length 8, 본문 "fake ok\n", 닫음
 *   split         ok 와 같은 응답을 1바이트씩 약 5 ms 간격으로 씀 (머리가 여러 read 에 나뉘어 도착)
 *   early-eof     Content-Length: 100 인데 본문 40바이트만 보내고 닫음 (응답 시작 후 중단)
 *   partial-head  "HTTP/1.1 200 OK\r\nContent-Le" 만 보내고 닫음 (응답 시작 전 EOF)
 *   garbage       "hello\r\n\r\n" 을 보내고 닫음 (상태 줄이 HTTP 가 아님)
 *   silent        요청을 읽고 아무 답도 안 한 채 연결을 열어 둠 (504 확인)
 *   hold          Content-Length: 10 과 본문 10바이트를 보내고 연결은 열어 둠 (끝을 EOF 가 아니라 길이로 아는지).
 *                 상대가 먼저 닫으면 stdout 에 "peer-closed" 를 찍는다 (최대 10초 기다림). 프록시가 본문 끝을
 *                 길이로 알아채고 업스트림을 닫았다는 증거다. 이 동안 다른 연결은 받지 못한다
 *   drip          Content-Length: 6, 머리를 바로 보내고 본문을 700 ms 마다 1바이트씩 (손으로 하는 느린 응답 확인.
 *                 UPSTREAM_TIMEOUT_MS 를 1000 으로 줄인 서버 빌드에서 전체 시간 > 제한이어도 간격 < 제한이면 끝까지 받는다)
 *
 * silent 의 연결은 닫지 않고 프로세스가 죽을 때까지 둔다 (fd 를 일부러 흘린다. 테스트 한 번 분량이라 적다).
 * 요청을 5초 안에 다 못 보내는 클라이언트는 버린다 (연결만 열고 말이 없는 클라이언트 하나가 순서 처리를 막지 않게).
 *
 * 빌드: make 가 build/tools/fake_upstream 으로 만든다 (-std=c11 -D_POSIX_C_SOURCE=200809L, glibc·musl·macOS)
 */
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define REQUEST_MAX     16384   /* 요청 머리를 모을 상한. 테스트 요청은 이보다 훨씬 작다 */
#define REQUEST_WAIT_MS 5000
#define HOLD_WAIT_MS    10000   /* hold: 상대가 닫기를 기다리는 최대 시간 */

#define OK_HEAD "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 8\r\nConnection: close\r\n\r\n"
#define OK_BODY "fake ok\n"

typedef enum { M_OK, M_SPLIT, M_EARLY_EOF, M_PARTIAL_HEAD, M_GARBAGE, M_SILENT, M_HOLD, M_DRIP } fake_mode;

static void sleep_ms(long ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;   /* 남은 시간만큼 다시 */
}

/* n 바이트를 다 쓴다. 상대가 끊었으면 -1 (SIGPIPE 는 main 에서 무시) */
static int write_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* 한 바이트씩, 사이에 gap_ms 를 쉬며 쓴다. 바이트마다 따로 나가도록 TCP_NODELAY 가 켜져 있어야 한다 */
static int write_slow(int fd, const char *p, size_t n, long gap_ms)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (write_all(fd, p + i, 1) < 0)
            return -1;
        sleep_ms(gap_ms);
    }
    return 0;
}

/* 요청 머리가 "\r\n\r\n" 까지 도착했는지 */
static int has_head_end(const char *buf, size_t len)
{
    size_t i;

    for (i = 0; i + 4 <= len; i++)
        if (memcmp(buf + i, "\r\n\r\n", 4) == 0)
            return 1;
    return 0;
}

/* 요청 머리를 읽는다. 성공 0, EOF·오류·시간 초과·너무 김 -1. 본문은 읽지 않는다 (테스트의 가짜 경로는 GET) */
static int read_request(int fd)
{
    char buf[REQUEST_MAX];
    size_t len = 0;

    while (!has_head_end(buf, len)) {
        struct pollfd pfd;
        ssize_t r;
        int pr;

        if (len == sizeof buf)
            return -1;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pr = poll(&pfd, 1, REQUEST_WAIT_MS);
        if (pr < 0 && errno == EINTR)
            continue;
        if (pr <= 0)
            return -1;
        r = read(fd, buf + len, sizeof buf - len);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        len += (size_t)r;
    }
    return 0;
}

/* 상대가 연결을 닫을 때까지 최대 ms 밀리초 기다린다. 닫혔으면 1, 시간 초과·오류 0. 상대가 보내는 바이트는 버린다 */
static int wait_peer_close(int fd, int ms)
{
    for (;;) {
        struct pollfd pfd;
        char tmp[256];
        ssize_t r;
        int pr;

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pr = poll(&pfd, 1, ms);
        if (pr < 0 && errno == EINTR)
            continue;
        if (pr <= 0)
            return 0;
        r = read(fd, tmp, sizeof tmp);
        if (r < 0 && errno == EINTR)
            continue;
        if (r > 0)
            continue;   /* 보낸 바이트는 버리고 계속 기다린다 (시간은 새로 센다. 테스트에서는 오지 않는다) */
        return 1;
    }
}

/* 연결 하나를 처리한다. 반환값 1 이면 fd 를 닫지 않고 둔다 (silent) */
static int serve(int fd, fake_mode mode)
{
    char body40[40];
    int one = 1;

    /* 1바이트 쓰기가 Nagle 에 합쳐지지 않게. 실패해도 테스트가 약해질 뿐이라 무시 */
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    if (read_request(fd) < 0)
        return 0;

    switch (mode) {
    case M_OK:
        if (write_all(fd, OK_HEAD OK_BODY, sizeof(OK_HEAD OK_BODY) - 1) < 0)
            return 0;
        break;
    case M_SPLIT:
        (void)write_slow(fd, OK_HEAD OK_BODY, sizeof(OK_HEAD OK_BODY) - 1, 5);
        break;
    case M_EARLY_EOF:
        memset(body40, 'x', sizeof body40);
        if (write_all(fd, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\n",
                      sizeof("HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\n") - 1) == 0)
            (void)write_all(fd, body40, sizeof body40);
        break;
    case M_PARTIAL_HEAD:
        (void)write_all(fd, "HTTP/1.1 200 OK\r\nContent-Le", sizeof("HTTP/1.1 200 OK\r\nContent-Le") - 1);
        break;
    case M_GARBAGE:
        (void)write_all(fd, "hello\r\n\r\n", sizeof("hello\r\n\r\n") - 1);
        break;
    case M_SILENT:
        return 1;
    case M_HOLD:
        (void)write_all(fd, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789",
                        sizeof("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789") - 1);
        if (wait_peer_close(fd, HOLD_WAIT_MS)) {
            printf("peer-closed\n");
            fflush(stdout);
        }
        break;
    case M_DRIP:
        if (write_all(fd, "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\n",
                      sizeof("HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\n") - 1) == 0)
            (void)write_slow(fd, "abcdef", 6, 700);
        break;
    }
    return 0;
}

static int parse_mode(const char *s, fake_mode *out)
{
    static const struct { const char *name; fake_mode mode; } table[] = {
        { "ok", M_OK }, { "split", M_SPLIT }, { "early-eof", M_EARLY_EOF },
        { "partial-head", M_PARTIAL_HEAD }, { "garbage", M_GARBAGE },
        { "silent", M_SILENT }, { "hold", M_HOLD }, { "drip", M_DRIP },
    };
    size_t i;

    for (i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (strcmp(s, table[i].name) == 0) {
            *out = table[i].mode;
            return 0;
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    struct sockaddr_in addr;
    fake_mode mode;
    char *end;
    long port;
    int lfd, one = 1;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> <ok|split|early-eof|partial-head|garbage|silent|hold|drip>\n", argv[0]);
        return 2;
    }
    errno = 0;
    port = strtol(argv[1], &end, 10);
    if (errno != 0 || *argv[1] == '\0' || *end != '\0' || port < 1 || port > 65535 || parse_mode(argv[2], &mode) < 0) {
        fprintf(stderr, "%s: bad port or mode\n", argv[0]);
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);   /* 끊긴 클라이언트에 쓰면 EPIPE 로 받는다 (서버와 같은 방식) */

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        perror("socket");
        return 1;
    }
    (void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(lfd, 16) < 0) {
        perror("bind/listen");
        return 1;
    }
    printf("ready\n");
    fflush(stdout);

    for (;;) {
        int fd = accept(lfd, NULL, NULL);

        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            perror("accept");
            return 1;
        }
        if (serve(fd, mode) == 0)
            close(fd);
    }
}
