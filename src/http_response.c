/*
 * http_response.c — 상태 라인·헤더·오류 응답 생성
 *
 * CS:APP 대응:
 *   clienterror(): sprintf 로 HTML 본문을 만들고 "HTTP/1.0 %s %s" + Content-type/length 전송
 *   serve_static(): "HTTP/1.0 200 OK", Server, Connection: close, Content-length, Content-type
 *   Tiny의 sprintf(buf, "%s...", buf) 패턴은 정의되지 않은 동작이다 (이슈 #3 관찰).
 *   여기서는 snprintf 반환값으로 길이를 누적하고 cap 을 넘으면 -1.
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

int http_build_header(char *out, size_t cap, int status,
                      const char *content_type, long content_length, int keep_alive)
{
    /* TODO(#6): "HTTP/1.1 %d %s\r\n", Date, Server, Content-Type, Content-Length,
     *           Connection: keep-alive|close, "\r\n" */
    (void)out; (void)cap; (void)status;
    (void)content_type; (void)content_length; (void)keep_alive;
    return -1;
}

int http_build_error(char *out, size_t cap, int status, int keep_alive)
{
    /* TODO(#6): 본문 길이를 먼저 계산 → http_build_header → 본문 이어 쓰기 */
    (void)out; (void)cap; (void)status; (void)keep_alive;
    return -1;
}
