/*
 * test_proxy.c — 프록시 요청 다시 쓰기 proxy_build_request (이슈 #8)
 *
 * 요청은 실제 http_parse_request 로 만들고 (NUL 종료 없는 정확한 크기의 힙 복사본),
 * 파서가 막는 입력(Transfer-Encoding, 깨진 헤더 블록)만 손으로 만든다.
 * 소켓과 이름 해석은 쓰지 않는다 (connect 는 서버 스모크 테스트, 응답 중계는 test_proxy_relay.c).
 */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include "test.h"

#include "buffer.h"
#include "http_request.h"
#include "proxy.h"

#define SUFFIX "Host: 127.0.0.1:8001\r\nX-Forwarded-For: 203.0.113.7\r\nVia: 1.1 jungle-webserver\r\n"
#define IP "203.0.113.7"

static http_request req;        /* 수 KB 라서 스택에 두지 않는다 */
static proxy_backend be;
static buffer out;
static size_t consumed;
static int status;
static char *g_buf;             /* req.headers / req.body 가 가리키므로 다음 parse 전까지 살려 둔다 */

static void setup(void)
{
    memset(&be, 0, sizeof be);
    strcpy(be.name, "127.0.0.1:8001");
    buffer_free(&out);
}

/* 정확한 크기의 힙 복사본을 파서에 넘기고 PARSE_DONE 을 요구한다 */
static int parse_bytes(const char *s, size_t n)
{
    parse_result r;

    free(g_buf);
    g_buf = malloc(n ? n : 1);
    if (g_buf == NULL)
        exit(2);
    memcpy(g_buf, s, n);
    r = http_parse_request(g_buf, n, &req, &consumed, &status);
    CHECK_INT(r, PARSE_DONE);
    return r == PARSE_DONE;
}

static int parse_str(const char *s)
{
    return parse_bytes(s, strlen(s));
}

/* 손으로 만든 요청: 헤더 블록은 정확한 크기의 힙 복사본 */
static void hand_req(const char *method, const char *target, const char *headers)
{
    memset(&req, 0, sizeof req);
    strcpy(req.method_str, method);
    strcpy(req.target, target);
    req.version_minor = 1;
    req.content_length = -1;
    free(g_buf);
    g_buf = NULL;
    if (headers != NULL) {
        size_t n;
        g_buf = test_bytes(headers, &n);
        req.headers = g_buf;
        req.headers_len = n;
    }
}

#define BUILD() proxy_build_request(&req, &be, IP, &out)
#define EXPECT(lit) CHECK_MEM(buffer_peek(&out), buffer_pending(&out), (lit), sizeof(lit) - 1)

static size_t count_of(const char *hay, size_t n, const char *needle)
{
    size_t k = strlen(needle), c = 0, i;

    for (i = 0; i + k <= n; i++)
        if (memcmp(hay + i, needle, k) == 0)
            c++;
    return c;
}

/* R1 */
static void test_get_query_percent(void)
{
    setup();
    REQUIRE(parse_str("GET /index.html?x=1&y=%20 HTTP/1.1\r\nHost: client.example\r\n"
                      "Accept: */*\r\nUser-Agent: t\r\n\r\n"));
    CHECK_INT(BUILD(), 0);
    EXPECT("GET /index.html?x=1&y=%20 HTTP/1.1\r\nAccept: */*\r\nUser-Agent: t\r\n"
           SUFFIX "Connection: close\r\n\r\n");
    buffer_free(&out);
}

/* R2 */
static void test_path_preserved(void)
{
    setup();
    REQUIRE(parse_str("GET /api/a%2Fb/c..d?q=%41&r HTTP/1.1\r\nHost: x\r\n\r\n"));
    CHECK_INT(BUILD(), 0);
    EXPECT("GET /api/a%2Fb/c..d?q=%41&r HTTP/1.1\r\n" SUFFIX "Connection: close\r\n\r\n");
    buffer_free(&out);
}

/* R3 */
static void test_post_binary_body_and_pipelined_rest(void)
{
    static const char head[] = "POST /api/echo?k=v HTTP/1.1\r\nHost: x\r\n"
                               "Content-Type: application/octet-stream\r\nContent-Length: 8\r\n\r\n";
    static const char body[8] = { 'a', '\0', 'b', '\r', '\n', '\r', '\n', 'c' };
    static const char next[] = "GET /next HTTP/1.1\r\nHost: x\r\n\r\n";
    static const char exp_head[] = "POST /api/echo?k=v HTTP/1.1\r\n"
                                   "Content-Type: application/octet-stream\r\n"
                                   SUFFIX "Content-Length: 8\r\nConnection: close\r\n\r\n";
    char in[512], exp[512];
    size_t n = 0, en = 0;

    setup();
    memcpy(in + n, head, sizeof head - 1); n += sizeof head - 1;
    memcpy(in + n, body, sizeof body); n += sizeof body;
    memcpy(in + n, next, sizeof next - 1); n += sizeof next - 1;
    REQUIRE(parse_bytes(in, n));
    CHECK_INT(consumed, sizeof head - 1 + sizeof body);

    memcpy(exp + en, exp_head, sizeof exp_head - 1); en += sizeof exp_head - 1;
    memcpy(exp + en, body, sizeof body); en += sizeof body;
    CHECK_INT(BUILD(), 0);
    CHECK_MEM(buffer_peek(&out), buffer_pending(&out), exp, en);
    CHECK_INT(count_of(buffer_peek(&out), buffer_pending(&out), "/next"), 0);
    buffer_free(&out);
}

/* R4 */
static void test_content_length_leading_zeros(void)
{
    const char *p;
    size_t n;

    setup();
    REQUIRE(parse_str("POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 005\r\n\r\nhello"));
    CHECK_INT(BUILD(), 0);
    p = buffer_peek(&out);
    n = buffer_pending(&out);
    CHECK_INT(count_of(p, n, "Content-Length"), 1);
    CHECK_INT(count_of(p, n, "Content-Length: 5\r\n"), 1);
    CHECK(n >= 9 && memcmp(p + n - 9, "\r\n\r\nhello", 9) == 0);
    buffer_free(&out);
}

/* R5 */
static void test_content_length_zero_and_absent(void)
{
    setup();
    REQUIRE(parse_str("POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n"));
    CHECK_INT(BUILD(), 0);
    EXPECT("POST /api/echo HTTP/1.1\r\n" SUFFIX "Content-Length: 0\r\nConnection: close\r\n\r\n");
    buffer_free(&out);

    REQUIRE(parse_str("POST /api/echo HTTP/1.1\r\nHost: x\r\n\r\n"));
    CHECK_INT(BUILD(), 0);
    CHECK_INT(count_of(buffer_peek(&out), buffer_pending(&out), "Content-Length"), 0);
    EXPECT("POST /api/echo HTTP/1.1\r\n" SUFFIX "Connection: close\r\n\r\n");
    buffer_free(&out);
}

/* R6 */
static void test_hop_by_hop_dropped(void)
{
    setup();
    REQUIRE(parse_str("GET / HTTP/1.1\r\nHost: x\r\nconnection: keep-alive\r\nKEEP-ALIVE: timeout=5\r\n"
                      "Proxy-Authorization: Basic eA==\r\nproxy-connection: keep-alive\r\n"
                      "TE: trailers\r\nTrailer: X-T\r\nUpgrade: h2c\r\nExpect: 100-continue\r\n"
                      "X-Keep: 1\r\n\r\n"));
    CHECK_INT(BUILD(), 0);
    EXPECT("GET / HTTP/1.1\r\nX-Keep: 1\r\n" SUFFIX "Connection: close\r\n\r\n");
    buffer_free(&out);
}

/* R7: 파서는 Transfer-Encoding 을 501 로 막으므로 손으로 만든다 */
static void test_transfer_encoding_dropped(void)
{
    setup();
    hand_req("POST", "/x", "Transfer-Encoding: chunked\r\nX-A: 1\r\n");
    CHECK_INT(BUILD(), 0);
    EXPECT("POST /x HTTP/1.1\r\nX-A: 1\r\n" SUFFIX "Connection: close\r\n\r\n");
    buffer_free(&out);
}

/* R8 */
static void test_connection_listed_names(void)
{
    setup();
    REQUIRE(parse_str("GET / HTTP/1.1\r\nHost: x\r\nConnection: close, X-Foo\r\nX-Foo: 1\r\n"
                      "Connection: ,x-bar ,\t X-Baz\r\nX-Bar: 2\r\nx-baz: 3\r\nX-Foo-Not: 4\r\n"
                      "X-Qux: 5\r\n\r\n"));
    CHECK_INT(BUILD(), 0);
    EXPECT("GET / HTTP/1.1\r\nX-Foo-Not: 4\r\nX-Qux: 5\r\n" SUFFIX "Connection: close\r\n\r\n");
    buffer_free(&out);
}

/* R8b: 이름을 적은 Connection 줄이 그 헤더보다 뒤에 와도 지운다 */
static void test_connection_listed_names_after(void)
{
    setup();
    REQUIRE(parse_str("GET / HTTP/1.1\r\nHost: x\r\nX-Late: 1\r\nX-Keep: 1\r\nConnection: x-late\r\n\r\n"));
    CHECK_INT(BUILD(), 0);
    EXPECT("GET / HTTP/1.1\r\nX-Keep: 1\r\n" SUFFIX "Connection: close\r\n\r\n");
    buffer_free(&out);
}

/* R9 */
static void test_connection_cannot_delete_generated_lines(void)
{
    setup();
    REQUIRE(parse_str("POST /api/echo HTTP/1.1\r\nHost: x\r\n"
                      "Connection: Content-Length, Host, X-Forwarded-For, Connection\r\n"
                      "Content-Length: 3\r\n\r\nabc"));
    CHECK_INT(BUILD(), 0);
    EXPECT("POST /api/echo HTTP/1.1\r\n" SUFFIX "Content-Length: 3\r\nConnection: close\r\n\r\nabc");
    buffer_free(&out);
}

/* R10 */
static void test_client_host_and_xff_replaced(void)
{
    const char *p;
    size_t n;

    setup();
    REQUIRE(parse_str("GET / HTTP/1.1\r\nX-Forwarded-For: 1.2.3.4\r\nx-forwarded-for: 5.6.7.8\r\n"
                      "Host: evil\r\n\r\n"));
    CHECK_INT(BUILD(), 0);
    EXPECT("GET / HTTP/1.1\r\n" SUFFIX "Connection: close\r\n\r\n");
    p = buffer_peek(&out);
    n = buffer_pending(&out);
    CHECK_INT(count_of(p, n, "1.2.3.4"), 0);
    CHECK_INT(count_of(p, n, "5.6.7.8"), 0);
    CHECK_INT(count_of(p, n, "evil"), 0);
    buffer_free(&out);
}

/* R11 */
static void test_expect_dropped_on_post(void)
{
    setup();
    REQUIRE(parse_str("POST /api/echo HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\n"
                      "Content-Length: 2\r\n\r\nhi"));
    CHECK_INT(BUILD(), 0);
    CHECK_INT(count_of(buffer_peek(&out), buffer_pending(&out), "Expect"), 0);
    EXPECT("POST /api/echo HTTP/1.1\r\n" SUFFIX "Content-Length: 2\r\nConnection: close\r\n\r\nhi");
    buffer_free(&out);
}

/* R11b: 클라이언트가 보낸 Via 는 원래 바이트 그대로 남고 프록시의 Via 가 그 뒤에 붙는다 */
static void test_client_via_preserved(void)
{
    setup();
    REQUIRE(parse_str("GET / HTTP/1.1\r\nHost: x\r\nvIa: 1.0 fixture-edge\r\nAccept: */*\r\n\r\n"));
    CHECK_INT(BUILD(), 0);
    EXPECT("GET / HTTP/1.1\r\nvIa: 1.0 fixture-edge\r\nAccept: */*\r\n" SUFFIX
           "Connection: close\r\n\r\n");
    buffer_free(&out);
}

/* R12 */
static void test_verbatim_and_near_miss(void)
{
    setup();
    REQUIRE(parse_str("GET / HTTP/1.1\r\nHost: x\r\nx-CUSTOM:   spaced value \t\r\n"
                      "Connection-Extra: 1\r\nTEx: 1\r\nUpgrade-Insecure-Requests: 1\r\n"
                      "Keep-Alive-X: 1\r\nHostname: a\r\nX-Forwarded-For-Extra: 1\r\nExpected: 1\r\n"
                      "Content-Length-X: 1\r\nProxy: 1\r\nProxy-X: 1\r\n\r\n"));
    CHECK_INT(BUILD(), 0);
    EXPECT("GET / HTTP/1.1\r\nx-CUSTOM:   spaced value \t\r\n"
           "Connection-Extra: 1\r\nTEx: 1\r\nUpgrade-Insecure-Requests: 1\r\n"
           "Keep-Alive-X: 1\r\nHostname: a\r\nX-Forwarded-For-Extra: 1\r\nExpected: 1\r\n"
           "Content-Length-X: 1\r\nProxy: 1\r\n" SUFFIX "Connection: close\r\n\r\n");
    buffer_free(&out);
}

/* R13 */
static void test_http10_no_headers(void)
{
    setup();
    REQUIRE(parse_str("GET / HTTP/1.0\r\n\r\n"));
    CHECK_INT(req.headers_len, 0);
    CHECK_INT(BUILD(), 0);
    EXPECT("GET / HTTP/1.1\r\n" SUFFIX "Connection: close\r\n\r\n");
    buffer_free(&out);
}

/* R14 */
static void test_head_method(void)
{
    setup();
    REQUIRE(parse_str("HEAD /index.html HTTP/1.1\r\nHost: x\r\n\r\n"));
    CHECK_INT(BUILD(), 0);
    EXPECT("HEAD /index.html HTTP/1.1\r\n" SUFFIX "Connection: close\r\n\r\n");
    buffer_free(&out);
}

/* R15 */
static void test_ipv6_names(void)
{
    setup();
    strcpy(be.name, "[::1]:8001");
    REQUIRE(parse_str("GET / HTTP/1.1\r\nHost: x\r\n\r\n"));
    CHECK_INT(proxy_build_request(&req, &be, "::1", &out), 0);
    EXPECT("GET / HTTP/1.1\r\nHost: [::1]:8001\r\nX-Forwarded-For: ::1\r\nVia: 1.1 jungle-webserver\r\nConnection: close\r\n\r\n");
    buffer_free(&out);
}

/* R16 */
static void test_appends_after_existing(void)
{
    setup();
    REQUIRE(buffer_append_str(&out, "keepXYZ") == 0);
    buffer_consume(&out, 4);
    REQUIRE(parse_str("GET / HTTP/1.0\r\n\r\n"));
    CHECK_INT(BUILD(), 0);
    EXPECT("XYZGET / HTTP/1.1\r\n" SUFFIX "Connection: close\r\n\r\n");
    buffer_free(&out);
}

/* R17 */
static void test_large_body(void)
{
    static const char head[] = "POST /up HTTP/1.1\r\nHost: x\r\nContent-Length: 1048576\r\n\r\n";
    static const char exp_head[] = "POST /up HTTP/1.1\r\n" SUFFIX
                                   "Content-Length: 1048576\r\nConnection: close\r\n\r\n";
    size_t n = sizeof head - 1 + HTTP_MAX_BODY, i;
    char *in = malloc(n);

    setup();
    REQUIRE(in != NULL);
    memcpy(in, head, sizeof head - 1);
    for (i = 0; i < (size_t)HTTP_MAX_BODY; i++)
        in[sizeof head - 1 + i] = (char)('A' + i % 26);
    CHECK_INT(parse_bytes(in, n), 1);
    CHECK_INT(BUILD(), 0);
    CHECK_INT(buffer_pending(&out), sizeof exp_head - 1 + HTTP_MAX_BODY);
    /* 길이가 맞을 때만 바이트를 비교한다 (실패한 빌드의 빈 출력을 기대 길이로 읽지 않도록) */
    if (buffer_pending(&out) == sizeof exp_head - 1 + HTTP_MAX_BODY) {
        CHECK_MEM(buffer_peek(&out), sizeof exp_head - 1, exp_head, sizeof exp_head - 1);
        CHECK(memcmp(buffer_peek(&out) + sizeof exp_head - 1, in + sizeof head - 1,
                     HTTP_MAX_BODY) == 0);
    }
    free(in);
    buffer_free(&out);
}

/* R18: 크기 한도에서 실패하면 out 은 그대로 */
static void test_failure_at_size_limit_keeps_out(void)
{
    size_t fill = BUFFER_MAX - 30;
    char *q = malloc(fill);

    setup();
    REQUIRE(q != NULL);
    memset(q, 'q', fill);
    REQUIRE(buffer_append(&out, q, fill) == 0);
    free(q);
    REQUIRE(parse_str("GET / HTTP/1.0\r\n\r\n"));
    CHECK_INT(BUILD(), -1);     /* 요청 줄 16바이트는 들어가고 Host 줄(22바이트)에서 넘친다 */
    CHECK_INT(buffer_pending(&out), fill);
    CHECK(buffer_peek(&out)[fill - 1] == 'q');
    buffer_free(&out);
}

#define KEEP() \
    do { buffer_free(&out); buffer_append_str(&out, "keep"); } while (0)
#define EXPECT_KEEP() CHECK_MEM(buffer_peek(&out), buffer_pending(&out), "keep", 4)

/* R19 */
static void test_invalid_arguments(void)
{
    static const char abc[] = "abc";

    setup();
    REQUIRE(parse_str("GET / HTTP/1.1\r\nHost: x\r\n\r\n"));

    KEEP(); CHECK_INT(proxy_build_request(NULL, &be, IP, &out), -1); EXPECT_KEEP();
    KEEP(); CHECK_INT(proxy_build_request(&req, NULL, IP, &out), -1); EXPECT_KEEP();
    KEEP(); CHECK_INT(proxy_build_request(&req, &be, NULL, &out), -1); EXPECT_KEEP();
    CHECK_INT(proxy_build_request(&req, &be, IP, NULL), -1);
    KEEP(); CHECK_INT(proxy_build_request(&req, &be, "", &out), -1); EXPECT_KEEP();
    KEEP(); CHECK_INT(proxy_build_request(&req, &be, "1.2.3.4\r\nX-Evil: 1", &out), -1); EXPECT_KEEP();

    be.name[0] = '\0';
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();
    strcpy(be.name, "a b");
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();
    strcpy(be.name, "127.0.0.1:8001");

    hand_req("GET", "/x", NULL);
    req.body_len = 3;               /* body == NULL */
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();

    hand_req("POST", "/x", NULL);
    req.content_length = 5;
    req.body = abc;
    req.body_len = 3;
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();

    hand_req("POST", "/x", NULL);   /* 길이 없는 본문: 밀수 모양이라 거절 */
    req.body = abc;
    req.body_len = 3;
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();

    hand_req("GET", "/x", NULL);
    req.headers_len = 5;            /* headers == NULL */
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();

    hand_req("GET", "/x\r\nX-Evil: 1\r\n\r\nGET /y", NULL);     /* 요청 줄 주입 */
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();
    hand_req("GE T", "/x", NULL);
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();

    hand_req("GET", "/x", NULL);
    memset(be.name, 'a', sizeof be.name);   /* NUL 없음 */
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();
    strcpy(be.name, "127.0.0.1:8001");

    hand_req("GET", "x", NULL);     /* target 이 '/' 로 시작하지 않음 */
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();

    hand_req("", "/x", NULL);
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();
    buffer_free(&out);
}

/* R20 */
static void test_malformed_header_block(void)
{
    setup();
    hand_req("GET", "/x", "X-A: 1");
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();
    hand_req("GET", "/x", "NoColon\r\n");
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();
    hand_req("GET", "/x", "X-A: 1\r\n: v\r\n");
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();
    /* 파서가 400 으로 막는 줄: 이름에 공백, 앞 공백(이어 쓰기 줄), 값에 제어 문자·맨 CR */
    hand_req("GET", "/x", "Host : x\r\n");
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();
    hand_req("GET", "/x", "X-A: 1\r\n X-B: 2\r\n");
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();
    hand_req("GET", "/x", "X-A: a\x01" "b\r\n");
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();
    hand_req("GET", "/x", "X-A: a\rb\r\n");
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();
    hand_req("GET", "/x", "X-A: a\x7f\r\n");
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();
    hand_req("GET", "/x", "X-A: a\tb\r\n");      /* 값 안의 탭은 허용 */
    CHECK_INT(BUILD(), 0);
    buffer_free(&out);
}

/* R21: 다시 쓴 머리의 크기 한도. "GET / HTTP/1.1\r\nHost: x\r\nX-A: <k 바이트>\r\n\r\n" 를 파서에 넘긴다 */
static int parse_filler(size_t k)
{
    char *s = malloc(64 + k);
    size_t n;
    int ok;

    if (s == NULL)
        exit(2);
    n = (size_t)sprintf(s, "GET / HTTP/1.1\r\nHost: x\r\nX-A: ");
    memset(s + n, 'a', k);
    n += k;
    memcpy(s + n, "\r\n\r\n", 4);
    n += 4;
    ok = parse_bytes(s, n);
    free(s);
    return ok;
}

static void test_rewritten_head_size_limit(void)
{
    size_t k0 = 10, r0, k;

    setup();
    /* 기준: 작은 요청을 다시 쓴 길이 r0 (줄 하나의 바이트 수가 k 와 1:1 이라 r(k) = r0 + k - k0) */
    REQUIRE(parse_filler(k0));
    CHECK_INT(BUILD(), 0);
    r0 = buffer_pending(&out);
    buffer_free(&out);

    /* 다시 쓴 머리가 정확히 8192 바이트: 통과 */
    k = k0 + HTTP_MAX_HEADER - r0;
    REQUIRE(parse_filler(k));
    CHECK_INT(BUILD(), 0);
    CHECK_INT(buffer_pending(&out), HTTP_MAX_HEADER);
    buffer_free(&out);

    /* 1바이트 넘으면 실패하고 out 은 그대로 */
    REQUIRE(parse_filler(k + 1));
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();

    /* 프런트(파서)가 허용하는 가장 큰 머리 8192바이트: 입력이 8192 이므로 다시 쓰면 반드시 넘는다 */
    k = HTTP_MAX_HEADER - (sizeof "GET / HTTP/1.1\r\nHost: x\r\nX-A: \r\n\r\n" - 1);
    REQUIRE(parse_filler(k));
    CHECK_INT(consumed, HTTP_MAX_HEADER);
    KEEP(); CHECK_INT(BUILD(), -1); EXPECT_KEEP();
    buffer_free(&out);
}

int main(void)
{
    RUN(test_get_query_percent);
    RUN(test_path_preserved);
    RUN(test_post_binary_body_and_pipelined_rest);
    RUN(test_content_length_leading_zeros);
    RUN(test_content_length_zero_and_absent);
    RUN(test_hop_by_hop_dropped);
    RUN(test_transfer_encoding_dropped);
    RUN(test_connection_listed_names);
    RUN(test_connection_listed_names_after);
    RUN(test_connection_cannot_delete_generated_lines);
    RUN(test_client_host_and_xff_replaced);
    RUN(test_expect_dropped_on_post);
    RUN(test_client_via_preserved);
    RUN(test_verbatim_and_near_miss);
    RUN(test_http10_no_headers);
    RUN(test_head_method);
    RUN(test_ipv6_names);
    RUN(test_appends_after_existing);
    RUN(test_large_body);
    RUN(test_failure_at_size_limit_keeps_out);
    RUN(test_invalid_arguments);
    RUN(test_malformed_header_block);
    RUN(test_rewritten_head_size_limit);
    free(g_buf);
    return test_report();
}
