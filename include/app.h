/*
 * app.h — 내장 앱 라우트 (GET /healthz, POST /api/echo)
 *
 * CS:APP 대응: tiny.c serve_dynamic() + cgi-bin/adder.c
 *   (CGI는 하지 않기로 했다. 동적 응답을 서버 안의 함수로 만든다 — CLAUDE.md 설계 결정)
 *   처음에는 adder 와 같은 GET /api/add 를 두었다가 2026-11-25 에 헬스체크 /healthz 로 바꿨다
 *   (덧셈은 범용 웹서버의 기능이 아니라 시연용이었다. docs/decisions.md 2026-11-25 헬스체크)
 * 담당: 트랙 C · 이슈 #10
 */
#ifndef APP_H
#define APP_H

#include "buffer.h"
#include "http_request.h"

#define APP_PREFIX "/api/"
#define APP_HEALTH_PATH "/healthz"

/* req->target(파서가 만든 정규형)이 APP_PREFIX 로 시작하면 1, 아니면 0. handler_route 가 부른다.
 * proxy_match 와 같은 문자열을 본다 (PR #18 리뷰: 라우팅 판정은 한 표현으로).
 * 표기만 바꾼 "/%61pi/echo" 는 파서가 "/api/echo" 로 바꿔 오므로 앱(또는 /api 프록시 라우트)으로 간다 (#31) */
int app_match(const http_request *req);

/*
 * req->target 의 '?' 앞이 정확히 APP_HEALTH_PATH 이면 1, 아니면 0. handler_route 가 프록시보다 먼저 부른다.
 * 접두사가 아니라 정확 일치라서 "/healthz/x", "/healthzz" 는 0 (다른 경로를 가로채지 않는다).
 * 질의는 보지 않는다 ("/healthz?probe=1" 도 1)
 */
int app_health_match(const http_request *req);

/*
 * 헬스체크 응답 전체를 out 뒤에 덧붙인다. app_health_match 가 1 인 요청에만 부른다.
 *   - GET, HEAD : 200 "ok\n" text/plain. 이 응답을 쓴 인스턴스가 살아 있다는 뜻 (백엔드 상태는 보지 않는다)
 *   - 그 외     : 405 + "Allow: GET, HEAD"
 *   - 모든 응답에 "X-Instance: <instance_port>" (어느 인스턴스가 답했는지)
 *   - HEAD 면 본문 없이 헤더만
 * 반환은 app_handle 과 같다 (405 도 0, 쓰지 못했을 때만 -1, out 은 그대로)
 */
int app_health(const http_request *req, const char *instance_port, buffer *out);

/*
 * 앱 경로 요청의 응답 전체(헤더 + 본문)를 out 뒤에 덧붙인다. app_match 가 1 인 요청에만 부른다.
 *   - POST /api/echo : 200 + req->body 그대로
 *   - 그 외 /api/ 경로 404, 메서드가 맞지 않으면 405 + "Allow: POST"
 *   - 오류 응답을 포함한 모든 응답 헤더에 "X-Instance: <instance_port>" (Round-Robin 분산 확인용)
 *     오류는 http_append_error(..., extra_headers) 로 만든다
 *   - HEAD 면 본문 없이 헤더만
 * 4xx 도 응답을 쓴 것이므로 0. 메모리 부족 등으로 쓰지 못했을 때만 -1 (out 은 그대로)
 */
int app_handle(const http_request *req, const char *instance_port, buffer *out);

#endif /* APP_H */
