/*
 * app.c — 내장 동적 앱 라우트
 *
 * CS:APP 대응:
 *   serve_dynamic(): Fork → setenv("QUERY_STRING") → Dup2(fd, STDOUT) → Execve(adder)
 *   adder.c        : QUERY_STRING "1&2" 를 나눠 atoi 로 더하고 HTML 출력
 *   여기서는 fork/exec 없이 함수 호출로 같은 일을 한다.
 * Tiny 와 다른 점:
 *   - adder 는 atoi 라 "abc" 도 0, 범위를 넘는 수도 아무 값으로 계산한다 → 400
 *   - a+b 가 넘치는지 더하기 전에 검사한다 (signed 오버플로는 정의되지 않은 동작)
 *   - 헤더는 adder 가 printf 로 직접 쓰지 않고 http_append_header 로 만든다
 * 담당: 트랙 C · 이슈 #10
 *
 * 라우트는 원본 target 의 '?' 앞 경로와 바이트 그대로 비교한다 (app_match 와 같은 기준).
 * "/api/%61dd" 처럼 인코딩한 경로는 404.
 *   경로        메서드       응답
 *   /api/add    GET, HEAD    200 "<a+b>\n" text/plain. 그 외 메서드 405 "Allow: GET, HEAD"
 *   /api/echo   POST         200 본문 그대로 application/octet-stream. 그 외 405 "Allow: POST"
 *   그 외 /api/ 404
 * /api/add 의 질의 (디코딩하지 않은 원본 그대로 읽는다):
 *   - a, b 가 정확히 한 번씩. 순서는 상관없고, 다른 이름은 무시한다
 *   - 값은 10진 정수 [-]숫자. 앞뒤 공백, '+', 퍼센트 인코딩, 빈 값은 400
 *   - long long 범위를 넘거나 a+b 가 넘치면 400
 */
#include "app.h"
#include "http_response.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH_ADD  "/api/add"
#define PATH_ECHO "/api/echo"

int app_match(const http_request *req)
{
    return strncmp(req->target, APP_PREFIX, sizeof APP_PREFIX - 1) == 0;
}

/* target 의 '?' 앞이 path 와 정확히 같은가 */
static int path_is(const char *target, const char *path)
{
    size_t n = strcspn(target, "?");

    return n == strlen(path) && memcmp(target, path, n) == 0;
}

/* s[0..n) 이 [-]숫자 모양의 long long 이면 *out 에 넣고 0, 아니면 -1 */
static int parse_int(const char *s, size_t n, long long *out)
{
    char tmp[32];
    char *end;
    size_t i = (n > 0 && s[0] == '-') ? 1 : 0;

    if (n == i || n >= sizeof tmp)          /* 빈 값, "-", 너무 긴 값 */
        return -1;
    for (; i < n; i++)                      /* strtoll 은 앞 공백·'+' 를 받아 주므로 직접 막는다 */
        if (s[i] < '0' || s[i] > '9')
            return -1;
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    errno = 0;
    *out = strtoll(tmp, &end, 10);
    if (errno == ERANGE || *end != '\0')
        return -1;
    return 0;
}

/* "a=1&b=2" 에서 a, b 를 읽는다. 성공 0, 누락·중복·형식 오류 -1 */
static int parse_add_query(const char *q, long long *a, long long *b)
{
    int have_a = 0, have_b = 0;

    while (*q != '\0') {
        size_t len = strcspn(q, "&");

        if (len >= 2 && q[1] == '=' && (q[0] == 'a' || q[0] == 'b')) {
            int *have = (q[0] == 'a') ? &have_a : &have_b;
            long long *v = (q[0] == 'a') ? a : b;

            if (*have || parse_int(q + 2, len - 2, v) < 0)
                return -1;
            *have = 1;
        }
        q += len;
        if (*q == '&')
            q++;
    }
    return (have_a && have_b) ? 0 : -1;
}

/* 200 응답 하나 (헤더 + 본문). HEAD 면 Content-Length 는 그대로 두고 본문은 쓰지 않는다 */
static int append_ok(buffer *out, const http_request *req, const char *type,
                     const char *body, size_t body_len, const char *extra)
{
    size_t mark = buffer_mark(out);
    http_response_head h;

    h.status = 200;
    h.content_type = type;
    h.content_length = (long)body_len;
    h.keep_alive = req->keep_alive;
    h.extra_headers = extra;
    if (http_append_header(out, &h) < 0)
        return -1;
    if (req->method != HTTP_HEAD && body_len > 0 && buffer_append(out, body, body_len) < 0) {
        buffer_rollback(out, mark);
        return -1;
    }
    return 0;
}

int app_handle(const http_request *req, const char *instance_port, buffer *out)
{
    char inst[96], allow[128], sum[32];
    int head = req->method == HTTP_HEAD;
    long long a, b;
    int n;

    /* 오류를 포함한 모든 응답에 붙인다 (Round-Robin 으로 어느 인스턴스가 답했는지 보는 용도) */
    n = snprintf(inst, sizeof inst, "X-Instance: %s\r\n", instance_port != NULL ? instance_port : "");
    if (n < 0 || (size_t)n >= sizeof inst)
        return -1;

    if (path_is(req->target, PATH_ADD)) {
        if (req->method != HTTP_GET && req->method != HTTP_HEAD) {
            snprintf(allow, sizeof allow, "Allow: GET, HEAD\r\n%s", inst);
            return http_append_error(out, 405, req->keep_alive, head, allow);
        }
        if (parse_add_query(req->query, &a, &b) < 0
            || (b > 0 && a > LLONG_MAX - b) || (b < 0 && a < LLONG_MIN - b))
            return http_append_error(out, 400, req->keep_alive, head, inst);
        n = snprintf(sum, sizeof sum, "%lld\n", a + b);
        return append_ok(out, req, "text/plain; charset=utf-8", sum, (size_t)n, inst);
    }

    if (path_is(req->target, PATH_ECHO)) {
        if (req->method != HTTP_POST) {
            snprintf(allow, sizeof allow, "Allow: POST\r\n%s", inst);
            return http_append_error(out, 405, req->keep_alive, head, allow);
        }
        /* 본문은 NUL 로 끝나지 않고 0 바이트가 섞일 수 있다. 길이로만 다룬다 */
        return append_ok(out, req, "application/octet-stream", req->body, req->body_len, inst);
    }

    return http_append_error(out, 404, req->keep_alive, head, inst);
}
