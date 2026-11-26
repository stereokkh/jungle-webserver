/*
 * test_app.c — app 모듈 단위 테스트 (이슈 #10, 헬스체크 2026-11-25)
 *
 * 요청은 실제 파서(http_parse_request)로 만들어서 서버가 넘기는 것과 같은 req 로 시험한다.
 * 응답은 Date 가 매번 바뀌므로 상태 줄·필요한 헤더·본문만 따로 본다.
 */
#include "test.h"

#include "app.h"
#include "http_request.h"

static http_request req;   /* 수 KB 라 스택에 두지 않는다 */
static buffer out;

/* raw 를 파싱하고 app_handle 을 부른다. 반환값은 app_handle 의 것 (파싱 실패면 -2) */
static int call_n(const char *raw, size_t n)
{
    size_t consumed = 0;
    int status = 0;

    buffer_free(&out);
    buffer_init(&out);
    if (http_parse_request(raw, n, NULL, &req, &consumed, &status) != PARSE_DONE)
        return -2;
    return app_handle(&req, "8001", &out);
}

static int call(const char *raw)
{
    return call_n(raw, strlen(raw));
}

/* out 을 NUL 로 끝나는 문자열로 (본문에 0 이 없을 때만 strstr 용으로 쓴다) */
static const char *text(void)
{
    static char s[4096];
    size_t n = buffer_pending(&out) < sizeof s - 1 ? buffer_pending(&out) : sizeof s - 1;

    memcpy(s, out.data + out.off, n);
    s[n] = '\0';
    return s;
}

/* 빈 줄 뒤의 본문 */
static const char *body_of(size_t *len)
{
    const char *all = out.data + out.off;
    size_t total = buffer_pending(&out);
    const char *end = NULL;
    size_t i;

    for (i = 0; i + 4 <= total; i++)
        if (memcmp(all + i, "\r\n\r\n", 4) == 0) {
            end = all + i + 4;
            break;
        }
    if (end == NULL) {
        *len = 0;
        return "";
    }
    *len = total - (size_t)(end - all);
    return end;
}

#define HAS(s) CHECK(strstr(text(), (s)) != NULL)
#define LACKS(s) CHECK(strstr(text(), (s)) == NULL)

static void expect_status(const char *raw, const char *status_line)
{
    REQUIRE(call(raw) == 0);
    CHECK(strncmp(text(), status_line, strlen(status_line)) == 0);
    HAS("X-Instance: 8001\r\n");   /* 오류를 포함한 모든 응답 */
}

/* raw 를 파싱하고 app_health 를 부른다 (파싱 실패면 -2) */
static int call_health(const char *raw)
{
    size_t consumed = 0;
    int status = 0;

    buffer_free(&out);
    buffer_init(&out);
    if (http_parse_request(raw, strlen(raw), NULL, &req, &consumed, &status) != PARSE_DONE)
        return -2;
    return app_health(&req, "8001", &out);
}

static void test_health_ok(void)
{
    size_t n;
    const char *b;

    REQUIRE(call_health("GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n") == 0);
    HAS("HTTP/1.1 200 OK\r\n");
    HAS("Content-Type: text/plain; charset=utf-8\r\n");
    HAS("Content-Length: 3\r\n");
    HAS("Connection: keep-alive\r\n");
    HAS("X-Instance: 8001\r\n");
    b = body_of(&n);
    CHECK_MEM(b, n, "ok\n", 3);

    REQUIRE(call_health("GET /healthz HTTP/1.0\r\n\r\n") == 0);   /* 1.0 기본은 close */
    HAS("Connection: close\r\n");
}

static void test_health_head(void)
{
    size_t n;

    REQUIRE(call_health("HEAD /healthz HTTP/1.1\r\nHost: x\r\n\r\n") == 0);
    HAS("HTTP/1.1 200 OK\r\n");
    HAS("Content-Length: 3\r\n");       /* GET 과 같은 길이, 본문은 없음 */
    body_of(&n);
    CHECK_INT((long)n, 0);
}

static void test_health_405(void)
{
    REQUIRE(call_health("POST /healthz HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n") == 0);
    HAS("HTTP/1.1 405 Method Not Allowed\r\n");
    HAS("Allow: GET, HEAD\r\n");
    HAS("X-Instance: 8001\r\n");
    HAS("Connection: keep-alive\r\n");  /* 405 뒤에도 연결 유지 */
}

/* 정확 일치만 (접두사로 다른 경로를 가로채지 않는다). 질의는 보지 않는다. 판정은 파서 정규형으로 */
static void test_health_match(void)
{
    static const char *const yes[] = { "/healthz", "/healthz?probe=1", "/%68ealthz" };   /* 점 조각은 파서가 400 */
    static const char *const no[] = { "/healthz/", "/healthz/x", "/healthzz", "/health", "/HEALTHZ",
                                      "/api/healthz", "/x/healthz" };
    char raw[128];
    size_t i;

    for (i = 0; i < sizeof yes / sizeof yes[0]; i++) {
        snprintf(raw, sizeof raw, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", yes[i]);
        REQUIRE(call_health(raw) == 0);
        CHECK_INT(app_health_match(&req), 1);
    }
    for (i = 0; i < sizeof no / sizeof no[0]; i++) {
        snprintf(raw, sizeof raw, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", no[i]);
        REQUIRE(call_health(raw) == 0);
        CHECK_INT(app_health_match(&req), 0);
    }
}

/* instance_port 가 지나치게 길면 -1, out 은 그대로 */
static void test_health_failure_keeps_out(void)
{
    size_t before;

    REQUIRE(call_health("GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n") == 0);
    before = buffer_pending(&out);
    CHECK_INT(app_health(&req, "0123456789012345678901234567890123456789012345678901234567890123456789"
                               "0123456789012345678901234567890123456789", &out), -1);
    CHECK_INT((long)buffer_pending(&out), (long)before);
}

static void test_echo(void)
{
    static const char raw[] =
        "POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n\r\nhi\0\r\nyo";
    size_t n;
    const char *b;

    REQUIRE(call_n(raw, sizeof raw - 1) == 0);
    HAS("HTTP/1.1 200 OK\r\n");
    HAS("Content-Type: application/octet-stream\r\n");
    HAS("Content-Length: 7\r\n");
    HAS("X-Instance: 8001\r\n");
    b = body_of(&n);
    CHECK_MEM(b, n, "hi\0\r\nyo", 7);     /* 0 바이트와 CRLF 도 그대로 */

    REQUIRE(call("POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n") == 0);
    HAS("Content-Length: 0\r\n");
    body_of(&n);
    CHECK_INT((long)n, 0);

    REQUIRE(call("POST /api/echo?x=1 HTTP/1.1\r\nHost: x\r\n\r\n") == 0);   /* 질의는 경로가 아님 */
    HAS("HTTP/1.1 200 OK\r\n");
}

static void test_method_405(void)
{
    size_t n;

    expect_status("GET /api/echo HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 405 Method Not Allowed\r\n");
    HAS("Allow: POST\r\n");
    HAS("Connection: keep-alive\r\n");  /* 405 뒤에도 연결 유지 */

    expect_status("HEAD /api/echo HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 405 Method Not Allowed\r\n");
    HAS("Allow: POST\r\n");
    body_of(&n);
    CHECK_INT((long)n, 0);              /* HEAD 라 오류 본문도 없음 */
}

/* 파서 정규형으로 라우팅한다 (#31, PR #18 결정 2 를 바꿈): 표기만 바꾼 경로도 같은 라우트 */
static void test_echo_normalized(void)
{
    CHECK_INT(call("POST /api/%65cho HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nhi"), 0);
    HAS("HTTP/1.1 200 OK\r\n");
    HAS("\r\n\r\nhi");
    CHECK_INT(call("POST /%61pi/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nhi"), 0);
    HAS("HTTP/1.1 200 OK\r\n");
    CHECK_INT(app_match(&req), 1);
}

static void test_unknown_404(void)
{
    expect_status("GET /api/ HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 404 Not Found\r\n");
    expect_status("POST /api/echox HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", "HTTP/1.1 404 Not Found\r\n");
    expect_status("POST /api/echo/ HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", "HTTP/1.1 404 Not Found\r\n");
    expect_status("POST /api/ECHO HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", "HTTP/1.1 404 Not Found\r\n");
    /* 덧셈(/api/add)은 2026-11-25 에 /healthz 로 바꿔서 이제 없는 경로다 */
    expect_status("GET /api/add?a=1&b=2 HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 404 Not Found\r\n");
    /* 헬스체크는 app_handle 이 아니라 app_health 가 답한다 (handler_route 가 앞에서 나눈다) */
    expect_status("GET /api/healthz HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 404 Not Found\r\n");
    LACKS("/api/");                     /* 오류 본문에 요청 경로를 넣지 않음 (#3 XSS) */
}

static void test_connection(void)
{
    REQUIRE(call("GET /api/nope HTTP/1.0\r\n\r\n") == 0);   /* 1.0 기본은 close */
    HAS("Connection: close\r\n");
    REQUIRE(call("GET /api/nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n") == 0);
    HAS("Connection: close\r\n");
}

static void test_failure_keeps_out(void)
{
    size_t before;

    /* 앞에 다른 응답이 쌓여 있어도 뒤에 덧붙이기만 한다 */
    REQUIRE(call("POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nhi") == 0);
    before = buffer_pending(&out);
    REQUIRE(app_handle(&req, "8001", &out) == 0);
    CHECK_INT((long)buffer_pending(&out), (long)(before * 2));

    /* instance_port 가 지나치게 길면 -1, out 은 그대로 */
    before = buffer_pending(&out);
    CHECK_INT(app_handle(&req, "0123456789012345678901234567890123456789012345678901234567890123456789"
                               "0123456789012345678901234567890123456789", &out), -1);
    CHECK_INT((long)buffer_pending(&out), (long)before);
}

/* 헤더는 들어가고 본문에서 실패해도 out 은 그대로 (append_ok 의 rollback).
 * rollback 이 없으면 200 헤더만 남고 서버가 500 을 덧붙여 응답이 깨진다 */
static void test_body_failure_rolls_back(void)
{
    static char raw[2100], fill[BUFFER_MAX - 1024];
    int n = snprintf(raw, sizeof raw,
                     "POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 2000\r\n\r\n");

    memset(raw + n, 'x', 2000);
    REQUIRE(call_n(raw, (size_t)n + 2000) == 0);
    buffer_clear(&out);
    REQUIRE(buffer_append(&out, fill, sizeof fill) == 0);   /* 헤더는 들어가고 본문은 안 들어갈 만큼만 남김 */
    CHECK_INT(app_handle(&req, "8001", &out), -1);
    CHECK_INT((long)buffer_pending(&out), (long)sizeof fill);
}

int main(void)
{
    buffer_init(&out);
    RUN(test_health_ok);
    RUN(test_health_head);
    RUN(test_health_405);
    RUN(test_health_match);
    RUN(test_health_failure_keeps_out);
    RUN(test_echo);
    RUN(test_method_405);
    RUN(test_echo_normalized);
    RUN(test_unknown_404);
    RUN(test_connection);
    RUN(test_failure_keeps_out);
    RUN(test_body_failure_rolls_back);
    buffer_free(&out);
    return test_report();
}
