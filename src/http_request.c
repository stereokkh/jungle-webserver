/*
 * http_request.c — 증분 HTTP/1.1 요청 파서
 *
 * CS:APP 대응:
 *   doit()            : Rio_readlineb 로 요청 라인 → sscanf("%s %s %s", method, uri, version)
 *   read_requesthdrs(): "\r\n" 빈 줄까지 읽고 버림
 *   parse_uri()       : "cgi-bin" 포함 여부로 정적/동적 판단, '?' 로 인자 분리
 * 여기서는 버퍼 전체를 받아 "\r\n\r\n" 경계를 찾고, 헤더를 해석하고,
 * Content-Length 만큼 본문이 다 왔는지까지 확인한다.
 * 담당: 트랙 B · 이슈 #5 (단위 테스트: tests/unit/)
 */
#include "http_request.h"

#include <string.h>

parse_result http_parse_request(const char *buf, size_t len,
                                http_request *req, size_t *consumed, int *status)
{
    /* TODO(#5):
     *  - "\r\n\r\n" 이 없으면: len > HTTP_MAX_HEADER 이면 431, 아니면 NEED_MORE
     *  - 요청 라인: 메서드(모르면 501), 대상(길면 414), 버전(HTTP/1.0|1.1 아니면 505)
     *  - target → path(디코딩) / query 분리, path 에 NUL·잘못된 %XX 가 있으면 400
     *  - 헤더: 이름 대소문자 무시. Content-Length 음수·오버플로·서로 다른 중복 → 400,
     *          Transfer-Encoding → 501
     *  - 본문: content_length 만큼 안 왔으면 NEED_MORE, 너무 크면 413
     *  - *consumed = 헤더 + 본문 길이 */
    (void)buf;
    (void)len;
    memset(req, 0, sizeof *req);
    *consumed = 0;
    *status = 501;
    return PARSE_ERROR;
}
