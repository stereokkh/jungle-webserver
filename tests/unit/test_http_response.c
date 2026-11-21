/*
 * test_http_response.c — http_response 모듈 단위 테스트 (이슈 #6)
 *
 * Date 헤더가 매초 바뀌므로 응답 전체를 바이트 비교하지 않고, out_str 로 복사해 필요한 줄을 strstr 로 찾는다.
 */
#include "test.h"

#include "buffer.h"
#include "http_response.h"

/* out 의 남은 바이트를 NUL 종료 문자열로 복사한다 (strstr 용). 호출자가 free */
static char *out_str(const buffer *out)
{
    size_t n = buffer_pending(out);
    char *s = malloc(n + 1);
    if (s == NULL) {
        perror("malloc");
        exit(2);
    }
    memcpy(s, buffer_peek(out), n);
    s[n] = '\0';
    return s;
}

/* s 안에 sub 가 몇 번 나오는지 */
static int count(const char *s, const char *sub)
{
    int c = 0;
    size_t n = strlen(sub);
    for (s = strstr(s, sub); s != NULL; s = strstr(s + n, sub))
        c++;
    return c;
}

/* "Content-Length: N" 의 N. 없으면 -1 */
static long content_length_of(const char *s)
{
    const char *p = strstr(s, "\r\nContent-Length: ");
    return p ? strtol(p + 18, NULL, 10) : -1;
}

static void test_reason(void)
{
    CHECK_STR(http_reason(200), "OK");
    CHECK_STR(http_reason(400), "Bad Request");
    CHECK_STR(http_reason(403), "Forbidden");
    CHECK_STR(http_reason(404), "Not Found");
    CHECK_STR(http_reason(405), "Method Not Allowed");
    CHECK_STR(http_reason(413), "Content Too Large");
    CHECK_STR(http_reason(414), "URI Too Long");
    CHECK_STR(http_reason(431), "Request Header Fields Too Large");
    CHECK_STR(http_reason(500), "Internal Server Error");
    CHECK_STR(http_reason(501), "Not Implemented");
    CHECK_STR(http_reason(502), "Bad Gateway");
    CHECK_STR(http_reason(504), "Gateway Timeout");
    CHECK_STR(http_reason(505), "HTTP Version Not Supported");
    CHECK_STR(http_reason(201), "Unknown");
    CHECK_STR(http_reason(-1), "Unknown");
}

/* 헤더의 모든 줄이 순서대로, 빈 줄로 끝난다 */
static void test_header_full(void)
{
    buffer out = BUFFER_INIT;
    http_response_head h = { 200, "text/plain", 5, 1, "X-Instance: 8001\r\n" };
    char *s;
    const char *date, *server, *type, *len, *conn, *inst;

    REQUIRE(http_append_header(&out, &h) == 0);
    s = out_str(&out);

    CHECK(strncmp(s, "HTTP/1.1 200 OK\r\n", 17) == 0);
    date = strstr(s, "\r\nDate: ");
    server = strstr(s, "\r\nServer: jungle-webserver\r\n");
    type = strstr(s, "\r\nContent-Type: text/plain\r\n");
    len = strstr(s, "\r\nContent-Length: 5\r\n");
    conn = strstr(s, "\r\nConnection: keep-alive\r\n");
    inst = strstr(s, "\r\nX-Instance: 8001\r\n");
    CHECK(date && server && type && len && conn && inst);
    CHECK(date < server && server < type && type < len && len < conn && conn < inst);

    /* 빈 줄은 맨 끝에 한 번만 */
    CHECK_INT(count(s, "\r\n\r\n"), 1);
    CHECK(strcmp(s + strlen(s) - 4, "\r\n\r\n") == 0);

    free(s);
    buffer_free(&out);
}

/* Date: "Sat, 21 Nov 2026 06:00:00 GMT" 형식 (값은 매초 바뀌므로 모양만 본다) */
static void test_date_format(void)
{
    static const char *const wday = "Sun,Mon,Tue,Wed,Thu,Fri,Sat,";
    static const char *const mon = "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec ";
    buffer out = BUFFER_INIT;
    http_response_head h = { 200, NULL, 0, 1, NULL };
    char *s, w[5] = { 0 }, m[5] = { 0 };
    const char *d, *end;

    REQUIRE(http_append_header(&out, &h) == 0);
    s = out_str(&out);
    d = strstr(s, "\r\nDate: ");
    REQUIRE(d != NULL);
    d += 8;
    end = strstr(d, "\r\n");
    REQUIRE(end != NULL);

    CHECK_INT(end - d, 29);
    memcpy(w, d, 4);          /* "Sat," */
    memcpy(m, d + 8, 4);      /* "Nov " */
    CHECK(strstr(wday, w) != NULL);
    CHECK(strstr(mon, m) != NULL);
    CHECK(d[4] == ' ' && d[7] == ' ' && d[11] == ' ' && d[16] == ' ');
    CHECK(d[19] == ':' && d[22] == ':');
    CHECK(strncmp(d + 25, " GMT", 4) == 0);

    free(s);
    buffer_free(&out);
}

/* content_type NULL → Content-Type 없음, content_length < 0 → Content-Length 없음 + 항상 close */
static void test_header_omit(void)
{
    buffer out = BUFFER_INIT;
    http_response_head h = { 200, NULL, -1, 1, NULL };
    char *s;

    REQUIRE(http_append_header(&out, &h) == 0);
    s = out_str(&out);
    CHECK(strstr(s, "Content-Type:") == NULL);
    CHECK(strstr(s, "Content-Length:") == NULL);
    CHECK(strstr(s, "\r\nConnection: close\r\n") != NULL);   /* keep_alive = 1 이어도 */
    CHECK(strstr(s, "keep-alive") == NULL);
    free(s);
    buffer_free(&out);
}

/* keep_alive 0 → close. Content-Length 0 은 생략하지 않는다 */
static void test_header_close_and_zero_length(void)
{
    buffer out = BUFFER_INIT;
    http_response_head h = { 200, "text/plain", 0, 0, "" };
    char *s;

    REQUIRE(http_append_header(&out, &h) == 0);
    s = out_str(&out);
    CHECK(strstr(s, "\r\nConnection: close\r\n") != NULL);
    CHECK(strstr(s, "\r\nContent-Length: 0\r\n") != NULL);
    CHECK_INT(count(s, "\r\n\r\n"), 1);                       /* "" 은 추가 헤더 없음 */
    free(s);
    buffer_free(&out);
}

/* extra_headers 가 CRLF 로 끝나지 않으면 -1, out 은 그대로 */
static void test_header_bad_extra(void)
{
    buffer out = BUFFER_INIT;
    http_response_head h = { 405, NULL, 0, 1, "Allow: GET, HEAD" };

    REQUIRE(buffer_append_str(&out, "keep") == 0);
    CHECK_INT(http_append_header(&out, &h), -1);
    CHECK_MEM(buffer_peek(&out), buffer_pending(&out), "keep", 4);

    h.extra_headers = "\n";
    CHECK_INT(http_append_header(&out, &h), -1);
    CHECK_INT(http_append_error(&out, 405, 1, 0, "Allow: GET"), -1);

    /* 빈 줄이 끼면 헤더가 거기서 끝난 것으로 읽혀 본문 길이가 어긋난다 */
    CHECK_INT(http_append_error(&out, 405, 1, 0, "Allow: POST\r\n\r\n"), -1);
    CHECK_INT(http_append_error(&out, 405, 1, 0, "\r\n"), -1);
    CHECK_INT(http_append_error(&out, 405, 1, 0, "Allow: POST\r\n\r\nX-Instance: 8001\r\n"), -1);
    CHECK_MEM(buffer_peek(&out), buffer_pending(&out), "keep", 4);

    /* 짝 없는 LF·CR: LF 로만 줄을 끊는 쪽에서는 "\n\r\n" 이 빈 줄 (응답 분할) */
    CHECK_INT(http_append_error(&out, 405, 1, 0, "Allow: POST\n\r\n"), -1);
    CHECK_INT(http_append_error(&out, 405, 1, 0, "Allow: POST\nX-Evil: 1\r\n"), -1);
    CHECK_INT(http_append_error(&out, 405, 1, 0, "Allow: POST\rX-Evil: 1\r\n"), -1);
    CHECK_INT(http_append_error(&out, 405, 1, 0, "Allow: POST\r\nX-Evil: 1\r"), -1);
    CHECK_MEM(buffer_peek(&out), buffer_pending(&out), "keep", 4);

    /* Content-Type 에 줄바꿈이 있으면 그 뒤가 새 헤더가 된다 */
    h.extra_headers = NULL;
    h.content_type = "text/html\r\nSet-Cookie: a=1";
    CHECK_INT(http_append_header(&out, &h), -1);
    h.content_type = "text/html\nX: 1";
    CHECK_INT(http_append_header(&out, &h), -1);
    CHECK_MEM(buffer_peek(&out), buffer_pending(&out), "keep", 4);
    h.content_type = NULL;

    /* 여러 줄은 괜찮다 */
    CHECK_INT(http_append_error(&out, 405, 1, 1, "Allow: POST\r\nX-Instance: 8001\r\n"), 0);
    buffer_clear(&out);
    REQUIRE(buffer_append_str(&out, "keep") == 0);

    /* 상태 코드는 3자리 (100~599) */
    h.extra_headers = NULL;
    h.status = 99;
    CHECK_INT(http_append_header(&out, &h), -1);
    h.status = 600;
    CHECK_INT(http_append_header(&out, &h), -1);
    CHECK_INT(http_append_error(&out, 0, 0, 0, NULL), -1);
    CHECK_INT(http_append_error(&out, 1000, 0, 0, NULL), -1);
    CHECK_MEM(buffer_peek(&out), buffer_pending(&out), "keep", 4);
    h.status = 100;
    CHECK_INT(http_append_header(&out, &h), 0);
    buffer_clear(&out);
    h.status = 599;
    CHECK_INT(http_append_header(&out, &h), 0);

    CHECK_INT(http_append_header(NULL, &h), -1);
    CHECK_INT(http_append_header(&out, NULL), -1);
    buffer_free(&out);
}

/* 이미 있는 내용 뒤에 덧붙인다 (앞 내용은 그대로) */
static void test_header_appends(void)
{
    buffer out = BUFFER_INIT;
    http_response_head h = { 404, NULL, 0, 0, NULL };
    char *s;

    REQUIRE(buffer_append_str(&out, "prev") == 0);
    REQUIRE(http_append_header(&out, &h) == 0);
    s = out_str(&out);
    CHECK(strncmp(s, "prevHTTP/1.1 404 Not Found\r\n", 28) == 0);
    free(s);
    buffer_free(&out);
}

/* 오류 응답: 고정 HTML 본문, Content-Length = 실제 본문 길이 */
static void test_error_body(void)
{
    buffer out = BUFFER_INIT;
    char *s;
    const char *body;

    REQUIRE(http_append_error(&out, 404, 0, 0, NULL) == 0);
    s = out_str(&out);
    CHECK(strncmp(s, "HTTP/1.1 404 Not Found\r\n", 24) == 0);
    CHECK(strstr(s, "\r\nContent-Type: text/html; charset=utf-8\r\n") != NULL);
    CHECK(strstr(s, "\r\nConnection: close\r\n") != NULL);
    body = strstr(s, "\r\n\r\n");
    REQUIRE(body != NULL);
    body += 4;
    CHECK_STR(body, "<html><body><h1>404 Not Found</h1></body></html>\n");
    CHECK_INT(content_length_of(s), (long)strlen(body));
    free(s);
    buffer_free(&out);
}

/* HEAD: 헤더(Content-Length 포함)는 GET 과 같고 본문만 없다 */
static void test_error_head_only(void)
{
    buffer get = BUFFER_INIT, head = BUFFER_INIT;
    char *g, *h;

    REQUIRE(http_append_error(&get, 403, 1, 0, NULL) == 0);
    REQUIRE(http_append_error(&head, 403, 1, 1, NULL) == 0);
    g = out_str(&get);
    h = out_str(&head);

    CHECK(content_length_of(h) > 0);
    CHECK_INT(content_length_of(h), content_length_of(g));
    CHECK(strcmp(h + strlen(h) - 4, "\r\n\r\n") == 0);       /* 빈 줄 뒤에 아무것도 없음 */
    CHECK(strstr(h, "<html>") == NULL);
    CHECK(strstr(h, "\r\nConnection: keep-alive\r\n") != NULL);

    free(g);
    free(h);
    buffer_free(&get);
    buffer_free(&head);
}

/* 405: 호출자가 넘긴 Allow, X-Instance 가 그대로 들어간다 (정적 / 앱 echo) */
static void test_error_405_extra(void)
{
    buffer out = BUFFER_INIT;
    char *s;

    REQUIRE(http_append_error(&out, 405, 1, 0, "Allow: GET, HEAD\r\n") == 0);
    s = out_str(&out);
    CHECK(strstr(s, "\r\nAllow: GET, HEAD\r\n") != NULL);
    free(s);
    buffer_clear(&out);

    REQUIRE(http_append_error(&out, 405, 1, 0, "Allow: POST\r\nX-Instance: 8001\r\n") == 0);
    s = out_str(&out);
    CHECK(strncmp(s, "HTTP/1.1 405 Method Not Allowed\r\n", 33) == 0);
    CHECK(strstr(s, "\r\nAllow: POST\r\nX-Instance: 8001\r\n\r\n<html>") != NULL);
    CHECK(strstr(s, "GET") == NULL);
    free(s);
    buffer_free(&out);
}

/* out 을 BUFFER_MAX 근처까지 채워 일부러 실패시키고, out 이 호출 전 그대로인지 본다 */
static void test_failure_rolls_back(void)
{
    buffer out = BUFFER_INIT, probe = BUFFER_INIT;
    http_response_head h = { 200, "text/plain", 5, 1, NULL };
    size_t fill, hlen, blen;
    char *big, *p;

    big = malloc(BUFFER_MAX);
    REQUIRE(big != NULL);
    memset(big, 'x', BUFFER_MAX);

    /* 1. 헤더 첫 줄부터 실패 */
    fill = BUFFER_MAX - 10;
    REQUIRE(buffer_append(&out, big, fill) == 0);
    CHECK_INT(http_append_header(&out, &h), -1);
    CHECK_INT(buffer_pending(&out), fill);
    CHECK_INT(http_append_error(&out, 404, 0, 0, NULL), -1);
    CHECK_INT(buffer_pending(&out), fill);

    /* 2. 헤더는 들어가고 본문에서 실패 → 헤더까지 되돌려야 한다 (여러 단계 rollback).
     *    헤더 길이는 Date 가 고정 길이라 매번 같다 */
    REQUIRE(http_append_error(&probe, 404, 0, 1, NULL) == 0);
    hlen = buffer_pending(&probe);
    p = out_str(&probe);
    blen = (size_t)content_length_of(p);
    free(p);
    buffer_free(&probe);

    buffer_clear(&out);
    fill = BUFFER_MAX - hlen - blen + 1;                     /* 본문이 1바이트 모자람 */
    REQUIRE(buffer_append(&out, big, fill) == 0);
    CHECK_INT(http_append_error(&out, 404, 0, 0, NULL), -1);
    CHECK_INT(buffer_pending(&out), fill);
    CHECK_INT(buffer_peek(&out)[fill - 1], 'x');

    /* 같은 상태에서 HEAD(본문 없음)는 들어간다 */
    CHECK_INT(http_append_error(&out, 404, 0, 1, NULL), 0);
    CHECK_INT(buffer_pending(&out), fill + hlen);

    free(big);
    buffer_free(&out);
}

int main(void)
{
    RUN(test_reason);
    RUN(test_header_full);
    RUN(test_date_format);
    RUN(test_header_omit);
    RUN(test_header_close_and_zero_length);
    RUN(test_header_bad_extra);
    RUN(test_header_appends);
    RUN(test_error_body);
    RUN(test_error_head_only);
    RUN(test_error_405_extra);
    RUN(test_failure_rolls_back);
    return test_report();
}
