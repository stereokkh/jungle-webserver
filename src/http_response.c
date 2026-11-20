/*
 * http_response.c — 상태 라인·헤더·오류 응답 생성
 *
 * CS:APP 대응:
 *   clienterror(): sprintf 로 HTML 본문을 만들고 "HTTP/1.0 %s %s" + Content-type/length 전송
 *   serve_static(): "HTTP/1.0 200 OK", Server, Connection: close, Content-length, Content-type
 *   Tiny의 sprintf(buf, "%s...", buf) 패턴은 정의되지 않은 동작이다 (이슈 #3 관찰).
 *   여기서는 buffer_appendf(vsnprintf 로 길이를 먼저 잼)로 덧붙이고, 실패하면 -1.
 * 담당: 트랙 C · 이슈 #6 (단위 테스트: tests/unit/)
 */
#include "http_response.h"

#include <stdio.h>

const char *http_reason(int status)
{
    /* TODO(#6): switch (status) { case 200: return "OK"; ... } */
    (void)status;
    return "Unknown";
}

int http_append_header(buffer *out, const http_response_head *h)
{
    /* TODO(#6): buffer_appendf 로 "HTTP/1.1 %d %s\r\n", Server, Content-Type, Content-Length,
     *           Connection: keep-alive|close, extra_headers, "\r\n".
     *           중간에 실패하면 out->len 을 시작 시점 값으로 되돌려 "out 은 그대로"를 지킨다 */
    (void)out; (void)h;
    return -1;
}

int http_append_error(buffer *out, int status, int keep_alive, int head_only)
{
    /* TODO(#6): 본문 HTML 을 먼저 만들고 길이 계산 → http_append_header → head_only 가 아니면 본문.
     *           405 면 extra_headers = "Allow: GET, HEAD\r\n" */
    (void)out; (void)status; (void)keep_alive; (void)head_only;
    return -1;
}
