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

#include <string.h>

int app_handle(const http_request *req, const char *instance_port,
               char *out, size_t cap, size_t *out_len)
{
    /* TODO(#10):
     *  - GET  /api/add?a=1&b=2 : 누락·형식 오류·범위 초과 → 400
     *  - POST /api/echo        : req->body 를 그대로 돌려줌 (Content-Length 기준)
     *  - 응답 헤더에 X-Instance: <instance_port>
     *  - 그 외 경로는 return 0 */
    (void)req; (void)instance_port; (void)out; (void)cap;
    *out_len = 0;
    return 0;
}
