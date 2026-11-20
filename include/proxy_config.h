/*
 * proxy_config.h — Reverse Proxy 설정 파일과 라우팅 정책 (순수 함수)
 *
 * CS:APP 대응: 없음 (Tiny에 없는 기능)
 * 담당: 트랙 B · 이슈 #8
 */
#ifndef PROXY_CONFIG_H
#define PROXY_CONFIG_H

#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>

#define PROXY_MAX_ROUTES   16
#define PROXY_MAX_BACKENDS 8

typedef struct {
    struct sockaddr_storage addr;   /* 설정 로드 시 getaddrinfo 로 한 번 해석 */
    socklen_t addrlen;
    char name[64];                  /* "127.0.0.1:8001" (로그·Host 헤더용) */
} proxy_backend;

typedef struct {
    char prefix[128];               /* "/api/" */
    proxy_backend backends[PROXY_MAX_BACKENDS];
    size_t nbackends;
    size_t next;                    /* Round-Robin 다음 순번 (#9) */
} proxy_route;

typedef struct {
    proxy_route routes[PROXY_MAX_ROUTES];
    size_t nroutes;
} proxy_config;

/* 설정 파일을 읽는다. 형식 오류·주소 해석 실패면 -1 (서버 시작 거절) */
int proxy_config_load(const char *path, proxy_config *cfg);

/* 가장 긴 접두사가 맞는 라우트. 없으면 NULL */
proxy_route *proxy_match(proxy_config *cfg, const char *target);

/* 라우트의 다음 백엔드 (Round-Robin) */
const proxy_backend *proxy_next_backend(proxy_route *route);

#endif /* PROXY_CONFIG_H */
