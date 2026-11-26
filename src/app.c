/*
 * app.c — 내장 앱 라우트
 *
 * CS:APP 대응:
 *   serve_dynamic(): Fork → setenv("QUERY_STRING") → Dup2(fd, STDOUT) → Execve(adder)
 *   adder.c        : QUERY_STRING "1&2" 를 나눠 atoi 로 더하고 HTML 출력
 *   여기서는 fork/exec 없이 함수 호출로 동적 응답을 만든다.
 * Tiny 와 다른 점:
 *   - 헤더는 adder 가 printf 로 직접 쓰지 않고 http_append_header 로 만든다
 *   - adder 같은 덧셈(GET /api/add)을 두었다가 2026-11-25 에 헬스체크(GET /healthz)로 바꿨다.
 *     덧셈은 범용 웹서버가 할 일이 아니고, 운영에서는 "이 인스턴스가 살아 있나"를 묻는 주소가 쓸모 있다
 *     (Docker HEALTHCHECK, 쿠버네티스 probe, 가동 감시). 결정: docs/decisions.md 2026-11-25 헬스체크
 * 담당: 트랙 C · 이슈 #10
 *
 * 라우트는 파서가 만든 정규형 target 의 '?' 앞 경로와 바이트 그대로 비교한다 (app_match 와 같은 기준, #31).
 *   경로        메서드       응답
 *   /healthz    GET, HEAD    200 "ok\n" text/plain. 그 외 메서드 405 "Allow: GET, HEAD"
 *                            (handler_route 가 프록시보다 먼저 보낸다: 프런트든 백엔드든 자기 자신이 답한다)
 *   /api/echo   POST         200 본문 그대로 application/octet-stream. 그 외 405 "Allow: POST"
 *   그 외 /api/ 404
 */
#include "app.h"
#include "http_response.h"

#include <stdio.h>
#include <string.h>

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

int app_health_match(const http_request *req)
{
    return path_is(req->target, APP_HEALTH_PATH);
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

/* "X-Instance: <port>\r\n" 을 inst 에 만든다. 너무 길면 -1 */
static int make_instance(char *inst, size_t size, const char *instance_port)
{
    int n = snprintf(inst, size, "X-Instance: %s\r\n", instance_port != NULL ? instance_port : "");

    return (n < 0 || (size_t)n >= size) ? -1 : 0;
}

int app_health(const http_request *req, const char *instance_port, buffer *out)
{
    char inst[96], allow[128];

    if (make_instance(inst, sizeof inst, instance_port) < 0)
        return -1;
    if (req->method != HTTP_GET && req->method != HTTP_HEAD) {
        snprintf(allow, sizeof allow, "Allow: GET, HEAD\r\n%s", inst);
        return http_append_error(out, 405, req->keep_alive, 0, allow);
    }
    return append_ok(out, req, "text/plain; charset=utf-8", "ok\n", 3, inst);
}

int app_handle(const http_request *req, const char *instance_port, buffer *out)
{
    char inst[96], allow[128];
    int head = req->method == HTTP_HEAD;

    /* 오류를 포함한 모든 응답에 붙인다 (Round-Robin 으로 어느 인스턴스가 답했는지 보는 용도) */
    if (make_instance(inst, sizeof inst, instance_port) < 0)
        return -1;

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
