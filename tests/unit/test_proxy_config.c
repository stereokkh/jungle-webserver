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

static proxy_config cfg;            /* 29KB 쯤이라 스택에 두지 않는다 */
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

/* E21: 정규화된 target 에 없는 모양의 접두사 (빈 조각, 점 조각) 는 죽은 라우트라 시작할 때 거절 */
static void test_err_prefix_not_canonical(void)
{
    static const char *const bad[] = {
        "//", "//api", "/a//b", "/api//",
        "/.", "/..", "/./api", "/a/../b", "/a/.", "/a/./", "/a/..",
    };
    char text[64];
    size_t i;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        snprintf(text, sizeof text, "%s 127.0.0.1:1\n", bad[i]);
        expect_err(conf_load(text), 1, "not in canonical form");
    }
}

/* C15: 점이 들어가도 "."·".." 조각이 아니면 정규형이다 */
static void test_load_prefix_dots_not_segments(void)
{
    CHECK_INT(conf_load("/ 127.0.0.1:1\n/.well-known/ 127.0.0.1:2\n/a./ 127.0.0.1:3\n"
                        "/.../ 127.0.0.1:4\n/a..b 127.0.0.1:5\n/..a/ 127.0.0.1:6\n"), 0);
    CHECK_INT(g_msg[0], 0);
    CHECK_INT(cfg.nroutes, 6);
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

    /* 표기만 바꾼 "/%61pi/" 도 파서 정규형이 "/api/" 라 같은 프록시 라우트다 (#31, 결정 9e4e39a 를 바꿈) */
    buf = test_bytes("GET /%61pi/add HTTP/1.1\r\nHost: x\r\n\r\n", &len);
    REQUIRE(http_parse_request(buf, len, NULL, &req, &used, &st) == PARSE_DONE);
    route = NULL;
    CHECK_INT(handler_route(&req, &cfg, &route), ROUTE_PROXY);
    CHECK(route == &cfg.routes[0]);
    free(buf);
}

/* ---- 페일오버 옵션: backup=, failover=, tries= (2026-11-25) ---- */

static void test_opt_defaults(void)
{
    REQUIRE(conf_load("/a/ 127.0.0.1:1 127.0.0.1:2\n") == 0);
    CHECK_INT(cfg.routes[0].nbackends, 2);
    CHECK_INT(cfg.routes[0].failover, 0);               /* 없으면 off (예전 동작) */
    CHECK_INT(cfg.routes[0].tries, 2);                  /* 없으면 주 + 백업 전부 */
    CHECK_INT(cfg.routes[0].next, 0);
    CHECK_INT(cfg.routes[0].backends[0].is_backup, 0);
    CHECK_INT(cfg.routes[0].backends[1].is_backup, 0);
    CHECK_INT(cfg.routes[0].backends[0].down_until_ms, 0);
}

static void test_opt_full_line(void)
{
    REQUIRE(conf_load("/api/ 127.0.0.1:18801 127.0.0.1:18802 backup=127.0.0.1:18803 failover=connect,timeout tries=3\n") == 0);
    CHECK_INT(cfg.nroutes, 1);
    CHECK_INT(cfg.routes[0].nbackends, 3);
    check_v4(&cfg.routes[0].backends[0], "127.0.0.1", 18801);
    check_v4(&cfg.routes[0].backends[2], "127.0.0.1", 18803);
    CHECK_STR(cfg.routes[0].backends[2].name, "127.0.0.1:18803");   /* Host 헤더가 되는 이름에 "backup=" 이 안 붙는다 */
    CHECK_INT(cfg.routes[0].backends[0].is_backup, 0);
    CHECK_INT(cfg.routes[0].backends[1].is_backup, 0);
    CHECK_INT(cfg.routes[0].backends[2].is_backup, 1);
    CHECK_INT(cfg.routes[0].failover, PROXY_FAILOVER_CONNECT | PROXY_FAILOVER_TIMEOUT);
    CHECK_INT(cfg.routes[0].tries, 3);
}

/* 옵션은 접두사 뒤 어디에나. 백엔드 인덱스는 줄의 등장 순서이고, Round-Robin 은 주 백엔드끼리만 돈다 */
static void test_opt_anywhere_and_order(void)
{
    proxy_route *rt;
    size_t i;
    static const int want[] = { 1, 2, 1, 2 };

    REQUIRE(conf_load("/a/ backup=127.0.0.1:3 failover=timeout 127.0.0.1:1 tries=2 127.0.0.1:2\n") == 0);
    rt = &cfg.routes[0];
    CHECK_INT(rt->nbackends, 3);
    CHECK_INT(rt->backends[0].is_backup, 1);
    check_v4(&rt->backends[0], "127.0.0.1", 3);
    check_v4(&rt->backends[1], "127.0.0.1", 1);
    check_v4(&rt->backends[2], "127.0.0.1", 2);
    CHECK_INT(rt->failover, PROXY_FAILOVER_TIMEOUT);
    CHECK_INT(rt->tries, 2);
    for (i = 0; i < 4; i++)
        CHECK_INT(proxy_pick_backend(rt, 0, 0, proxy_rr_begin(rt)), want[i]);
}

static void test_opt_failover_values(void)
{
    REQUIRE(conf_load("/a/ 127.0.0.1:1 failover=off\n") == 0);
    CHECK_INT(cfg.routes[0].failover, 0);
    REQUIRE(conf_load("/a/ 127.0.0.1:1 failover=connect\n") == 0);
    CHECK_INT(cfg.routes[0].failover, PROXY_FAILOVER_CONNECT);
    REQUIRE(conf_load("/a/ 127.0.0.1:1 failover=timeout\n") == 0);
    CHECK_INT(cfg.routes[0].failover, PROXY_FAILOVER_TIMEOUT);
    REQUIRE(conf_load("/a/ 127.0.0.1:1 failover=timeout,connect\n") == 0);
    CHECK_INT(cfg.routes[0].failover, PROXY_FAILOVER_CONNECT | PROXY_FAILOVER_TIMEOUT);
}

/* backup= 이나 tries= 가 failover=off(또는 없음)와 같이 있어도 오류가 아니다. 백업은 안 쓰이고 tries 는 효과가 없다 */
static void test_opt_backup_tries_with_failover_off(void)
{
    REQUIRE(conf_load("/a/ 127.0.0.1:1 backup=127.0.0.1:2 tries=2 failover=off\n") == 0);
    CHECK_INT(cfg.routes[0].failover, 0);
    CHECK_INT(cfg.routes[0].backends[1].is_backup, 1);
    CHECK_INT(cfg.routes[0].tries, 2);
    REQUIRE(conf_load("/a/ 127.0.0.1:1 backup=127.0.0.1:2\n") == 0);   /* failover 줄이 없어도 */
    CHECK_INT(cfg.routes[0].failover, 0);
    /* 그리고 실제로 안 쓰인다: 주 백엔드 하나가 계속 고른다 (모든 요청이 첫 시도에서 끝난다) */
    CHECK_INT(proxy_pick_backend(&cfg.routes[0], 0, 0, proxy_rr_begin(&cfg.routes[0])), 0);
    proxy_mark_down(&cfg.routes[0], 0, 0);                              /* failover 꺼짐: 표시 안 함 */
    CHECK_INT(proxy_pick_backend(&cfg.routes[0], 0, 0, proxy_rr_begin(&cfg.routes[0])), 0);
}

static void test_opt_tries_values(void)
{
    REQUIRE(conf_load("/a/ 127.0.0.1:1 127.0.0.1:2 127.0.0.1:3 backup=127.0.0.1:4\n") == 0);
    CHECK_INT(cfg.routes[0].tries, 4);                  /* 기본은 주 + 백업 */
    REQUIRE(conf_load("/a/ 127.0.0.1:1 tries=1\n") == 0);
    CHECK_INT(cfg.routes[0].tries, 1);
    REQUIRE(conf_load("/a/ 127.0.0.1:1 tries=8\n") == 0);
    CHECK_INT(cfg.routes[0].tries, 8);                  /* 백엔드 수보다 커도 오류는 아니다 (더 고를 후보가 없을 뿐) */
}

static void test_opt_backup_ipv6_and_many(void)
{
    REQUIRE(conf_load("/a/ 127.0.0.1:1 backup=[::1]:8001\n") == 0);
    check_v6(&cfg.routes[0].backends[1], "::1", 8001);
    CHECK_STR(cfg.routes[0].backends[1].name, "[::1]:8001");
    CHECK_INT(cfg.routes[0].backends[1].is_backup, 1);
    /* 주 5 + 백업 3 = 8 은 된다 */
    REQUIRE(conf_load("/a/ 127.0.0.1:1 127.0.0.1:2 127.0.0.1:3 127.0.0.1:4 127.0.0.1:5 "
                      "backup=127.0.0.1:6 backup=127.0.0.1:7 backup=127.0.0.1:8\n") == 0);
    CHECK_INT(cfg.routes[0].nbackends, 8);
    CHECK_INT(cfg.routes[0].tries, 8);
    /* 같은 주소를 주와 백업에 같이 써도 된다 (C12 와 같다) */
    REQUIRE(conf_load("/a/ 127.0.0.1:1 backup=127.0.0.1:1\n") == 0);
}

static void test_err_opt_duplicate(void)
{
    expect_err(conf_load("/a/ 127.0.0.1:1 failover=connect failover=timeout\n"), 1, "duplicate option 'failover'");
    expect_err(conf_load("/a/ 127.0.0.1:1 failover=off failover=off\n"), 1, "duplicate option 'failover'");
    expect_err(conf_load("/a/ 127.0.0.1:1 tries=2 tries=2\n"), 1, "duplicate option 'tries'");
}

static void test_err_opt_failover_values(void)
{
    expect_err(conf_load("/a/ 127.0.0.1:1 failover=off,connect\n"), 1, "cannot be combined");
    expect_err(conf_load("/a/ 127.0.0.1:1 failover=connect,off\n"), 1, "cannot be combined");
    expect_err(conf_load("/a/ 127.0.0.1:1 failover=timeout,off,connect\n"), 1, "cannot be combined");
    expect_err(conf_load("/a/ 127.0.0.1:1 failover=off,off\n"), 1, "duplicate failover value");
    expect_err(conf_load("/a/ 127.0.0.1:1 failover=bogus\n"), 1, "unknown failover value 'bogus'");
    expect_err(conf_load("/a/ 127.0.0.1:1 failover=connect,bogus\n"), 1, "unknown failover value 'bogus'");
    expect_err(conf_load("/a/ 127.0.0.1:1 failover=Connect\n"), 1, "unknown failover value");   /* 대소문자 구분 */
    expect_err(conf_load("/a/ 127.0.0.1:1 failover=connect,connect\n"), 1, "duplicate failover value 'connect'");
    expect_err(conf_load("/a/ 127.0.0.1:1 failover=timeout,connect,timeout\n"), 1, "duplicate failover value 'timeout'");
    expect_err(conf_load("/a/ 127.0.0.1:1 failover=connect,,timeout\n"), 1, "empty item");
    expect_err(conf_load("/a/ 127.0.0.1:1 failover=,connect\n"), 1, "empty item");
    expect_err(conf_load("/a/ 127.0.0.1:1 failover=connect,\n"), 1, "empty item");
    expect_err(conf_load("/a/ 127.0.0.1:1 failover=,\n"), 1, "empty item");
    expect_err(conf_load("/a/ 127.0.0.1:1 failover=\n"), 1, "no value");
}

static void test_err_opt_tries_values(void)
{
    expect_err(conf_load("/a/ 127.0.0.1:1 tries=0\n"), 1, "invalid tries '0'");
    expect_err(conf_load("/a/ 127.0.0.1:1 tries=9\n"), 1, "invalid tries '9'");
    expect_err(conf_load("/a/ 127.0.0.1:1 tries=07\n"), 1, "invalid tries '07'");
    expect_err(conf_load("/a/ 127.0.0.1:1 tries=00\n"), 1, "invalid tries");
    expect_err(conf_load("/a/ 127.0.0.1:1 tries=10\n"), 1, "invalid tries '10'");
    expect_err(conf_load("/a/ 127.0.0.1:1 tries=100\n"), 1, "invalid tries");
    expect_err(conf_load("/a/ 127.0.0.1:1 tries=99999999999999999999\n"), 1, "invalid tries");
    expect_err(conf_load("/a/ 127.0.0.1:1 tries=-1\n"), 1, "invalid tries");
    expect_err(conf_load("/a/ 127.0.0.1:1 tries=+1\n"), 1, "invalid tries");
    expect_err(conf_load("/a/ 127.0.0.1:1 tries=1x\n"), 1, "invalid tries");
    expect_err(conf_load("/a/ 127.0.0.1:1 tries=x\n"), 1, "invalid tries");
    expect_err(conf_load("/a/ 127.0.0.1:1 tries=2,3\n"), 1, "invalid tries");
    expect_err(conf_load("/a/ 127.0.0.1:1 tries=\n"), 1, "no value");
}

static void test_err_opt_shape(void)
{
    expect_err(conf_load("/a/ 127.0.0.1:1 foo=bar\n"), 1, "unknown option 'foo'");
    expect_err(conf_load("/a/ 127.0.0.1:1 Backup=127.0.0.1:2\n"), 1, "unknown option 'Backup'");   /* 대소문자 구분 */
    expect_err(conf_load("/a/ 127.0.0.1:1 foo=\n"), 1, "unknown option 'foo'");
    expect_err(conf_load("/a/ 127.0.0.1:1 =x\n"), 1, "no name");
    expect_err(conf_load("/a/ 127.0.0.1:1 =\n"), 1, "no name");
    expect_err(conf_load("/a/ 127.0.0.1:1 backup=\n"), 1, "no value");
    expect_err(conf_load("/a/ 127.0.0.1:1 127.0.0.1:2=3\n"), 1, "unknown option");   /* 백엔드 토큰에 '=' 를 붙인 오타 */
    expect_err(conf_load("/a/ 127.0.0.1:1 backup=1=2:80\n"), 1, "invalid backend '1=2:80'");   /* 이름에 '=' */
    expect_err(conf_load("/a/ 127.0.0.1:1 backup=nocolon\n"), 1, "invalid backend 'nocolon'");
    expect_err(conf_load("/a/ 127.0.0.1:1 backup=127.0.0.1:0\n"), 1, "invalid port");
    expect_err(conf_load("/a/ 127.0.0.1:1 backup=127.0.0.1:08001\n"), 1, "invalid port");
    expect_err(conf_load("/a/ 127.0.0.1:1 backup=127.0.0.1:2 backup=bad\n"), 1, "invalid backend 'bad'");
    expect_err(conf_load("/ok/ 127.0.0.1:1\n/a/ 127.0.0.1:1 failover=zzz\n"), 2, "unknown failover value");   /* 줄 번호 */
}

static void test_err_opt_too_long(void)
{
    char text[256];
    int n;

    /* 71바이트 옵션 토큰: "backup=" + 63바이트 이름이 한도(70)라서 이름이 63바이트를 넘는 backup= 도 여기서 걸린다.
     * (한도 안의 긴 이름은 getaddrinfo 가 이름 해석을 시도하므로, 이 파일의 "DNS 안 함" 약속 때문에 시험하지 않는다) */
    n = snprintf(text, sizeof text, "/a/ 127.0.0.1:1 backup=%.*s:80\n", 61, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    REQUIRE(n > 0 && (size_t)n < sizeof text);
    expect_err(conf_load(text), 1, "option too long");
    n = snprintf(text, sizeof text, "/a/ 127.0.0.1:1 failover=%.*s\n", 80, "connect,connect,connect,connect,connect,connect,connect,connect,connect,connect");
    REQUIRE(n > 0 && (size_t)n < sizeof text);
    expect_err(conf_load(text), 1, "option too long");
}

static void test_err_opt_backends(void)
{
    /* 백업뿐인 줄 */
    expect_err(conf_load("/a/ backup=127.0.0.1:1\n"), 1, "no primary backend");
    expect_err(conf_load("/a/ backup=127.0.0.1:1 backup=127.0.0.1:2 failover=connect\n"), 1, "no primary backend");
    /* 옵션뿐인 줄은 백엔드가 없다 */
    expect_err(conf_load("/a/ failover=connect tries=2\n"), 1, "no backend for prefix");
    /* 주 + 백업 합계가 8 을 넘음 (어느 쪽이 9번째든) */
    expect_err(conf_load("/a/ 127.0.0.1:1 127.0.0.1:2 127.0.0.1:3 127.0.0.1:4 127.0.0.1:5 "
                         "backup=127.0.0.1:6 backup=127.0.0.1:7 backup=127.0.0.1:8 backup=127.0.0.1:9\n"),
               1, "too many backends");
    expect_err(conf_load("/a/ backup=127.0.0.1:1 backup=127.0.0.1:2 backup=127.0.0.1:3 backup=127.0.0.1:4 "
                         "backup=127.0.0.1:5 backup=127.0.0.1:6 backup=127.0.0.1:7 127.0.0.1:8 127.0.0.1:9\n"),
               1, "too many backends");
    expect_err(conf_load("/a/ 127.0.0.1:1 127.0.0.1:2 127.0.0.1:3 127.0.0.1:4 127.0.0.1:5 127.0.0.1:6 "
                         "127.0.0.1:7 127.0.0.1:8 failover=connect backup=127.0.0.1:9\n"),
               1, "too many backends");
}

/* 주석은 옵션에서도 토큰 맨 앞에서만 */
static void test_opt_comment(void)
{
    REQUIRE(conf_load("/a/ 127.0.0.1:1 failover=connect # tries=bogus backup=\n") == 0);
    CHECK_INT(cfg.routes[0].failover, PROXY_FAILOVER_CONNECT);
    CHECK_INT(cfg.routes[0].tries, 1);
}

/* ---- 백엔드 고르기: proxy_rr_begin / proxy_pick_backend / proxy_mark_down (순수 함수, 시각은 인자) ---- */

static proxy_route mk_rt;

/* layout 글자 하나가 백엔드 하나: 'P' 주, 'B' 백업 (줄의 등장 순서). tries 는 백엔드 수로 둔다 */
static proxy_route *mk(const char *layout, unsigned failover)
{
    size_t i;

    memset(&mk_rt, 0, sizeof mk_rt);
    for (i = 0; layout[i] != '\0'; i++)
        mk_rt.backends[i].is_backup = layout[i] == 'B';
    mk_rt.nbackends = strlen(layout);
    mk_rt.failover = failover;
    mk_rt.tries = mk_rt.nbackends;
    return &mk_rt;
}

#define PICK(rt, now, tried, rr) proxy_pick_backend((rt), (now), (tried), (rr))
#define BIT(i) (1u << (i))

/* 페일오버가 꺼진 라우트: 고르는 순서가 #9 의 Round-Robin 과 같다 (0, 1, ..., n-1, 0 ...). 백업은 절대 안 나온다 */
static void test_pick_rr_same_as_old(void)
{
    proxy_route *rt = mk("PPP", 0);
    size_t i;

    for (i = 0; i < 7; i++)
        CHECK_INT(PICK(rt, 0, 0, proxy_rr_begin(rt)), i % 3);
    CHECK_INT(rt->next, 7 % 3);

    rt = mk("BPBP", 0);                                  /* 주 백엔드는 인덱스 1, 3. 순번은 "주 중 몇 번째" */
    for (i = 0; i < 6; i++) {
        size_t rr = proxy_rr_begin(rt);

        CHECK_INT(rr, i % 2);
        CHECK_INT(PICK(rt, 0, 0, rr), i % 2 == 0 ? 1 : 3);
    }
    rt = mk("P", 0);
    for (i = 0; i < 3; i++)
        CHECK_INT(PICK(rt, 0, 0, proxy_rr_begin(rt)), 0);
    CHECK_INT(rt->next, 0);
}

/* 페일오버가 꺼진 라우트는 rr_start 번째 주 백엔드 하나뿐이다: 이미 시도했으면 백업이 있어도 다음 주 백엔드나 백업으로 넘어가지 않고 -1 */
static void test_pick_failover_off_single_candidate(void)
{
    proxy_route *rt = mk("PBP", 0);                      /* 주 백엔드는 인덱스 0, 2. 백업은 1 */

    CHECK_INT(PICK(rt, 0, 0, 0), 0);
    CHECK_INT(PICK(rt, 0, 0, 1), 2);
    CHECK_INT(PICK(rt, 0, BIT(0), 0), -1);               /* 시도한 것: 다음 주 백엔드(2)도 백업(1)도 아니다 */
    CHECK_INT(PICK(rt, 0, BIT(2), 1), -1);
    CHECK_INT(PICK(rt, 0, BIT(1), 0), 0);                /* 백업을 시도했다는 비트는 이 후보와 상관없다 */
    rt->backends[0].down_until_ms = 1000000;             /* 손으로 단 표시도 꺼진 라우트에서는 보지 않는다 */
    CHECK_INT(PICK(rt, 0, 0, 0), 0);
    rt = mk("BB", 0);                                    /* 주 백엔드가 없으면 백업을 대신 고르지 않는다 */
    CHECK_INT(PICK(rt, 0, 0, 0), -1);
}

static void test_pick_skip_down(void)
{
    proxy_route *rt = mk("PPP", PROXY_FAILOVER_CONNECT);

    proxy_mark_down(rt, 1, 1000);
    CHECK_INT(rt->backends[1].down_until_ms, 1000 + PROXY_BACKEND_DOWN_MS);
    CHECK_INT(PICK(rt, 1000, 0, 0), 0);
    CHECK_INT(PICK(rt, 1000, 0, 1), 2);                  /* 시작 위치가 죽은 것이면 RR 순서로 다음 */
    CHECK_INT(PICK(rt, 1000, 0, 2), 2);
    CHECK_INT(PICK(rt, 1000, BIT(2), 1), 0);             /* 2 는 해 봤고 1 은 죽음 → 한 바퀴 돌아 0 */
    CHECK_INT(PICK(rt, 1000, BIT(2) | BIT(0), 1), -1);   /* 남은 건 죽은 1 뿐이고 0, 2 는 죽지 않았다 → "전부 죽음"이 아니라 후보 없음 (아래 all-down 규칙) */
}

static void test_pick_skip_tried(void)
{
    proxy_route *rt = mk("PPP", PROXY_FAILOVER_CONNECT);

    CHECK_INT(PICK(rt, 0, BIT(0), 0), 1);
    CHECK_INT(PICK(rt, 0, BIT(0) | BIT(1), 0), 2);
    CHECK_INT(PICK(rt, 0, BIT(0) | BIT(1) | BIT(2), 0), -1);
    CHECK_INT(PICK(rt, 0, BIT(1), 1), 2);                /* 시작 위치 1 은 해 봤다 → 2 */
    CHECK_INT(PICK(rt, 0, BIT(1) | BIT(2), 1), 0);       /* 한 바퀴 돌아 0 */
    CHECK_INT(PICK(rt, 0, 0, 7), 1);                     /* 범위 밖 시작 위치는 나머지 (7 % 3 == 1) */
}

static void test_pick_backups_only_after_primaries(void)
{
    proxy_route *rt = mk("PPBB", PROXY_FAILOVER_CONNECT);   /* 인덱스 2, 3 이 백업 (이 순서로 쓴다) */

    CHECK_INT(PICK(rt, 0, 0, 0), 0);
    CHECK_INT(PICK(rt, 0, BIT(0), 0), 1);
    CHECK_INT(PICK(rt, 0, BIT(0) | BIT(1), 0), 2);       /* 주가 다 소진돼서야 백업 */
    CHECK_INT(PICK(rt, 0, BIT(0) | BIT(1) | BIT(2), 0), 3);
    CHECK_INT(PICK(rt, 0, BIT(0) | BIT(1) | BIT(2) | BIT(3), 0), -1);
    /* 주 백엔드가 살아 있는 동안 백업은 안 나온다 (죽은 것은 주가 아니라 백업 쪽으로) */
    proxy_mark_down(rt, 0, 100);
    CHECK_INT(PICK(rt, 100, 0, 0), 1);
    proxy_mark_down(rt, 1, 100);
    CHECK_INT(PICK(rt, 100, 0, 0), 2);                   /* 주가 둘 다 죽음 → 안 죽은 백업을 먼저 */
    proxy_mark_down(rt, 2, 100);
    CHECK_INT(PICK(rt, 100, 0, 0), 3);                   /* 백업 순서: 2 가 죽었으면 3 */
    /* 백업의 RR 은 없다: 시작 위치와 무관하게 적은 순서 */
    rt = mk("BPBP", PROXY_FAILOVER_CONNECT);              /* 주 1, 3 / 백업 0, 2 */
    CHECK_INT(PICK(rt, 0, BIT(1) | BIT(3), 0), 0);
    CHECK_INT(PICK(rt, 0, BIT(1) | BIT(3), 1), 0);
    CHECK_INT(PICK(rt, 0, BIT(1) | BIT(3) | BIT(0), 1), 2);
}

/* 전부 죽은 표시여도 기다리지 않고 (주 RR → 백업) 순서로 안 해 본 것을 고른다 */
static void test_pick_all_down_fallback(void)
{
    proxy_route *rt = mk("PPB", PROXY_FAILOVER_CONNECT | PROXY_FAILOVER_TIMEOUT);

    proxy_mark_down(rt, 0, 0);
    proxy_mark_down(rt, 1, 0);
    proxy_mark_down(rt, 2, 0);
    CHECK_INT(PICK(rt, 100, 0, 0), 0);
    CHECK_INT(PICK(rt, 100, 0, 1), 1);
    CHECK_INT(PICK(rt, 100, BIT(0), 0), 1);
    CHECK_INT(PICK(rt, 100, BIT(0) | BIT(1), 0), 2);
    CHECK_INT(PICK(rt, 100, BIT(0) | BIT(1) | BIT(2), 0), -1);
    /* 일부만 죽었으면 그 일부를 건너뛴다: 라우트의 백엔드가 "전부" 죽었을 때만 표시를 무시 (시도해 본 것도 센다) */
    rt = mk("PPB", PROXY_FAILOVER_CONNECT);
    proxy_mark_down(rt, 0, 0);
    proxy_mark_down(rt, 2, 0);
    CHECK_INT(PICK(rt, 100, 0, 0), 1);
    /* 1 은 해 봤고(표시는 이 요청 중에 만료됐다고 보자) 남은 0, 2 는 죽음. 1 이 안 죽었으니 전부 죽은 게 아니다 → 후보 없음 */
    CHECK_INT(PICK(rt, 100, BIT(1), 0), -1);
    /* 같은 상황에서 1 도 죽은 표시면 전부 죽음 → 표시를 무시하고 안 해 본 주 0 부터 */
    proxy_mark_down(rt, 1, 0);
    CHECK_INT(PICK(rt, 100, BIT(1), 0), 0);
    CHECK_INT(PICK(rt, 100, BIT(1) | BIT(0), 0), 2);
}

/* 죽은 표시는 시간으로만 풀린다 */
static void test_pick_down_expires_by_time(void)
{
    proxy_route *rt = mk("PP", PROXY_FAILOVER_TIMEOUT);

    proxy_mark_down(rt, 0, 5000);
    CHECK_INT(PICK(rt, 5000, 0, 0), 1);
    CHECK_INT(PICK(rt, 5000 + PROXY_BACKEND_DOWN_MS - 1, 0, 0), 1);   /* 만료 직전까지 죽음 */
    CHECK_INT(PICK(rt, 5000 + PROXY_BACKEND_DOWN_MS, 0, 0), 0);       /* 만료 시각부터 살아남 */
    CHECK_INT(PICK(rt, 5000 + PROXY_BACKEND_DOWN_MS + 1, 0, 0), 0);
    CHECK_INT(PICK(rt, 4000, 0, 0), 1);                               /* 표시한 시각보다 이른 now 도 죽음 */
    proxy_mark_down(rt, 0, 9000);                                     /* 다시 부르면 만료가 새로 잡힌다 */
    CHECK_INT(rt->backends[0].down_until_ms, 9000 + PROXY_BACKEND_DOWN_MS);
}

static void test_mark_down_off_and_bounds(void)
{
    proxy_route *rt = mk("PP", 0);
    proxy_route before;

    proxy_mark_down(rt, 0, 1000);                        /* 페일오버 꺼짐: 기억하지 않는다 */
    CHECK_INT(rt->backends[0].down_until_ms, 0);
    CHECK_INT(PICK(rt, 1000, 0, 0), 0);

    rt = mk("PP", PROXY_FAILOVER_CONNECT);
    memcpy(&before, rt, sizeof before);
    proxy_mark_down(rt, 2, 1000);                        /* 범위 밖 (nbackends == 2) */
    proxy_mark_down(rt, 100, 1000);
    CHECK(memcmp(&before, rt, sizeof before) == 0);
    proxy_mark_down(rt, 1, 1000);
    CHECK_INT(rt->backends[0].down_until_ms, 0);
    CHECK_INT(rt->backends[1].down_until_ms, 1000 + PROXY_BACKEND_DOWN_MS);
}

/* pick 은 route 를 바꾸지 않는다 (RR 순번은 proxy_rr_begin 만 민다) */
static void test_pick_does_not_mutate(void)
{
    proxy_route *rt = mk("PBP", PROXY_FAILOVER_CONNECT);
    proxy_route before;

    proxy_mark_down(rt, 0, 0);
    memcpy(&before, rt, sizeof before);
    (void)PICK(rt, 5, 0, 1);
    (void)PICK(rt, 5, BIT(2), 0);
    (void)PICK(rt, 5, 0xFFu, 0);
    CHECK(memcmp(&before, rt, sizeof before) == 0);
    CHECK_INT(PICK(rt, 5, 0xFFu, 0), -1);
}

/* server.c 의 up_start_attempt 와 같은 반복: 시도마다 실패한다고 치고(죽은 표시) 고른 순서를 order 에 쌓는다.
 * tries 한도는 pick 이 아니라 호출자가 센다 (proxy_config.h) — 여기서는 server.c 처럼 호출자가 센다 */
static size_t attempt_all_fail(proxy_route *rt, int64_t now, int *order)
{
    size_t rr = proxy_rr_begin(rt), n = 0;
    unsigned tried = 0;

    while (n < rt->tries) {
        int i = PICK(rt, now, tried, rr);

        if (i < 0)
            break;
        order[n++] = i;
        tried |= BIT(i);
        proxy_mark_down(rt, (size_t)i, now);
    }
    return n;
}

static void test_pick_tries_is_callers_job(void)
{
    int order[PROXY_MAX_BACKENDS];
    proxy_route *rt = mk("PPPB", PROXY_FAILOVER_CONNECT);

    /* pick 은 tries 를 모른다: 호출자가 세지 않으면 백엔드 수만큼 계속 고른다 */
    rt->tries = 99;
    CHECK_INT(attempt_all_fail(rt, 0, order), 4);
    CHECK_INT(order[0], 0); CHECK_INT(order[1], 1); CHECK_INT(order[2], 2); CHECK_INT(order[3], 3);

    rt = mk("PPPB", PROXY_FAILOVER_CONNECT);
    rt->tries = 2;
    CHECK_INT(attempt_all_fail(rt, 0, order), 2);
    CHECK_INT(order[0], 0); CHECK_INT(order[1], 1);

    /* 다음 요청은 순번 1 에서 시작. 방금 0, 1 이 죽은 표시라 안 죽은 주 2 가 첫 시도, 그다음은 안 죽은 백업 3 */
    CHECK_INT(attempt_all_fail(rt, 100, order), 2);
    CHECK_INT(order[0], 2); CHECK_INT(order[1], 3);

    /* 모든 주가 죽은 표시면 안 죽은 백업이 첫 시도 */
    rt = mk("PPB", PROXY_FAILOVER_CONNECT);
    rt->tries = 3;
    proxy_mark_down(rt, 0, 0);
    proxy_mark_down(rt, 1, 0);
    CHECK_INT(attempt_all_fail(rt, 10, order), 3);
    CHECK_INT(order[0], 2); CHECK_INT(order[1], 0); CHECK_INT(order[2], 1);
}

/* 설정 파일에서 읽은 라우트로 한 번 더: 주/백업 섞인 줄 */
static void test_pick_with_loaded_route(void)
{
    proxy_route *rt;
    int order[PROXY_MAX_BACKENDS];

    REQUIRE(conf_load("/a/ backup=127.0.0.1:9 127.0.0.1:1 127.0.0.1:2 failover=connect tries=3\n") == 0);
    rt = &cfg.routes[0];
    CHECK_INT(attempt_all_fail(rt, 0, order), 3);        /* 주 1, 2, 그다음 백업 0 */
    CHECK_INT(order[0], 1); CHECK_INT(order[1], 2); CHECK_INT(order[2], 0);
    CHECK_INT(proxy_rr_begin(rt), 1);                    /* 순번은 요청당 한 칸 (위 요청이 0 을 썼다). 주가 둘이라 0, 1, 0 ... */
    CHECK_INT(proxy_rr_begin(rt), 0);
}

int main(void)
{
    RUN(test_opt_defaults);
    RUN(test_opt_full_line);
    RUN(test_opt_anywhere_and_order);
    RUN(test_opt_failover_values);
    RUN(test_opt_backup_tries_with_failover_off);
    RUN(test_opt_tries_values);
    RUN(test_opt_backup_ipv6_and_many);
    RUN(test_opt_comment);
    RUN(test_err_opt_duplicate);
    RUN(test_err_opt_failover_values);
    RUN(test_err_opt_tries_values);
    RUN(test_err_opt_shape);
    RUN(test_err_opt_too_long);
    RUN(test_err_opt_backends);
    RUN(test_pick_rr_same_as_old);
    RUN(test_pick_failover_off_single_candidate);
    RUN(test_pick_skip_down);
    RUN(test_pick_skip_tried);
    RUN(test_pick_backups_only_after_primaries);
    RUN(test_pick_all_down_fallback);
    RUN(test_pick_down_expires_by_time);
    RUN(test_mark_down_off_and_bounds);
    RUN(test_pick_does_not_mutate);
    RUN(test_pick_tries_is_callers_job);
    RUN(test_pick_with_loaded_route);
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
    RUN(test_err_prefix_not_canonical);
    RUN(test_load_prefix_dots_not_segments);
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
