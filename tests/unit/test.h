/*
 * test.h — 최소 단위 테스트 매크로 (외부 프레임워크 없이)
 *
 * 사용법: tests/unit/test_<모듈>.c 를 만들면 make unit 이 자동으로 빌드·실행한다.
 *   각 테스트 파일은 main.c 를 뺀 src/ 의 모든 .c 와 링크된다.
 *
 *   #include "test.h"
 *   #include "mime.h"
 *
 *   static void test_html(void)
 *   {
 *       CHECK_STR(mime_type("a.html"), "text/html; charset=utf-8");
 *   }
 *
 *   int main(void)
 *   {
 *       RUN(test_html);
 *       return test_report();
 *   }
 *
 * CHECK 는 실패해도 계속 진행한다 (한 번에 여러 실패를 본다).
 * 다음 줄이 앞의 결과에 의존하면 REQUIRE 를 쓴다 (실패 시 그 테스트 함수만 중단).
 */
#ifndef UNIT_TEST_H
#define UNIT_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_checks;
static int t_failures;
static const char *t_current = "";

#define T_FAIL_HEAD() \
    fprintf(stderr, "  FAIL %s:%d (%s): ", __FILE__, __LINE__, t_current)

#define CHECK(cond)                                   \
    do {                                              \
        t_checks++;                                   \
        if (!(cond)) {                                \
            t_failures++;                             \
            T_FAIL_HEAD();                            \
            fprintf(stderr, "%s\n", #cond);           \
        }                                             \
    } while (0)

/* 정수 비교. 실패하면 두 값을 출력한다 */
#define CHECK_INT(actual, expected)                                       \
    do {                                                                  \
        long long t_a_ = (long long)(actual);                             \
        long long t_e_ = (long long)(expected);                           \
        t_checks++;                                                       \
        if (t_a_ != t_e_) {                                               \
            t_failures++;                                                 \
            T_FAIL_HEAD();                                                \
            fprintf(stderr, "%s == %lld, expected %lld\n",                \
                    #actual, t_a_, t_e_);                                 \
        }                                                                 \
    } while (0)

/* NUL 종료 문자열 비교. NULL 도 안전하게 처리한다 */
#define CHECK_STR(actual, expected)                                       \
    do {                                                                  \
        const char *t_a_ = (actual);                                      \
        const char *t_e_ = (expected);                                    \
        t_checks++;                                                       \
        if (t_a_ == NULL || t_e_ == NULL ? t_a_ != t_e_                   \
                                         : strcmp(t_a_, t_e_) != 0) {     \
            t_failures++;                                                 \
            T_FAIL_HEAD();                                                \
            fprintf(stderr, "%s == \"%s\", expected \"%s\"\n", #actual,   \
                    t_a_ ? t_a_ : "(null)", t_e_ ? t_e_ : "(null)");      \
        }                                                                 \
    } while (0)

/* 길이가 있는 바이트열 비교 (응답 버퍼처럼 NUL 종료가 아닌 것) */
#define CHECK_MEM(actual, actual_len, expected, expected_len)             \
    do {                                                                  \
        size_t t_al_ = (size_t)(actual_len);                              \
        size_t t_el_ = (size_t)(expected_len);                            \
        t_checks++;                                                       \
        if (t_al_ != t_el_ || memcmp((actual), (expected), t_el_) != 0) { \
            t_failures++;                                                 \
            T_FAIL_HEAD();                                                \
            fprintf(stderr, "%s (%zu bytes) != expected (%zu bytes)\n",   \
                    #actual, t_al_, t_el_);                               \
        }                                                                 \
    } while (0)

/* 실패하면 현재 테스트 함수를 끝낸다 (void 함수 안에서만) */
#define REQUIRE(cond)                                 \
    do {                                              \
        int t_before_ = t_failures;                   \
        CHECK(cond);                                  \
        if (t_failures != t_before_)                  \
            return;                                   \
    } while (0)

#define RUN(fn)                                       \
    do {                                              \
        t_current = #fn;                              \
        fn();                                         \
    } while (0)

/* NUL 종료가 없는 정확한 크기의 힙 복사본.
 * 파서에 넘기면 len 을 넘어 읽는 버그를 ASan(make asan)이 잡는다. 호출자가 free */
static inline char *test_bytes(const char *s, size_t *len)
{
    size_t n = strlen(s);
    char *p = malloc(n ? n : 1);
    if (p == NULL) {
        perror("malloc");
        exit(2);
    }
    memcpy(p, s, n);
    *len = n;
    return p;
}

static inline int test_report_(const char *file)
{
    if (t_failures == 0)
        printf("  ok   %s (%d checks)\n", file, t_checks);
    else
        fprintf(stderr, "  FAIL %s: %d of %d checks failed\n",
                file, t_failures, t_checks);
    return t_failures == 0 ? 0 : 1;
}

/* main 의 마지막에 return test_report(); (테스트 파일 이름을 출력한다) */
#define test_report() test_report_(__FILE__)

#endif /* UNIT_TEST_H */
