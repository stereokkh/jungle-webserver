/*
 * handler.h — 요청 하나를 어디로 보낼지 결정 (헬스체크 → 프록시 → 앱 → 정적)
 *
 * CS:APP 대응: tiny.c doit() 의 분기
 *   is_static = parse_uri(...); if (is_static) serve_static(...) else serve_dynamic(...)
 * 담당: 트랙 A (호출부) · B/C (각 처리) · 이슈 #4 에서 확정, #8 에서 순서 검증
 */
#ifndef HANDLER_H
#define HANDLER_H

#include "http_request.h"
#include "proxy_config.h"

typedef enum { ROUTE_PROXY, ROUTE_APP, ROUTE_STATIC, ROUTE_HEALTH } route_kind;

/*
 * 처리 순서: 헬스체크(app_health_match, 정확히 "/healthz") → 설정된 프록시 경로 → 내장 앱 경로(app_match) → 정적 파일.
 *   - 헬스체크가 프록시보다 먼저인 이유: "이 인스턴스가 살아 있나"를 묻는 주소라서 프런트에 보낸 것을
 *     백엔드가 대신 답하면 안 된다. 정확 일치라 프록시 라우트("/" 포함)를 가로채는 것은 이 한 경로뿐이다
 *     (docs/decisions.md 2026-11-25 헬스체크)
 *   - cfg 가 NULL 이면 프록시 없음 (백엔드 인스턴스)
 *   - ROUTE_PROXY 면 *route 에 매칭된 라우트, 아니면 *route = NULL (ROUTE_HEALTH 도 NULL)
 *     (서버가 proxy_match 를 다시 부르지 않아도 되게)
 *   - 메서드 검사는 여기서 하지 않는다. ROUTE_STATIC 인데 GET/HEAD 가 아니면 서버가 405
 */
route_kind handler_route(const http_request *req, proxy_config *cfg, proxy_route **route);

#endif /* HANDLER_H */
