/*
 * http_response.h — 상태 라인·헤더·오류 응답 생성 (순수 함수)
 *
 * CS:APP 대응: tiny.c clienterror(), serve_static() 의 헤더 sprintf 부분
 * 담당: 트랙 C · 이슈 #6
 * 현대식으로 바뀌는 점: sprintf → snprintf + 길이 검사, HTTP/1.1, Connection 헤더,
 *                       오류 코드 확장 (400 403 404 413 414 431 500 501 502 504 505)
 */
#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include <stddef.h>

/* 200 → "OK", 404 → "Not Found" ... 모르면 "Unknown" */
const char *http_reason(int status);

/* 상태 라인 + 헤더(빈 줄 포함)를 out 에 쓴다. content_length < 0 이면 생략.
 * 성공: 쓴 바이트 수, cap 부족: -1 */
int http_build_header(char *out, size_t cap, int status,
                      const char *content_type, long content_length, int keep_alive);

/* 헤더 + 간단한 HTML 오류 본문. 성공: 바이트 수, cap 부족: -1 */
int http_build_error(char *out, size_t cap, int status, int keep_alive);

#endif /* HTTP_RESPONSE_H */
