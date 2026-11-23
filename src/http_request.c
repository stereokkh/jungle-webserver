/*
 * http_request.c — 증분 HTTP/1.1 요청 파서
 *
 * CS:APP 대응:
 *   doit()            : Rio_readlineb 로 요청 라인 → sscanf("%s %s %s", method, uri, version)
 *   read_requesthdrs(): "\r\n" 빈 줄까지 읽고 버림
 *   parse_uri()       : "cgi-bin" 포함 여부로 정적/동적 판단, '?' 로 인자 분리
 * 여기서는 버퍼 전체를 받아 "\r\n\r\n" 경계를 찾고, 헤더를 해석하고,
 * Content-Length 만큼 본문이 다 왔는지까지 확인한다.
 *
 * Tiny 와 다른 점 (이슈 #3 에서 재현한 것 포함):
 *   - Tiny 는 Rio_readlineb 가 한 줄을 다 받을 때까지 read() 에서 멈춘다 (재현 ①: nc 로
 *     연결만 열어 두면 다른 손님이 전부 기다림). 이 파일은 소켓을 만지지 않는다. 받은 만큼만
 *     보고 덜 왔으면 NEED_MORE 를 돌려준다. 상태를 저장하지 않으므로 server.c 는 더 읽은 뒤
 *     같은 버퍼 처음부터 다시 부른다 (헤더 상한이 8KB 라 다시 파싱해도 싸다).
 *   - Tiny 는 헤더를 읽고 버린다. 여기서는 Host, Content-Length, Connection 을 해석하고
 *     원본 헤더 블록 위치(headers)를 남긴다 (프록시 전달용, #9).
 *   - Tiny 는 uri 를 그대로 "." 뒤에 붙여 파일을 연다 (재현 ②: ../ 로 classic/ 밖 파일이 열림).
 *     여기서는 정규화된 target, 끝까지 디코딩한 path, 원본 query 를 나눠 담는다 (normalize_target).
 *     라우팅·백엔드 전달·정적 파일이 모두 한 정규형에서 나오므로 점 조각·빈 조각·퍼센트 인코딩으로
 *     프록시 라우트를 건너뛰는 요청이 없다 (PR #26 리뷰, #31).
 *     static_open(#22) 의 글자 단위 정리 + realpath 는 두 번째 안전장치로 그대로 둔다.
 *   - Tiny 는 version 을 읽기만 하고 버리고(응답은 항상 1.0), 메서드는 strcasecmp 로 비교한다.
 *     여기서는 HTTP/1.0·1.1 만 받고(그 외 505), 메서드는 대소문자를 구분한다 (RFC 9112 3.1).
 *
 * buf 는 NUL 로 끝나지 않는다. strstr, strchr, sscanf, strtol 처럼 NUL 을 찾아 가는 함수는
 * len 을 넘어 읽을 수 있어서 buf 에는 쓰지 않는다 (memchr, memcmp, 직접 반복만).
 * NUL 종료된 우리 배열(req->target 등)에만 str* 함수를 쓴다.
 *
 * 담당: 트랙 B · 이슈 #5 (단위 테스트: tests/unit/test_http_request.c)
 */
#include "http_request.h"

#include <limits.h>
#include <string.h>

/* 헤더를 한 줄씩 보면서 여러 줄에 걸쳐 모으는 정보 */
typedef struct {
    int saw_host;
    int saw_te;          /* Transfer-Encoding */
    int conn_close;      /* Connection 값에 close 가 있었다 */
    int conn_keep_alive; /* Connection 값에 keep-alive 가 있었다 */
} header_state;

static parse_result fail(int *status, int code)
{
    *status = code;
    return PARSE_ERROR;
}

/* buf 앞 n 바이트 안에서 pat 이 처음 나오는 위치. 없으면 NULL.
 * pat 전체가 n 안에 들어와야 찾은 것으로 본다 (n 을 넘어 읽지 않는다) */
static const char *find(const char *buf, size_t n, const char *pat, size_t plen)
{
    size_t i;

    for (i = 0; i + plen <= n; i++)
        if (memcmp(buf + i, pat, plen) == 0)
            return buf + i;
    return NULL;
}

/* 메서드와 헤더 이름에 쓸 수 있는 글자 (RFC 9110 5.6.2 tchar) */
static int is_tchar(unsigned char c)
{
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        return 1;
    return c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

/* Host 값에 쓸 수 있는 글자: 이름·IPv4·[IPv6]·:포트 (RFC 3986 reg-name + IP-literal). 공백 등은 안 됨 */
static int is_host_char(unsigned char c)
{
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        return 1;
    return c != '\0' && strchr("-._~!$&'()*+,;=%:[]", c) != NULL;
}

static unsigned char lower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c - 'A' + 'a') : c;
}

/* p[0..n) 이 lit 과 대소문자 무시하고 같은가 (헤더 이름, Connection 토큰 비교용) */
static int ci_equal(const char *p, size_t n, const char *lit)
{
    size_t i;

    if (strlen(lit) != n)
        return 0;
    for (i = 0; i < n; i++)
        if (lower((unsigned char)p[i]) != lower((unsigned char)lit[i]))
            return 0;
    return 1;
}

static int hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* p[0..n) 앞뒤의 공백·탭을 걷어 낸다 (헤더 값의 OWS) */
static void trim_ows(const char **p, size_t *n)
{
    while (*n > 0 && ((*p)[0] == ' ' || (*p)[0] == '\t')) {
        (*p)++;
        (*n)--;
    }
    while (*n > 0 && ((*p)[*n - 1] == ' ' || (*p)[*n - 1] == '\t'))
        (*n)--;
}

/* "HTTP/x.y". 1.0·1.1 이면 0 + *minor, 모양은 맞는데 다른 버전이면 505, 모양이 틀리면 400 */
static int parse_version(const char *v, size_t n, int *minor)
{
    if (n != 8 || memcmp(v, "HTTP/", 5) != 0 || v[6] != '.'
        || v[5] < '0' || v[5] > '9' || v[7] < '0' || v[7] > '9')
        return 400;
    if (v[5] != '1' || (v[7] != '0' && v[7] != '1'))
        return 505;
    *minor = v[7] - '0';
    return 0;
}

/*
 * 요청 라인 "METHOD SP target SP HTTP/1.x" (CRLF 제외, 길이 n).
 * 성공 0, 실패면 상태 코드. 순서: 모양 400 → 메서드 400/501 → 대상 414/400 → 버전 400/505.
 * 오류여도 응답의 Connection 결정을 위해 version_minor 는 먼저 채워 둔다 (헤더 주석).
 *
 * Tiny 의 sscanf("%s %s %s") 는 공백이 여러 개여도, 단어가 넷이어도 통과시킨다.
 * 여기서는 공백 정확히 두 개만 받는다.
 */
static int parse_request_line(const char *p, size_t n, http_request *req)
{
    const char *sp1, *sp2, *t, *v;
    size_t mlen, tlen, vlen, i;
    int vstatus;

    sp1 = memchr(p, ' ', n);
    if (sp1 == NULL)
        return 400;
    mlen = (size_t)(sp1 - p);
    t = sp1 + 1;
    sp2 = memchr(t, ' ', n - mlen - 1);
    if (sp2 == NULL)
        return 400;
    tlen = (size_t)(sp2 - t);
    v = sp2 + 1;
    vlen = n - mlen - 1 - tlen - 1;   /* 세 번째 공백이 있으면 v 에 섞여서 400 이 된다 */

    vstatus = parse_version(v, vlen, &req->version_minor);

    /* 메서드: 토큰 글자만. 모르는 메서드는 501 */
    if (mlen == 0)
        return 400;
    for (i = 0; i < mlen; i++)
        if (!is_tchar((unsigned char)p[i]))
            return 400;
    i = mlen < sizeof req->method_str ? mlen : sizeof req->method_str - 1;
    memcpy(req->method_str, p, i);
    req->method_str[i] = '\0';
    if (mlen == 3 && memcmp(p, "GET", 3) == 0)
        req->method = HTTP_GET;
    else if (mlen == 4 && memcmp(p, "HEAD", 4) == 0)
        req->method = HTTP_HEAD;
    else if (mlen == 4 && memcmp(p, "POST", 4) == 0)
        req->method = HTTP_POST;
    else {
        req->method = HTTP_METHOD_OTHER;
        return 501;
    }

    /* 대상: origin-form("/" 로 시작)만. Forward Proxy 용 "http://..." 는 범위 밖이라 400 */
    if (tlen > HTTP_MAX_URI)
        return 414;
    if (tlen == 0 || t[0] != '/')
        return 400;
    for (i = 0; i < tlen; i++)
        if ((unsigned char)t[i] < 0x21 || (unsigned char)t[i] > 0x7E || t[i] == '#')
            return 400;               /* '#'(fragment) 는 요청 대상에 올 수 없다 (RFC 9112 3.2) */
    memcpy(req->target, t, tlen);
    req->target[tlen] = '\0';

    return vstatus;
}

/* NUL 종료된 path 를 '/' 로 나눈 조각 중 "." 이나 ".." 가 있는가.
 * "...", ".a", "a..b", "a." 같은 이름은 점 조각이 아니다. 빈 조각은 normalize_target 이 먼저 처리한다 */
static int has_dot_segment(const char *p)
{
    while (*p != '\0') {
        const char *end;
        size_t n;

        if (*p == '/') {
            p++;
            continue;
        }
        end = strchr(p, '/');
        n = end ? (size_t)(end - p) : strlen(p);
        if ((n == 1 && p[0] == '.') || (n == 2 && p[0] == '.' && p[1] == '.'))
            return 1;
        p += n;
    }
    return 0;
}

/* RFC 3986 2.3 비예약 문자: 인코딩하든 안 하든 같은 URI 다 */
static int is_unreserved(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || c == '-' || c == '.' || c == '_' || c == '~';
}

/*
 * req->target(원본, NUL 종료) 을 정규화된 요청 대상으로 바꾸고 path·query 를 채운다.
 * 성공 0, 실패 400. (#31, docs/decisions.md 2026-11-23)
 *
 * '?' 앞 경로를 한 번 훑어 정규형을 만든다. 하나라도 걸리면 400:
 *   1. %XX 문법 오류 ("%G1", 끝이 잘린 "%2")
 *   2. 인코딩 안 된 '\', 그리고 디코딩하면 '/', '\', NUL·제어 문자(0x01-0x1F, 0x7F) 가 되는 %XX
 *        - %2F: 서버마다 구분자인지 데이터인지 해석이 갈린다 (Tomcat 400, Apache 404 기본값)
 *        - '\': IIS·nginx Windows 빌드·Envoy 정규화는 '/' 로 읽는다. %5C 만 막으면 같은 우회가
 *          인코딩만 빼고 들어온다. RFC 3986 pchar 에 없는 글자라 정상 클라이언트는 보내지 않는다
 *          (Tomcat ALLOW_BACKSLASH=false 와 같음, PR #34 리뷰. '/' 로 바꾸는 선택지는 #35)
 *        - %00: 통과시키면 path 가 C 문자열로는 중간에서 끊겨 검사한 이름과 연 파일이 달라진다
 *   3. 빈 조각 "//": 기본 400, opt->merge_slashes 면 '/' 하나로 합친다.
 *      끝 '/' 하나("/a/")는 허용, "/a//" 는 빈 조각이다
 *   4. 비예약 문자의 %XX 만 글자로 디코딩한다 ("%61" → "a", "%7E" → "~"). 나머지 %XX 는 두고
 *      16진수만 대문자로 ("%eb" → "%EB")
 *   5. 디코딩한 path 에 "." / ".." 조각 (PR #27). '.' 은 비예약 문자라 "%2E%2E" 도 4 에서 ".." 가 된다.
 *      curl·브라우저는 보내기 전에 ".." 를 지우므로 (#3 재현 ②) 정상 요청은 영향 없음
 * 정규형은 원본보다 길어지지 않으므로 HTTP_MAX_URI 안에 들어간다.
 *
 * 그다음 target = 정규형 경로 + 원본의 '?' 부분 (빈 query "/a?" 도 그대로),
 * path = 정규형 경로를 한 번 끝까지 디코딩한 것. 디코딩은 한 번만이라 "%252F" 는 path 에서 글자 "%2F" 다.
 *
 * Tiny 와 이전 코드는 라우팅을 원본 target, 파일을 디코딩·정리한 경로로 해서 같은 요청을 두 가지로
 * 읽었다 ("//admin/x", "/%61dmin/x", "/admin%2Fx" 가 프록시 라우트를 건너뜀, #31 재현).
 * 이제 라우팅·백엔드 전달·정적 파일이 모두 이 한 정규형에서 나온다.
 */
static int normalize_target(http_request *req, const http_parse_options *opt)
{
    static const char hex[] = "0123456789ABCDEF";
    const char *q = strchr(req->target, '?');
    size_t raw_len = q ? (size_t)(q - req->target) : strlen(req->target);
    char *norm = req->path;           /* 정규형 경로를 잠시 여기에 만든다 (원본보다 길어지지 않음) */
    size_t i, o = 0;

    if (q)
        strcpy(req->query, q + 1);
    else
        req->query[0] = '\0';

    for (i = 0; i < raw_len; i++) {
        unsigned char c = (unsigned char)req->target[i];

        if (c == '%') {
            int hi, lo;

            if (i + 2 >= raw_len)
                return 400;                                       /* 1 */
            hi = hex_value((unsigned char)req->target[i + 1]);
            lo = hex_value((unsigned char)req->target[i + 2]);
            if (hi < 0 || lo < 0)
                return 400;                                       /* 1 */
            c = (unsigned char)(hi * 16 + lo);
            i += 2;
            if (c == '/' || c == '\\' || c < 0x20 || c == 0x7F)
                return 400;                                       /* 2 */
            if (is_unreserved(c)) {
                norm[o++] = (char)c;                              /* 4 */
            } else {
                norm[o++] = '%';
                norm[o++] = hex[hi];
                norm[o++] = hex[lo];
            }
            continue;
        }
        if (c == '\\')
            return 400;                                           /* 2 */
        if (c == '/' && o > 0 && norm[o - 1] == '/') {            /* 3 */
            if (opt == NULL || !opt->merge_slashes)
                return 400;
            continue;
        }
        norm[o++] = (char)c;
    }

    /* target = 정규형 경로 + 원본의 '?' 부분. query 는 위에서 따로 복사해 두었다 */
    memcpy(req->target, norm, o);
    if (q) {
        req->target[o] = '?';
        strcpy(req->target + o + 1, req->query);
    } else {
        req->target[o] = '\0';
    }

    /* path = 정규형 경로를 끝까지 디코딩. 남은 %XX 는 위에서 검사해서 금지 바이트가 나오지 않는다 */
    for (i = 0, o = 0; req->target[i] != '\0' && req->target[i] != '?'; i++) {
        unsigned char c = (unsigned char)req->target[i];

        if (c == '%') {
            c = (unsigned char)(hex_value((unsigned char)req->target[i + 1]) * 16
                                + hex_value((unsigned char)req->target[i + 2]));
            i += 2;
        }
        req->path[o++] = (char)c;
    }
    req->path[o] = '\0';

    if (has_dot_segment(req->path))
        return 400;                                               /* 5 */
    return 0;
}

/* 1*DIGIT 만 받는다. 음수("-1"), 빈 값, 숫자 아닌 글자, long 넘침이면 -1 */
static int parse_content_length(const char *v, size_t n, long *out)
{
    long x = 0;
    size_t i;

    if (n == 0)
        return -1;
    for (i = 0; i < n; i++) {
        int d;

        if (v[i] < '0' || v[i] > '9')
            return -1;
        d = v[i] - '0';
        if (x > (LONG_MAX - d) / 10)
            return -1;
        x = x * 10 + d;
    }
    *out = x;
    return 0;
}

/* "close, Upgrade" 처럼 쉼표로 나뉜 토큰 목록에서 close / keep-alive 를 찾는다 */
static void parse_connection(const char *v, size_t n, header_state *hs)
{
    while (n > 0) {
        const char *comma = memchr(v, ',', n);
        size_t tok_len = comma ? (size_t)(comma - v) : n;
        const char *tok = v;
        size_t tlen = tok_len;

        trim_ows(&tok, &tlen);
        if (ci_equal(tok, tlen, "close"))
            hs->conn_close = 1;
        else if (ci_equal(tok, tlen, "keep-alive"))
            hs->conn_keep_alive = 1;

        if (comma == NULL)
            break;
        v += tok_len + 1;
        n -= tok_len + 1;
    }
}

/*
 * 헤더 한 줄 "Name: value" (CRLF 제외). 성공 0, 실패면 상태 코드.
 *   - 이름은 토큰 글자만: "Host : x"(콜론 앞 공백), " x"(obs-fold 이어 쓰기 줄) 는 400
 *   - 값에 제어 문자(탭 제외)가 있으면 400. 줄 안에 섞인 맨 LF·CR 도 여기서 걸린다.
 *     프록시가 이 헤더 블록을 그대로 백엔드에 넘기므로(#9), 줄 경계를 다르게 읽을 바이트는 미리 막는다
 */
static int parse_header_line(const char *line, size_t n, http_request *req, header_state *hs)
{
    const char *colon = memchr(line, ':', n);
    const char *v;
    size_t nlen, vlen, i;

    if (colon == NULL)
        return 400;
    nlen = (size_t)(colon - line);
    if (nlen == 0)
        return 400;
    for (i = 0; i < nlen; i++)
        if (!is_tchar((unsigned char)line[i]))
            return 400;

    v = colon + 1;
    vlen = n - nlen - 1;
    trim_ows(&v, &vlen);
    for (i = 0; i < vlen; i++) {
        unsigned char c = (unsigned char)v[i];
        if ((c < 0x20 && c != '\t') || c == 0x7F)
            return 400;
    }

    if (ci_equal(line, nlen, "host")) {
        if (hs->saw_host || vlen > HTTP_MAX_HOST)
            return 400;               /* Host 두 개는 어느 쪽을 믿을지 모른다 */
        for (i = 0; i < vlen; i++)    /* 잘못된 Host 값은 400 (RFC 9112 3.2). "example com" 등 */
            if (!is_host_char((unsigned char)v[i]))
                return 400;
        hs->saw_host = 1;
        memcpy(req->host, v, vlen);
        req->host[vlen] = '\0';
    } else if (ci_equal(line, nlen, "content-length")) {
        long cl;
        if (parse_content_length(v, vlen, &cl) < 0)
            return 400;
        /* 중복은 값이 같아도 400. RFC 는 같은 값이면 받아도 되지만, 프록시가 원본 헤더를
         * 그대로 넘기므로(#9) 백엔드가 두 줄을 다르게 해석할 여지를 아예 없앤다 */
        if (req->content_length >= 0)
            return 400;
        req->content_length = cl;
    } else if (ci_equal(line, nlen, "transfer-encoding")) {
        hs->saw_te = 1;
    } else if (ci_equal(line, nlen, "connection")) {
        parse_connection(v, vlen, hs);
    }
    return 0;
}

/*
 * 헤더 블록 p[0..n) (각 줄이 CRLF 로 끝남, 마지막 빈 줄은 포함 안 함).
 * 줄 형식 오류 400 을 먼저 다 보고, 그다음 Host 누락 400, Transfer-Encoding 501 순서.
 */
static int parse_headers(const char *p, size_t n, http_request *req)
{
    header_state hs;
    size_t pos = 0;

    memset(&hs, 0, sizeof hs);
    while (pos < n) {
        const char *line = p + pos;
        const char *cr = memchr(line, '\r', n - pos);
        size_t llen;
        int st;

        if (cr == NULL || cr + 1 >= p + n || cr[1] != '\n')
            return 400;               /* 줄 안의 맨 CR */
        llen = (size_t)(cr - line);
        st = parse_header_line(line, llen, req, &hs);
        if (st != 0)
            return st;
        pos += llen + 2;
    }

    if (req->version_minor == 1 && !hs.saw_host)
        return 400;                   /* HTTP/1.1 은 Host 필수 (RFC 9112 3.2) */
    if (hs.saw_te)
        return 501;                   /* chunked 는 범위 밖 */

    if (hs.conn_close)
        req->keep_alive = 0;
    else if (hs.conn_keep_alive)
        req->keep_alive = 1;
    return 0;
}

parse_result http_parse_request(const char *buf, size_t len, const http_parse_options *opt,
                                http_request *req, size_t *consumed, int *status)
{
    size_t window = len < HTTP_MAX_HEADER ? len : HTTP_MAX_HEADER;
    size_t skip = 0;
    const char *start, *head_end, *line_end;
    size_t head_len, line_len, body_len;
    int st;

    memset(req, 0, sizeof *req);
    req->content_length = -1;
    *consumed = 0;                    /* DONE: 소비한 길이, 본문 대기 NEED_MORE: 필요한 총 길이, 나머지 0 */
    *status = 0;

    /* 0. 요청 라인 앞의 빈 줄(CRLF)은 건너뛴다 (RFC 9112 2.2). keep-alive 에서 POST 본문 뒤에
     *    CRLF 를 하나 더 붙여 보내는 클라이언트가 있다. 건너뛴 바이트도 8KB 창과 consumed 에 들어간다 */
    while (skip + 2 <= window && buf[skip] == '\r' && buf[skip + 1] == '\n')
        skip += 2;
    start = buf + skip;

    /* 1. 431 을 가장 먼저 판정 (헤더 주석). 앞 8KB 안에서만 빈 줄을 찾으므로
     *    TCP 가 어떻게 나눠 보내든 같은 요청은 같은 결과가 나온다 */
    head_end = find(start, window - skip, "\r\n\r\n", 4);
    if (head_end == NULL) {
        if (len < HTTP_MAX_HEADER)
            return PARSE_NEED_MORE;
        /* 431 이어도 첫 줄이 와 있으면 버전은 채운다 (응답의 Connection 결정용, 헤더 주석).
         * 다른 필드도 채워질 수 있지만 ERROR 에서는 status 만 의미가 있다 */
        line_end = find(start, window - skip, "\r\n", 2);
        if (line_end != NULL)
            (void)parse_request_line(start, (size_t)(line_end - start), req);
        return fail(status, 431);
    }
    head_len = (size_t)(head_end - buf) + 4;   /* buf 처음부터 빈 줄 끝까지 */

    /* 2. 요청 라인: 첫 CRLF 까지. 빈 줄을 찾았으니 CRLF 는 반드시 있다 */
    line_end = find(start, (size_t)(head_end - start) + 4, "\r\n", 2);
    line_len = (size_t)(line_end - start);
    st = parse_request_line(start, line_len, req);
    if (st != 0)
        return fail(status, st);
    st = normalize_target(req, opt);
    if (st != 0)
        return fail(status, st);

    /* 3. 헤더: 요청 라인 다음 줄부터 빈 줄 직전까지 */
    req->keep_alive = req->version_minor == 1;
    req->headers = line_end + 2;
    req->headers_len = (size_t)(head_end + 2 - req->headers);   /* 마지막 헤더 줄의 CRLF 까지 */
    st = parse_headers(req->headers, req->headers_len, req);
    if (st != 0)
        return fail(status, st);

    /* 4. 본문: Content-Length 만큼. 없으면 본문 없음 (chunked 는 위에서 501) */
    if (req->content_length > HTTP_MAX_BODY)
        return fail(status, 413);
    body_len = req->content_length > 0 ? (size_t)req->content_length : 0;
    if (len - head_len < body_len) {
        /* 헤더는 끝났고 본문만 덜 옴: 요청 전체 길이를 알려 준다 (헤더 주석).
         * 서버는 in 이 이만큼 찰 때까지 다시 부르지 않는다. 본문이 잘게 와도 헤더를 매번
         * 다시 파싱하지 않게 하려는 것이다 (PR #24 리뷰). 소비한 바이트 수가 아니다 */
        *consumed = head_len + body_len;
        return PARSE_NEED_MORE;
    }
    req->body = body_len > 0 ? buf + head_len : NULL;
    req->body_len = body_len;

    *consumed = head_len + body_len;
    return PARSE_DONE;
}
