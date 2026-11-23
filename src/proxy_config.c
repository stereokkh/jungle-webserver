/*
 * proxy_config.c — Reverse Proxy 설정 파일과 라우팅 정책
 *
 * CS:APP 대응: 없음 (Tiny에 없는 기능)
 * 설정 파일 형식 (#8 에서 확정):
 *   # 경로 접두사   백엔드...                      (한 줄에 라우트 하나)
 *   /api/          127.0.0.1:8001 127.0.0.1:8003   # 뒤에 오는 주석도 된다
 *   /static/       localhost:8002
 *   /v6/           [::1]:8001
 *   - 공백(SP, TAB)으로 나눈다. 줄 끝은 LF, CRLF 모두 받고 마지막 줄은 LF 가 없어도 된다
 *   - '#' 는 토큰 맨 앞에 있을 때만 주석이다 ("/a#b" 는 접두사 글자 오류)
 *   - 빈 파일, 주석만 있는 파일은 "no routes" 로 거절한다 (-c 를 줬다면 프록시를 쓰겠다는 뜻이므로)
 *   - 오류는 stderr 에 "파일:줄번호: 이유" 로 한 줄 출력하고 -1, cfg 는 전부 0 으로 되돌린다
 *     (줄이 없는 파일 전체 오류 - 열기·읽기 실패, 라우트 없음 - 는 "파일: 이유")
 *   - 백엔드 주소는 여기서 getaddrinfo 로 한 번만 해석한다. 요청 처리 중에는 이름 해석을 하지 않는다
 * 우리가 Tiny 와 다른 점:
 *   Tiny 의 open_clientfd 는 연결할 때마다 getaddrinfo 를 부른다 (DNS 가 느리면 서버 전체가 멈춘다).
 *   여기서는 시작할 때 한 번만 해석하고, 백엔드마다 주소를 하나만 저장하므로 IPv4 를 먼저 고른다
 *   (이유는 proxy_config.h)
 * 담당: 트랙 B · 이슈 #8 (단위 테스트: tests/unit/test_proxy_config.c)
 */
#include "proxy_config.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define CONF_LINE_MAX    1024   /* 줄 끝(CRLF) 제외 */
#define CONF_PREFIX_MAX  127    /* prefix[128] 의 마지막은 NUL 자리 */
#define CONF_NAME_MAX    63     /* name[64] 의 마지막은 NUL 자리 */

#define BACKEND_SHAPE_MSG "invalid backend '%s' (expected host:port or [ipv6]:port)"

/* 로드 중 상태. 중복 접두사 메시지에 "처음 나온 줄" 이 필요해서 라우트별로 줄 번호를 기억한다 */
typedef struct {
    const char *path;
    size_t lineno;
    size_t first_line[PROXY_MAX_ROUTES];
} conf_state;

/* "경로:줄: 이유" 를 stderr 로 내고 -1 */
static int conf_fail(const conf_state *st, const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s:%zu: ", st->path, st->lineno);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return -1;
}

static int is_ws(int c)
{
    return c == ' ' || c == '\t';
}

static int is_alnum(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_hex(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* 접두사 글자: '%', '?', '#', 제어 문자, 비 ASCII 는 없다 (원본 target 과 바이트 그대로 비교하므로) */
static int is_prefix_char(int c)
{
    return is_alnum(c) || (c != '\0' && strchr("-._~!$&'()*+,;=:@/", c) != NULL);
}

static int is_host_char(int c)
{
    return is_alnum(c) || c == '-' || c == '.' || c == '_';
}

/* 숫자와 '.' 로만 된 호스트가 점 넷의 10진 IPv4 (각 1-3자리, 255 이하, 앞자리 0 없음) 가 아닌가.
 * glibc getaddrinfo 는 "1" "127.1" "0" 같은 inet_aton 옛 형식도 주소로 받아 주므로
 * 오타("0:8001" 처럼 옥텟이 빠진 것)가 조용히 통과한다. 시작 때 거절하려고 미리 막는다.
 * 앞자리 0 도 막는다: inet_aton 은 "010" 을 8진수로 읽어서 "127.0.0.010" 이 127.0.0.8 이 된다
 * (여기서 10진으로 검사한 값과 getaddrinfo 가 해석한 주소가 달라진다).
 * 자릿수는 누적하기 전에 센다 (토큰이 63바이트까지라 먼저 곱하면 정수가 넘칠 수 있다) */
static int is_odd_numeric_host(const char *h, size_t n)
{
    size_t i, dots = 0, digits = 0;
    long v = 0;

    for (i = 0; i < n; i++)
        if (h[i] != '.' && (h[i] < '0' || h[i] > '9'))
            return 0;                   /* 이름이면 해석에 맡긴다 */
    for (i = 0; i <= n; i++) {
        if (i == n || h[i] == '.') {
            if (digits == 0 || digits > 3 || v > 255)
                return 1;
            if (i < n)
                dots++;
            digits = 0;
            v = 0;
        } else {
            if (digits == 3 || (digits == 1 && v == 0))
                return 1;               /* 4자리 이상, 앞자리 0 ("00", "010") */
            digits++;
            v = v * 10 + (h[i] - '0');
        }
    }
    return dots != 3;
}

/* 백엔드 토큰 하나(NUL 종료, 길이 검사 끝남)를 검사하고 해석해서 be 에 채운다 */
static int parse_backend(const conf_state *st, const char *tok, proxy_backend *be)
{
    char host[CONF_NAME_MAX + 1];
    const char *port;
    size_t hlen, plen, i;
    long pv = 0;
    int bracket = (tok[0] == '[');
    struct addrinfo hints, *res, *p, *pick = NULL;
    int rc;

    if (bracket) {
        const char *close = strchr(tok, ']');

        if (close == NULL || close == tok + 1 || close[1] != ':')
            return conf_fail(st, BACKEND_SHAPE_MSG, tok);
        hlen = (size_t)(close - tok) - 1;
        for (i = 0; i < hlen; i++)
            if (!is_hex((unsigned char)tok[1 + i]) && tok[1 + i] != ':' && tok[1 + i] != '.')
                return conf_fail(st, BACKEND_SHAPE_MSG, tok);
        memcpy(host, tok + 1, hlen);
        port = close + 2;
    } else {
        const char *colon = strrchr(tok, ':');

        if (colon == NULL || colon == tok)
            return conf_fail(st, BACKEND_SHAPE_MSG, tok);
        hlen = (size_t)(colon - tok);
        for (i = 0; i < hlen; i++)
            if (!is_host_char((unsigned char)tok[i]))
                return conf_fail(st, BACKEND_SHAPE_MSG, tok);
        memcpy(host, tok, hlen);
        port = colon + 1;
        if (is_odd_numeric_host(host, hlen))
            return conf_fail(st, BACKEND_SHAPE_MSG, tok);
    }
    host[hlen] = '\0';

    plen = strlen(port);
    if (plen < 1 || plen > 5 || port[0] == '0')
        return conf_fail(st, "invalid port in '%s' (1-65535)", tok);
    for (i = 0; i < plen; i++) {
        if (port[i] < '0' || port[i] > '9')
            return conf_fail(st, "invalid port in '%s' (1-65535)", tok);
        pv = pv * 10 + (port[i] - '0');
    }
    if (pv > 65535)
        return conf_fail(st, "invalid port in '%s' (1-65535)", tok);

    memset(&hints, 0, sizeof hints);
    hints.ai_socktype = SOCK_STREAM;
    if (bracket) {
        hints.ai_family = AF_INET6;
        hints.ai_flags = AI_NUMERICSERV | AI_NUMERICHOST;
    } else {
        hints.ai_family = AF_UNSPEC;
        hints.ai_flags = AI_NUMERICSERV;
    }
    /* AI_ADDRCONFIG 는 쓰지 않는다: 결과가 환경마다 달라지고 loopback 만 있는 컨테이너에서 빈 결과가 나온다 */
    rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0)
        return conf_fail(st, "cannot resolve '%s': %s", tok, gai_strerror(rc));

    /* 괄호 주소는 첫 IPv6, 이름·IPv4 는 첫 IPv4 (없으면 첫 IPv6) */
    for (p = res; p != NULL; p = p->ai_next)
        if (p->ai_addr != NULL && p->ai_addrlen <= sizeof be->addr
            && p->ai_family == (bracket ? AF_INET6 : AF_INET)) {
            pick = p;
            break;
        }
    if (pick == NULL && !bracket)
        for (p = res; p != NULL; p = p->ai_next)
            if (p->ai_addr != NULL && p->ai_addrlen <= sizeof be->addr
                && p->ai_family == AF_INET6) {
                pick = p;
                break;
            }
    if (pick == NULL) {
        freeaddrinfo(res);
        return conf_fail(st, "no usable address for '%s'", tok);
    }

    memset(&be->addr, 0, sizeof be->addr);
    memcpy(&be->addr, pick->ai_addr, pick->ai_addrlen);
    be->addrlen = (socklen_t)pick->ai_addrlen;
    memcpy(be->name, tok, strlen(tok) + 1);
    freeaddrinfo(res);
    return 0;
}

/* 토큰 하나를 찾는다: 공백을 건너뛰고 공백이 아닌 연속. 없거나 '#' 로 시작하면(주석) 0 */
static int next_token(const char *line, size_t len, size_t *pos, const char **t, size_t *tl)
{
    size_t p = *pos, n;

    while (p < len && is_ws((unsigned char)line[p]))
        p++;
    if (p == len || line[p] == '#') {
        *pos = len;
        return 0;
    }
    for (n = 0; p + n < len && !is_ws((unsigned char)line[p + n]); n++)
        ;
    *t = line + p;
    *tl = n;
    *pos = p + n;
    return 1;
}

/* 줄 하나(줄 끝 제거됨, NUL 종료 아님)를 처리한다. 라우트가 있으면 cfg 에 추가 */
static int parse_line(conf_state *st, const char *line, size_t len, proxy_config *cfg)
{
    size_t pos = 0, tl, i;
    const char *t;
    proxy_route *rt;
    char tok[CONF_NAME_MAX + 1];

    if (!next_token(line, len, &pos, &t, &tl))
        return 0;                       /* 빈 줄, 주석 줄 */

    /* 1. 접두사 */
    if (t[0] != '/')
        return conf_fail(st, "prefix must start with '/'");
    if (tl > CONF_PREFIX_MAX)
        return conf_fail(st, "prefix too long (max %d bytes)", CONF_PREFIX_MAX);
    for (i = 0; i < tl; i++)
        if (!is_prefix_char((unsigned char)t[i]))
            return conf_fail(st, "invalid character in prefix");

    /* 2. 중복 (바이트까지 같은 접두사만. "/api" 와 "/api/" 는 다르다) */
    for (i = 0; i < cfg->nroutes; i++)
        if (strlen(cfg->routes[i].prefix) == tl && memcmp(cfg->routes[i].prefix, t, tl) == 0)
            return conf_fail(st, "duplicate prefix '%.*s' (first on line %zu)",
                             (int)tl, t, st->first_line[i]);

    /* 3. 라우트 개수 */
    if (cfg->nroutes == PROXY_MAX_ROUTES)
        return conf_fail(st, "too many routes (max %d)", PROXY_MAX_ROUTES);

    rt = &cfg->routes[cfg->nroutes];
    memcpy(rt->prefix, t, tl);
    rt->prefix[tl] = '\0';

    /* 4. 백엔드를 왼쪽부터 */
    while (next_token(line, len, &pos, &t, &tl)) {
        if (rt->nbackends == PROXY_MAX_BACKENDS)
            return conf_fail(st, "too many backends (max %d)", PROXY_MAX_BACKENDS);
        if (tl > CONF_NAME_MAX)
            return conf_fail(st, "backend name too long (max %d bytes)", CONF_NAME_MAX);
        memcpy(tok, t, tl);
        tok[tl] = '\0';
        if (parse_backend(st, tok, &rt->backends[rt->nbackends]) < 0)
            return -1;
        rt->nbackends++;
    }

    /* 5. 백엔드가 하나도 없음 */
    if (rt->nbackends == 0)
        return conf_fail(st, "no backend for prefix '%s'", rt->prefix);

    st->first_line[cfg->nroutes] = st->lineno;
    cfg->nroutes++;
    return 0;
}

int proxy_config_load(const char *path, proxy_config *cfg)
{
    /* 크기가 27KB 쯤이라 지역 복사본을 만들지 않고 *cfg 에 바로 쓴다. 실패하면 전부 0 으로 되돌린다 */
    char line[CONF_LINE_MAX + 2];
    conf_state st;
    FILE *fp;
    int c;

    if (cfg != NULL)
        memset(cfg, 0, sizeof *cfg);
    if (path == NULL || cfg == NULL)
        return -1;

    fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "%s: cannot open: %s\n", path, strerror(errno));
        return -1;
    }

    memset(&st, 0, sizeof st);
    st.path = path;

    /* fgets 는 NUL 바이트를 구분하지 못하고 긴 줄을 조용히 나눠 읽는다. getc 로 한 바이트씩 본다 */
    for (;;) {
        size_t len = 0;

        st.lineno++;
        for (;;) {
            c = getc(fp);
            if (c == EOF || c == '\n')
                break;
            if (c == '\0') {
                conf_fail(&st, "NUL byte in line");
                goto fail;
            }
            if (len >= CONF_LINE_MAX + 1) {     /* 내용 1024 + CR 1 까지만 허용 */
                conf_fail(&st, "line too long (max %d bytes)", CONF_LINE_MAX);
                goto fail;
            }
            line[len++] = (char)c;
        }
        if (c == EOF && ferror(fp)) {
            fprintf(stderr, "%s: read error: %s\n", path, strerror(errno));
            goto fail;
        }
        if (c == EOF && len == 0)
            break;                              /* 마지막 LF 뒤의 빈 끝 */

        if (c == '\n' && len > 0 && line[len - 1] == '\r')
            len--;                              /* CRLF 만 줄 끝. LF 없이 끝난 마지막 줄의 CR 은 글자로 남겨 오류가 된다 */
        if (len > CONF_LINE_MAX) {
            conf_fail(&st, "line too long (max %d bytes)", CONF_LINE_MAX);
            goto fail;
        }
        if (parse_line(&st, line, len, cfg) < 0)
            goto fail;
        if (c == EOF)
            break;
    }

    if (cfg->nroutes == 0) {
        fprintf(stderr, "%s: no routes\n", path);
        goto fail;
    }
    fclose(fp);
    return 0;

fail:
    fclose(fp);
    memset(cfg, 0, sizeof *cfg);
    return -1;
}

proxy_route *proxy_match(proxy_config *cfg, const char *target)
{
    size_t plen, i, best_len = 0;
    proxy_route *best = NULL;

    if (cfg == NULL || target == NULL || cfg->nroutes == 0)
        return NULL;

    plen = strcspn(target, "?");        /* '?' 뒤 질의는 비교하지 않는다 */
    for (i = 0; i < cfg->nroutes; i++) {
        const char *prefix = cfg->routes[i].prefix;
        size_t l = strlen(prefix);

        if (l == 0 || l > plen || memcmp(target, prefix, l) != 0)
            continue;
        /* 경로 경계: prefix 가 '/' 로 끝나거나, 경로가 거기서 끝나거나, 다음 글자가 '/' */
        if (!(prefix[l - 1] == '/' || l == plen || target[l] == '/'))
            continue;
        if (l > best_len) {             /* 같은 길이는 로더가 중복으로 거절하므로 없다 */
            best_len = l;
            best = &cfg->routes[i];
        }
    }
    return best;
}

const proxy_backend *proxy_next_backend(proxy_route *route)
{
    size_t i;

    /* Round-Robin (#9, C9): 부를 때마다 next 를 한 칸 민다. 백엔드가 하나면 늘 0 번.
     * 서버가 단일 스레드 루프라 락이 없다. next 가 범위 밖이어도(손으로 채운 route) 나머지 연산으로 맞춘다.
     * 이 함수 하나만 되돌리면(항상 backends[0]) 이슈 #9 의 "RR 은 마지막, 늦으면 뺀다" 가 된다 */
    if (route == NULL || route->nbackends == 0)
        return NULL;
    i = route->next % route->nbackends;
    route->next = (i + 1) % route->nbackends;
    return &route->backends[i];
}
