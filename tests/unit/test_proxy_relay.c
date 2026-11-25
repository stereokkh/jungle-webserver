/*
 * test_proxy_relay.c — 업스트림 응답 중계 proxy_relay_* (이슈 #9)
 *
 * 소켓 없이 바이트를 먹이고 out 에 쌓인 것을 비교한다. 입력은 전부 NUL 종료 없는 정확한 크기의
 * 힙 복사본으로 넘겨서 머리 해석이 길이를 넘어 읽으면 make asan 이 잡는다.
 * Round-Robin(proxy_rr_begin·proxy_pick_backend)은 tests/unit/test_contract.c 와 test_proxy_config.c 에 있다.
 */
#include <stdint.h>

#include "test.h"

#include "buffer.h"
#include "http_request.h"
#include "proxy.h"

#define H200 "HTTP/1.1 200 OK\r\n"

static proxy_relay r;
static buffer out;

static void begin(int head)
{
    proxy_relay_free(&r);
    buffer_free(&out);
    proxy_relay_init(&r, head);
}

/* 정확한 크기의 힙 복사본으로 n 바이트를 먹인다 */
static proxy_relay_result feed_n(const char *s, size_t n)
{
    char *p = malloc(n ? n : 1);
    proxy_relay_result res;

    if (p == NULL) {
        perror("malloc");
        exit(2);
    }
    memcpy(p, s, n);
    res = proxy_relay_input(&r, p, n, &out);
    free(p);
    return res;
}

static proxy_relay_result feed(const char *s)
{
    return feed_n(s, strlen(s));
}

static void check_out(const char *expected)
{
    CHECK_MEM(buffer_pending(&out) ? buffer_peek(&out) : "", buffer_pending(&out), expected,
              strlen(expected));
}

/* s 전체를 한 번에 먹였을 때의 결과 */
static proxy_relay_result whole(const char *s, int head)
{
    begin(head);
    return feed(s);
}

/* 1. 응답 전체가 한 번에 */
static void test_whole_response(void)
{
    const char *resp = H200 "Content-Length: 5\r\nContent-Type: text/plain\r\n\r\nhello";

    CHECK_INT(whole(resp, 0), PROXY_RELAY_COMPLETE);
    check_out(resp);
    CHECK_INT(r.started, 1);
    CHECK_INT(r.status, 200);
    CHECK_INT(r.body_mode, PROXY_BODY_LENGTH);
    CHECK_INT(r.body_left, 0);
    CHECK_INT(proxy_relay_eof(&r), PROXY_RELAY_COMPLETE);
}

/* 2. 한 바이트씩: 마지막 바이트에서만 COMPLETE */
static void test_byte_at_a_time(void)
{
    const char *resp = H200 "Content-Length: 5\r\nX-A: b\r\n\r\nhello";
    size_t n = strlen(resp), i;

    begin(0);
    for (i = 0; i < n; i++) {
        proxy_relay_result res = feed_n(resp + i, 1);

        if (i + 1 < n) {
            CHECK_INT(res, PROXY_RELAY_MORE);
        } else {
            CHECK_INT(res, PROXY_RELAY_COMPLETE);
        }
    }
    check_out(resp);
}

/* 1..n 바이트 단위로 쪼개도 결과가 같다 (TE 로 끝까지 읽는 응답과 CL 응답 둘 다) */
static void test_every_chunk_size(void)
{
    const char *resps[] = {
        H200 "Content-Length: 11\r\nX-A: b\r\n\r\nhello world",
        H200 "Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
    };
    size_t k, i, c;

    for (k = 0; k < 2; k++) {
        size_t n = strlen(resps[k]);

        for (c = 1; c <= n; c++) {
            proxy_relay_result res = PROXY_RELAY_MORE;

            begin(0);
            for (i = 0; i < n; i += c)
                res = feed_n(resps[k] + i, n - i < c ? n - i : c);
            if (k == 0) {
                CHECK_INT(res, PROXY_RELAY_COMPLETE);
            } else {
                CHECK_INT(res, PROXY_RELAY_MORE);
                CHECK_INT(proxy_relay_eof(&r), PROXY_RELAY_COMPLETE);
            }
            check_out(resps[k]);
        }
    }
}

/* 3. 머리 끝 \r\n\r\n 한가운데서 끊김 */
static void test_split_inside_blank_line(void)
{
    const char *head = H200 "Content-Length: 5\r\n\r\n";
    size_t hl = strlen(head), cut;

    for (cut = hl - 3; cut < hl; cut++) {       /* "\r" | "\n\r\n", "\r\n" | "\r\n", "\r\n\r" | "\n" */
        begin(0);
        CHECK_INT(feed_n(head, cut), PROXY_RELAY_MORE);
        CHECK_INT(r.started, 0);
        CHECK_INT(feed_n(head + cut, hl - cut), PROXY_RELAY_MORE);
        CHECK_INT(r.started, 1);
        CHECK_INT(feed("hello"), PROXY_RELAY_COMPLETE);
        check_out(H200 "Content-Length: 5\r\n\r\nhello");
    }
}

/* 4. Content-Length 100 인데 40바이트에서 EOF */
static void test_truncated(void)
{
    char body[40];
    char expect[200];
    const char *head = H200 "Content-Length: 100\r\n\r\n";
    char *resp;
    size_t hl = strlen(head);

    memset(body, 'x', sizeof body);
    memcpy(expect, head, hl);
    memcpy(expect + hl, body, sizeof body);
    resp = malloc(hl + sizeof body);
    REQUIRE(resp != NULL);
    memcpy(resp, expect, hl + sizeof body);
    begin(0);
    CHECK_INT(feed_n(resp, hl + sizeof body), PROXY_RELAY_MORE);
    CHECK_INT(r.body_left, 60);
    CHECK_INT(proxy_relay_eof(&r), PROXY_RELAY_TRUNCATED);
    CHECK_MEM(buffer_peek(&out), buffer_pending(&out), expect, hl + sizeof body);
    free(resp);
}

/* 5. 머리가 끝나기 전 EOF → BAD, out 은 그대로 */
static void test_eof_before_head(void)
{
    begin(0);
    CHECK_INT(feed("HTTP/1.1 200 OK\r\nContent-Le"), PROXY_RELAY_MORE);
    CHECK_INT(proxy_relay_eof(&r), PROXY_RELAY_BAD);
    CHECK_INT(buffer_pending(&out), 0);
    CHECK_INT(r.started, 0);

    begin(0);
    CHECK_INT(proxy_relay_eof(&r), PROXY_RELAY_BAD);
    CHECK_INT(buffer_pending(&out), 0);
}

/* 6. HEAD 요청: Content-Length 가 있어도 본문 없음. 머리 뒤 바이트는 버린다 */
static void test_head_request(void)
{
    const char *head = H200 "Content-Length: 1000\r\n\r\n";

    CHECK_INT(whole(head, 1), PROXY_RELAY_COMPLETE);
    check_out(head);
    CHECK_INT(r.body_mode, PROXY_BODY_NONE);
    CHECK_INT(proxy_relay_want(&r, 4096), 0);
    CHECK_INT(proxy_relay_eof(&r), PROXY_RELAY_COMPLETE);

    CHECK_INT(whole(H200 "Content-Length: 1000\r\n\r\nGARBAGE", 1), PROXY_RELAY_COMPLETE);
    check_out(head);
    CHECK_INT(feed("more garbage"), PROXY_RELAY_COMPLETE);   /* 끝난 뒤 들어온 바이트도 버린다 */
    check_out(head);

    /* 머리가 쪼개져 와도 같다 */
    begin(1);
    CHECK_INT(feed(H200 "Content-Len"), PROXY_RELAY_MORE);
    CHECK_INT(feed("gth: 1000\r\n\r\nxyz"), PROXY_RELAY_COMPLETE);
    check_out(head);
}

/* 7. 204·304 는 Content-Length 가 있어도 머리에서 끝 */
static void test_204_304(void)
{
    CHECK_INT(whole("HTTP/1.1 204 No Content\r\nContent-Length: 10\r\n\r\nabc", 0), PROXY_RELAY_COMPLETE);
    check_out("HTTP/1.1 204 No Content\r\nContent-Length: 10\r\n\r\n");
    CHECK_INT(r.status, 204);

    CHECK_INT(whole("HTTP/1.1 304 Not Modified\r\nContent-Length: 10\r\n\r\n", 0), PROXY_RELAY_COMPLETE);
    check_out("HTTP/1.1 304 Not Modified\r\nContent-Length: 10\r\n\r\n");
    CHECK_INT(r.status, 304);
}

/* 8. 길이도 TE 도 없으면 EOF 까지 */
static void test_until_eof(void)
{
    begin(0);
    CHECK_INT(feed(H200 "Server: x\r\n\r\nabc"), PROXY_RELAY_MORE);
    CHECK_INT(r.body_mode, PROXY_BODY_UNTIL_EOF);
    check_out(H200 "Server: x\r\n\r\nabc");
    CHECK_INT(feed("def"), PROXY_RELAY_MORE);
    check_out(H200 "Server: x\r\n\r\nabcdef");
    CHECK_INT(proxy_relay_eof(&r), PROXY_RELAY_COMPLETE);

    /* 머리만 받고 바로 EOF 여도 응답은 시작했으므로 COMPLETE (본문 0바이트) */
    begin(0);
    CHECK_INT(feed(H200 "Server: x\r\n\r\n"), PROXY_RELAY_MORE);
    CHECK_INT(proxy_relay_eof(&r), PROXY_RELAY_COMPLETE);

    /* 이유 문구 없는 상태 줄 */
    CHECK_INT(whole("HTTP/1.1 200\r\n\r\nx", 0), PROXY_RELAY_MORE);
    check_out("HTTP/1.1 200\r\n\r\nx");
}

/* 9. Transfer-Encoding 이 있으면 Content-Length 는 무시하고 원본 그대로 */
static void test_transfer_encoding(void)
{
    const char *resp = H200 "Transfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n5\r\nhello\r\n0\r\n\r\n";

    CHECK_INT(whole(resp, 0), PROXY_RELAY_MORE);
    CHECK_INT(r.body_mode, PROXY_BODY_UNTIL_EOF);
    check_out(resp);
    CHECK_INT(proxy_relay_eof(&r), PROXY_RELAY_COMPLETE);

    /* Content-Length: 0 + TE 도 TE 가 이긴다 */
    CHECK_INT(whole(H200 "content-length: 0\r\ntransfer-encoding: gzip\r\n\r\nabc", 0), PROXY_RELAY_MORE);
    CHECK_INT(r.body_mode, PROXY_BODY_UNTIL_EOF);
    check_out(H200 "content-length: 0\r\ntransfer-encoding: gzip\r\n\r\nabc");

    /* 깨진 Content-Length 도 TE 가 있으면 무시한다 (C2). 순서와 상관없이 */
    CHECK_INT(whole(H200 "Content-Length: abc\r\nTransfer-Encoding: chunked\r\n\r\nx", 0), PROXY_RELAY_MORE);
    CHECK_INT(r.body_mode, PROXY_BODY_UNTIL_EOF);
    CHECK_INT(whole(H200 "Transfer-Encoding: chunked\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nx", 0),
              PROXY_RELAY_MORE);
    CHECK_INT(whole(H200 "Content-Length: abc\r\n\r\nx", 0), PROXY_RELAY_BAD);
}

/* 10. Content-Length 보다 많이 오면 그 길이만 */
static void test_extra_bytes_dropped(void)
{
    CHECK_INT(whole(H200 "Content-Length: 5\r\n\r\nhelloEXTRA", 0), PROXY_RELAY_COMPLETE);
    check_out(H200 "Content-Length: 5\r\n\r\nhello");

    begin(0);
    CHECK_INT(feed(H200 "Content-Length: 5\r\n\r\nhe"), PROXY_RELAY_MORE);
    CHECK_INT(feed("lloEXTRA"), PROXY_RELAY_COMPLETE);
    check_out(H200 "Content-Length: 5\r\n\r\nhello");
}

/* 11. 상태 줄이 깨진 경우 */
static void test_bad_status_lines(void)
{
    static const char *const bad[] = {
        "HTTP/2 200 OK\r\n\r\n",
        "HTTP/1.1 20 OK\r\n\r\n",
        "HTTP/1.1 2000 OK\r\n\r\n",
        "HTTP/1.1 099 X\r\n\r\n",
        "HTTP/1.1 000 X\r\n\r\n",
        "FOO 200 OK\r\n\r\n",
        "HTTP/1.1 600 X\r\n\r\n",
        "HTTP/1.1 100 Continue\r\n\r\n",        /* C1 */
        "HTTP/1.1 101 Switching Protocols\r\n\r\n",
        "HTTP/1.1 199 X\r\n\r\n",
        "HTTP/1.1 200 O\001K\r\n\r\n",          /* 이유 문구 안의 제어 문자 */
        "HTTP/1.1 200 O\177K\r\n\r\n",
        "HTTP/1.1 200OK\r\n\r\n",
        "HTTP/1.1  200 OK\r\n\r\n",
        "HTTP/1.1 2x0 OK\r\n\r\n",
        "HTTP/1.x 200 OK\r\n\r\n",
        "http/1.1 200 OK\r\n\r\n",
        " HTTP/1.1 200 OK\r\n\r\n",
        "hello\r\n\r\n",
        "\r\n\r\n",
        "HTTP/1.1 200 OK \r\r\n\r\n",
    };
    size_t i;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK_INT(whole(bad[i], 0), PROXY_RELAY_BAD);
        CHECK_INT(buffer_pending(&out), 0);
        CHECK_INT(r.started, 0);
    }
    /* 늦게 읽는 쪽이 아니라 머리가 끝난 순간 BAD 를 낸다 */
    begin(0);
    CHECK_INT(feed("hello\r\n"), PROXY_RELAY_MORE);
    CHECK_INT(feed("\r\n"), PROXY_RELAY_BAD);

    /* 반대로 문법에 맞는 변형은 받는다 */
    CHECK_INT(whole("HTTP/1.1 200 \r\n\r\n", 0), PROXY_RELAY_MORE);                 /* 빈 이유 문구 */
    CHECK_INT(whole("HTTP/1.1 599 Weird\tReason \x80\xff\r\n\r\n", 0), PROXY_RELAY_MORE);
    CHECK_INT(whole("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n", 0), PROXY_RELAY_COMPLETE);
}

/* 12. 머리 크기 한도: 빈 줄까지 정확히 HTTP_MAX_HEADER 바이트는 통과 */
static char *make_head(size_t total, int terminated, size_t *len)
{
    static const char start[] = H200 "Content-Length: 0\r\nX-Pad: ";
    size_t sl = sizeof start - 1, tail = terminated ? 4 : 0;
    char *s = malloc(total);

    if (s == NULL || total < sl + tail) {
        perror("make_head");
        exit(2);
    }
    memcpy(s, start, sl);
    memset(s + sl, 'a', total - sl - tail);
    if (terminated)
        memcpy(s + total - 4, "\r\n\r\n", 4);
    *len = total;
    return s;
}

static void test_head_size_limit(void)
{
    size_t n;
    char *s = make_head(HTTP_MAX_HEADER, 1, &n);

    begin(0);
    CHECK_INT(feed_n(s, n), PROXY_RELAY_COMPLETE);              /* 딱 한도 */
    CHECK_MEM(buffer_peek(&out), buffer_pending(&out), s, n);
    free(s);

    s = make_head(HTTP_MAX_HEADER + 1, 1, &n);                  /* 빈 줄까지 한도 +1 */
    begin(0);
    CHECK_INT(feed_n(s, n), PROXY_RELAY_BAD);
    CHECK_INT(buffer_pending(&out), 0);
    free(s);

    s = make_head(HTTP_MAX_HEADER, 0, &n);                      /* 끝나지 않은 한도 크기: 1바이트 여유 */
    begin(0);
    CHECK_INT(feed_n(s, n), PROXY_RELAY_MORE);
    CHECK_INT(proxy_relay_want(&r, 1000), 1);
    CHECK_INT(feed("a"), PROXY_RELAY_BAD);                      /* 한 바이트 더: 끝나지 않으면 BAD */
    CHECK_INT(proxy_relay_want(&r, 1000), 0);
    free(s);

    s = make_head(HTTP_MAX_HEADER + 1, 0, &n);                  /* 한 번에 한도 넘게 */
    begin(0);
    CHECK_INT(feed_n(s, n), PROXY_RELAY_BAD);
    free(s);

    /* 한 바이트씩 먹여도 한도에서 끊긴다 */
    s = make_head(HTTP_MAX_HEADER + 1, 0, &n);
    begin(0);
    {
        size_t i;
        proxy_relay_result res = PROXY_RELAY_MORE;

        for (i = 0; i < n && res == PROXY_RELAY_MORE; i++) {
            CHECK_INT(proxy_relay_want(&r, 1000), HTTP_MAX_HEADER + 1 - i);
            res = feed_n(s + i, 1);
        }
        CHECK_INT(res, PROXY_RELAY_BAD);
        CHECK_INT(i, n);
    }
    free(s);
}

/* 13. Content-Length 값 */
static proxy_relay_result with_cl(const char *value, const char *body)
{
    char buf[256];

    snprintf(buf, sizeof buf, H200 "Content-Length:%s\r\n\r\n%s", value, body);
    return whole(buf, 0);
}

static void test_content_length_values(void)
{
    static const char *const bad[] = {
        " abc", " -1", " +5", " 1 2", " 5, 5", " ", "", " 0x5", " 5a", " 1.5", " 1e3",
        " 123456789012345678901234567890",              /* 30자리 */
        " 9007199254740993",                            /* 2^53 + 1 */
        " 99999999999999999999",                        /* 64비트도 넘는다 */
        " 18446744073709551616",
    };
    size_t i;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK_INT(with_cl(bad[i], ""), PROXY_RELAY_BAD);
        CHECK_INT(buffer_pending(&out), 0);
    }
    CHECK_INT(with_cl("   7  ", "1234567"), PROXY_RELAY_COMPLETE);   /* OWS */
    check_out(H200 "Content-Length:   7  \r\n\r\n1234567");
    CHECK_INT(with_cl("\t7\t", "1234567"), PROXY_RELAY_COMPLETE);
    CHECK_INT(with_cl(" 007", "1234567"), PROXY_RELAY_COMPLETE);     /* 앞자리 0 은 숫자로 본다 */
#if SIZE_MAX >= 9007199254740992ULL
    CHECK_INT(with_cl(" 9007199254740992", ""), PROXY_RELAY_MORE);   /* 2^53 은 허용 (본문은 안 온다) */
    CHECK_INT(r.body_mode, PROXY_BODY_LENGTH);
#else
    CHECK_INT(with_cl(" 9007199254740992", ""), PROXY_RELAY_BAD);    /* 32비트 size_t 는 담지 못한다 (C5) */
#endif

    /* 줄이 여럿: 같으면 통과, 다르면 BAD, 이름은 대소문자 무시 */
    CHECK_INT(whole(H200 "Content-Length: 5\r\ncontent-length: 5\r\n\r\nhello", 0), PROXY_RELAY_COMPLETE);
    CHECK_INT(whole(H200 "Content-Length: 5\r\nContent-Length: 6\r\n\r\nhello", 0), PROXY_RELAY_BAD);
    CHECK_INT(whole(H200 "Content-Length: 5\r\nX: y\r\nCONTENT-LENGTH: 05\r\n\r\nhello", 0),
              PROXY_RELAY_COMPLETE);                                 /* 숫자로 같다 */
    /* 이름이 비슷하기만 한 줄은 길이가 아니다 */
    CHECK_INT(whole(H200 "X-Content-Length: 5\r\nContent-Length-X: 5\r\n\r\nhi", 0), PROXY_RELAY_MORE);
    CHECK_INT(r.body_mode, PROXY_BODY_UNTIL_EOF);
}

/* 14. 줄 문법: 맨 LF, obs-fold, 이름 안의 공백 */
static void test_head_syntax(void)
{
    static const char *const bad[] = {
        "HTTP/1.1 200 OK\nContent-Length: 5\n\nhello",             /* 맨 LF (머리 끝도 LF) */
        "HTTP/1.1 200 OK\r\nContent-Length: 5\nX: y\r\n\r\nhello",  /* 한 줄만 맨 LF */
        "HTTP/1.1 200 OK\r\nX-A: b\r\n c\r\n\r\n",                  /* obs-fold (SP) */
        "HTTP/1.1 200 OK\r\nX-A: b\r\n\tc\r\n\r\n",                 /* obs-fold (TAB) */
        "HTTP/1.1 200 OK\r\nFoo : bar\r\n\r\n",                     /* 이름 뒤 공백 */
        "HTTP/1.1 200 OK\r\nFo o: bar\r\n\r\n",
        "HTTP/1.1 200 OK\r\n Foo: bar\r\n\r\n",
        "HTTP/1.1 200 OK\r\n: bar\r\n\r\n",                         /* 빈 이름 */
        "HTTP/1.1 200 OK\r\nNoColon\r\n\r\n",
        "HTTP/1.1 200 OK\r\nX-A: a\rb\r\n\r\n",                     /* 값 안의 맨 CR */
        "HTTP/1.1 200 OK\r\nX-A: a\001b\r\n\r\n",                   /* 값 안의 제어 문자 */
        "HTTP/1.1 200 OK\r\nX-A: a\177b\r\n\r\n",
    };
    size_t i;

    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK_INT(whole(bad[i], 0), PROXY_RELAY_BAD);
        CHECK_INT(buffer_pending(&out), 0);
    }
    /* 맨 LF 는 머리가 끝나기를 기다리지 않고 바로 거절한다 */
    begin(0);
    CHECK_INT(feed("HTTP/1.1 200 OK\nContent-Le"), PROXY_RELAY_BAD);

    /* 머리 뒤 본문에 있는 맨 LF·CR 은 머리 검사에 들어가지 않는다 */
    CHECK_INT(whole(H200 "Content-Length: 6\r\n\r\na\nb\rc\n", 0), PROXY_RELAY_COMPLETE);
    /* 값 안의 TAB, obs-text 는 허용 */
    CHECK_INT(whole(H200 "X-A: a\tb \x80\xfe\r\nContent-Length: 0\r\n\r\n", 0), PROXY_RELAY_COMPLETE);
    /* 값이 빈 헤더 */
    CHECK_INT(whole(H200 "X-A:\r\nContent-Length: 0\r\n\r\n", 0), PROXY_RELAY_COMPLETE);
}

/* 15. proxy_relay_want */
static void test_want(void)
{
    begin(0);
    CHECK_INT(proxy_relay_want(&r, 0), HTTP_MAX_HEADER + 1);        /* 시작 전: room 은 무시 */
    CHECK_INT(proxy_relay_want(&r, 5), HTTP_MAX_HEADER + 1);
    CHECK_INT(feed(H200), PROXY_RELAY_MORE);
    CHECK_INT(proxy_relay_want(&r, 5), HTTP_MAX_HEADER + 1 - strlen(H200));

    begin(0);
    CHECK_INT(feed(H200 "Content-Length: 100\r\n\r\nabcde"), PROXY_RELAY_MORE);
    CHECK_INT(r.body_left, 95);
    CHECK_INT(proxy_relay_want(&r, 50), 50);                        /* room 이 작다 */
    CHECK_INT(proxy_relay_want(&r, 95), 95);
    CHECK_INT(proxy_relay_want(&r, 500), 95);                       /* body_left 가 작다 */
    CHECK_INT(proxy_relay_want(&r, 0), 0);                          /* 역압력: 읽지 않는다 */

    begin(0);
    CHECK_INT(feed(H200 "X: y\r\n\r\nabc"), PROXY_RELAY_MORE);      /* UNTIL_EOF */
    CHECK_INT(proxy_relay_want(&r, 500), 500);
    CHECK_INT(proxy_relay_want(&r, PROXY_RELAY_MAX), PROXY_RELAY_MAX);
    CHECK_INT(proxy_relay_want(&r, 0), 0);

    CHECK_INT(whole(H200 "Content-Length: 0\r\n\r\n", 0), PROXY_RELAY_COMPLETE);
    CHECK_INT(proxy_relay_want(&r, 500), 0);                        /* 읽을 것이 없다 */
}

/* 16. Content-Length 0 과 HTTP/1.0 */
static void test_cl_zero_and_http10(void)
{
    CHECK_INT(whole(H200 "Content-Length: 0\r\n\r\n", 0), PROXY_RELAY_COMPLETE);
    check_out(H200 "Content-Length: 0\r\n\r\n");
    CHECK_INT(r.body_mode, PROXY_BODY_NONE);

    CHECK_INT(whole("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nhi", 0), PROXY_RELAY_COMPLETE);
    check_out("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nhi");

    CHECK_INT(whole("HTTP/1.0 200 OK\r\n\r\nhi", 0), PROXY_RELAY_MORE);   /* 1.0 은 보통 EOF 까지 */
    CHECK_INT(proxy_relay_eof(&r), PROXY_RELAY_COMPLETE);
}

/* 17. out 이 가득 차 덧붙이기가 실패하는 경우 (BUFFER_MAX 근처까지 채워서 흉내 낸다) */
static void test_nomem(void)
{
    const char *resp = H200 "Content-Length: 5\r\n\r\nhello";
    char *filler = malloc(BUFFER_MAX);
    size_t room = 10;                                   /* 머리 하나가 들어갈 수 없는 여유 */

    REQUIRE(filler != NULL);
    memset(filler, 'x', BUFFER_MAX);
    begin(0);
    REQUIRE(buffer_append(&out, filler, BUFFER_MAX - room) == 0);
    CHECK_INT(feed(resp), PROXY_RELAY_NOMEM);
    CHECK_INT(r.started, 0);
    CHECK_INT(buffer_pending(&out), BUFFER_MAX - room);         /* out 은 그대로 */
    CHECK_MEM(buffer_peek(&out), buffer_pending(&out), filler, BUFFER_MAX - room);   /* 바이트도 그대로 */
    CHECK_INT(proxy_relay_eof(&r), PROXY_RELAY_BAD);            /* 시작 못 했으면 BAD (서버는 502) */

    /* 자리를 만들고 다시 시도하면 시작한다 */
    buffer_clear(&out);
    CHECK_INT(proxy_relay_input(&r, "", 0, &out), PROXY_RELAY_COMPLETE);
    CHECK_INT(r.started, 1);
    check_out(resp);

    /* 시작 후 본문 덧붙이기 실패: out 과 body_left 는 그대로 */
    begin(0);
    CHECK_INT(feed(H200 "Content-Length: 100\r\n\r\n"), PROXY_RELAY_MORE);
    {
        size_t before = buffer_pending(&out);

        REQUIRE(buffer_append(&out, filler, BUFFER_MAX - before - 3) == 0);
        CHECK_INT(feed("abcdef"), PROXY_RELAY_NOMEM);
        CHECK_INT(buffer_pending(&out), BUFFER_MAX - 3);
        CHECK_MEM(buffer_peek(&out), before, H200 "Content-Length: 100" "\r\n\r\n", before);
        CHECK_MEM(buffer_peek(&out) + before, BUFFER_MAX - 3 - before, filler, BUFFER_MAX - 3 - before);
        CHECK_INT(r.body_left, 100);
    }
    free(filler);
}

int main(void)
{
    RUN(test_whole_response);
    RUN(test_byte_at_a_time);
    RUN(test_every_chunk_size);
    RUN(test_split_inside_blank_line);
    RUN(test_truncated);
    RUN(test_eof_before_head);
    RUN(test_head_request);
    RUN(test_204_304);
    RUN(test_until_eof);
    RUN(test_transfer_encoding);
    RUN(test_extra_bytes_dropped);
    RUN(test_bad_status_lines);
    RUN(test_head_size_limit);
    RUN(test_content_length_values);
    RUN(test_head_syntax);
    RUN(test_want);
    RUN(test_cl_zero_and_http10);
    RUN(test_nomem);
    proxy_relay_free(&r);
    buffer_free(&out);
    return test_report();
}
