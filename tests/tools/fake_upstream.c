/*
 * fake_upstream.c — 프록시 스모크 테스트용 가짜 백엔드 (이슈 #9)
 *
 * 실제 백엔드(같은 바이너리)는 항상 올바른 응답만 준다. 깨진 머리, 일찍 끊기는 본문, 대답 없는 서버,
 * 한 바이트씩 오는 응답처럼 프록시가 502·504·연결 종료로 처리해야 하는 응답을 만들어 내는 도구다.
 * 서버 코드와 링크하지 않는 독립 실행 파일이다. 테스트 전용이라 쓰기는 블로킹이고 연결은 하나씩 순서대로 처리한다.
 *
 * 사용법: fake_upstream <port> <mode> [logfile]
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
 *   host          200, 본문은 받은 요청의 Host 헤더 값 + "\n" (프록시가 백엔드마다 Host 를 새로 쓰는지 확인. 페일오버로
 *                 넘어온 요청의 Host 가 처음 고른 백엔드가 아니라 이 백엔드의 이름이어야 한다)
 *   blackhole     연결을 받지 않는다 (accept 를 안 한다). 대기열(backlog)을 스스로 연결해서 가득 채워 두므로 이 뒤에 오는
 *                 connect 는 SYN 이 버려져 끝나지 않고 멈춘다 (프록시의 connect 시간 초과·페일오버 확인용).
 *                 대기열을 채울 수 있으면 "ready", 이 환경이 SYN 을 버리지 않아 못 채우면 "unsupported" 를 찍고 끝낸다
 *                 (테스트는 이때 그 검사를 건너뛴다. "RST 로 답함"은 ECONNREFUSED·ECONNRESET 뿐이다). 시스템 호출 오류(fd 고갈 등)와
 *                 그 밖의 연결 오류(ENETUNREACH 등)는 "unsupported" 가 아니라 stderr 에 이유를 찍고
 *                 ready 없이 끝낸다 (시작 실패로 보여야 하고 조용한 건너뜀이 되면 안 된다). 다른 모드와 달리 요청을 읽지 않는다
 *
 * 세 번째 인자 logfile (선택): 요청을 받을 때마다 "<요청 줄> body=<받은 본문 바이트 수>" 한 줄을 이 파일에 덧붙인다
 * (요청을 읽은 직후, 모드별 응답 전). 페일오버 테스트가 "이 백엔드가 요청을 몇 번 받았나"를 줄 수로 센다.
 * blackhole 은 요청을 받지 않으므로 기록하지 않는다. 기록 파일은 ready 를 찍기 전에 만들어 두므로(요청이 0건이어도 파일은 있다)
 * 못 열면 시작 실패이고, 기록 중 쓰기에 실패하면 프로세스가 끝난다 (테스트가 "파일 없음·기록 실패"를 "요청 0건"으로 읽지 않게)
 *
 * silent 의 연결은 닫지 않고 프로세스가 죽을 때까지 둔다 (fd 를 일부러 흘린다. 테스트 한 번 분량이라 적다).
 * 요청을 5초 안에 다 못 보내는 클라이언트는 버린다 (연결만 열고 말이 없는 클라이언트 하나가 순서 처리를 막지 않게).
 *
 * 빌드: make 가 build/tools/fake_upstream 으로 만든다 (-std=c11 -D_POSIX_C_SOURCE=200809L, glibc·musl·macOS)
 */
#include <errno.h>
#include <fcntl.h>
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

typedef enum { M_OK, M_SPLIT, M_EARLY_EOF, M_PARTIAL_HEAD, M_GARBAGE, M_SILENT, M_HOLD, M_DRIP, M_HOST, M_BLACKHOLE } fake_mode;

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

/* 머리 끝 "\r\n\r\n" 의 시작 위치. 아직 안 왔으면 len. 머리 줄은 [0, 이 값)에 있고 본문은 이 값 + 4 부터다 */
static size_t find_head_end(const char *buf, size_t len)
{
    size_t i;

    for (i = 0; i + 4 <= len; i++)
        if (memcmp(buf + i, "\r\n\r\n", 4) == 0)
            return i;
    return len;
}

/* 요청 머리가 "\r\n\r\n" 까지 도착했는지 */
static int has_head_end(const char *buf, size_t len)
{
    return find_head_end(buf, len) < len;
}

/* 요청 기록 파일의 fd (세 번째 인자). -1 이면 기록하지 않는다. main 이 ready 를 찍기 전에 열어 둔다:
 * 파일이 있다는 것 자체가 "이 가짜가 기록할 수 있다"는 증거라서, 테스트는 없는 파일을 요청 0건으로 읽지 않는다 */
static int g_log_fd = -1;

/* 대소문자 무시로 s 가 p(소문자)로 시작하는지 */
static int starts_with_ci(const char *s, size_t avail, const char *p)
{
    size_t i;

    for (i = 0; p[i] != '\0'; i++) {
        char c;

        if (i >= avail)
            return 0;
        c = s[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != p[i])
            return 0;
    }
    return 1;
}

/* 요청 하나를 g_log_fd 에 한 줄로 덧붙인다: "<요청 줄> body=<받은 본문 바이트 수>\n". 머리 뒤에 Content-Length 만큼 본문이
 * 더 올 때까지 읽어서 센다 (프록시가 본문을 이 백엔드에 몇 번, 몇 바이트 보냈는지 테스트가 줄 수와 숫자로 확인한다).
 * 연결을 하나씩 처리하므로 줄이 섞이지 않는다. 쓰기에 실패하면 프로세스를 끝낸다 (아래) */
static void log_request(int fd, const char *req, size_t len)
{
    char line[512];
    size_t he = 0, i, rl = 0, have, want = 0, off;

    while (he + 4 <= len && memcmp(req + he, "\r\n\r\n", 4) != 0)
        he++;
    he += 4;                                    /* 머리 끝 (본문 시작). read_request 가 머리를 확인했다 */
    while (rl < he && req[rl] != '\r' && rl < sizeof line - 32)
        rl++;
    for (i = 0; i + 16 < he; i++)
        if (req[i] == '\n' && starts_with_ci(req + i + 1, he - i - 1, "content-length:")) {
            for (i += 16; i < he && req[i] == ' '; i++)
                ;
            for (; i < he && req[i] >= '0' && req[i] <= '9'; i++)
                want = want * 10 + (size_t)(req[i] - '0');
            break;
        }
    have = len - he;
    while (have < want) {
        char tmp[4096];
        struct pollfd pfd;
        ssize_t r;
        int pr;

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pr = poll(&pfd, 1, REQUEST_WAIT_MS);
        if (pr < 0 && errno == EINTR)
            continue;
        if (pr <= 0)
            break;
        r = read(fd, tmp, want - have < sizeof tmp ? want - have : sizeof tmp);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        have += (size_t)r;
    }
    i = (size_t)snprintf(line, sizeof line, "%.*s body=%zu\n", (int)rl, req, have);
    if (i >= sizeof line)
        i = sizeof line - 1;
    for (off = 0; off < i;) {
        ssize_t w = write(g_log_fd, line + off, i - off);

        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            /* 기록을 못 남기면 테스트가 "요청 0건"과 구별하지 못한다. 조용히 계속 답하지 말고 죽는다 */
            perror("fake_upstream: log write");
            exit(1);
        }
        off += (size_t)w;
    }
}

/* 요청 머리를 buf(REQUEST_MAX 바이트)에 읽는다. 성공 0 (받은 길이는 *outlen), EOF·오류·시간 초과·너무 김 -1.
 * 본문은 읽지 않는다 (테스트의 가짜 경로는 GET. 본문이 있는 요청을 세려면 log_request 가 읽는다) */
static int read_request(int fd, char *buf, size_t *outlen)
{
    size_t len = 0;

    while (!has_head_end(buf, len)) {
        struct pollfd pfd;
        ssize_t r;
        int pr;

        if (len == REQUEST_MAX)
            return -1;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pr = poll(&pfd, 1, REQUEST_WAIT_MS);
        if (pr < 0 && errno == EINTR)
            continue;
        if (pr <= 0)
            return -1;
        r = read(fd, buf + len, REQUEST_MAX - len);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        len += (size_t)r;
    }
    *outlen = len;
    return 0;
}

/* host 모드: 요청 머리에서 "Host: " 줄의 값을 찾아 응답을 만든다. 없으면 빈 값.
 * 찾는 범위는 머리 줄([0, 머리 끝))뿐이다: req 는 머리 뒤 본문 바이트까지 담고 있을 수 있어서, 본문에 "\r\nHost: " 가 있어도 쓰지 않는다 */
static int write_host_reply(int fd, const char *req, size_t len)
{
    char out[512], value[256];
    size_t i, vl = 0, hend = find_head_end(req, len);
    int n;

    value[0] = '\0';
    for (i = 0; i + 8 <= hend; i++) {
        if (memcmp(req + i, "\r\nHost: ", 8) == 0) {
            for (i += 8; i < hend && req[i] != '\r' && vl < sizeof value - 1; i++)
                value[vl++] = req[i];
            value[vl] = '\0';
            break;
        }
    }
    n = snprintf(out, sizeof out,
                 "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s\n",
                 vl + 1, value);
    return n > 0 ? write_all(fd, out, (size_t)n) : -1;
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
    char body40[40], req[REQUEST_MAX];
    size_t reqlen = 0;
    int one = 1;

    /* 1바이트 쓰기가 Nagle 에 합쳐지지 않게. 실패해도 테스트가 약해질 뿐이라 무시 */
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    if (read_request(fd, req, &reqlen) < 0)
        return 0;
    if (g_log_fd >= 0)
        log_request(fd, req, reqlen);

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
    case M_HOST:
        (void)write_host_reply(fd, req, reqlen);
        break;
    case M_BLACKHOLE:   /* main 이 따로 처리한다 (여기까지 오지 않는다) */
        break;
    }
    return 0;
}

/* blackhole: 대기열이 가득 차서 새 연결의 SYN 이 버려지는 상태를 만든다.
 * 자기 자신에게 논블로킹 connect 를 반복해 대기열을 채우고, 한 번 연결이 300 ms 안에 끝나지 않으면 가득 찬 것으로 본다
 * (listen 의 backlog 가 0 이면 리눅스는 연결 하나까지 받는다). 연결 fd 는 닫지 않고 둔다 (닫으면 대기열이 비어 풀린다).
 * 성공 1, 이 커널이 가득 찬 대기열에 SYN 을 버리지 않아 채울 수 없으면 0 (ECONNREFUSED·ECONNRESET 으로 RST 를 보내거나
 * backlog 를 늘려 잡는다), 시스템 호출 오류(fd 고갈)와 그 밖의 SO_ERROR 는 -1 로 이유를 찍는다. 0 과 -1 을 섞으면 환경 문제가 "미지원"으로 보여
 * 테스트가 connect 시간 초과 검사를 조용히 건너뛴다 */
static int fill_backlog(const struct sockaddr_in *addr)
{
    int i;

    for (i = 0; i < 16; i++) {
        struct pollfd p;
        int fd = socket(AF_INET, SOCK_STREAM, 0), flags, rc, err = 0;
        socklen_t el = sizeof err;

        if (fd < 0) {
            perror("fake_upstream: socket");
            return -1;
        }
        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            perror("fake_upstream: fcntl");
            return -1;
        }
        rc = connect(fd, (const struct sockaddr *)addr, sizeof *addr);
        if (rc < 0 && errno != EINPROGRESS) {
            perror("fake_upstream: connect");
            return -1;
        }
        p.fd = fd;
        p.events = POLLOUT;
        p.revents = 0;
        do {
            rc = poll(&p, 1, 300);
        } while (rc < 0 && errno == EINTR);   /* 시그널로 끊기면 같은 연결을 다시 기다린다 */
        if (rc < 0) {
            perror("fake_upstream: poll");
            return -1;
        }
        if (rc == 0)
            return 1;                   /* 연결이 끝나지 않는다: 가득 찼다 */
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0) {
            perror("fake_upstream: getsockopt");
            return -1;
        }
        if (err == ECONNREFUSED || err == ECONNRESET)
            return 0;                   /* 커널이 SYN 을 버리지 않고 RST 로 답했다: 이 환경은 blackhole 을 못 만든다 (미지원) */
        if (err != 0) {
            /* 그 밖의 오류(ENETUNREACH, EADDRNOTAVAIL, ETIMEDOUT 등)는 환경이 "SYN 을 버린다"가 아니라 다른 문제다.
             * 미지원으로 돌리면 테스트가 connect 시간 초과 검사를 조용히 건너뛰므로 시작 실패로 알린다 */
            fprintf(stderr, "fake_upstream: backlog fill connect: %s\n", strerror(err));
            return -1;
        }
        /* 연결됐다: 이 fd 는 열어 둔 채 다음 연결로 */
    }
    return 0;                           /* 16개가 다 연결됐다: 이 커널은 backlog 를 늘려 잡는다 */
}

static int parse_mode(const char *s, fake_mode *out)
{
    static const struct { const char *name; fake_mode mode; } table[] = {
        { "ok", M_OK }, { "split", M_SPLIT }, { "early-eof", M_EARLY_EOF },
        { "partial-head", M_PARTIAL_HEAD }, { "garbage", M_GARBAGE },
        { "silent", M_SILENT }, { "hold", M_HOLD }, { "drip", M_DRIP }, { "host", M_HOST }, { "blackhole", M_BLACKHOLE },
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

    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s <port> <ok|split|early-eof|partial-head|garbage|silent|hold|drip|host|blackhole> [logfile]\n",
                argv[0]);
        return 2;
    }
    errno = 0;
    port = strtol(argv[1], &end, 10);
    if (errno != 0 || *argv[1] == '\0' || *end != '\0' || port < 1 || port > 65535 || parse_mode(argv[2], &mode) < 0) {
        fprintf(stderr, "%s: bad port or mode\n", argv[0]);
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);   /* 끊긴 클라이언트에 쓰면 EPIPE 로 받는다 (서버와 같은 방식) */

    /* 기록 파일은 ready 를 찍기 전에 만들어 둔다: 못 열면 시작 실패 (테스트는 ready 를 못 봐서 가짜 시작 실패로 센다).
     * 요청이 하나도 없어도 파일은 있으므로, 테스트는 "파일이 없다"를 "요청 0건"으로 읽지 않는다 */
    if (argc == 4) {
        g_log_fd = open(argv[3], O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (g_log_fd < 0) {
            perror("fake_upstream: open log");
            return 1;
        }
    }

    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        perror("socket");
        return 1;
    }
    (void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(lfd, mode == M_BLACKHOLE ? 0 : 16) < 0) {
        perror("bind/listen");
        return 1;
    }
    if (mode == M_BLACKHOLE) {
        int filled = fill_backlog(&addr);

        if (filled < 0)
            return 1;                   /* 시스템 호출 오류: "unsupported" 가 아니라 시작 실패 (ready 를 못 본 테스트가 실패로 센다) */
        if (filled == 0) {
            printf("unsupported\n");
            fflush(stdout);
            return 0;
        }
        printf("ready\n");
        fflush(stdout);
        for (;;)
            pause();                    /* accept 하지 않는다. 죽일 때까지 대기열을 채운 채로 둔다 */
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
