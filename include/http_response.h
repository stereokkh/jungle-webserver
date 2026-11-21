/*
 * http_response.h — 상태 라인·헤더·오류 응답 생성
 *
 * CS:APP 대응: tiny.c clienterror(), serve_static() 의 헤더 sprintf 부분
 * 담당: 트랙 C · 이슈 #6 (단위 테스트: tests/unit/test_http_response.c)
 * 현대식으로 바뀌는 점: 고정 배열 + sprintf → buffer 에 덧붙이기 + 길이 검사, HTTP/1.1,
 *                       Connection 헤더, 추가 헤더(X-Instance, Allow) 자리,
 *                       오류 코드 확장 (400 403 404 405 413 414 431 500 501 502 504 505),
 *                       오류 본문에 요청 경로를 넣지 않음 (Tiny clienterror 는 cause 를 그대로 넣어
 *                       반사형 XSS, 이슈 #3 관찰), 오류에도 Content-Length
 *
 * 소켓·파일을 건드리지 않는다. 결과는 입력과 현재 시각(Date 헤더)으로만 정해진다.
 *
 * 만들어지는 모양 (http_append_header, 줄 순서 고정):
 *
 *   HTTP/1.1 404 Not Found\r\n
 *   Date: Sat, 21 Nov 2026 06:00:00 GMT\r\n        ← 항상 (영어 요일·월, 로케일 무관)
 *   Server: jungle-webserver\r\n                   ← 항상 (버전은 넣지 않음)
 *   Content-Type: text/html; charset=utf-8\r\n     ← content_type 이 NULL 이면 생략
 *   Content-Length: 48\r\n                         ← content_length < 0 이면 생략
 *   Connection: close\r\n                          ← keep_alive (길이 생략이면 항상 close)
 *   X-Instance: 8001\r\n                           ← extra_headers 그대로
 *   \r\n
 *
 * 사용 예:
 *
 *   // 정적 파일 200 (static_file.h 의 순서 2)
 *   http_response_head h = { 200, f.content_type, (long)f.size, keep_alive, NULL };
 *   if (http_append_header(&c->out, &h) < 0) → 연결 닫기
 *
 *   // 정적 파일 405
 *   http_append_error(&c->out, 405, keep_alive, is_head, "Allow: GET, HEAD\r\n");
 *
 *   // 앱 오류 (X-Instance 는 앱 응답 전부에)
 *   http_append_error(out, 400, keep_alive, is_head, "X-Instance: 8001\r\n");
 *
 * 테스트에서 응답 전체를 CHECK_MEM 으로 바이트 비교하지 않는다 (Date 가 매초 바뀜).
 * 필요한 줄을 strstr 로 찾는다 (tests/unit/test_http_response.c 의 out_str 참고).
 */
#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include "buffer.h"

/* 200 → "OK", 404 → "Not Found" ... 위 목록 + 200 외에는 "Unknown". 항상 NULL 이 아니다 */
const char *http_reason(int status);

/* 응답 헤더에 들어갈 값. 인자가 많아서 구조체로 받는다 */
typedef struct {
    int status;                  /* 100~599. 밖이면 -1 */
    const char *content_type;    /* NULL 이면 Content-Type 생략. '\r' '\n' 이 있으면 -1 */
    long content_length;         /* < 0 이면 Content-Length 생략. 이때 keep_alive 와 상관없이
                                  * Connection: close (길이를 모르면 클라이언트가 응답 끝을
                                  * 연결 종료로만 알 수 있다) */
    int keep_alive;              /* Connection: keep-alive / close */
    const char *extra_headers;   /* "X-Instance: 8001\r\n" 처럼 CRLF 로 끝나는 줄들. 없으면 NULL 또는 "".
                                  * CRLF 로 끝나지 않거나, 빈 줄("\r\n" 로 시작, "\r\n\r\n" 포함)이나
                                  * 짝 없는 '\r'·'\n' 이 있으면 -1 (응답 분할 방지. 호출 코드의 실수를 잡기 위한 검사.
                                  * 우리 코드가 만든 값만 넣는다. 요청에서 온 값을 넣지 않는다) */
} http_response_head;

/* 상태 라인 + 헤더 + 빈 줄을 out 뒤에 덧붙인다. 성공 0, 실패 -1 (out 은 그대로) */
int http_append_header(buffer *out, const http_response_head *h);

/* 오류 응답 전체(헤더 + 짧은 HTML 본문)를 out 뒤에 덧붙인다.
 * 본문: "<html><body><h1>404 Not Found</h1></body></html>\n" (상태 코드와 문구만, 요청 내용은 넣지 않음)
 * Content-Type 은 "text/html; charset=utf-8", Content-Length 는 본문 길이.
 * head_only 가 1 이면 (HEAD 요청) Content-Length 는 본문 길이 그대로 두고 본문은 쓰지 않는다.
 * extra_headers 는 http_response_head 와 같다 (CRLF 로 끝나는 줄들, 없으면 NULL).
 * 405 의 Allow 는 호출자가 넣는다: 정적 "Allow: GET, HEAD\r\n", 앱 "Allow: POST\r\nX-Instance: 8001\r\n"
 * keep_alive 는 호출자가 정한다. 요청이 깨져 다음 요청의 시작을 알 수 없는 오류
 * (400, 413, 414, 431, 501, 505 등 파서 오류)는 0 을 넘긴다.
 * 성공 0, 실패 -1 (out 은 그대로) */
int http_append_error(buffer *out, int status, int keep_alive, int head_only,
                      const char *extra_headers);

#endif /* HTTP_RESPONSE_H */
