/*
 * handler.h — 요청 하나를 어디로 보낼지 결정 (프록시 → 앱 → 정적)
 *
 * CS:APP 대응: tiny.c doit() 의 분기
 *   is_static = parse_uri(...); if (is_static) serve_static(...) else serve_dynamic(...)
 * 담당: 트랙 A (호출부) · B/C (각 처리) · 이슈 #4 에서 확정, #8 에서 순서 검증
 */
#ifndef HANDLER_H
#define HANDLER_H

#include "http_request.h"
#include "proxy_config.h"

typedef enum { ROUTE_PROXY, ROUTE_APP, ROUTE_STATIC } route_kind;

/* 처리 순서: 설정된 프록시 경로 → 내장 앱 경로 → 정적 파일.
 * cfg 가 NULL 이면 프록시 없음 (백엔드 인스턴스) */
route_kind handler_route(const http_request *req, proxy_config *cfg);

#endif /* HANDLER_H */
