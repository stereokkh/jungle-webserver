/*
 * proxy.c — 업스트림(백엔드) 중계
 *
 * CS:APP 대응: 없음
 * 구현 상태: proxy_build_request (요청 다시 쓰기) 는 #8 에서 구현했다.
 *   연결·중계(proxy_connect_start 와 아래 흐름)는 #9 에서 구현한다.
 * 흐름 (#9): handler_route → ROUTE_PROXY → proxy_next_backend → proxy_connect_start
 *   → POLLOUT 에서 SO_ERROR 확인 → 요청 송신 → 응답 수신 → 클라이언트로 중계
 *   - 응답 시작 전 실패: 502 (연결 실패) / 504 (타임아웃)
 *   - 응답 시작 후 실패: 연결 종료 + 자원 회수
 *   - 자동 재시도 없음
 * 담당: 트랙 B · 이슈 #8 (proxy_build_request), #9 (나머지)
 */
#include "proxy.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* ---- 헤더 블록 읽기 도우미 ----
 * req->headers 는 입력 버퍼 안을 가리키고 NUL 로 끝나지 않는다.
 * 그래서 이 파일의 헤더 처리에는 str* 함수를 쓰지 않고 memchr / memcmp / 직접 반복문만 쓴다.
 * (http_request.c 에도 비슷한 도우미가 있지만 거기를 고치지 않으려고 여기에 따로 둔다) */

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

/* a[0..an) 와 b[0..bn) 가 대소문자 무시로 완전히 같은가 */
static int name_eq(const char *a, size_t an, const char *b, size_t bn)
{
    size_t i;

    if (an != bn)
        return 0;
    for (i = 0; i < an; i++)
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i]))
            return 0;
    return 1;
}

/* 헤더 줄 하나: [start, start+len) 은 CRLF 포함, name 은 ':' 앞, value 는 ':' 뒤 CRLF 앞 */
typedef struct {
    const char *start;
    size_t len;
    const char *name;
    size_t nlen;
    const char *value;
    size_t vlen;
} header_line;

static int is_tchar(unsigned char c)
{
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        return 1;
    return c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

/* h[pos] 에서 시작하는 줄을 읽는다. 파서가 만든 블록이면 항상 성공하고,
 * 파서(http_request.c parse_header_line)가 400 으로 막는 모양이면 -1:
 * CRLF 가 없음, ':' 가 없음, 이름이 비었거나 토큰 글자가 아님("Host : x", " x"),
 * 값에 제어 문자(탭 제외)나 맨 CR·LF 가 있음. 이 함수를 직접 부르는 호출자에게도 같은 기준을 준다 */
static int read_line(const char *h, size_t hlen, size_t pos, header_line *ln)
{
    const char *lf = memchr(h + pos, '\n', hlen - pos);
    const char *colon;
    size_t eol, i;

    if (lf == NULL || lf == h + pos || lf[-1] != '\r')
        return -1;
    eol = (size_t)(lf - h) - 1;                 /* CR 의 위치 */
    colon = memchr(h + pos, ':', eol - pos);
    if (colon == NULL || colon == h + pos)
        return -1;
    for (i = pos; i < (size_t)(colon - h); i++)
        if (!is_tchar((unsigned char)h[i]))
            return -1;
    for (i = (size_t)(colon - h) + 1; i < eol; i++) {
        unsigned char c = (unsigned char)h[i];

        if ((c < 0x20 && c != '\t') || c == 0x7F)
            return -1;
    }
    ln->start = h + pos;
    ln->len = (size_t)(lf - h) + 1 - pos;
    ln->name = h + pos;
    ln->nlen = (size_t)(colon - (h + pos));
    ln->value = colon + 1;
    ln->vlen = eol - (size_t)(colon + 1 - h);
    return 0;
}

/* 어떤 Connection 줄이든 그 값(쉼표로 나눈 토큰)에 name 이 있는가.
 * 헤더 줄 뒤에 오는 Connection 줄도 센다. O(줄 수²) 이지만 헤더가 8KB 로 제한돼 작고,
 * 고정 크기 배열이 없어서 넘칠 일이 없다 */
static int listed_in_connection(const char *h, size_t hlen, const char *name, size_t nlen)
{
    size_t pos = 0;
    header_line ln;

    while (pos < hlen && read_line(h, hlen, pos, &ln) == 0) {
        pos += ln.len;
        if (!name_eq(ln.name, ln.nlen, "connection", 10))
            continue;
        for (size_t i = 0; i <= ln.vlen; ) {
            size_t j = i, b, e;

            while (j < ln.vlen && ln.value[j] != ',')
                j++;
            b = i;
            e = j;
            while (b < e && (ln.value[b] == ' ' || ln.value[b] == '\t'))
                b++;
            while (e > b && (ln.value[e - 1] == ' ' || ln.value[e - 1] == '\t'))
                e--;
            if (e > b && name_eq(ln.value + b, e - b, name, nlen))
                return 1;
            i = j + 1;
        }
    }
    return 0;
}

/* 이 헤더 줄을 백엔드로 넘기지 않는가 (이유는 proxy.h 주석) */
static int is_dropped(const char *h, size_t hlen, const header_line *ln)
{
    static const char *const names[] = {
        "connection", "keep-alive", "te", "trailer", "transfer-encoding", "upgrade",
        "host", "x-forwarded-for", "content-length", "expect",
    };
    size_t i;

    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (name_eq(ln->name, ln->nlen, names[i], strlen(names[i])))
            return 1;
    if (ln->nlen >= 6 && name_eq(ln->name, 6, "proxy-", 6))
        return 1;
    return listed_in_connection(h, hlen, ln->name, ln->nlen);
}

/* 바이트가 0x21-0x7E (보이는 ASCII, 공백·제어 문자 없음) 인 비어 있지 않은 문자열인가 */
static int is_visible_token(const char *s, size_t max)
{
    size_t n = 0;

    while (n < max && s[n] != '\0') {
        if ((unsigned char)s[n] < 0x21 || (unsigned char)s[n] > 0x7E)
            return 0;
        n++;
    }
    return n > 0 && n < max;
}

int proxy_build_request(const http_request *req, const proxy_backend *backend,
                        const char *client_ip, buffer *out)
{
    size_t mark, pos = 0;
    header_line ln;

    if (req == NULL || backend == NULL || client_ip == NULL || out == NULL)
        return -1;
    if (!is_visible_token(req->method_str, sizeof req->method_str)
        || !is_visible_token(req->target, sizeof req->target) || req->target[0] != '/')
        return -1;                                              /* 요청 줄 주입·범위 밖 읽기 차단 */
    if (!is_visible_token(backend->name, sizeof backend->name))
        return -1;
    if (!is_visible_token(client_ip, strlen(client_ip) + 1))   /* 헤더 주입 차단 */
        return -1;
    if (req->content_length >= 0 && (size_t)req->content_length != req->body_len)
        return -1;
    if (req->content_length < 0 && req->body_len > 0)           /* 길이 없는 본문은 밀수 모양 */
        return -1;
    if (req->body_len > 0 && req->body == NULL)
        return -1;
    if (req->headers_len > 0 && req->headers == NULL)
        return -1;

    mark = buffer_mark(out);

    if (buffer_appendf(out, "%s %s HTTP/1.1\r\n", req->method_str, req->target) < 0)
        goto fail;

    while (pos < req->headers_len) {
        if (read_line(req->headers, req->headers_len, pos, &ln) < 0)
            goto fail;
        pos += ln.len;
        if (is_dropped(req->headers, req->headers_len, &ln))
            continue;
        if (buffer_append(out, ln.start, ln.len) < 0)
            goto fail;
    }

    /* 프록시가 쓰는 줄은 필터링이 끝난 뒤에 만들므로 클라이언트가 지울 수 없다.
     * Via: RFC 9110 7.6.3 은 게이트웨이가 요청마다 Via 를 보내라고 한다 (가명 사용 가능) */
    if (buffer_appendf(out, "Host: %s\r\nX-Forwarded-For: %s\r\nVia: 1.1 jungle-webserver\r\n",
                       backend->name, client_ip) < 0)
        goto fail;
    if (req->content_length >= 0
        && buffer_appendf(out, "Content-Length: %zu\r\n", req->body_len) < 0)
        goto fail;
    if (buffer_append_str(out, "Connection: close\r\n\r\n") < 0)
        goto fail;
    /* 다시 쓴 머리(요청 줄 ~ 빈 줄)는 원래보다 길어진다 (Host 이름, X-Forwarded-For, Via, Connection).
     * 프런트 한도에 꽉 찬 요청은 같은 바이너리인 백엔드의 한도(HTTP_MAX_HEADER)를 넘어 431 을 받으므로
     * 보내지 않고 여기서 실패시킨다 */
    if (buffer_pending(out) - mark > HTTP_MAX_HEADER)
        goto fail;
    if (req->body_len > 0 && buffer_append(out, req->body, req->body_len) < 0)
        goto fail;
    return 0;

fail:
    buffer_rollback(out, mark);
    return -1;
}

int proxy_connect_start(const proxy_backend *backend)
{
    /* TODO(#9): socket → net_set_nonblocking → connect. EINPROGRESS 면 fd 반환 */
    (void)backend;
    return -1;
}
