/*
 * http_request.h — 증분 HTTP/1.1 요청 파서 (소켓과 무관한 순수 함수)
 *
 * CS:APP 대응: tiny.c doit() 의 요청 라인 sscanf, read_requesthdrs(), parse_uri() 의 URI 분리
 * 담당: 트랙 B · 이슈 #5
 * 현대식으로 바뀌는 점: Rio 로 한 줄씩 블로킹 읽기 → 버퍼에 쌓인 만큼 파싱하고
 *                       "더 필요 / 완료 / 오류" + 소비 바이트 수를 돌려준다.
 *                       헤더를 버리지 않고 Host, Content-Length, Connection 을 해석하고,
 *                       원본 헤더 블록은 프록시가 그대로 전달할 수 있게 위치를 남긴다.
 *                       경로는 정규형(target)과 끝까지 디코딩한 형태(path)를 만든다 (#31).
 */
#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H

#include <stddef.h>

#define HTTP_MAX_URI      2048        /* 요청 대상이 더 길면 414 */
#define HTTP_MAX_HEADER   8192        /* 요청 라인 + 헤더 + 빈 줄(\r\n\r\n)까지, 넘으면 431 */
#define HTTP_MAX_BODY     (1L << 20)  /* Content-Length 가 더 크면 413 */
#define HTTP_MAX_HOST     255

typedef enum { HTTP_GET, HTTP_POST, HTTP_HEAD, HTTP_METHOD_OTHER } http_method;

/*
 * 파서 옵션 (#31, docs/decisions.md 2026-11-23 결정 4). 서버가 시작할 때 한 번 채우고 요청마다 같은 것을 넘긴다.
 * 옵션이 늘면 인자를 추가하지 않고 이 구조체에 필드를 더한다 (예정: #35 백슬래시 처리 방식).
 * 모든 필드가 0 인 구조체가 기본값이고, http_parse_request 에 NULL 을 넘겨도 기본값이다
 */
typedef struct {
    int merge_slashes;   /* 0: 경로에 빈 조각 "//" 가 있으면 400 (기본). 1: "/" 하나로 합친다 (--merge-slashes) */
} http_parse_options;

typedef enum {
    PARSE_DONE,       /* 요청 하나 완성. *consumed = 요청 라인 + 헤더 + 본문 길이 */
    PARSE_NEED_MORE,  /* 아직 덜 왔다. 더 읽은 뒤 같은 버퍼 처음부터 다시 부른다.
                       * *consumed = 헤더가 끝났으면 요청 전체 길이(헤더 + 본문), 아니면 0.
                       * 소비한 길이가 아니므로 consume 하지 않는다 */
    PARSE_ERROR       /* *status 에 응답할 상태 코드 (400, 413, 414, 431, 501, 505) */
} parse_result;

/*
 * 파싱 결과. 크기가 수 KB 이므로 conn 안(힙)에 두고 스택에 만들지 않는다.
 *
 * 포인터 수명: headers, body 는 http_parse_request 에 넘긴 buf 안을 가리킨다.
 *   호출자(server.c)는 이 요청의 응답 생성이 끝난 뒤에 buffer_consume(in, consumed) 한다.
 *   그 전에 입력 버퍼에 append / consume / compact 하지 않는다.
 *   예외는 프록시 요청이다: 페일오버(2026-11-25)가 요청을 다른 백엔드용으로 다시 만들 때마다 proxy_build_request 가
 *   headers, body 를 다시 읽으므로, 프록시로 넘긴 요청의 in 은 업스트림 시도가 끝날 때까지(연결이 닫힐 때까지) consume 하지 않는다.
 *   그동안 그 연결은 클라이언트 입력을 읽지 않으니(CONN_UP_*) in 이 바뀔 일이 없다.
 *   나머지 필드는 값 복사라 언제든 써도 된다.
 */
typedef struct {
    http_method method;
    char method_str[16];            /* 원본 메서드 문자열 (501 로그·프록시 전달용) */
    /* 정규화된 요청 대상 = 정규형 경로 + 원본의 '?' 부분 그대로. 원본 문자열은 남기지 않는다 (#31).
     *   예: "/%61pi/a%eb?x=%41" → "/api/a%EB?x=%41"
     * 라우팅(proxy_match, app_match, app_handle)과 백엔드 전달(proxy_build_request)이 모두 이것을 쓴다.
     * 판정한 문자열과 백엔드가 받는 문자열이 하나뿐이라 서로 다를 수 없다 (docs/decisions.md 2026-11-23).
     * 정규형 경로에는 다음이 없다 (원본에 있으면 파서가 400):
     *   인코딩 안 된 '\', 디코딩하면 '/' '\' NUL·제어 문자가 되는 %XX, 빈 조각 "//"
     *   (merge_slashes 면 합쳐서 없앰), "." / ".." 조각.
     * 비예약 문자(A-Z a-z 0-9 - . _ ~)의 %XX 는 글자로 디코딩되어 있고, 남은 %XX 는 16진수 대문자다.
     * 디코딩은 한 번만 한다 ("%252F" 는 정규형에서도 "%252F") */
    char target[HTTP_MAX_URI + 1];
    char path[HTTP_MAX_URI + 1];    /* target 의 경로 부분을 끝까지 퍼센트 디코딩한 것 "/a b" (정적 파일 접근용).
                                     * 위 규칙 때문에 NUL·제어 문자·'\'·점 조각·빈 조각이 없고,
                                     * '/' 는 원래의 경로 구분자뿐이다 */
    char query[HTTP_MAX_URI + 1];   /* '?' 뒤 "x=1", 원본 그대로 (디코딩·정규화 안 함). 없으면 "" */
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
 * buf[0..len) 에서 요청 하나를 파싱한다. opt 가 NULL 이면 기본값 (http_parse_options 주석).
 *   - buf 는 NUL 로 끝나지 않는다. len 을 넘어 읽지 않는다 (make asan 이 검사)
 *   - 상태를 저장하지 않는다. NEED_MORE 면 *consumed 를 보고 언제 다시 부를지 정한다:
 *       0  → 헤더가 아직 덜 옴. 더 읽은 뒤 같은 버퍼 처음부터 다시 부른다
 *            (헤더 상한이 8KB 라 다시 파싱해도 비용이 작다)
 *       >0 → 헤더는 끝나고 본문만 덜 옴. 값 = 요청 전체 길이 (건너뛴 앞 빈 줄 + 헤더 + 본문,
 *            DONE 이 될 때의 *consumed 와 같은 값). buf 로 넘기는 미소비 바이트
 *            (buffer_pending) 가 이 값 이상이 될 때까지 다시 부르지 않는다 (PR #24 리뷰).
 *            본문이 1 바이트씩 오면 바이트마다 헤더 전체를 다시 파싱하게 되고, 바이트마다
 *            유휴 타이머도 리셋돼 타임아웃으로도 끊기지 않기 때문이다.
 *            소비한 길이가 아니므로 consume 하지 않는다. 413 을 먼저 판정하므로
 *            값은 HTTP_MAX_HEADER + HTTP_MAX_BODY 이하다
 *   - DONE 이면 buf + *consumed 부터가 다음 요청이다 (keep-alive 파이프라이닝 잔여)
 *   - ERROR 면 *status 만 의미가 있고 *req 는 일부만 채워졌을 수 있다
 *     (단, 응답의 Connection 결정을 위해 version_minor 는 가능하면 채운다.
 *      method 도 HEAD 오류 응답의 본문 생략을 위해 약속한다 (PR #29): 요청 라인이 앞 8KB 안에 있고
 *      공백 둘 + 올바른 메서드 토큰까지 읽혔으면 채운다 (HEAD → HTTP_HEAD). 그 전에 실패하면
 *      (공백 부족, 빈 메서드, 토큰 아닌 글자, 첫 줄이 8KB 를 넘는 431) 초기값 HTTP_GET)
 *   - Transfer-Encoding 이 있으면 501 (chunked 는 범위 밖)
 *   - Content-Length 가 두 줄 이상이면 값이 같아도 400 (프록시가 원본 헤더를 그대로 넘기므로)
 *   - 요청 라인 앞의 빈 줄(CRLF)은 건너뛴다 (RFC 9112 2.2). 건너뛴 바이트도 아래 8KB 와 consumed 에 들어간다
 *   - 431 을 다른 오류보다 먼저 판정한다. TCP 가 어떻게 나눠 보내든 같은 요청은 같은 코드를 받아야 한다:
 *       buf 앞 HTTP_MAX_HEADER 바이트 안에서 "\r\n\r\n" 을 찾는다
 *       → 있으면 계속, 없고 len >= HTTP_MAX_HEADER 면 431, 없고 더 짧으면 NEED_MORE
 *     그 뒤에 요청 라인(501, 414, 505), 경로(400), 헤더(400, 501), 본문(413, NEED_MORE) 순서
 *     (예: 대상 9000 바이트 GET 은 한 번에 오든 나눠 오든 항상 431)
 */
parse_result http_parse_request(const char *buf, size_t len, const http_parse_options *opt,
                                http_request *req, size_t *consumed, int *status);

#endif /* HTTP_REQUEST_H */
