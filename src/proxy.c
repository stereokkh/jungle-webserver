/*
 * proxy.c — 업스트림(백엔드) 중계
 *
 * CS:APP 대응: 없음
 * 구현 상태: proxy_build_request (요청 다시 쓰기) 는 #8, 연결(proxy_connect_start·proxy_connect_result)과
 *   응답 중계(proxy_relay_*, 머리 해석 + 본문 길이 판단)는 #9 에서 구현했다.
 *   소켓을 만지는 것은 connect 두 함수뿐이고, proxy_relay_* 는 바이트와 buffer 만 다루는 순수 함수다
 *   (단위 테스트 tests/unit/test_proxy_relay.c). poll 루프와 read/write 는 server.c 몫이다.
 * 흐름 (#9): handler_route → ROUTE_PROXY → proxy_next_backend → proxy_connect_start
 *   → POLLOUT 에서 SO_ERROR 확인 → 요청 송신 → 응답 수신 → 클라이언트로 중계
 *   - 응답 시작 전 실패: 502 (연결 실패·깨진 응답) / 504 (타임아웃)
 *   - 응답 시작 후 실패: 연결 종료 + 자원 회수
 *   - 자동 재시도 없음
 * 담당: 트랙 B · 이슈 #8 (proxy_build_request), #9 (나머지)
 */
#include "proxy.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "net.h"

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
    int fd, saved;

    if (backend == NULL) {
        errno = EINVAL;
        return -1;
    }
    fd = socket(((const struct sockaddr *)&backend->addr)->sa_family, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (net_set_nonblocking(fd) < 0)
        goto fail;
    /* 논블로킹 connect 는 보통 EINPROGRESS 로 돌아오고 결과는 POLLOUT 뒤 SO_ERROR 로 본다.
     * EINTR 도 연결은 커널이 이어서 하는 중이다 (다시 connect 를 부르면 EALREADY).
     * 0 이어도 fd 를 돌려줘 서버는 같은 길(POLLOUT → proxy_connect_result)을 탄다 */
    if (connect(fd, (const struct sockaddr *)&backend->addr, backend->addrlen) < 0
        && errno != EINPROGRESS && errno != EINTR)
        goto fail;
    return fd;

fail:
    saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

int proxy_connect_result(int fd)
{
    int err = 0;
    socklen_t len = sizeof err;

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
        return errno;
    return err;
}

/* ---- 업스트림 응답 중계 ----
 * 머리만 해석하고 바이트는 그대로 넘긴다 (규칙은 proxy.h). 데이터는 NUL 로 끝나지 않으므로
 * 여기서도 str* 함수를 쓰지 않는다. 시작 전에는 머리를 r->head 에 모았다가, 해석이 끝나면
 * (머리 + 이미 받은 본문)을 out 에 한 번에 덧붙인다. 그래서 시작 전 실패는 out 을 건드리지 않아
 * 서버가 그 자리에 502 를 써 넣을 수 있다 */

#define RELAY_CL_LIMIT ((uint64_t)1 << 53)     /* 이보다 큰 Content-Length 는 거절 (C5) */

void proxy_relay_init(proxy_relay *r, int request_was_head)
{
    memset(r, 0, sizeof *r);
    buffer_init(&r->head);
    r->request_was_head = request_was_head != 0;
    r->body_mode = PROXY_BODY_UNTIL_EOF;
}

void proxy_relay_free(proxy_relay *r)
{
    buffer_free(&r->head);
}

static int is_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

/* Content-Length 값 v[0..n) → *out. 앞뒤 SP/TAB 은 뗀다. 숫자만 1자 이상, 2^53·size_t 초과는 -1 */
static int parse_content_length(const char *v, size_t n, uint64_t *out)
{
    uint64_t acc = 0;
    size_t b = 0, e = n, i;

    while (b < e && (v[b] == ' ' || v[b] == '\t'))
        b++;
    while (e > b && (v[e - 1] == ' ' || v[e - 1] == '\t'))
        e--;
    if (b == e)
        return -1;
    for (i = b; i < e; i++) {
        unsigned d;

        if (!is_digit((unsigned char)v[i]))
            return -1;
        d = (unsigned)(v[i] - '0');
        if (acc > (RELAY_CL_LIMIT - d) / 10)        /* acc * 10 + d 가 한도를 넘는다 */
            return -1;
        acc = acc * 10 + d;
    }
    if (acc > (uint64_t)SIZE_MAX)                   /* 32비트 size_t */
        return -1;
    *out = acc;
    return 0;
}

/* 이유 문구·헤더 값에 올 수 있는 바이트: TAB, 0x20-0x7E, 0x80 이상 (obs-text). 그 밖의 제어 문자는 불가 */
static int is_text_byte(unsigned char c)
{
    return c == '\t' || (c >= 0x20 && c != 0x7F);
}

/* 상태 줄 h[0..hl) 을 검사하고 줄 끝 다음 위치를 *next 에 준다. 맞으면 0 */
static int parse_status_line(const char *h, size_t hl, int *status, size_t *next)
{
    size_t i;

    if (hl < 14 || memcmp(h, "HTTP/1.", 7) != 0 || !is_digit((unsigned char)h[7]) || h[8] != ' ')
        return -1;
    for (i = 9; i < 12; i++)
        if (!is_digit((unsigned char)h[i]))
            return -1;
    *status = (h[9] - '0') * 100 + (h[10] - '0') * 10 + (h[11] - '0');
    i = 12;
    if (h[i] == ' ') {                              /* SP 이유 문구 (비어 있어도 된다) */
        for (i++; i < hl && h[i] != '\r'; i++)
            if (!is_text_byte((unsigned char)h[i]))
                return -1;
    }
    if (i + 1 >= hl || h[i] != '\r' || h[i + 1] != '\n')    /* 숫자 바로 뒤가 SP·CRLF 가 아니면 여기서 걸린다 */
        return -1;
    *next = i + 2;
    return 0;
}

/* 머리 h[0..hl) (맨 끝이 빈 줄의 CRLF) 를 해석한다. 성공 0. 결과: 상태, Content-Length, Transfer-Encoding 유무 */
static int parse_response_head(const char *h, size_t hl, int *status, int *has_cl, uint64_t *cl,
                               int *has_te)
{
    size_t pos, i, colon;
    uint64_t v;
    int bad_cl = 0;

    *has_cl = *has_te = 0;
    if (parse_status_line(h, hl, status, &pos) < 0)
        return -1;
    if (*status < 200 || *status > 599)             /* 1xx 는 BAD (C1) */
        return -1;
    while (pos < hl) {
        if (h[pos] == '\r')                         /* 빈 줄 = 머리 끝 */
        {
            if (pos + 2 != hl || h[pos + 1] != '\n')
                return -1;
            /* Content-Length 가 깨졌어도 Transfer-Encoding 이 있으면 어차피 무시한다 (C2) */
            return (bad_cl && !*has_te) ? -1 : 0;
        }
        if (h[pos] == ' ' || h[pos] == '\t')        /* obs-fold */
            return -1;
        for (i = pos; i < hl && h[i] != ':'; i++)
            if (!is_tchar((unsigned char)h[i]))
                return -1;                          /* 이름이 토큰 글자가 아니다 ("Foo : x", 줄바꿈) */
        if (i == hl || i == pos)
            return -1;                              /* ':' 없음, 빈 이름 */
        colon = i;
        for (i++; i < hl && h[i] != '\r'; i++)
            if (!is_text_byte((unsigned char)h[i]))
                return -1;                          /* 맨 LF 도 여기서 걸린다 */
        if (i + 1 >= hl || h[i + 1] != '\n')
            return -1;
        if (name_eq(h + pos, colon - pos, "content-length", 14)) {
            if (parse_content_length(h + colon + 1, i - (colon + 1), &v) < 0) {
                bad_cl = 1;                         /* TE 가 같이 있는지는 머리 끝에서 본다 */
            } else {
                if (*has_cl && v != *cl)            /* 줄이 여럿이면 값이 같아야 한다 (밀수 방지) */
                    bad_cl = 1;
                *has_cl = 1;
                *cl = v;
            }
        } else if (name_eq(h + pos, colon - pos, "transfer-encoding", 17)) {
            *has_te = 1;
        }
        pos = i + 2;
    }
    return -1;                                      /* 빈 줄 없이 끝남 (호출자가 CRLFCRLF 를 보장하므로 안 온다) */
}

/* 시작 후: 본문 바이트를 out 에 넘긴다 (끝을 넘는 바이트는 버린다) */
static proxy_relay_result relay_body(proxy_relay *r, const char *data, size_t n, buffer *out)
{
    size_t take = n;

    if (r->body_mode == PROXY_BODY_NONE)
        return PROXY_RELAY_COMPLETE;
    if (r->body_mode == PROXY_BODY_LENGTH && take > r->body_left)
        take = r->body_left;
    if (take > 0 && buffer_append(out, data, take) < 0)
        return PROXY_RELAY_NOMEM;
    if (r->body_mode == PROXY_BODY_LENGTH) {
        r->body_left -= take;
        if (r->body_left == 0)
            return PROXY_RELAY_COMPLETE;
    }
    return PROXY_RELAY_MORE;
}

/* 시작 전: 모은 바이트에 머리가 다 있으면 해석해서 시작한다 */
static proxy_relay_result relay_try_start(proxy_relay *r, buffer *out)
{
    const char *p = buffer_peek(&r->head);
    size_t have = buffer_pending(&r->head);
    size_t scan = have < HTTP_MAX_HEADER ? have : HTTP_MAX_HEADER;
    size_t end = 0, i, take;
    int status, has_cl, has_te;
    uint64_t cl = 0;

    /* 빈 줄(CRLFCRLF)을 앞 HTTP_MAX_HEADER 바이트 안에서 찾는다. 맨 LF 는 찾으면서 바로 거절한다
     * (끝나지 않는 머리를 타임아웃까지 기다리지 않으려고) */
    for (i = 0; i < scan; i++) {
        if (p[i] != '\n')
            continue;
        if (i == 0 || p[i - 1] != '\r')
            return PROXY_RELAY_BAD;
        if (i >= 3 && p[i - 2] == '\n' && p[i - 3] == '\r') {
            end = i + 1;
            break;
        }
    }
    if (end == 0)
        return have > HTTP_MAX_HEADER ? PROXY_RELAY_BAD : PROXY_RELAY_MORE;
    if (parse_response_head(p, end, &status, &has_cl, &cl, &has_te) < 0)
        return PROXY_RELAY_BAD;

    /* 본문 길이 규칙: HEAD·204·304 는 없음 (결정 3), TE 는 EOF 까지 (C2), CL 은 그 길이, 아무것도 없으면 EOF 까지 */
    if (r->request_was_head || status == 204 || status == 304 || (!has_te && has_cl && cl == 0)) {
        r->body_mode = PROXY_BODY_NONE;
        r->body_left = 0;
    } else if (has_te || !has_cl) {
        r->body_mode = PROXY_BODY_UNTIL_EOF;
        r->body_left = 0;
    } else {
        r->body_mode = PROXY_BODY_LENGTH;
        r->body_left = (size_t)cl;
    }

    take = have - end;                              /* 머리와 함께 받은 본문 바이트 */
    if (r->body_mode == PROXY_BODY_NONE)
        take = 0;
    else if (r->body_mode == PROXY_BODY_LENGTH && take > r->body_left)
        take = r->body_left;
    /* 머리와 본문은 head 버퍼에 이어져 있어 한 번에 덧붙인다. buffer_append 는 실패하면 out 을 건드리지 않으므로
     * mark/rollback 이 따로 필요 없다. 실패하면 started 도 head 도 그대로다 */
    if (buffer_append(out, p, end + take) < 0)
        return PROXY_RELAY_NOMEM;
    r->status = status;
    r->started = 1;
    if (r->body_mode == PROXY_BODY_LENGTH)
        r->body_left -= take;
    buffer_free(&r->head);
    if (r->body_mode == PROXY_BODY_NONE || (r->body_mode == PROXY_BODY_LENGTH && r->body_left == 0))
        return PROXY_RELAY_COMPLETE;
    return PROXY_RELAY_MORE;
}

proxy_relay_result proxy_relay_input(proxy_relay *r, const char *data, size_t n, buffer *out)
{
    if (r->started)
        return relay_body(r, data, n, out);
    if (n > 0 && buffer_append(&r->head, data, n) < 0)
        return PROXY_RELAY_NOMEM;
    return relay_try_start(r, out);                 /* n == 0 이면 NOMEM 뒤 재시도 */
}

proxy_relay_result proxy_relay_eof(proxy_relay *r)
{
    if (!r->started)
        return PROXY_RELAY_BAD;
    if (r->body_mode == PROXY_BODY_LENGTH && r->body_left > 0)
        return PROXY_RELAY_TRUNCATED;
    return PROXY_RELAY_COMPLETE;
}

size_t proxy_relay_want(const proxy_relay *r, size_t room)
{
    size_t have;

    if (!r->started) {
        have = buffer_pending(&r->head);
        return have > HTTP_MAX_HEADER ? 0 : HTTP_MAX_HEADER + 1 - have;
    }
    if (r->body_mode == PROXY_BODY_NONE)
        return 0;
    if (r->body_mode == PROXY_BODY_LENGTH && r->body_left < room)
        return r->body_left;
    return room;
}
