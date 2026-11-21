/*
 * http_response.c — 상태 라인·헤더·오류 응답 생성
 *
 * CS:APP 대응:
 *   clienterror(): sprintf 로 HTML 본문을 만들고 "HTTP/1.0 %s %s" + Content-type 전송.
 *                  본문에 cause(요청 경로)를 그대로 넣어 반사형 XSS, Content-length 없음 (이슈 #3 관찰)
 *   serve_static(): "HTTP/1.0 200 OK", Server, Content-length, Content-type 을 Rio_writen 4번에 나눠 전송
 *   Tiny의 sprintf(buf, "%s...", buf) 패턴은 정의되지 않은 동작이다 (이슈 #3 관찰).
 *   여기서는 buffer_appendf(vsnprintf 로 길이를 먼저 잼)로 덧붙이고, 실패하면 -1.
 * 담당: 트랙 C · 이슈 #6 (단위 테스트: tests/unit/test_http_response.c)
 *
 * 실패 처리: 함수 시작에 buffer_mark, 중간 어느 단계든 실패하면 buffer_rollback 으로
 *            호출 전 상태로 되돌린다 (반쯤 쓴 헤더가 나가지 않게, docs/interfaces.md 규칙).
 */
#include "http_response.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define SERVER_NAME "jungle-webserver"

/* Date 헤더 길이: "Sun, 06 Nov 1994 08:49:37 GMT" = 29 자 (RFC 9110 5.6.7 IMF-fixdate) */
#define HTTP_DATE_LEN 29

const char *http_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Content Too Large";          /* RFC 9110 이름 (옛 이름 Payload Too Large) */
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

/*
 * 현재 시각을 "Sat, 21 Nov 2026 06:00:00 GMT" 형식으로 dst 에 쓴다. 성공 0, 실패 -1.
 * strftime 의 %a %b 는 setlocale 에 따라 "토", "11월" 처럼 바뀔 수 있어서 영어 이름을 직접 쓴다.
 */
static int format_http_date(char *dst, size_t cap)
{
    static const char *const wday[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    time_t now = time(NULL);
    struct tm tm;
    int n;

    if (now == (time_t)-1 || gmtime_r(&now, &tm) == NULL)   /* gmtime_r: 스레드 안전한 POSIX 판 */
        return -1;
    if (tm.tm_wday < 0 || tm.tm_wday > 6 || tm.tm_mon < 0 || tm.tm_mon > 11)
        return -1;
    n = snprintf(dst, cap, "%s, %02d %s %04d %02d:%02d:%02d GMT",
                 wday[tm.tm_wday], tm.tm_mday, mon[tm.tm_mon], tm.tm_year + 1900,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    return (n == HTTP_DATE_LEN && (size_t)n < cap) ? 0 : -1;
}

/* NULL, "" 이거나 CRLF 로 끝나면 1 (호출 코드의 실수 검사, http_response.h 참고) */
static int extra_headers_ok(const char *x)
{
    size_t n;

    if (x == NULL || x[0] == '\0')
        return 1;
    n = strlen(x);
    return n >= 2 && x[n - 2] == '\r' && x[n - 1] == '\n';
}

int http_append_header(buffer *out, const http_response_head *h)
{
    char date[HTTP_DATE_LEN + 1];
    size_t mark;
    int keep_alive;

    if (out == NULL || h == NULL || !extra_headers_ok(h->extra_headers))
        return -1;

    /* 길이를 모르면 연결 종료가 응답의 끝이므로 keep-alive 를 쓸 수 없다 */
    keep_alive = h->keep_alive && h->content_length >= 0;

    mark = buffer_mark(out);

    if (buffer_appendf(out, "HTTP/1.1 %d %s\r\n", h->status, http_reason(h->status)) < 0)
        goto fail;
    /* 시계를 못 읽는 경우는 Date 만 생략한다 (응답 자체는 보낸다) */
    if (format_http_date(date, sizeof date) == 0 &&
        buffer_appendf(out, "Date: %s\r\n", date) < 0)
        goto fail;
    if (buffer_append_str(out, "Server: " SERVER_NAME "\r\n") < 0)
        goto fail;
    if (h->content_type != NULL &&
        buffer_appendf(out, "Content-Type: %s\r\n", h->content_type) < 0)
        goto fail;
    if (h->content_length >= 0 &&
        buffer_appendf(out, "Content-Length: %ld\r\n", h->content_length) < 0)
        goto fail;
    if (buffer_appendf(out, "Connection: %s\r\n", keep_alive ? "keep-alive" : "close") < 0)
        goto fail;
    if (h->extra_headers != NULL && buffer_append_str(out, h->extra_headers) < 0)
        goto fail;
    if (buffer_append(out, "\r\n", 2) < 0)                   /* 빈 줄 = 헤더 끝 */
        goto fail;
    return 0;

fail:
    buffer_rollback(out, mark);
    return -1;
}

int http_append_error(buffer *out, int status, int keep_alive, int head_only,
                      const char *extra_headers)
{
    char body[128];
    http_response_head h;
    size_t mark;
    int n;

    if (out == NULL)
        return -1;

    /* 본문을 먼저 만들어야 Content-Length 를 쓸 수 있다. 요청에서 온 값은 넣지 않는다 (XSS 방지) */
    n = snprintf(body, sizeof body, "<html><body><h1>%d %s</h1></body></html>\n",
                 status, http_reason(status));
    if (n < 0 || (size_t)n >= sizeof body)
        return -1;

    h.status = status;
    h.content_type = "text/html; charset=utf-8";
    h.content_length = n;          /* HEAD 여도 GET 과 같은 길이 (docs/interfaces.md) */
    h.keep_alive = keep_alive;
    h.extra_headers = extra_headers;

    mark = buffer_mark(out);
    if (http_append_header(out, &h) < 0)                     /* 실패하면 이미 되돌려져 있음 */
        return -1;
    if (!head_only && buffer_append(out, body, (size_t)n) < 0) {
        buffer_rollback(out, mark);                          /* 헤더만 나가지 않게 헤더까지 되돌림 */
        return -1;
    }
    return 0;
}
