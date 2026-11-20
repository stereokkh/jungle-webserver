/*
 * test_buffer.c — buffer 모듈 단위 테스트 (이슈 #4)
 */
#include "test.h"

#include "buffer.h"

static void test_empty(void)
{
    buffer b = BUFFER_INIT;
    CHECK_INT(buffer_pending(&b), 0);
    CHECK_STR(buffer_peek(&b), "");
    buffer_consume(&b, 10);          /* 빈 버퍼 consume 도 안전 */
    buffer_compact(&b);
    buffer_free(&b);
    buffer_free(&b);                 /* 두 번 free 해도 안전 */
    CHECK(b.data == NULL);
}

static void test_append_and_consume(void)
{
    buffer b = BUFFER_INIT;

    REQUIRE(buffer_append(&b, "hello ", 6) == 0);
    REQUIRE(buffer_append_str(&b, "world") == 0);
    CHECK_MEM(buffer_peek(&b), buffer_pending(&b), "hello world", 11);

    buffer_consume(&b, 6);           /* 6 바이트 보냈다 */
    CHECK_MEM(buffer_peek(&b), buffer_pending(&b), "world", 5);
    CHECK_INT(b.off, 6);

    buffer_consume(&b, 5);           /* 다 보내면 처음으로 되돌린다 */
    CHECK_INT(buffer_pending(&b), 0);
    CHECK_INT(b.off, 0);
    CHECK_INT(b.len, 0);

    buffer_consume(&b, 100);         /* pending 보다 많이 consume 해도 넘치지 않는다 */
    CHECK_INT(b.off, 0);
    buffer_free(&b);
}

static void test_appendf(void)
{
    buffer b = BUFFER_INIT;

    REQUIRE(buffer_appendf(&b, "HTTP/1.1 %d %s\r\n", 404, "Not Found") == 0);
    REQUIRE(buffer_appendf(&b, "Content-Length: %ld\r\n", 12L) == 0);
    CHECK_MEM(buffer_peek(&b), buffer_pending(&b),
              "HTTP/1.1 404 Not Found\r\nContent-Length: 12\r\n", 44);
    buffer_free(&b);
}

/* 초기 크기(1KB)를 넘겨 여러 번 늘어나도 내용이 보존된다 */
static void test_grow(void)
{
    buffer b = BUFFER_INIT;
    char chunk[100];
    size_t i;
    int ok = 1;

    for (i = 0; i < 1000; i++) {
        memset(chunk, 'a' + (int)(i % 26), sizeof chunk);
        if (buffer_append(&b, chunk, sizeof chunk) < 0) {
            ok = 0;
            break;
        }
    }
    CHECK(ok);
    CHECK_INT(buffer_pending(&b), 100000);
    CHECK(b.cap >= b.len);
    CHECK_INT(b.data[0], 'a');
    CHECK_INT(b.data[99999], 'a' + (999 % 26));
    buffer_free(&b);
}

/* 입력 버퍼처럼 쓰기: 요청 하나 처리 후 남은 바이트(다음 요청)를 앞으로 당긴다 */
static void test_compact_keeps_rest(void)
{
    buffer b = BUFFER_INIT;
    const char *two = "GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n";

    REQUIRE(buffer_append_str(&b, two) == 0);
    buffer_consume(&b, 19);          /* 첫 요청 처리 */
    buffer_compact(&b);
    CHECK_INT(b.off, 0);
    CHECK_MEM(buffer_peek(&b), buffer_pending(&b), "GET /b HTTP/1.1\r\n\r\n", 19);
    buffer_free(&b);
}

/* 상한을 넘는 요청은 거절하고 기존 내용은 그대로 둔다 */
static void test_max_rejected(void)
{
    buffer b = BUFFER_INIT;

    REQUIRE(buffer_append_str(&b, "abc") == 0);
    CHECK_INT(buffer_reserve(&b, BUFFER_MAX), -1);
    CHECK_INT(buffer_reserve(&b, (size_t)-1), -1);   /* 오버플로 */
    CHECK_INT(buffer_append(&b, "x", (size_t)-1), -1);
    CHECK_MEM(buffer_peek(&b), buffer_pending(&b), "abc", 3);
    buffer_free(&b);
}

/* clear 는 메모리를 유지한 채 비운다 (keep-alive 재사용) */
static void test_clear_reuses(void)
{
    buffer b = BUFFER_INIT;
    char *before;

    REQUIRE(buffer_append_str(&b, "first response") == 0);
    before = b.data;
    buffer_clear(&b);
    CHECK_INT(buffer_pending(&b), 0);
    REQUIRE(buffer_append_str(&b, "second") == 0);
    CHECK(b.data == before);
    CHECK_MEM(buffer_peek(&b), buffer_pending(&b), "second", 6);
    buffer_free(&b);
}

int main(void)
{
    RUN(test_empty);
    RUN(test_append_and_consume);
    RUN(test_appendf);
    RUN(test_grow);
    RUN(test_compact_keeps_rest);
    RUN(test_max_rejected);
    RUN(test_clear_reuses);
    return test_report();
}
