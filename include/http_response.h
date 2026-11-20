/*
 * http_response.h — 상태 라인·헤더·오류 응답 생성 (순수 함수)
 *
 * CS:APP 대응: tiny.c clienterror(), serve_static() 의 헤더 sprintf 부분
 * 담당: 트랙 C · 이슈 #6
 * 현대식으로 바뀌는 점: 고정 배열 + sprintf → buffer 에 덧붙이기 + 길이 검사, HTTP/1.1,
 *                       Connection 헤더, 추가 헤더(X-Instance, Allow) 자리,
 *                       오류 코드 확장 (400 403 404 405 413 414 431 500 501 502 504 505)
 */
#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include "buffer.h"

/* 200 → "OK", 404 → "Not Found" ... 모르면 "Unknown". 항상 NULL 이 아니다 */
const char *http_reason(int status);

/* 응답 헤더에 들어갈 값. 인자가 많아서 구조체로 받는다 */
typedef struct {
    int status;
    const char *content_type;    /* NULL 이면 Content-Type 생략 */
    long content_length;         /* < 0 이면 Content-Length 생략 */
    int keep_alive;              /* Connection: keep-alive / close */
    const char *extra_headers;   /* "X-Instance: 8001\r\n" 처럼 CRLF 로 끝나는 줄들. 없으면 NULL */
} http_response_head;

/* 상태 라인 + 헤더 + 빈 줄을 out 뒤에 덧붙인다. 성공 0, 실패 -1 (out 은 그대로) */
int http_append_header(buffer *out, const http_response_head *h);

/* 오류 응답 전체(헤더 + 짧은 HTML 본문)를 out 뒤에 덧붙인다.
 * head_only 가 1 이면 (HEAD 요청) Content-Length 는 본문 길이 그대로 두고 본문은 쓰지 않는다.
 * extra_headers 는 http_response_head 와 같다 (CRLF 로 끝나는 줄들, 없으면 NULL).
 * 405 의 Allow 는 호출자가 넣는다: 정적 "Allow: GET, HEAD\r\n", 앱 "Allow: POST\r\nX-Instance: 8001\r\n"
 * 성공 0, 실패 -1 (out 은 그대로) */
int http_append_error(buffer *out, int status, int keep_alive, int head_only,
                      const char *extra_headers);

#endif /* HTTP_RESPONSE_H */
