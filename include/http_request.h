/*
 * http_request.h — 증분 HTTP/1.1 요청 파서 (소켓과 무관한 순수 함수)
 *
 * CS:APP 대응: tiny.c doit() 의 요청 라인 sscanf, read_requesthdrs(), parse_uri() 의 URI 분리
 * 담당: 트랙 B · 이슈 #5
 * 현대식으로 바뀌는 점: Rio 로 한 줄씩 블로킹 읽기 → 버퍼에 쌓인 만큼 파싱하고
 *                       "더 필요 / 완료 / 오류" + 소비 바이트 수를 돌려준다.
 *                       헤더를 버리지 않고 Host, Content-Length, Connection 을 해석하고,
 *                       원본 헤더 블록은 프록시가 그대로 전달할 수 있게 위치를 남긴다.
 */
#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <stddef.h>

#define HTTP_MAX_URI      2048        /* 요청 대상이 더 길면 414 */
#define HTTP_MAX_HEADER   8192        /* 요청 라인 + 헤더 + 빈 줄(\r\n\r\n)까지, 넘으면 431 */
#define HTTP_MAX_BODY     (1L << 20)  /* Content-Length 가 더 크면 413 */
#define HTTP_MAX_HOST     255

typedef enum { HTTP_GET, HTTP_POST, HTTP_HEAD, HTTP_METHOD_OTHER } http_method;

typedef enum {
    PARSE_DONE,       /* 요청 하나 완성. *consumed = 요청 라인 + 헤더 + 본문 길이 */
    PARSE_NEED_MORE,  /* 아직 덜 왔다. 더 읽은 뒤 같은 버퍼 처음부터 다시 부른다 */
    PARSE_ERROR       /* *status 에 응답할 상태 코드 (400, 413, 414, 431, 501, 505) */
} parse_result;

/*
 * 파싱 결과. 크기가 수 KB 이므로 conn 안(힙)에 두고 스택에 만들지 않는다.
 *
 * 포인터 수명: headers, body 는 http_parse_request 에 넘긴 buf 안을 가리킨다.
 *   호출자(server.c)는 이 요청의 응답 생성·프록시 요청 생성이 끝난 뒤에
 *   buffer_consume(in, consumed) 한다. 그 전에 입력 버퍼에 append / consume / compact 하지 않는다.
 *   나머지 필드는 값 복사라 언제든 써도 된다.
 */
typedef struct {
    http_method method;
    char method_str[16];            /* 원본 메서드 문자열 (501 로그·프록시 전달용) */
    char target[HTTP_MAX_URI + 1];  /* 원본 요청 대상 "/a%20b?x=1" (프록시 전달용, 디코딩 안 함) */
    char path[HTTP_MAX_URI + 1];    /* 퍼센트 디코딩한 경로 "/a b" (정적 파일 접근용).
                                     * 파서가 NUL·제어 문자(0x01-0x1F, 0x7F)·잘못된 %XX 를
                                     * 400 으로 거절하므로 path 에는 이런 바이트가 없다 */
    char query[HTTP_MAX_URI + 1];   /* '?' 뒤 "x=1", 디코딩 안 함. 없으면 "" */
    int version_minor;              /* HTTP/1.0 → 0, HTTP/1.1 → 1 */

    char host[HTTP_MAX_HOST + 1];   /* Host 헤더 값. 없으면 "" (HTTP/1.1 이면 400) */
    long content_length;            /* Content-Length. 헤더 없으면 -1 */
    int keep_alive;                 /* 1.1 기본 1, 1.0 기본 0, Connection: close / keep-alive 로 덮어씀 */

    /* 요청 라인 다음 줄부터 빈 줄 직전까지의 원본 헤더 줄들 (각 줄은 CRLF 로 끝남).
     * 프록시가 hop-by-hop 헤더만 빼고 그대로 전달한다 (#9) */
    const char *headers;
    size_t headers_len;

    const char *body;               /* 본문 시작. 본문 없으면 NULL */
    size_t body_len;                /* content_length 와 같음 (없으면 0) */
} http_request;

/*
 * buf[0..len) 에서 요청 하나를 파싱한다.
 *   - buf 는 NUL 로 끝나지 않는다. len 을 넘어 읽지 않는다 (make asan 이 검사)
 *   - 상태를 저장하지 않는다. NEED_MORE 후에는 더 읽고 처음부터 다시 부른다
 *     (헤더 상한이 8KB 라 다시 파싱해도 비용이 작다)
 *   - DONE 이면 buf + *consumed 부터가 다음 요청이다 (keep-alive 파이프라이닝 잔여)
 *   - ERROR 면 *status 만 의미가 있고 *req 는 일부만 채워졌을 수 있다
 *     (단, 응답의 Connection 결정을 위해 version_minor 는 가능하면 채운다)
 *   - Transfer-Encoding 이 있으면 501 (chunked 는 범위 밖)
 *   - Content-Length 가 두 줄 이상이면 값이 같아도 400 (프록시가 원본 헤더를 그대로 넘기므로)
 *   - 요청 라인 앞의 빈 줄(CRLF)은 건너뛴다 (RFC 9112 2.2). 건너뛴 바이트도 아래 8KB 와 consumed 에 들어간다
 *   - 431 을 다른 오류보다 먼저 판정한다. TCP 가 어떻게 나눠 보내든 같은 요청은 같은 코드를 받아야 한다:
 *       buf 앞 HTTP_MAX_HEADER 바이트 안에서 "\r\n\r\n" 을 찾는다
 *       → 있으면 계속, 없고 len >= HTTP_MAX_HEADER 면 431, 없고 더 짧으면 NEED_MORE
 *     그 뒤에 요청 라인(501, 414, 505), 경로(400), 헤더(400, 501), 본문(413, NEED_MORE) 순서
 *     (예: 대상 9000 바이트 GET 은 한 번에 오든 나눠 오든 항상 431)
 */
parse_result http_parse_request(const char *buf, size_t len,
                                http_request *req, size_t *consumed, int *status);

#endif /* HTTP_REQUEST_H */
