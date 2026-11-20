/*
 * app.h — 내장 동적 앱 라우트 (GET /api/add, POST /api/echo)
 *
 * CS:APP 대응: tiny.c serve_dynamic() + cgi-bin/adder.c
 *   (CGI는 하지 않기로 했다. 같은 계산을 서버 안의 함수로 처리 — CLAUDE.md 설계 결정)
 * 담당: 트랙 C · 이슈 #10
 */
#ifndef APP_H
#define APP_H

#include <stddef.h>
#include "http_request.h"

/* req 가 앱 경로면 응답 전체(헤더+본문)를 out 에 쓰고 1, 앱 경로가 아니면 0, cap 부족 -1.
 * instance_port 는 응답 헤더에 넣어 Round-Robin 분산을 확인하는 데 쓴다 */
int app_handle(const http_request *req, const char *instance_port,
               char *out, size_t cap, size_t *out_len);

#endif /* APP_H */
