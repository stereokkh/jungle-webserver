/*
 * handler.c — 요청 라우팅 (프록시 → 앱 → 정적)
 *
 * CS:APP 대응: doit() 의 is_static 분기
 *   Tiny는 strstr(uri, "cgi-bin") 이 있으면 동적으로 본다 → 경로 어디에든 있으면 오판.
 *   여기서는 경로 "접두사"로만 판단한다.
 * 담당: 이슈 #4 (틀), #8 (프록시 경로 매칭), #10 (앱 경로)
 */
#include "handler.h"

#include <stddef.h>
#include <string.h>

route_kind handler_route(const http_request *req, proxy_config *cfg)
{
    /* TODO(#8): cfg != NULL && proxy_match(cfg, req->target) → ROUTE_PROXY
     * TODO(#10): req->path 가 "/api/" 로 시작 → ROUTE_APP */
    (void)req; (void)cfg;
    return ROUTE_STATIC;
}
