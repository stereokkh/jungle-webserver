/*
 * proxy_config.c — Reverse Proxy 설정 파일과 라우팅 정책
 *
 * CS:APP 대응: 없음 (Tiny에 없는 기능)
 * 설정 파일 형식 (안, #8 에서 확정):
 *   # 경로 접두사   백엔드...
 *   /api/          127.0.0.1:8001 127.0.0.1:8003
 *   /static/       127.0.0.1:8002
 * 담당: 트랙 B · 이슈 #8 (단위 테스트: tests/unit/)
 */
#include "proxy_config.h"

#include <netdb.h>
#include <stdio.h>
#include <string.h>

int proxy_config_load(const char *path, proxy_config *cfg)
{
    /* TODO(#8): 줄 단위 파싱, '#' 주석, 백엔드 host:port 를 getaddrinfo 로 해석해 addr 에 저장.
     *  잘못된 줄·라우트/백엔드 개수 초과·해석 실패 → -1 */
    (void)path;
    memset(cfg, 0, sizeof *cfg);
    return -1;
}

proxy_route *proxy_match(proxy_config *cfg, const char *target)
{
    /* TODO(#8): 모든 라우트 중 target 이 prefix 로 시작하는 것 가운데 가장 긴 것 */
    (void)cfg; (void)target;
    return NULL;
}

const proxy_backend *proxy_next_backend(proxy_route *route)
{
    /* TODO(#9): 처음엔 backends[0] 만, 중계가 안정되면 next 를 돌려 Round-Robin */
    if (route == NULL || route->nbackends == 0)
        return NULL;
    return &route->backends[0];
}
