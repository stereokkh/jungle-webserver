/*
 * test_http_request.c — 증분 HTTP 요청 파서 (이슈 #5)
 *
 * 입력은 항상 NUL 종료 없는 정확한 크기의 힙 복사본으로 넘긴다.
 * 파서가 len 을 넘어 한 바이트라도 읽으면 make asan 이 잡는다.
 */
#include "test.h"

#include "http_request.h"

static http_request req;   /* 수 KB 라서 스택에 두지 않는다 */
static size_t consumed;
static int status;

/* 마지막 입력 버퍼. req.headers / req.body 가 가리키므로 다음 parse 전까지 살려 둔다 */
static char *g_buf;

/* s 의 앞 n 바이트만 넘긴다 (조각으로 도착한 상황) */
static parse_result parse_n(const char *s, size_t n)
{
    free(g_buf);
    g_buf = malloc(n ? n : 1);
    if (g_buf == NULL)
        exit(2);
    memcpy(g_buf, s, n);
    return http_parse_request(g_buf, n, &req, &consumed, &status);
}

static parse_result parse(const char *s)
{
    return parse_n(s, strlen(s));
}

/* 오류 하나를 기대하는 짧은 확인 */
#define CHECK_ERROR(s, code)                      \
    do {                                          \
        CHECK_INT(parse(s), PARSE_ERROR);         \
        CHECK_INT(status, (code));                \
        CHECK_INT(consumed, 0);                   \
    } while (0)

static void test_simple_get(void)
{
    const char *s = "GET /index.html HTTP/1.1\r\nHost: localhost:8080\r\nAccept: */*\r\n\r\n";

    REQUIRE(parse(s) == PARSE_DONE);
    CHECK_INT(consumed, strlen(s));
    CHECK_INT(req.method, HTTP_GET);
    CHECK_STR(req.method_str, "GET");
    CHECK_STR(req.target, "/index.html");
    CHECK_STR(req.path, "/index.html");
    CHECK_STR(req.query, "");
    CHECK_INT(req.version_minor, 1);
    CHECK_STR(req.host, "localhost:8080");
    CHECK_INT(req.content_length, -1);
    CHECK_INT(req.keep_alive, 1);
    CHECK_MEM(req.headers, req.headers_len,
              "Host: localhost:8080\r\nAccept: */*\r\n", 35);
    CHECK(req.body == NULL);
    CHECK_INT(req.body_len, 0);
}

/* 헤더가 하나도 없는 HTTP/1.0 (Host 필수 아님) */
static void test_http10_no_headers(void)
{
    REQUIRE(parse("GET / HTTP/1.0\r\n\r\n") == PARSE_DONE);
    CHECK_INT(consumed, 18);
    CHECK_INT(req.version_minor, 0);
    CHECK_INT(req.keep_alive, 0);
    CHECK_INT(req.headers_len, 0);
    CHECK_STR(req.host, "");
}

static void test_post_body(void)
{
    const char *s = "POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello";

    REQUIRE(parse(s) == PARSE_DONE);
    CHECK_INT(req.method, HTTP_POST);
    CHECK_INT(req.content_length, 5);
    CHECK_MEM(req.body, req.body_len, "hello", 5);
    CHECK_INT(consumed, strlen(s));
}

/* 1 바이트씩 도착: 마지막 바이트 전까지는 전부 NEED_MORE + consumed 0 (Tiny 재현 ①의 반대) */
static void test_one_byte_at_a_time(void)
{
    const char *s = "POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello";
    size_t n = strlen(s), k;
    int bad = 0;

    for (k = 0; k < n; k++) {
        if (parse_n(s, k) != PARSE_NEED_MORE || consumed != 0) {
            bad = 1;
            fprintf(stderr, "    prefix %zu bytes: not NEED_MORE\n", k);
            break;
        }
    }
    CHECK(!bad);
    CHECK_INT(parse_n(s, n), PARSE_DONE);
    CHECK_INT(consumed, n);
}

/* 요청 두 개가 붙어 옴 (keep-alive 파이프라이닝): consumed 뒤가 다음 요청 */
static void test_pipelined(void)
{
    const char *s = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n"
                    "POST /b HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\nhi"
                    "GET /c HTT";
    size_t first, second;
    static char rest[256];

    REQUIRE(parse(s) == PARSE_DONE);
    CHECK_STR(req.target, "/a");
    first = consumed;
    CHECK_INT(first, 28);

    strcpy(rest, s + first);
    REQUIRE(parse(rest) == PARSE_DONE);
    CHECK_STR(req.target, "/b");
    CHECK_MEM(req.body, req.body_len, "hi", 2);
    second = consumed;

    /* 세 번째는 덜 왔다 */
    CHECK_INT(parse(rest + second), PARSE_NEED_MORE);
    CHECK_INT(consumed, 0);
}

/* target / path / query 분리와 퍼센트 디코딩 */
static void test_target_split(void)
{
    REQUIRE(parse("GET /docs/%2E%2E/secret?q=a%20b HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.target, "/docs/%2E%2E/secret?q=a%20b");
    CHECK_STR(req.path, "/docs/../secret");   /* ".." 차단은 static_open 몫 */
    CHECK_STR(req.query, "q=a%20b");

    REQUIRE(parse("GET /a%20b?x=1&y=%3D HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.path, "/a b");
    CHECK_STR(req.query, "x=1&y=%3D");

    REQUIRE(parse("GET /%61pi/add HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.target, "/%61pi/add");      /* 라우팅은 이 원본으로 (decisions 2026-11-20) */
    CHECK_STR(req.path, "/api/add");
}

/* path 의 금지 바이트와 잘못된 %XX → 400 */
static void test_bad_path_bytes(void)
{
    CHECK_ERROR("GET /secret.txt%00.html HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /a%0Ab HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /a%7F HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /a%G1 HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /a%2 HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /a% HTTP/1.1\r\nHost: x\r\n\r\n", 400);

    /* query 는 디코딩하지 않으므로 %00 이 있어도 path 검사 대상이 아니다 */
    CHECK_INT(parse("GET /a?x=%00 HTTP/1.1\r\nHost: x\r\n\r\n"), PARSE_DONE);
}

static void test_request_line_shape(void)
{
    CHECK_ERROR("GET  / HTTP/1.1\r\nHost: x\r\n\r\n", 400);          /* 공백 두 개 */
    CHECK_ERROR("GET / HTTP/1.1 extra\r\nHost: x\r\n\r\n", 400);     /* 단어 넷 */
    CHECK_ERROR("GET /\r\nHost: x\r\n\r\n", 400);                    /* 버전 없음 */
    CHECK_ERROR("GET\r\n\r\n", 400);                                 /* Tiny Q11 */
    CHECK_ERROR("GET http://example.com/ HTTP/1.1\r\nHost: x\r\n\r\n", 400); /* absolute-form */
}

/* 요청 라인 앞의 빈 줄은 건너뛴다 (RFC 9112 2.2). 건너뛴 바이트도 consumed 에 들어간다 */
static void test_leading_empty_lines(void)
{
    const char *s = "\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n";

    REQUIRE(parse(s) == PARSE_DONE);
    CHECK_INT(consumed, strlen(s));
    CHECK_STR(req.target, "/");
    CHECK_MEM(req.headers, req.headers_len, "Host: x\r\n", 9);

    CHECK_INT(parse("\r\n\r\n"), PARSE_NEED_MORE);   /* 아직 요청 라인이 안 옴 */
    CHECK_INT(parse("\r\n"), PARSE_NEED_MORE);
}

/* 헤더는 작고 본문이 8KB 보다 큰 요청: 431 이 아니라 DONE (8KB 창은 헤더에만 적용) */
static void test_large_body(void)
{
    static char s[10000];
    const char *head = "POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 9000\r\n\r\n";
    size_t hl = strlen(head);

    strcpy(s, head);
    memset(s + hl, 'b', 9000);
    s[hl + 9000] = '\0';
    REQUIRE(parse(s) == PARSE_DONE);
    CHECK_INT(consumed, hl + 9000);
    CHECK_INT(req.body_len, 9000);
    CHECK(req.body == g_buf + hl);

    CHECK_INT(parse_n(s, hl + 8999), PARSE_NEED_MORE);   /* 본문 1 바이트 모자람 */
    CHECK_INT(consumed, 0);
}

static void test_methods(void)
{
    CHECK_ERROR("get / HTTP/1.1\r\nHost: x\r\n\r\n", 501);     /* Tiny 는 strcasecmp 로 통과 */
    CHECK_ERROR("PUT / HTTP/1.1\r\nHost: x\r\n\r\n", 501);
    CHECK_STR(req.method_str, "PUT");
    CHECK_INT(req.method, HTTP_METHOD_OTHER);
    CHECK_INT(req.version_minor, 1);                            /* 오류여도 버전은 채움 */
    CHECK_ERROR("G@T / HTTP/1.1\r\nHost: x\r\n\r\n", 400);

    REQUIRE(parse("HEAD / HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_INT(req.method, HTTP_HEAD);
}

static void test_versions(void)
{
    CHECK_ERROR("GET / HTTP/9.9\r\nHost: x\r\n\r\n", 505);     /* Tiny Q3 */
    CHECK_ERROR("GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505);
    CHECK_ERROR("GET / HTTP/1.2\r\nHost: x\r\n\r\n", 505);
    CHECK_ERROR("GET / HTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET / http/1.1\r\nHost: x\r\n\r\n", 400);
}

static void test_host(void)
{
    CHECK_ERROR("GET / HTTP/1.1\r\n\r\n", 400);                         /* 1.1 은 필수 */
    CHECK_ERROR("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400);   /* 중복 */
    CHECK_INT(parse("GET / HTTP/1.1\r\nhOsT:   spaced  \r\n\r\n"), PARSE_DONE);
    CHECK_STR(req.host, "spaced");                                      /* 이름 대소문자, OWS */
}

static void test_content_length(void)
{
    CHECK_ERROR("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n", 400);
    CHECK_ERROR("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n", 400);
    CHECK_ERROR("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: \r\n\r\n", 400);
    CHECK_ERROR("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999999\r\n\r\n", 400);
    CHECK_ERROR("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!", 400);
    CHECK_ERROR("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1048577\r\n\r\n", 413);
    CHECK_ERROR("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", 501);

    /* 같은 값 중복은 받는다 */
    REQUIRE(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\ncontent-length: 2\r\n\r\nok")
            == PARSE_DONE);
    CHECK_MEM(req.body, req.body_len, "ok", 2);

    /* 상한 정확히 1MB 는 413 이 아니라 본문을 기다린다 */
    CHECK_INT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1048576\r\n\r\n"), PARSE_NEED_MORE);
    CHECK_INT(consumed, 0);
}

static void test_connection(void)
{
    REQUIRE(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n") == PARSE_DONE);
    CHECK_INT(req.keep_alive, 0);
    REQUIRE(parse("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n") == PARSE_DONE);
    CHECK_INT(req.keep_alive, 1);
    REQUIRE(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive, close\r\n\r\n") == PARSE_DONE);
    CHECK_INT(req.keep_alive, 0);                                       /* close 가 이김 */
    REQUIRE(parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\n\r\n") == PARSE_DONE);
    CHECK_INT(req.keep_alive, 1);                                       /* 1.1 기본값 유지 */
}

static void test_header_syntax(void)
{
    CHECK_ERROR("GET / HTTP/1.1\r\nHost : x\r\n\r\n", 400);               /* 콜론 앞 공백 */
    CHECK_ERROR("GET / HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n", 400);     /* obs-fold */
    CHECK_ERROR("GET / HTTP/1.1\r\nHost: x\r\nNoColon\r\n\r\n", 400);
    CHECK_ERROR("GET / HTTP/1.1\r\nHost: x\nEvil: y\r\n\r\n", 400);       /* 줄 안의 맨 LF */
    CHECK_ERROR("GET / HTTP/1.1\r\nHost: x\rEvil: y\r\n\r\n", 400);       /* 줄 안의 맨 CR */
    CHECK_ERROR("GET / HTTP/1.1\r\n: x\r\n\r\n", 400);                    /* 빈 이름 */
}

/* 맨 LF 만 쓰는 클라이언트는 빈 줄을 못 찾아 NEED_MORE (Tiny Q6). 결국 8KB 에서 431 */
static void test_bare_lf_request(void)
{
    CHECK_INT(parse("GET / HTTP/1.0\n\n"), PARSE_NEED_MORE);
    CHECK_INT(consumed, 0);
}

/* 414 와 431: 같은 요청은 TCP 분할과 상관없이 같은 코드 (PR #18 리뷰 → decisions 2026-11-20) */
static void test_uri_and_header_limits(void)
{
    static char s[12000];
    size_t n;

    /* 대상 3000 바이트, 전체는 8KB 안 → 414 */
    strcpy(s, "GET /");
    memset(s + 5, 'a', 2999);
    strcpy(s + 5 + 2999, " HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK_ERROR(s, 414);

    /* 대상 9000 바이트 (전체 9026) → 한 번에 와도 431 */
    strcpy(s, "GET /");
    memset(s + 5, 'a', 8999);
    strcpy(s + 5 + 8999, " HTTP/1.1\r\nHost: x\r\n\r\n");
    n = strlen(s);
    CHECK_INT(n, 9026);
    CHECK_ERROR(s, 431);

    /* 같은 요청이 4000 씩 나눠 와도 같은 결과 */
    CHECK_INT(parse_n(s, 4000), PARSE_NEED_MORE);
    CHECK_INT(parse_n(s, 8000), PARSE_NEED_MORE);
    CHECK_INT(parse_n(s, n), PARSE_ERROR);
    CHECK_INT(status, 431);

    /* 4500 씩 나눠 와도 */
    CHECK_INT(parse_n(s, 4500), PARSE_NEED_MORE);
    CHECK_INT(parse_n(s, 9000), PARSE_ERROR);
    CHECK_INT(status, 431);
}

/* 빈 줄까지 정확히 8192 바이트면 통과, 8193 이면 431 */
static void test_header_limit_boundary(void)
{
    static char s[9000];
    const char *head = "GET / HTTP/1.1\r\nHost: x\r\nX-Pad: ";
    size_t hl = strlen(head);
    size_t pad = HTTP_MAX_HEADER - hl - 4;   /* 뒤에 "\r\n\r\n" */

    strcpy(s, head);
    memset(s + hl, 'p', pad);
    strcpy(s + hl + pad, "\r\n\r\n");
    CHECK_INT(strlen(s), HTTP_MAX_HEADER);
    CHECK_INT(parse(s), PARSE_DONE);
    CHECK_INT(consumed, HTTP_MAX_HEADER);

    strcpy(s, head);
    memset(s + hl, 'p', pad + 1);
    strcpy(s + hl + pad + 1, "\r\n\r\n");
    CHECK_ERROR(s, 431);
}

int main(void)
{
    RUN(test_simple_get);
    RUN(test_http10_no_headers);
    RUN(test_post_body);
    RUN(test_one_byte_at_a_time);
    RUN(test_pipelined);
    RUN(test_target_split);
    RUN(test_bad_path_bytes);
    RUN(test_request_line_shape);
    RUN(test_leading_empty_lines);
    RUN(test_large_body);
    RUN(test_methods);
    RUN(test_versions);
    RUN(test_host);
    RUN(test_content_length);
    RUN(test_connection);
    RUN(test_header_syntax);
    RUN(test_bare_lf_request);
    RUN(test_uri_and_header_limits);
    RUN(test_header_limit_boundary);
    free(g_buf);
    return test_report();
}
