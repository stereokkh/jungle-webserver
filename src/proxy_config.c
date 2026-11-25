/*
 * proxy_config.c — Reverse Proxy 설정 파일과 라우팅 정책
 *
 * CS:APP 대응: 없음 (Tiny에 없는 기능)
 * 설정 파일 형식 (#8 에서 확정):
 *   # 경로 접두사   백엔드...                      (한 줄에 라우트 하나)
 *   /api/          127.0.0.1:8001 127.0.0.1:8003   # 뒤에 오는 주석도 된다
 *   /static/       localhost:8002
 *   /v6/           [::1]:8001
 *   /ha/           127.0.0.1:8001 127.0.0.1:8002 backup=127.0.0.1:8003 failover=connect,timeout tries=3
 *   - '=' 가 있는 토큰은 옵션(backup=, failover=, tries=)이다. 뜻은 proxy_config.h, 여기서는 읽는 규칙만 둔다
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
 * 백엔드 고르기 (proxy_rr_begin, proxy_pick_backend, proxy_mark_down)는 이 파일 맨 아래. 시계를 읽지 않고
 *   현재 시각을 인자로 받는 순수 함수라서 단위 테스트가 시간을 마음대로 돌린다
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
#define CONF_OPT_MAX     (CONF_NAME_MAX + 7)   /* 옵션 토큰: "backup=" + 백엔드 이름 최대 길이 */

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

/* 접두사 글자. 접두사는 정규화된 target 의 경로와 바이트 그대로 비교한다 (#31, docs/interfaces.md "경로 정규형").
 * 접두사 쪽은 정규화하지 않으므로 '%' 는 받지 않는다: 정규형의 %XX 는 16진수 대문자이고 비예약 문자가 아니어야 해서,
 * 받으려면 접두사도 같은 규칙으로 검사해야 한다 (비 ASCII 경로 라우팅은 지금 필요 없음).
 * '?' 는 경로와 query 를 나누는 글자, '#'·제어 문자·비 ASCII 는 경로에 쓰려면 %XX 로 인코딩해야 하는 글자다 */
static int is_prefix_char(int c)
{
    return is_alnum(c) || (c != '\0' && strchr("-._~!$&'()*+,;=:@/", c) != NULL);
}

/* 정규형이 아닌 접두사인가: 빈 조각 "//" (끝 '/' 하나는 괜찮다), "."·".." 조각.
 * 정규화된 target 에는 이것들이 없으므로 (--merge-slashes 여도 "//" 는 합쳐져 사라진다)
 * 이런 접두사는 어떤 요청에도 안 맞는 죽은 라우트가 된다. 시작할 때 거절한다. p[0] == '/' 가 전제 */
static int is_noncanonical_prefix(const char *p, size_t n)
{
    size_t i = 0, start, len;

    while (i < n) {                     /* 여기서 p[i] 는 언제나 '/' */
        start = ++i;
        while (i < n && p[i] != '/')
            i++;
        len = i - start;
        if (len == 0 && i < n)
            return 1;                   /* "//" */
        if ((len == 1 && p[start] == '.') || (len == 2 && p[start] == '.' && p[start + 1] == '.'))
            return 1;                   /* ".", ".." 조각 */
    }
    return 0;
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

/* 한 줄에 한 번만 쓸 수 있는 옵션이 이미 나왔는지 기억하는 비트 (backup= 은 여러 번 되므로 없다) */
enum { OPT_FAILOVER = 1, OPT_TRIES = 2 };

static int item_is(const char *item, size_t n, const char *name)
{
    return strlen(name) == n && memcmp(item, name, n) == 0;
}

/* "failover=" 의 값을 읽어 rt->failover 에 넣는다. 쉼표로 나눈 off·connect·timeout.
 * 빈 항목("a,,b", 끝 쉼표), 중복, 모르는 값은 오류. off 는 혼자서만 (off,connect 는 오류).
 * 값이 통째로 비었으면 parse_option 이 먼저 거른다 */
static int parse_failover(const conf_state *st, proxy_route *rt, const char *v, size_t vl)
{
    unsigned flags = 0;
    size_t items = 0, b = 0, e;
    int has_off = 0;

    for (;;) {
        unsigned bit = 0;

        for (e = b; e < vl && v[e] != ','; e++)
            ;
        if (e == b)
            return conf_fail(st, "empty item in failover value (expected off, connect, timeout)");
        if (item_is(v + b, e - b, "off")) {
            if (has_off)
                return conf_fail(st, "duplicate failover value 'off'");
            has_off = 1;
        } else if (item_is(v + b, e - b, "connect")) {
            bit = PROXY_FAILOVER_CONNECT;
        } else if (item_is(v + b, e - b, "timeout")) {
            bit = PROXY_FAILOVER_TIMEOUT;
        } else {
            return conf_fail(st, "unknown failover value '%.*s' (expected off, connect, timeout)",
                             (int)(e - b), v + b);
        }
        if (flags & bit)
            return conf_fail(st, "duplicate failover value '%.*s'", (int)(e - b), v + b);
        flags |= bit;
        items++;
        if (e == vl)
            break;
        b = e + 1;
    }
    if (has_off && items > 1)
        return conf_fail(st, "failover=off cannot be combined with other values");
    rt->failover = flags;
    return 0;
}

/* "tries=" 의 값: 앞자리 0 없는 10진수 1..PROXY_MAX_BACKENDS (포트와 같은 이유로 표기를 한 가지만 둔다) */
static int parse_tries(const conf_state *st, proxy_route *rt, const char *v, size_t vl)
{
    size_t i, n = 0;
    int ok = vl <= 2 && v[0] != '0';    /* 세 자리부터는 어차피 8 을 넘는다. 자릿수를 먼저 막아 n 이 넘칠 일이 없다 */

    for (i = 0; ok && i < vl; i++) {
        if (v[i] < '0' || v[i] > '9')
            ok = 0;
        else
            n = n * 10 + (size_t)(v[i] - '0');
    }
    if (!ok || n < 1 || n > PROXY_MAX_BACKENDS)
        return conf_fail(st, "invalid tries '%.*s' (1-%d, decimal, no leading zero)", (int)vl, v, PROXY_MAX_BACKENDS);
    rt->tries = n;
    return 0;
}

/* '=' 가 있는 토큰 하나(t[0..tl), eq 는 첫 '=')를 옵션으로 읽는다. backup= 은 백엔드 칸을 하나 더 쓴다 */
static int parse_option(const conf_state *st, proxy_route *rt, const char *t, size_t tl,
                        const char *eq, unsigned *seen)
{
    size_t kl = (size_t)(eq - t), vl = tl - kl - 1;
    const char *v = eq + 1;

    if (tl > CONF_OPT_MAX)
        return conf_fail(st, "option too long (max %d bytes)", CONF_OPT_MAX);
    if (kl == 0)
        return conf_fail(st, "option has no name ('=' at the start of a token)");
    if (!item_is(t, kl, "backup") && !item_is(t, kl, "failover") && !item_is(t, kl, "tries"))
        return conf_fail(st, "unknown option '%.*s' (expected backup, failover, tries)", (int)kl, t);
    if (vl == 0)
        return conf_fail(st, "option '%.*s' has no value", (int)kl, t);

    if (item_is(t, kl, "backup")) {
        char tok[CONF_NAME_MAX + 1];
        proxy_backend *be = &rt->backends[rt->nbackends];

        if (rt->nbackends == PROXY_MAX_BACKENDS)
            return conf_fail(st, "too many backends (max %d)", PROXY_MAX_BACKENDS);
        memcpy(tok, v, vl);                     /* tl <= CONF_OPT_MAX 라서 vl <= CONF_NAME_MAX 다 (위에서 걸렀다) */
        tok[vl] = '\0';
        if (parse_backend(st, tok, be) < 0)     /* name 에는 "backup=" 없이 host:port 만 들어간다 (Host 헤더가 된다) */
            return -1;
        be->is_backup = 1;
        rt->nbackends++;
        return 0;
    }
    if (item_is(t, kl, "failover")) {
        if (*seen & OPT_FAILOVER)
            return conf_fail(st, "duplicate option 'failover'");
        *seen |= OPT_FAILOVER;
        return parse_failover(st, rt, v, vl);
    }
    if (*seen & OPT_TRIES)
        return conf_fail(st, "duplicate option 'tries'");
    *seen |= OPT_TRIES;
    return parse_tries(st, rt, v, vl);
}

/* 줄 하나(줄 끝 제거됨, NUL 종료 아님)를 처리한다. 라우트가 있으면 cfg 에 추가 */
static int parse_line(conf_state *st, const char *line, size_t len, proxy_config *cfg)
{
    size_t pos = 0, tl, i;
    const char *t;
    proxy_route *rt;
    char tok[CONF_NAME_MAX + 1];
    unsigned seen = 0;

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
    if (is_noncanonical_prefix(t, tl))
        return conf_fail(st, "prefix not in canonical form (no '//', '.' or '..' segments)");

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

    /* 4. 토큰을 왼쪽부터: '=' 가 없으면 주 백엔드, 있으면 옵션 (순서 무관). 백엔드 이름에는 '=' 가 못 들어가므로
     *    '=' 하나로 갈라도 주 백엔드의 모양 검사(parse_backend)는 그대로다 */
    while (next_token(line, len, &pos, &t, &tl)) {
        const char *eq = memchr(t, '=', tl);

        if (eq != NULL) {
            if (parse_option(st, rt, t, tl, eq, &seen) < 0)
                return -1;
            continue;
        }
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

    /* 5. 백엔드가 하나도 없음, 또는 백업뿐임 (주 백엔드가 있어야 요청이 어디서 시작할지 정해진다) */
    if (rt->nbackends == 0)
        return conf_fail(st, "no backend for prefix '%s'", rt->prefix);
    for (i = 0; i < rt->nbackends && rt->backends[i].is_backup; i++)
        ;
    if (i == rt->nbackends)
        return conf_fail(st, "no primary backend for prefix '%s' (backup= needs at least one plain backend)",
                         rt->prefix);

    /* 6. tries 를 안 적었으면 주 + 백업 전부. 기본은 "다 해 본다" 이고, 모든 백엔드가 죽었을 때 응답이
     *    끝없이 늦어지는 것을 막고 싶을 때만 tries 로 줄인다 */
    if (!(seen & OPT_TRIES))
        rt->tries = rt->nbackends;

    st->first_line[cfg->nroutes] = st->lineno;
    cfg->nroutes++;
    return 0;
}

int proxy_config_load(const char *path, proxy_config *cfg)
{
    /* 크기가 29KB 쯤이라 지역 복사본을 만들지 않고 *cfg 에 바로 쓴다. 실패하면 전부 0 으로 되돌린다 */
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

/* ---------------------------------------------------------------
 * 백엔드 고르기 (순수 함수. 계약은 proxy_config.h)
 * --------------------------------------------------------------- */

/* 주 백엔드(is_backup == 0)의 인덱스를 등장 순서대로 prim 에 담고 개수를 돌려준다. nbackends 가 배열보다 크면 잘라 본다 */
static size_t list_primaries(const proxy_route *route, size_t *prim)
{
    size_t i, n = 0, nb = route->nbackends < PROXY_MAX_BACKENDS ? route->nbackends : PROXY_MAX_BACKENDS;

    for (i = 0; i < nb; i++)
        if (!route->backends[i].is_backup)
            prim[n++] = i;
    return n;
}

size_t proxy_rr_begin(proxy_route *route)
{
    size_t prim[PROXY_MAX_BACKENDS], np, k;

    /* Round-Robin (#9, C9): 요청마다 next 를 한 칸 민다. 순번은 "주 백엔드들 중 몇 번째"라 백업이 섞여 있어도
     * 주 백엔드끼리만 0, 1, ..., n-1, 0 으로 돈다. 서버가 단일 스레드 루프라 락이 없다.
     * 이 함수가 항상 0 을 돌려주게 바꾸면(항상 첫 주 백엔드) 이슈 #9 의 "RR 은 마지막, 늦으면 뺀다" 가 된다 */
    if (route == NULL)
        return 0;
    np = list_primaries(route, prim);
    if (np == 0)
        return 0;
    k = route->next % np;
    route->next = (k + 1) % np;
    return k;
}

int proxy_pick_backend(const proxy_route *route, int64_t now_ms, unsigned tried_mask, size_t rr_start)
{
    size_t prim[PROXY_MAX_BACKENDS], order[PROXY_MAX_BACKENDS], np, n = 0, i, pass;
    int all_down;

    if (route == NULL)
        return -1;
    np = list_primaries(route, prim);
    /* 페일오버가 꺼져 있으면 후보는 rr_start 번째 주 백엔드 하나뿐이다 (예전 Round-Robin 그대로). 백업은 쓰이지 않고,
     * 죽은 표시는 proxy_mark_down 이 안 붙이므로 보지 않는다. 이미 시도했으면 -1 (헤더 계약) */
    if (route->failover == 0) {
        size_t only;

        if (np == 0)
            return -1;
        only = prim[rr_start % np];
        return (tried_mask & (1u << only)) ? -1 : (int)only;
    }
    /* 후보 순서: 주 백엔드를 rr_start 부터 한 바퀴, 그다음 백업 */
    for (i = 0; i < np; i++)
        order[n++] = prim[(rr_start % np + i) % np];
    for (i = 0; i < route->nbackends && i < PROXY_MAX_BACKENDS; i++)
        if (route->backends[i].is_backup)
            order[n++] = i;

    /* pass 0: 안 해 봤고 안 죽은 첫 후보. pass 1: 죽은 표시를 무시 (라우트의 백엔드가 전부 죽은 표시일 때만:
     * 기다리지 않고 다음 후보를 시도해 살아난 백엔드를 바로 찾는다). "전부"는 이미 시도한 것까지 센다.
     * 시도한 것을 빼고 세면, 앞 시도의 표시가 이 요청 중에 만료된 경우에 (그 백엔드만 살아 있고 나머지는 죽음)
     * 죽은 쪽을 표시 무시로 고르게 돼서 "전부 죽었을 때만" 이라는 규칙보다 넓어진다 */
    for (i = 0; i < n; i++)
        if (now_ms >= route->backends[order[i]].down_until_ms)
            break;
    all_down = n > 0 && i == n;
    for (pass = 0; pass < (all_down ? 2u : 1u); pass++)
        for (i = 0; i < n; i++) {
            const proxy_backend *be = &route->backends[order[i]];

            if (tried_mask & (1u << order[i]))
                continue;
            if (pass == 0 && now_ms < be->down_until_ms)
                continue;
            return (int)order[i];
        }
    return -1;
}

void proxy_mark_down(proxy_route *route, size_t idx, int64_t now_ms)
{
    if (route == NULL || route->failover == 0 || idx >= route->nbackends || idx >= PROXY_MAX_BACKENDS)
        return;
    route->backends[idx].down_until_ms = now_ms + PROXY_BACKEND_DOWN_MS;
}
