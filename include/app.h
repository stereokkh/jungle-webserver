/*
 * app.h — 내장 동적 앱 라우트 (GET /api/add, POST /api/echo)
 *
 * CS:APP 대응: tiny.c serve_dynamic() + cgi-bin/adder.c
 *   (CGI는 하지 않기로 했다. 같은 계산을 서버 안의 함수로 처리 — CLAUDE.md 설계 결정)
 * 담당: 트랙 C · 이슈 #10
 */
#ifndef APP_H
#define APP_H

#include "buffer.h"
#include "http_request.h"

#define APP_PREFIX "/api/"

/* req->path 가 APP_PREFIX 로 시작하면 1, 아니면 0. handler_route 가 부른다 */
int app_match(const http_request *req);

/*
 * 앱 경로 요청의 응답 전체(헤더 + 본문)를 out 뒤에 덧붙인다. app_match 가 1 인 요청에만 부른다.
 *   - GET  /api/add?a=1&b=2 : 200 + 합. 누락·형식 오류·범위 초과 → 400
 *   - POST /api/echo        : 200 + req->body 그대로
 *   - 그 외 /api/ 경로 404, 메서드가 맞지 않으면 405
 *   - 모든 응답 헤더에 "X-Instance: <instance_port>" (Round-Robin 분산 확인용)
 *   - HEAD 면 본문 없이 헤더만
 * 4xx 도 응답을 쓴 것이므로 0. 메모리 부족 등으로 쓰지 못했을 때만 -1 (out 은 그대로)
 */
int app_handle(const http_request *req, const char *instance_port, buffer *out);

#endif /* APP_H */
