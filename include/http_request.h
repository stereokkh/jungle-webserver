/*
 * http_request.h — 증분 HTTP/1.1 요청 파서 (소켓과 무관한 순수 함수)
 *
 * CS:APP 대응: tiny.c doit() 의 요청 라인 sscanf, read_requesthdrs(), parse_uri() 의 URI 분리
 * 담당: 트랙 B · 이슈 #5
 * 현대식으로 바뀌는 점: Rio 로 한 줄씩 블로킹 읽기 → 버퍼에 쌓인 만큼 파싱하고
 *                       "더 필요 / 완료 / 오류" + 소비 바이트 수를 돌려준다.
 *                       헤더를 버리지 않고 Host, Content-Length, Connection 을 해석한다.
 */
#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <stddef.h>

#define HTTP_MAX_URI      2048   /* 넘으면 414 */
#define HTTP_MAX_HEADER   8192   /* 요청 라인 + 헤더 전체, 넘으면 431 */
#define HTTP_MAX_BODY     (1L << 20)  /* 넘으면 413 */

typedef enum { HTTP_GET, HTTP_POST, HTTP_HEAD, HTTP_METHOD_OTHER } http_method;

typedef enum {
    PARSE_DONE,       /* 요청 하나 완성. *consumed 만큼 버퍼에서 빼면 다음 요청이 남는다 */
    PARSE_NEED_MORE,  /* 더 읽어야 함 */
    PARSE_ERROR       /* *status 에 응답할 상태 코드 (400, 413, 414, 431, 501, 505 ...) */
} parse_result;

/* TODO(#4): 필드 확정. body 는 입력 버퍼 안을 가리키므로 버퍼보다 오래 쓰지 않는다 */
typedef struct {
    http_method method;
    char target[HTTP_MAX_URI + 1];  /* 원본 요청 대상 (프록시 전달용, 디코딩 안 함) */
    char path[HTTP_MAX_URI + 1];    /* 퍼센트 디코딩한 경로 (파일 접근용) */
    char query[HTTP_MAX_URI + 1];   /* '?' 뒤, 디코딩 안 함 */
    int version_minor;              /* HTTP/1.0 → 0, HTTP/1.1 → 1 */
    char host[256];
    long content_length;            /* 헤더 없으면 -1 */
    int keep_alive;                 /* Connection 헤더 + 버전 기본값으로 결정 */
    const char *body;
    size_t body_len;
} http_request;

parse_result http_parse_request(const char *buf, size_t len,
                                http_request *req, size_t *consumed, int *status);

#endif /* HTTP_REQUEST_H */
