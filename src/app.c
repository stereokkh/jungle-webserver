/*
 * app.c — 내장 동적 앱 라우트
 *
 * CS:APP 대응:
 *   serve_dynamic(): Fork → setenv("QUERY_STRING") → Dup2(fd, STDOUT) → Execve(adder)
 *   adder.c        : QUERY_STRING "1&2" 를 나눠 더하고 HTML 출력
 *   여기서는 fork/exec 없이 함수 호출로 같은 일을 한다.
 * 담당: 트랙 C · 이슈 #10
 */
#include "app.h"
#include "http_response.h"

#include <string.h>

int app_match(const http_request *req)
{
    return strncmp(req->target, APP_PREFIX, sizeof APP_PREFIX - 1) == 0;
}

int app_handle(const http_request *req, const char *instance_port, buffer *out)
{
    /* TODO(#10):
     *  - GET  /api/add?a=1&b=2 : 누락·형식 오류·범위 초과 → 400 (strtol + errno, 오버플로 검사)
     *  - POST /api/echo        : req->body 를 그대로 돌려줌 (body_len 기준, NUL 종료 아님)
     *  - 그 외 /api/ 경로 404, 메서드 불일치 405, HEAD 면 본문 없이
     *  - 응답 헤더에 X-Instance: <instance_port> (http_response_head.extra_headers) */
    (void)req; (void)instance_port; (void)out;
    return -1;
}
