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

/* 파서 옵션. NULL 이면 기본값. merge_slashes 시험에서만 바꾼다 */
static const http_parse_options *g_opt;

/* s 의 앞 n 바이트만 넘긴다 (조각으로 도착한 상황) */
static parse_result parse_n(const char *s, size_t n)
{
    free(g_buf);
    g_buf = malloc(n ? n : 1);
    if (g_buf == NULL)
        exit(2);
    memcpy(g_buf, s, n);
    return http_parse_request(g_buf, n, g_opt, &req, &consumed, &status);
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

/* 1 바이트씩 도착: 마지막 바이트 전까지는 전부 NEED_MORE (Tiny 재현 ①의 반대).
 * consumed 는 빈 줄까지 오기 전엔 0, 빈 줄이 오면 그때부터 요청 전체 길이 n (PR #24 리뷰) */
static void test_one_byte_at_a_time(void)
{
    const char *s = "POST /api/echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello";
    size_t n = strlen(s), head = n - 5, k;
    int bad = 0;

    for (k = 0; k < n; k++) {
        size_t want = k >= head ? n : 0;

        if (parse_n(s, k) != PARSE_NEED_MORE || consumed != want) {
            bad = 1;
            fprintf(stderr, "    prefix %zu bytes: want NEED_MORE + consumed %zu, got consumed %zu\n",
                    k, want, consumed);
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
    CHECK_INT(second, 50);                    /* 다음 요청의 첫 바이트 'G' 를 먹지 않는다 */
    CHECK(rest[second] == 'G');

    /* 세 번째는 덜 왔다 */
    CHECK_INT(parse(rest + second), PARSE_NEED_MORE);
    CHECK_INT(consumed, 0);
}

/* target(정규형) / path / query 분리와 퍼센트 디코딩 (#31) */
static void test_target_split(void)
{
    /* '.' 은 비예약 문자라 %2E 는 정규형에서 글자가 된다. query 는 원본 그대로 */
    REQUIRE(parse("GET /docs/%2E%2Ex/secret?q=a%20b HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.target, "/docs/..x/secret?q=a%20b");
    CHECK_STR(req.path, "/docs/..x/secret");  /* "..x" 는 점 조각이 아니다 (점 조각은 test_dot_segments) */
    CHECK_STR(req.query, "q=a%20b");

    REQUIRE(parse("GET /a%20b?x=1&y=%3D HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.target, "/a%20b?x=1&y=%3D");   /* 비예약 문자가 아닌 %XX 는 정규형에 남는다 */
    CHECK_STR(req.path, "/a b");
    CHECK_STR(req.query, "x=1&y=%3D");

    REQUIRE(parse("GET /%61pi/add HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.target, "/api/add");        /* 라우팅·전달은 정규형으로 (decisions 2026-11-23) */
    CHECK_STR(req.path, "/api/add");

    /* 디코딩은 한 번만: %25 는 '%' 가 되고 거기서 멈춘다. 두 번 풀면 "/../secret" 이 된다.
     * static_open 은 글자 그대로의 "%2e%2e" 를 폴더 이름으로 보고 404 (PR #22 의 B 확인 요청) */
    REQUIRE(parse("GET /%252e%252e/secret HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.target, "/%252e%252e/secret");
    CHECK_STR(req.path, "/%2e%2e/secret");
    REQUIRE(parse("GET /a%252F HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);   /* %2F 가 아니다 */
    CHECK_STR(req.target, "/a%252F");
    CHECK_STR(req.path, "/a%2F");

    /* 빈 query 도 원본의 '?' 부분 그대로 */
    REQUIRE(parse("GET /a? HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.target, "/a?");
    CHECK_STR(req.query, "");
}

/* 정규형 규칙 2·4 (#31): 구분자로 읽힐 바이트는 400, 비예약 문자만 디코딩, 나머지는 16진수 대문자 */
static void test_normalize_encoding(void)
{
    /* 2. 디코딩하면 '/' 나 '\' 가 되는 %XX, 인코딩 안 된 '\' */
    CHECK_ERROR("GET /admin%2Fx HTTP/1.1\r\nHost: x\r\n\r\n", 400);    /* #31 재현 */
    CHECK_ERROR("GET /admin%2fx HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /api/a%2Fb HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /a%5Cb HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /a%5cb HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /a\\b HTTP/1.1\r\nHost: x\r\n\r\n", 400);         /* PR #34 리뷰 */
    CHECK_ERROR("GET /\\admin/x HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    /* query 는 경로가 아니므로 %2F, '\' 가 있어도 그대로 */
    REQUIRE(parse("GET /a?p=%2F&q=\\ HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.target, "/a?p=%2F&q=\\");
    CHECK_STR(req.query, "p=%2F&q=\\");

    /* 4. 비예약 문자(A-Z a-z 0-9 - . _ ~)만 글자로 */
    REQUIRE(parse("GET /%61dmin/x HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);   /* #31 재현 */
    CHECK_STR(req.target, "/admin/x");
    REQUIRE(parse("GET /%7Euser/%2D%2e%5F%30%41%7a HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.target, "/~user/-._0Az");
    CHECK_STR(req.path, "/~user/-._0Az");
    /* 나머지 %XX 는 남기고 16진수만 대문자 */
    REQUIRE(parse("GET /%eb%8b%a4/%3a%40%21%20 HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.target, "/%EB%8B%A4/%3A%40%21%20");
    CHECK_STR(req.path, "/\xeb\x8b\xa4/:@! ");
    /* 인코딩 안 한 예약 문자는 그대로 (인코딩한 것과는 다른 URI, RFC 3986 2.2) */
    REQUIRE(parse("GET /a:b@c!$&'()*+,;= HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.target, "/a:b@c!$&'()*+,;=");
}

/* 정규형 규칙 3 (#31): 빈 조각 "//" 는 기본 400, merge_slashes 면 하나로 */
static void test_normalize_empty_segments(void)
{
    static const http_parse_options merge = { 1 };

    CHECK_ERROR("GET //a HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET //admin/x HTTP/1.1\r\nHost: x\r\n\r\n", 400);     /* #31 재현 */
    CHECK_ERROR("GET /a//b HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /a// HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET // HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    /* 끝 '/' 하나는 빈 조각이 아니다 */
    REQUIRE(parse("GET /a/ HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.target, "/a/");
    REQUIRE(parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.target, "/");
    /* query 의 "//" 는 경로가 아니다 */
    REQUIRE(parse("GET /a?u=http://x HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.target, "/a?u=http://x");

    g_opt = &merge;
    REQUIRE(parse("GET /a//b///c HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.target, "/a/b/c");
    CHECK_STR(req.path, "/a/b/c");
    REQUIRE(parse("GET //admin/x//?q=//y HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.target, "/admin/x/?q=//y");
    CHECK_STR(req.path, "/admin/x/");
    CHECK_STR(req.query, "q=//y");
    REQUIRE(parse("GET // HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.target, "/");
    /* 합쳐도 다른 규칙은 그대로 */
    CHECK_ERROR("GET /a//../b HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET //a%2Fb HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    g_opt = NULL;
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

/* path 의 "." / ".." 조각 → 400 (PR #26 리뷰: 라우팅 전에 막아 정리 전후 경로가 다른 라우트로 가지 않게) */
static void test_dot_segments(void)
{
    /* 평문 */
    CHECK_ERROR("GET /.. HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /. HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /../secret HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /public/../admin/x HTTP/1.1\r\nHost: x\r\n\r\n", 400);   /* 혜성 재현 */
    CHECK_ERROR("GET /a/./b HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /a/.. HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /a/../ HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /a//../b HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /a/..?x=1 HTTP/1.1\r\nHost: x\r\n\r\n", 400);   /* '?' 앞까지가 path */

    /* 인코딩: 디코딩한 뒤에 본다 */
    CHECK_ERROR("GET /public/%2e%2e/admin/x HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /a/%2E%2e/b HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /a/.%2E HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /a/%2e/b HTTP/1.1\r\nHost: x\r\n\r\n", 400);
    CHECK_ERROR("GET /a%2F..%2Fb HTTP/1.1\r\nHost: x\r\n\r\n", 400);   /* %2F 도 디코딩하면 '/' */

    /* 점 조각이 아닌 이름은 통과 */
    REQUIRE(parse("GET /.../a. HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.path, "/.../a.");
    REQUIRE(parse("GET /.well-known/x HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.path, "/.well-known/x");
    REQUIRE(parse("GET /a..b/..c HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.path, "/a..b/..c");
    /* 빈 조각 "//a" 는 test_normalize_empty_segments (기본 400, #31) */
    /* query 의 ".." 는 path 가 아니다 */
    REQUIRE(parse("GET /a?p=../b HTTP/1.1\r\nHost: x\r\n\r\n") == PARSE_DONE);
    CHECK_STR(req.query, "p=../b");
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
    CHECK_INT(consumed, 0);                          /* 빈 줄만 건너뛴 것은 헤더 끝이 아니다 */
    CHECK_INT(parse("\r\n"), PARSE_NEED_MORE);

    /* 본문 대기 NEED_MORE 의 필요한 총 길이에도 건너뛴 빈 줄이 들어간다 (DONE 의 consumed 와 같은 기준) */
    CHECK_INT(parse("\r\nPOST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nab"), PARSE_NEED_MORE);
    CHECK_INT(consumed, strlen("\r\nPOST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nab") + 1);
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
    CHECK_INT(consumed, hl + 9000);                       /* 필요한 총 길이 */

    /* 헤더 직후, 본문이 하나도 안 왔어도 같은 값 */
    CHECK_INT(parse_n(s, hl), PARSE_NEED_MORE);
    CHECK_INT(consumed, hl + 9000);
    /* 빈 줄의 마지막 LF 가 아직이면 헤더가 안 끝난 것이라 0 */
    CHECK_INT(parse_n(s, hl - 1), PARSE_NEED_MORE);
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

/* ERROR 때 method 계약 (헤더 주석, PR #29): 요청 라인을 읽은 뒤의 오류면 HEAD 가 남고,
 * 그 전의 오류면 초기값 GET. server.c 가 이걸 보고 HEAD 오류 응답의 본문을 뺀다 */
static void test_method_on_error(void)
{
    static char big[HTTP_MAX_HEADER + 64];
    size_t n;

    CHECK_ERROR("HEAD / HTTP/2.0\r\nHost: x\r\n\r\n", 505);       /* 버전 오류 */
    CHECK_INT(req.method, HTTP_HEAD);
    CHECK_ERROR("HEAD /a%zz HTTP/1.1\r\nHost: x\r\n\r\n", 400);   /* 경로 디코딩 오류 */
    CHECK_INT(req.method, HTTP_HEAD);
    CHECK_ERROR("HEAD / HTTP/1.1\r\n\r\n", 400);                  /* 헤더 단계 (Host 없음) */
    CHECK_INT(req.method, HTTP_HEAD);

    CHECK_ERROR("HEAD /x\r\nHost: x\r\n\r\n", 400);               /* 공백 하나: 메서드 모름 */
    CHECK_INT(req.method, HTTP_GET);
    CHECK_ERROR("HE@D / HTTP/1.1\r\nHost: x\r\n\r\n", 400);       /* 토큰 아닌 글자 */
    CHECK_INT(req.method, HTTP_GET);

    /* 431: 첫 줄이 8KB 안에 있으면 HEAD, 첫 줄 자체가 8KB 를 넘으면 GET */
    n = (size_t)sprintf(big, "HEAD / HTTP/1.1\r\nX: ");
    memset(big + n, 'a', HTTP_MAX_HEADER);
    strcpy(big + n + HTTP_MAX_HEADER, "\r\n\r\n");
    CHECK_ERROR(big, 431);
    CHECK_INT(req.method, HTTP_HEAD);

    n = (size_t)sprintf(big, "HEAD /");
    memset(big + n, 'a', HTTP_MAX_HEADER);
    strcpy(big + n + HTTP_MAX_HEADER, " HTTP/1.1\r\n\r\n");
    CHECK_ERROR(big, 431);
    CHECK_INT(req.method, HTTP_GET);
}

static void test_host(void)
{
    CHECK_ERROR("GET / HTTP/1.1\r\n\r\n", 400);                         /* 1.1 은 필수 */
    CHECK_ERROR("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400);   /* 중복 */
    CHECK_INT(parse("GET / HTTP/1.1\r\nhOsT:   spaced  \r\n\r\n"), PARSE_DONE);
    CHECK_STR(req.host, "spaced");                                      /* 이름 대소문자, OWS */

    CHECK_ERROR("GET / HTTP/1.1\r\nHost: example com\r\n\r\n", 400);   /* 잘못된 Host 값 */
    CHECK_ERROR("GET / HTTP/1.1\r\nHost: a/b\r\n\r\n", 400);
    CHECK_INT(parse("GET / HTTP/1.1\r\nHost: [::1]:8080\r\n\r\n"), PARSE_DONE);
    CHECK_STR(req.host, "[::1]:8080");
}

/* 문자열 리터럴로는 못 넘기는 NUL 바이트: 헤더 값에 있으면 400, 본문에 있으면 그대로 */
static void test_nul_bytes(void)
{
    static const char in_header[] = "GET / HTTP/1.1\r\nHost: x\r\nX-A: a\0b\r\n\r\n";
    static const char in_body[] = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\na\0b";

    CHECK_INT(parse_n(in_header, sizeof in_header - 1), PARSE_ERROR);
    CHECK_INT(status, 400);

    REQUIRE(parse_n(in_body, sizeof in_body - 1) == PARSE_DONE);
    CHECK_MEM(req.body, req.body_len, "a\0b", 3);
}

/* '#' 는 요청 대상에 올 수 없다 */
static void test_fragment_in_target(void)
{
    CHECK_ERROR("GET /docs#intro HTTP/1.1\r\nHost: x\r\n\r\n", 400);
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

    /* 같은 값 중복도 400 (프록시가 원본 헤더를 그대로 넘기므로, http_request.h) */
    CHECK_ERROR("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\ncontent-length: 2\r\n\r\nok", 400);

    /* 상한 정확히 1MB 는 413 이 아니라 본문을 기다린다. consumed = 헤더 + 1MB */
    CHECK_INT(parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1048576\r\n\r\n"), PARSE_NEED_MORE);
    CHECK_INT(consumed, strlen("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1048576\r\n\r\n") + 1048576);
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

    /* 같은 8192 바이트 요청의 마지막 바이트가 늦게 오면: 8191 은 NEED_MORE, 8192 에서 DONE */
    CHECK_INT(parse_n(s, HTTP_MAX_HEADER - 1), PARSE_NEED_MORE);
    CHECK_INT(parse_n(s, HTTP_MAX_HEADER), PARSE_DONE);

    strcpy(s, head);
    memset(s + hl, 'p', pad + 1);
    strcpy(s + hl + pad + 1, "\r\n\r\n");
    CHECK_ERROR(s, 431);
    CHECK_INT(req.version_minor, 1);          /* 431 이어도 첫 줄의 버전은 채운다 */
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
    RUN(test_dot_segments);
    RUN(test_request_line_shape);
    RUN(test_leading_empty_lines);
    RUN(test_large_body);
    RUN(test_methods);
    RUN(test_versions);
    RUN(test_method_on_error);
    RUN(test_host);
    RUN(test_nul_bytes);
    RUN(test_fragment_in_target);
    RUN(test_content_length);
    RUN(test_normalize_encoding);
    RUN(test_normalize_empty_segments);
    RUN(test_connection);
    RUN(test_header_syntax);
    RUN(test_bare_lf_request);
    RUN(test_uri_and_header_limits);
    RUN(test_header_limit_boundary);
    free(g_buf);
    return test_report();
}
