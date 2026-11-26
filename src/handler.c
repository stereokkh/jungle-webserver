/*
 * handler.c — 요청 라우팅 (헬스체크 → 프록시 → 앱 → 정적)
 *
 * CS:APP 대응: doit() 의 is_static 분기
 *   Tiny는 strstr(uri, "cgi-bin") 이 있으면 동적으로 본다 → 경로 어디에든 있으면 오판.
 *   여기서는 경로 "접두사"로만 판단한다.
 * 담당: 이슈 #4 (틀), #8 (프록시 경로 매칭), #10 (앱 경로, 헬스체크)
 */
#include "handler.h"
#include "app.h"

#include <stddef.h>
#include <string.h>

route_kind handler_route(const http_request *req, proxy_config *cfg, proxy_route **route)
{
    *route = NULL;
    if (app_health_match(req))   /* 프록시보다 먼저: 어느 인스턴스든 자기 자신이 답한다 (handler.h) */
        return ROUTE_HEALTH;
    if (cfg != NULL) {
        *route = proxy_match(cfg, req->target);   /* 구현: #8 */
        if (*route != NULL)
            return ROUTE_PROXY;
    }
    if (app_match(req))
        return ROUTE_APP;
    return ROUTE_STATIC;
}
