/*
 * test_proxy_config.c — 프록시 설정 파일 읽기와 라우트 선택 (이슈 #8)
 *
 * 설정은 mkstemp 로 만든 임시 파일에 써서 proxy_config_load 에 넘긴다.
 * 로더가 stderr 에 내는 오류 메시지는 dup2 로 임시 파일에 받아서 검사한다
 * (CHECK 가 stderr 에 쓰므로 CHECK 전에 되돌려 놓는다).
 * 이름 해석(DNS)은 어떤 테스트도 하지 않는다: 숫자 주소, 대괄호 IPv6, "localhost"(/etc/hosts) 만 쓴다.
 * 주소 조회 순서가 중요한 테스트는 이 파일에서 getaddrinfo 를 대신 정의해서(아래 fake_gai)
 * "fixture-proxy.test" 만 정해진 결과로 돌려주고, 다른 이름은 진짜 getaddrinfo 에 넘긴다.
 */
#define _GNU_SOURCE                 /* RTLD_NEXT */
#define _DARWIN_C_SOURCE
#include "test.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <dlfcn.h>
#include <unistd.h>

#include "handler.h"
#include "http_request.h"
#include "proxy_config.h"

static proxy_config cfg;            /* 27KB 쯤이라 스택에 두지 않는다 */
static const proxy_config zero_cfg; /* 전부 0 */
static char g_path[64];
static char g_msg[1024];

/* ---- getaddrinfo 대체 (실행 파일에 정의하면 proxy_config.o 의 호출이 이쪽으로 온다) ----
 * g_fake_mode: 0 = 항상 진짜, 1 = "fixture-proxy.test" 를 [::1, 127.0.0.1] 순서로, 2 = [::1] 만 */
#define FAKE_HOST "fixture-proxy.test"
static int g_fake_mode;
static int g_fake_calls;
static struct sockaddr_in6 g_fake6;
static struct sockaddr_in g_fake4;
static struct addrinfo g_fake_ai[2];

typedef int (*gai_fn)(const char *, const char *, const struct addrinfo *, struct addrinfo **);
typedef void (*freeai_fn)(struct addrinfo *);

int getaddrinfo(const char *host, const char *service, const struct addrinfo *hints,
                struct addrinfo **res)
{
    if (g_fake_mode != 0 && host != NULL && strcmp(host, FAKE_HOST) == 0) {
        int port = service != NULL ? atoi(service) : 0;

        g_fake_calls++;
        memset(g_fake_ai, 0, sizeof g_fake_ai);
        memset(&g_fake6, 0, sizeof g_fake6);
        memset(&g_fake4, 0, sizeof g_fake4);
        g_fake6.sin6_family = AF_INET6;
        g_fake6.sin6_port = htons((unsigned short)port);
        g_fake6.sin6_addr = in6addr_loopback;
        g_fake4.sin_family = AF_INET;
        g_fake4.sin_port = htons((unsigned short)port);
        g_fake4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        g_fake_ai[0].ai_family = AF_INET6;
        g_fake_ai[0].ai_socktype = SOCK_STREAM;
        g_fake_ai[0].ai_addr = (struct sockaddr *)&g_fake6;
        g_fake_ai[0].ai_addrlen = sizeof g_fake6;
        if (g_fake_mode == 1) {
            g_fake_ai[0].ai_next = &g_fake_ai[1];
            g_fake_ai[1].ai_family = AF_INET;
            g_fake_ai[1].ai_socktype = SOCK_STREAM;
            g_fake_ai[1].ai_addr = (struct sockaddr *)&g_fake4;
            g_fake_ai[1].ai_addrlen = sizeof g_fake4;
        }
        *res = &g_fake_ai[0];
        return 0;
    } else {
        void *sym = dlsym(RTLD_NEXT, "getaddrinfo");
        gai_fn real;

        memcpy(&real, &sym, sizeof real);
        return real(host, service, hints, res);
    }
}

void freeaddrinfo(struct addrinfo *res)
{
    void *sym;
    freeai_fn real;

    if (res == &g_fake_ai[0])
        return;                     /* 정적 배열이라 해제할 것이 없다 */
    sym = dlsym(RTLD_NEXT, "freeaddrinfo");
    memcpy(&real, &sym, sizeof real);
    real(res);
}

/* 설정 파일 경로를 로드하면서 stderr 를 g_msg 로 받는다 */
static int load_path(const char *path)
{
    char errp[] = "/tmp/proxy_err_XXXXXX";
    int efd, saved, rc;
    ssize_t n;

    fflush(stderr);
    saved = dup(2);
    efd = mkstemp(errp);
    if (saved < 0 || efd < 0 || dup2(efd, 2) < 0) {
        perror("redirect stderr");
        exit(2);
    }
    rc = proxy_config_load(path, &cfg);
    fflush(stderr);
    dup2(saved, 2);                 /* CHECK 가 stderr 에 쓰기 전에 되돌린다 */
    close(saved);
    lseek(efd, 0, SEEK_SET);
    n = read(efd, g_msg, sizeof g_msg - 1);
    g_msg[n < 0 ? 0 : n] = '\0';
    close(efd);
    unlink(errp);
    return rc;
}

static int conf_load_n(const char *text, size_t n)
{
    int fd, rc;
    size_t off = 0;

    strcpy(g_path, "/tmp/proxy_conf_XXXXXX");
    fd = mkstemp(g_path);
    if (fd < 0) {
        perror("mkstemp");
        exit(2);
    }
    while (off < n) {
        ssize_t w = write(fd, text + off, n - off);
        if (w < 0) {
            perror("write");
            exit(2);
        }
        off += (size_t)w;
    }
    close(fd);
    rc = load_path(g_path);
    unlink(g_path);
    return rc;
}

static int conf_load(const char *text)
{
    return conf_load_n(text, strlen(text));
}

static void check_zeroed(void)
{
    CHECK_INT(cfg.nroutes, 0);
    CHECK_INT(cfg.routes[0].nbackends, 0);
    CHECK_INT(cfg.routes[0].prefix[0], 0);
    CHECK(memcmp(&cfg, &zero_cfg, sizeof cfg) == 0);    /* 일부만 지운 것이 아니라 전부 0 */
}

/* 줄 번호가 붙은 오류: -1, cfg 전부 0, "<경로>:<줄>: ...<substr>..." */
static void expect_err(int rc, size_t line, const char *substr)
{
    char prefix[128];

    CHECK_INT(rc, -1);
    check_zeroed();
    snprintf(prefix, sizeof prefix, "%s:%zu: ", g_path, line);
    CHECK(strncmp(g_msg, prefix, strlen(prefix)) == 0);
    CHECK(strstr(g_msg, substr) != NULL);
    if (strstr(g_msg, substr) == NULL)
        fprintf(stderr, "    message was: %s", g_msg);
}

/* 파일 전체에 대한 오류: "<경로>: ...<substr>..." */
static void expect_err_file(int rc, const char *substr)
{
    char prefix[128];

    CHECK_INT(rc, -1);
    check_zeroed();
    snprintf(prefix, sizeof prefix, "%s: ", g_path);
    CHECK(strncmp(g_msg, prefix, strlen(prefix)) == 0);
    CHECK(strstr(g_msg, substr) != NULL);
}

static void check_v4(const proxy_backend *be, const char *ip, int port)
{
    const struct sockaddr_in *a = (const struct sockaddr_in *)&be->addr;
    char buf[INET_ADDRSTRLEN] = "";

    CHECK_INT(be->addr.ss_family, AF_INET);
    CHECK_INT(ntohs(a->sin_port), port);
    inet_ntop(AF_INET, &a->sin_addr, buf, sizeof buf);
    CHECK_STR(buf, ip);
    CHECK_INT(be->addrlen, sizeof(struct sockaddr_in));
}

static void check_v6(const proxy_backend *be, const char *ip, int port)
{
    const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)&be->addr;
    char buf[INET6_ADDRSTRLEN] = "";

    CHECK_INT(be->addr.ss_family, AF_INET6);
    CHECK_INT(ntohs(a->sin6_port), port);
    inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof buf);
    CHECK_STR(buf, ip);
    CHECK_INT(be->addrlen, sizeof(struct sockaddr_in6));
}

/* ---- 로드: 성공 ---- */

static void test_load_basic(void)             /* C1 */
{
    CHECK_INT(conf_load("/api/ 127.0.0.1:8001 127.0.0.1:8003\n/static/ 127.0.0.1:8002\n"), 0);
    CHECK_INT(g_msg[0], 0);
    REQUIRE(cfg.nroutes == 2);
    CHECK_STR(cfg.routes[0].prefix, "/api/");
    CHECK_INT(cfg.routes[0].nbackends, 2);
    CHECK_STR(cfg.routes[0].backends[0].name, "127.0.0.1:8001");
    CHECK_STR(cfg.routes[0].backends[1].name, "127.0.0.1:8003");
    check_v4(&cfg.routes[0].backends[0], "127.0.0.1", 8001);
    check_v4(&cfg.routes[0].backends[1], "127.0.0.1", 8003);
    CHECK_INT(cfg.routes[0].next, 0);
    CHECK_STR(cfg.routes[1].prefix, "/static/");
    CHECK_INT(cfg.routes[1].nbackends, 1);
    check_v4(&cfg.routes[1].backends[0], "127.0.0.1", 8002);
}

static void test_load_format_tolerance(void)  /* C2 */
{
    CHECK_INT(conf_load("# 머리 주석\r\n\r\n   \t\r\n\t/api/\t 127.0.0.1:8001   # 꼬리 주석\r\n# end"), 0);
    CHECK_INT(g_msg[0], 0);
    REQUIRE(cfg.nroutes == 1);
    CHECK_STR(cfg.routes[0].prefix, "/api/");
    REQUIRE(cfg.routes[0].nbackends == 1);
    check_v4(&cfg.routes[0].backends[0], "127.0.0.1", 8001);
}

static void test_load_last_line_without_lf(void)   /* C3 */
{
    CHECK_INT(conf_load("/a/ 127.0.0.1:1"), 0);
    CHECK_INT(g_msg[0], 0);
    REQUIRE(cfg.nroutes == 1);
    check_v4(&cfg.routes[0].backends[0], "127.0.0.1", 1);
}

static void test_load_ipv6(void)              /* C4 */
{
    CHECK_INT(conf_load("/v6/ [::1]:8001\n"), 0);
    CHECK_INT(g_msg[0], 0);
    REQUIRE(cfg.nroutes == 1 && cfg.routes[0].nbackends == 1);
    check_v6(&cfg.routes[0].backends[0], "::1", 8001);
    CHECK_STR(cfg.routes[0].backends[0].name, "[::1]:8001");
}

/* C5: /etc/hosts 에서 풀리는 이름만 쓴다 (DNS 없음). localhost 가 IPv6 도 돌려주는 환경(Docker, macOS)에서
 * IPv4 선호가 실제로 검사된다. 이 WSL 처럼 IPv4 만 돌려주면 단순 확인이다 */
static void test_load_localhost_prefers_ipv4(void)
{
    struct addrinfo hints, *res = NULL;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("localhost", "8001", &hints, &res) != 0) {
        printf("  skip localhost (no IPv4 result)\n");
        return;
    }
    freeaddrinfo(res);
    CHECK_INT(conf_load("/l/ localhost:8001\n"), 0);
    CHECK_INT(g_msg[0], 0);
    REQUIRE(cfg.nroutes == 1 && cfg.routes[0].nbackends == 1);
    check_v4(&cfg.routes[0].backends[0], "127.0.0.1", 8001);
    CHECK_STR(cfg.routes[0].backends[0].name, "localhost:8001");
}

/* C5b: ip6-localhost (/etc/hosts 에서 ::1 만) 이 있는 환경이면 IPv6 대체 선택이 검사된다.
 * AF_UNSPEC 로 조회해서 결과가 전부 IPv6 일 때만 쓴다 (AF_INET 조회는 files 에서 못 찾으면 DNS 로 간다) */
static void test_load_name_ipv6_only_fallback(void)
{
    struct addrinfo hints, *res = NULL, *p;
    int only6 = 1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("ip6-localhost", "8001", &hints, &res) != 0) {
        printf("  skip ip6-localhost (not in /etc/hosts)\n");
        return;
    }
    for (p = res; p != NULL; p = p->ai_next)
        if (p->ai_family != AF_INET6)
            only6 = 0;
    freeaddrinfo(res);
    if (!only6) {
        printf("  skip ip6-localhost (has IPv4 result)\n");
        return;
    }
    CHECK_INT(conf_load("/l/ ip6-localhost:8001\n"), 0);
    CHECK_INT(g_msg[0], 0);
    REQUIRE(cfg.nroutes == 1 && cfg.routes[0].nbackends == 1);
    check_v6(&cfg.routes[0].backends[0], "::1", 8001);
}

/* C5c: 조회 결과가 [::1, 127.0.0.1] 순서여도 IPv4 를 고른다 (조회는 한 번, 환경과 상관없이 결정적) */
static void test_load_prefers_ipv4_when_v6_first(void)
{
    g_fake_mode = 1;
    g_fake_calls = 0;
    CHECK_INT(conf_load("/fixture/ " FAKE_HOST ":8001\n"), 0);
    g_fake_mode = 0;
    CHECK_INT(g_fake_calls, 1);
    CHECK_INT(g_msg[0], 0);
    REQUIRE(cfg.nroutes == 1 && cfg.routes[0].nbackends == 1);
    check_v4(&cfg.routes[0].backends[0], "127.0.0.1", 8001);
    CHECK_STR(cfg.routes[0].backends[0].name, FAKE_HOST ":8001");
}

/* C5d: 이름이 IPv6 로만 풀리면 그 주소를 쓴다 */
static void test_load_name_v6_only_fake(void)
{
    g_fake_mode = 2;
    g_fake_calls = 0;
    CHECK_INT(conf_load("/fixture/ " FAKE_HOST ":8001\n"), 0);
    g_fake_mode = 0;
    CHECK_INT(g_fake_calls, 1);
    REQUIRE(cfg.nroutes == 1 && cfg.routes[0].nbackends == 1);
    check_v6(&cfg.routes[0].backends[0], "::1", 8001);
}

/* C13: 명세가 허용하는 접두사 글자를 모두 받고 입력 그대로 저장한다 */
static void test_load_prefix_all_allowed_chars(void)
{
    CHECK_INT(conf_load("/Az09-._~!$&'()*+,;=:@/ 127.0.0.1:1\n"), 0);
    CHECK_INT(g_msg[0], 0);
    REQUIRE(cfg.nroutes == 1);
    CHECK_STR(cfg.routes[0].prefix, "/Az09-._~!$&'()*+,;=:@/");
}

/* C14: 앞자리 0 이 없으면 0 이나 세 자리 옥텟도 받는다 */
static void test_load_ipv4_octet_edges(void)
{
    CHECK_INT(conf_load("/a/ 0.0.0.0:80 100.200.255.10:80\n"), 0);
    REQUIRE(cfg.nroutes == 1 && cfg.routes[0].nbackends == 2);
    check_v4(&cfg.routes[0].backends[0], "0.0.0.0", 80);
    check_v4(&cfg.routes[0].backends[1], "100.200.255.10", 80);
}

static void test_load_limits_ok(void)         /* C6 */
{
    static char text[8192];
    size_t n = 0, r, b;
    int port = 1;

    for (r = 0; r < 16; r++) {
        n += (size_t)snprintf(text + n, sizeof text - n, "/r%02zu/", r);
        for (b = 0; b < 8; b++)
            n += (size_t)snprintf(text + n, sizeof text - n, " 127.0.0.1:%d", port++);
        text[n++] = '\n';
    }
    CHECK_INT(conf_load_n(text, n), 0);
    REQUIRE(cfg.nroutes == 16);
    for (r = 0; r < 16; r++)
        CHECK_INT(cfg.routes[r].nbackends, 8);
    check_v4(&cfg.routes[15].backends[7], "127.0.0.1", 128);
}

static void test_load_port_boundaries(void)   /* C7 */
{
    CHECK_INT(conf_load("/a/ 127.0.0.1:1 127.0.0.1:65535\n"), 0);
    REQUIRE(cfg.nroutes == 1 && cfg.routes[0].nbackends == 2);
    check_v4(&cfg.routes[0].backends[0], "127.0.0.1", 1);
    check_v4(&cfg.routes[0].backends[1], "127.0.0.1", 65535);
}

static void test_load_prefix_127(void)        /* C8 */
{
    char text[256] = "/";

    memset(text + 1, 'a', 126);
    strcpy(text + 127, " 127.0.0.1:1\n");
    CHECK_INT(conf_load(text), 0);
    REQUIRE(cfg.nroutes == 1);
    CHECK_INT(strlen(cfg.routes[0].prefix), 127);
}

static void test_load_line_length_boundary(void)   /* C9 */
{
    static char text[1100];
    const char *route = "/a/ 127.0.0.1:1";
    size_t rl = strlen(route);

    memset(text, ' ', sizeof text);
    memcpy(text, route, rl);
    text[1024] = '\n';
    CHECK_INT(conf_load_n(text, 1025), 0);
    CHECK_INT(cfg.nroutes, 1);

    text[1024] = '\r';
    text[1025] = '\n';
    CHECK_INT(conf_load_n(text, 1026), 0);
    CHECK_INT(cfg.nroutes, 1);
}

static void test_load_prefix_slash_variants(void)  /* C10 */
{
    CHECK_INT(conf_load("/api 127.0.0.1:1\n/api/ 127.0.0.1:2\n"), 0);
    CHECK_INT(cfg.nroutes, 2);
}

static void test_load_reload_overwrites(void) /* C11 */
{
    CHECK_INT(conf_load("/api/ 127.0.0.1:8001 127.0.0.1:8003\n/static/ 127.0.0.1:8002\n"), 0);
    CHECK_INT(cfg.nroutes, 2);
    CHECK_INT(conf_load("/x/ 127.0.0.1:9\n"), 0);
    CHECK_INT(cfg.nroutes, 1);
    CHECK_STR(cfg.routes[0].prefix, "/x/");
    CHECK_INT(cfg.routes[1].prefix[0], 0);
    CHECK_INT(cfg.routes[0].nbackends, 1);
}

static void test_load_duplicate_backend_allowed(void)   /* C12 */
{
    CHECK_INT(conf_load("/a/ 127.0.0.1:1 127.0.0.1:1\n"), 0);
    REQUIRE(cfg.nroutes == 1);
    CHECK_INT(cfg.routes[0].nbackends, 2);
}

/* ---- 로드: 오류 ---- */

static void test_err_too_many_routes(void)    /* E1 */
{
    static char text[2048];
    size_t n = 0, r;

    for (r = 0; r < 17; r++)
        n += (size_t)snprintf(text + n, sizeof text - n, "/r%02zu/ 127.0.0.1:1\n", r);
    expect_err(conf_load_n(text, n), 17, "too many routes");
}

static void test_err_too_many_backends(void)  /* E2 */
{
    expect_err(conf_load("/a/ 127.0.0.1:1 127.0.0.1:2 127.0.0.1:3 127.0.0.1:4 127.0.0.1:5 "
                         "127.0.0.1:6 127.0.0.1:7 127.0.0.1:8 127.0.0.1:9\n"),
               1, "too many backends");
}

static void test_err_prefix(void)             /* E3 E4 E5 E6 */
{
    char text[256] = "/";

    memset(text + 1, 'a', 127);
    strcpy(text + 128, " 127.0.0.1:1\n");
    expect_err(conf_load(text), 1, "prefix too long");

    expect_err(conf_load("api/ 127.0.0.1:1\n"), 1, "must start with '/'");

    expect_err(conf_load("/a?b 127.0.0.1:1\n"), 1, "invalid character in prefix");
    expect_err(conf_load("/a%2F 127.0.0.1:1\n"), 1, "invalid character in prefix");
    expect_err(conf_load("/a#b 127.0.0.1:1\n"), 1, "invalid character in prefix");
    expect_err(conf_load("/\xED\x95\x9C/ 127.0.0.1:1\n"), 1, "invalid character in prefix");
    expect_err(conf_load("/a/\x01 127.0.0.1:1\n"), 1, "invalid character in prefix");
    expect_err(conf_load("/a/\r127.0.0.1:1\n"), 1, "invalid character in prefix");
}

static void test_err_no_backend(void)         /* E7 */
{
    expect_err(conf_load("/api/\n"), 1, "no backend");
    expect_err(conf_load("/api/   # c\n"), 1, "no backend");
}

static void test_err_duplicate_prefix(void)   /* E8 */
{
    expect_err(conf_load("/a/ 127.0.0.1:1\n/b/ 127.0.0.1:2\n/a/ 127.0.0.1:3\n"), 3, "duplicate prefix");
    CHECK(strstr(g_msg, "first on line 1") != NULL);
}

static void test_err_invalid_backend(void)    /* E9 */
{
    static const char *const bad[] = {
        "/a/ 127.0.0.1\n", "/a/ :8001\n", "/a/ ::1:8001\n", "/a/ [::1:8001\n",
        "/a/ [::1]8001\n", "/a/ []:8001\n", "/a/ host!:80\n",
        "/a/ [::g]:8001\n",            /* 대괄호 안 문자 검사 (16진수, ':', '.' 만) */
        "/a/ [fe80::1%lo]:8001\n",     /* zone id 는 getaddrinfo 가 받아 주므로 문자 검사가 막아야 한다 */
        "/a/ a:1:2\n",                 /* 마지막 ':' 에서 나눈다 → 호스트 "a:1" 이 잘못 */
        "/a/ 1:80\n", "/a/ 127.1:81\n", "/a/ 0:83\n", "/a/ 1.2.3:80\n",   /* inet_aton 옛 형식 */
        "/a/ 256.0.0.1:80\n", "/a/ 1.2.3.4.5:80\n", "/a/ 1..2.3:80\n",
        /* 앞자리 0: getaddrinfo 는 8진수로 읽어 127.0.0.010 을 127.0.0.8 로 푼다 */
        "/a/ 127.0.0.010:80\n", "/a/ 010.0.0.1:80\n", "/a/ 00.0.0.1:80\n", "/a/ 127.0.0.01:80\n",
        /* 자릿수가 많은 옥텟 (UBSan: 누적하다 정수가 넘치지 않아야 한다. 토큰 59바이트) */
        "/a/ 99999999999999999999999999999999999999999999999999.0.0.1:80\n",
        "/a/ 1.2.3.99999999999999999999999999999999999999999999999999:80\n",
    };
    size_t i;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++)
        expect_err(conf_load(bad[i]), 1, "invalid backend");
}

static void test_err_invalid_port(void)       /* E10 */
{
    static const char *const bad[] = {
        "/a/ 127.0.0.1:\n", "/a/ 127.0.0.1:0\n", "/a/ 127.0.0.1:65536\n", "/a/ 127.0.0.1:08001\n",
        "/a/ 127.0.0.1:80a\n", "/a/ 127.0.0.1:+80\n", "/a/ 127.0.0.1:123456\n",
        "/a/ 127.0.0.1:1#c\n", "/a/ 127.0.0.1:99999999999999999999\n",   /* 자리수 상한 (UBSan: 정수 넘침 방지) */
    };
    size_t i;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++)
        expect_err(conf_load(bad[i]), 1, "invalid port");
}

static void test_err_backend_name_length(void)     /* E11 */
{
    char text[256];
    size_t n;

    /* 63바이트: 길이 검사를 통과해 해석에서 실패 (대괄호라서 DNS 는 하지 않는다) */
    n = (size_t)snprintf(text, sizeof text, "/a/ [%.*s]:8001\n", 56,
                         "00000000000000000000000000000000000000000000000000000000000000");
    CHECK_INT(n, 4 + 63 + 1);
    expect_err(conf_load(text), 1, "cannot resolve");

    /* 64바이트: 길이에서 거절 */
    snprintf(text, sizeof text, "/a/ [%.*s]:8001\n", 57,
             "00000000000000000000000000000000000000000000000000000000000000");
    expect_err(conf_load(text), 1, "backend name too long");
}

static void test_err_resolve(void)            /* E12 E13 */
{
    /* 대괄호는 AI_NUMERICHOST 로 해석하므로 "abc" 로 DNS 를 묻지 않는다 */
    expect_err(conf_load("/a/ [abc]:8001\n"), 1, "cannot resolve");
    /* IPv4 주소를 대괄호에 넣음. 메시지 문구는 libc 마다 달라서 줄 번호까지만 본다 (substr "" 은 항상 맞음) */
    expect_err(conf_load("/a/ [127.0.0.1]:8001\n"), 1, "");
}

static void test_err_after_good_line(void)    /* E14 */
{
    expect_err(conf_load("/a/ 127.0.0.1:1\nbad 127.0.0.1:2\n"), 2, "must start with '/'");
    CHECK_INT(cfg.routes[0].nbackends, 0);      /* 첫 줄의 라우트까지 전부 지워졌다 (check_zeroed 가 전체 바이트를 본다) */
}

/* E14b: 오류 메시지는 줄바꿈 하나로 끝나는 한 줄 */
static void test_err_message_single_line(void)
{
    const char *nl;

    expect_err(conf_load("api/ 127.0.0.1:1\n"), 1, "must start with '/'");
    nl = strchr(g_msg, '\n');
    REQUIRE(nl != NULL);
    CHECK_INT(nl[1], 0);
}

/* E14c: LF 없이 CR 로 끝난 마지막 줄은 줄 끝이 아니다 */
static void test_err_trailing_lone_cr(void)
{
    expect_err(conf_load("/a/ 127.0.0.1:1\r"), 1, "invalid");
    expect_err(conf_load("/a/ 127.0.0.1:1\n/b/ 127.0.0.1:2\r"), 2, "invalid");
    CHECK_INT(conf_load("/a/ 127.0.0.1:1\r\n"), 0);      /* CRLF 는 그대로 받는다 */
    CHECK_INT(cfg.nroutes, 1);
}

static void test_err_line_too_long(void)      /* E15 */
{
    static char text[1200];
    const char *route = "/a/ 127.0.0.1:1";

    memset(text, ' ', sizeof text);
    memcpy(text, route, strlen(route));
    text[1025] = '\n';
    expect_err(conf_load_n(text, 1026), 1, "line too long");

    text[1025] = '\r';
    text[1026] = '\n';
    expect_err(conf_load_n(text, 1027), 1, "line too long");

    /* 줄 번호는 주석·빈 줄도 센다 */
    memmove(text + 6, text, 1027);
    memcpy(text, "#a\n#b\n", 6);
    expect_err(conf_load_n(text, 1033), 3, "line too long");
}

static void test_err_nul_byte(void)           /* E16 */
{
    static const char text[] = "/a/ 127.0.0.1:1\0x\n";

    expect_err(conf_load_n(text, sizeof text - 1), 1, "NUL byte");
}

static void test_err_no_routes(void)          /* E17 */
{
    expect_err_file(conf_load(""), "no routes");
    expect_err_file(conf_load("# x\n\n"), "no routes");
}

static void test_err_missing_file(void)       /* E18 */
{
    int fd;

    strcpy(g_path, "/tmp/proxy_conf_XXXXXX");
    fd = mkstemp(g_path);
    REQUIRE(fd >= 0);
    close(fd);
    unlink(g_path);
    expect_err_file(load_path(g_path), "cannot open");
}

static void test_err_directory(void)          /* E19 (glibc 에서 확인. musl/macOS 는 미확인) */
{
    strcpy(g_path, "/tmp");
    expect_err_file(load_path(g_path), "read error");
}

static void test_err_null_args(void)          /* E20 */
{
    memset(&cfg, 0xAB, sizeof cfg);
    CHECK_INT(proxy_config_load(NULL, &cfg), -1);
    check_zeroed();
    CHECK_INT(proxy_config_load("/nonexistent/none", NULL), -1);
}

/* ---- proxy_match ---- */

static void set_routes(const char *const *prefixes, size_t n)
{
    size_t i;

    memset(&cfg, 0, sizeof cfg);
    for (i = 0; i < n; i++)
        strcpy(cfg.routes[i].prefix, prefixes[i]);
    cfg.nroutes = n;
}

typedef struct { const char *target; int idx; } match_case;

static const char *const m1_routes[] = { "/api", "/api/", "/api/v2/", "/s/" };
static const match_case m1_cases[] = {
    { "/api/v2/x", 2 }, { "/api/v2", 1 }, { "/api/x", 1 }, { "/api/", 1 }, { "/api/?q", 1 },
    { "/api", 0 }, { "/api?x=1", 0 }, { "/apix", -1 }, { "/ap", -1 }, { "/s/a?b=/api/", 3 },
    { "/x?/api/", -1 }, { "/API/x", -1 }, { "/%61pi/x", -1 }, { "", -1 },
};

static void test_match_longest_and_boundary(void)   /* M1 */
{
    size_t i;

    set_routes(m1_routes, 4);
    for (i = 0; i < sizeof m1_cases / sizeof m1_cases[0]; i++) {
        proxy_route *r = proxy_match(&cfg, m1_cases[i].target);
        proxy_route *want = m1_cases[i].idx < 0 ? NULL : &cfg.routes[m1_cases[i].idx];

        if (r != want)
            fprintf(stderr, "    target \"%s\"\n", m1_cases[i].target);
        CHECK(r == want);
    }
}

static void test_match_only_slashless(void)   /* M2 */
{
    const char *const routes[] = { "/api" };

    set_routes(routes, 1);
    CHECK(proxy_match(&cfg, "/api/x") == &cfg.routes[0]);
    CHECK(proxy_match(&cfg, "/api-x") == NULL);
}

static void test_match_root_catch_all(void)   /* M3 */
{
    const char *const routes[] = { "/", "/api/" };

    set_routes(routes, 2);
    CHECK(proxy_match(&cfg, "/anything") == &cfg.routes[0]);
    CHECK(proxy_match(&cfg, "/api/x") == &cfg.routes[1]);
    CHECK(proxy_match(&cfg, "/") == &cfg.routes[0]);
}

static void test_match_order_independent(void)     /* M4 */
{
    const char *const rev[] = { "/s/", "/api/v2/", "/api/", "/api" };
    size_t i;

    set_routes(rev, 4);
    for (i = 0; i < sizeof m1_cases / sizeof m1_cases[0]; i++) {
        proxy_route *r = proxy_match(&cfg, m1_cases[i].target);

        if (m1_cases[i].idx < 0)
            CHECK(r == NULL);
        else if (r == NULL)
            CHECK(r != NULL);
        else
            CHECK_STR(r->prefix, m1_routes[m1_cases[i].idx]);
    }
}

static void test_match_long_prefix(void)      /* M5 */
{
    char slash[128], plain[128], t[160];
    const char *routes[2];

    slash[0] = '/';
    memset(slash + 1, 'a', 125);
    slash[126] = '/';
    slash[127] = '\0';
    plain[0] = '/';
    memset(plain + 1, 'b', 126);
    plain[127] = '\0';
    routes[0] = slash;
    routes[1] = plain;
    set_routes(routes, 2);

    CHECK(proxy_match(&cfg, slash) == &cfg.routes[0]);
    snprintf(t, sizeof t, "%sz", slash);        /* '/' 로 끝나는 접두사는 뒤가 뭐든 맞는다 */
    CHECK(proxy_match(&cfg, t) == &cfg.routes[0]);
    CHECK(proxy_match(&cfg, plain) == &cfg.routes[1]);
    snprintf(t, sizeof t, "%s/z", plain);
    CHECK(proxy_match(&cfg, t) == &cfg.routes[1]);
    snprintf(t, sizeof t, "%sz", plain);
    CHECK(proxy_match(&cfg, t) == NULL);
}

static void test_match_null_and_empty(void)   /* M6 */
{
    const char *const routes[] = { "/a/" };

    set_routes(routes, 1);
    CHECK(proxy_match(NULL, "/x") == NULL);
    CHECK(proxy_match(&cfg, NULL) == NULL);
    memset(&cfg, 0, sizeof cfg);
    CHECK(proxy_match(&cfg, "/a/") == NULL);
}

static void test_handler_route_integration(void)   /* M7 */
{
    static http_request req;
    const char *const routes[] = { "/api/" };
    proxy_route *route = NULL;
    char *buf;
    size_t len, used;
    int st;

    set_routes(routes, 1);

    buf = test_bytes("GET /api/add?a=1&b=2 HTTP/1.1\r\nHost: x\r\n\r\n", &len);
    REQUIRE(http_parse_request(buf, len, NULL, &req, &used, &st) == PARSE_DONE);
    CHECK_INT(handler_route(&req, &cfg, &route), ROUTE_PROXY);
    CHECK(route == &cfg.routes[0]);
    free(buf);

    buf = test_bytes("GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n", &len);
    REQUIRE(http_parse_request(buf, len, NULL, &req, &used, &st) == PARSE_DONE);
    route = &cfg.routes[0];
    CHECK_INT(handler_route(&req, &cfg, &route), ROUTE_STATIC);
    CHECK(route == NULL);
    free(buf);

    /* 표기만 바꾼 "/%61pi/" 는 프록시도 앱도 아니다 (결정 9e4e39a) */
    buf = test_bytes("GET /%61pi/add HTTP/1.1\r\nHost: x\r\n\r\n", &len);
    REQUIRE(http_parse_request(buf, len, NULL, &req, &used, &st) == PARSE_DONE);
    route = &cfg.routes[0];
    CHECK_INT(handler_route(&req, &cfg, &route), ROUTE_STATIC);
    CHECK(route == NULL);
    free(buf);
}

int main(void)
{
    RUN(test_load_basic);
    RUN(test_load_format_tolerance);
    RUN(test_load_last_line_without_lf);
    RUN(test_load_ipv6);
    RUN(test_load_localhost_prefers_ipv4);
    RUN(test_load_name_ipv6_only_fallback);
    RUN(test_load_prefers_ipv4_when_v6_first);
    RUN(test_load_name_v6_only_fake);
    RUN(test_load_prefix_all_allowed_chars);
    RUN(test_load_ipv4_octet_edges);
    RUN(test_load_limits_ok);
    RUN(test_load_port_boundaries);
    RUN(test_load_prefix_127);
    RUN(test_load_line_length_boundary);
    RUN(test_load_prefix_slash_variants);
    RUN(test_load_reload_overwrites);
    RUN(test_load_duplicate_backend_allowed);
    RUN(test_err_too_many_routes);
    RUN(test_err_too_many_backends);
    RUN(test_err_prefix);
    RUN(test_err_no_backend);
    RUN(test_err_duplicate_prefix);
    RUN(test_err_invalid_backend);
    RUN(test_err_invalid_port);
    RUN(test_err_backend_name_length);
    RUN(test_err_resolve);
    RUN(test_err_after_good_line);
    RUN(test_err_message_single_line);
    RUN(test_err_trailing_lone_cr);
    RUN(test_err_line_too_long);
    RUN(test_err_nul_byte);
    RUN(test_err_no_routes);
    RUN(test_err_missing_file);
    RUN(test_err_directory);
    RUN(test_err_null_args);
    RUN(test_match_longest_and_boundary);
    RUN(test_match_only_slashless);
    RUN(test_match_root_catch_all);
    RUN(test_match_order_independent);
    RUN(test_match_long_prefix);
    RUN(test_match_null_and_empty);
    RUN(test_handler_route_integration);
    return test_report();
}
