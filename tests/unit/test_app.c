/*
 * test_app.c — app 모듈 단위 테스트 (이슈 #10)
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
    if (http_parse_request(raw, n, &req, &consumed, &status) != PARSE_DONE)
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

static void test_add_ok(void)
{
    size_t n;
    const char *b;

    REQUIRE(call("GET /api/add?a=1&b=2 HTTP/1.1\r\nHost: x\r\n\r\n") == 0);
    HAS("HTTP/1.1 200 OK\r\n");
    HAS("Content-Type: text/plain; charset=utf-8\r\n");
    HAS("Content-Length: 2\r\n");
    HAS("Connection: keep-alive\r\n");
    HAS("X-Instance: 8001\r\n");
    b = body_of(&n);
    CHECK_MEM(b, n, "3\n", 2);
}

static void test_add_values(void)
{
    size_t n;
    const char *b;

    REQUIRE(call("GET /api/add?b=2&a=-7 HTTP/1.1\r\nHost: x\r\n\r\n") == 0);   /* 순서 무관, 음수 */
    b = body_of(&n);
    CHECK_MEM(b, n, "-5\n", 3);

    REQUIRE(call("GET /api/add?x=9&a=0&b=0&y= HTTP/1.1\r\nHost: x\r\n\r\n") == 0);  /* 다른 이름 무시 */
    b = body_of(&n);
    CHECK_MEM(b, n, "0\n", 2);

    /* long long 경계: 넘치지 않는 가장 큰 합 */
    REQUIRE(call("GET /api/add?a=9223372036854775806&b=1 HTTP/1.1\r\nHost: x\r\n\r\n") == 0);
    b = body_of(&n);
    CHECK_MEM(b, n, "9223372036854775807\n", 20);
    REQUIRE(call("GET /api/add?a=-9223372036854775808&b=0 HTTP/1.1\r\nHost: x\r\n\r\n") == 0);
    b = body_of(&n);
    CHECK_MEM(b, n, "-9223372036854775808\n", 21);
}

static void test_add_bad_400(void)
{
    static const char *const bad[] = {
        "",                       /* 질의 없음 */
        "a=1",                    /* b 누락 */
        "b=2",
        "a=&b=2",                 /* 빈 값 */
        "a=-&b=2",
        "a=abc&b=2",              /* atoi 면 0 */
        "a=12x&b=2",              /* 뒤에 쓰레기 */
        "a=+1&b=2",               /* strtoll 은 받지만 막는다 */
        "a=%201&b=2",             /* 퍼센트 인코딩 (공백) */
        "a=1.5&b=2",
        "a=1&a=2&b=3",            /* 중복 */
        "A=1&b=2",                /* 이름은 대소문자 구분 → a 누락 */
        "a=9223372036854775808&b=0",    /* 범위 초과 (strtoll ERANGE) */
        "a=99999999999999999999999999999999&b=0",   /* 너무 긴 값 */
        "a=9223372036854775807&b=1",    /* 합이 넘침 */
        "a=-9223372036854775808&b=-1",  /* 합이 아래로 넘침 */
    };
    char raw[256];
    size_t i;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        snprintf(raw, sizeof raw, "GET /api/add%s%s HTTP/1.1\r\nHost: x\r\n\r\n",
                 bad[i][0] ? "?" : "", bad[i]);
        expect_status(raw, "HTTP/1.1 400 Bad Request\r\n");
        HAS("Connection: keep-alive\r\n");     /* 요청 자체는 멀쩡하니 연결은 유지 */
    }
}

static void test_add_head(void)
{
    size_t n;

    REQUIRE(call("HEAD /api/add?a=40&b=2 HTTP/1.1\r\nHost: x\r\n\r\n") == 0);
    HAS("HTTP/1.1 200 OK\r\n");
    HAS("Content-Length: 3\r\n");       /* GET 과 같은 길이, 본문은 없음 */
    body_of(&n);
    CHECK_INT((long)n, 0);

    REQUIRE(call("HEAD /api/add?a=x&b=2 HTTP/1.1\r\nHost: x\r\n\r\n") == 0);
    HAS("HTTP/1.1 400 Bad Request\r\n");
    body_of(&n);
    CHECK_INT((long)n, 0);
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

    expect_status("POST /api/add?a=1&b=2 HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n",
                  "HTTP/1.1 405 Method Not Allowed\r\n");
    HAS("Allow: GET, HEAD\r\n");

    expect_status("GET /api/echo HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 405 Method Not Allowed\r\n");
    HAS("Allow: POST\r\n");

    expect_status("HEAD /api/echo HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 405 Method Not Allowed\r\n");
    HAS("Allow: POST\r\n");
    body_of(&n);
    CHECK_INT((long)n, 0);              /* HEAD 라 오류 본문도 없음 */
}

static void test_unknown_404(void)
{
    expect_status("GET /api/ HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 404 Not Found\r\n");
    expect_status("GET /api/addx?a=1&b=2 HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 404 Not Found\r\n");
    expect_status("GET /api/add/ HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 404 Not Found\r\n");
    expect_status("GET /api/ADD?a=1&b=2 HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 404 Not Found\r\n");
    /* 원본 target 기준이라 인코딩한 경로는 다른 경로다 (app_match 와 같은 기준) */
    expect_status("GET /api/%61dd?a=1&b=2 HTTP/1.1\r\nHost: x\r\n\r\n", "HTTP/1.1 404 Not Found\r\n");
    LACKS("/api/");                     /* 오류 본문에 요청 경로를 넣지 않음 (#3 XSS) */
}

static void test_connection(void)
{
    REQUIRE(call("GET /api/add?a=1&b=2 HTTP/1.0\r\n\r\n") == 0);   /* 1.0 기본은 close */
    HAS("Connection: close\r\n");
    REQUIRE(call("GET /api/nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n") == 0);
    HAS("Connection: close\r\n");
}

static void test_failure_keeps_out(void)
{
    size_t before;

    /* 앞에 다른 응답이 쌓여 있어도 뒤에 덧붙이기만 한다 */
    REQUIRE(call("GET /api/add?a=1&b=2 HTTP/1.1\r\nHost: x\r\n\r\n") == 0);
    before = buffer_pending(&out);
    REQUIRE(app_handle(&req, "8001", &out) == 0);
    CHECK_INT((long)buffer_pending(&out), (long)(before * 2));

    /* instance_port 가 지나치게 길면 -1, out 은 그대로 */
    before = buffer_pending(&out);
    CHECK_INT(app_handle(&req, "0123456789012345678901234567890123456789012345678901234567890123456789"
                               "0123456789012345678901234567890123456789", &out), -1);
    CHECK_INT((long)buffer_pending(&out), (long)before);
}

int main(void)
{
    buffer_init(&out);
    RUN(test_add_ok);
    RUN(test_add_values);
    RUN(test_add_bad_400);
    RUN(test_add_head);
    RUN(test_echo);
    RUN(test_method_405);
    RUN(test_unknown_404);
    RUN(test_connection);
    RUN(test_failure_keeps_out);
    buffer_free(&out);
    return test_report();
}
