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

/*
 * 설정 파일을 읽는다. 형식 오류·주소 해석 실패면 -1 (서버 시작 거절).
 *   - 한 줄에 라우트 하나: "<접두사> <host:port> [<host:port>...]", '#' 로 시작하는 토큰부터 줄 끝까지 주석.
 *     host 는 이름·IPv4, IPv6 는 "[::1]:8001". 공백은 SP/TAB, 줄 끝은 LF·CRLF
 *   - 접두사는 '/' 로 시작, 글자는 영숫자와 - . _ ~ ! $ & ' ( ) * + , ; = : @ / 만
 *     (% ? # 제어 문자 비 ASCII 는 오류. 원본 target 과 바이트 그대로 비교하기 때문)
 *   - 줄 끝의 CR 은 LF 바로 앞에 있을 때만 줄 끝이다 (LF 없이 CR 로 끝난 마지막 줄은 오류)
 *   - 한도: 줄 1024바이트, 접두사 127, 백엔드 토큰 63, 라우트 16, 라우트당 백엔드 8, 포트 1-65535
 *   - 포트는 앞자리 0 없는 10진수만 ("08001" 은 오류. 토큰이 그대로 Host 헤더 값이 되므로 한 가지 표기만 둔다).
 *     숫자로만 된 호스트는 앞자리 0 없는 점 넷의 10진 IPv4 만 ("127.0.0.010" 은 오류.
 *     getaddrinfo 가 앞자리 0 을 8진수로 읽어 검사한 주소와 다른 주소로 풀기 때문)
 *   - 같은 접두사가 두 번 나오면 오류, 라우트가 하나도 없으면(빈 파일 포함) 오류
 *   - 실패하면 -1, cfg 는 전부 0(nroutes == 0), stderr 에 "경로:줄: 이유" 한 줄.
 *     줄이 없는 파일 전체 오류(열기·읽기 실패, 라우트 없음)는 "경로: 이유" 한 줄.
 *     성공하면 아무것도 출력하지 않는다. cfg 는 호출할 때마다 처음부터 다시 채운다
 *   - 이름은 여기서 getaddrinfo 로 한 번만 해석한다. 백엔드마다 주소 하나만 저장하고
 *     재시도·페일오버가 없으므로, 이름이 IPv4 와 IPv6 로 모두 해석되면 IPv4 를 고른다.
 *     백엔드는 같은 바이너리를 다른 포트로 띄운 것이다. net_listen(#7, PR #25)은 IPv6 소켓 하나에
 *     IPV6_V6ONLY=0 으로 IPv4·IPv6 를 둘 다 받고, IPv6 를 못 쓰는 환경에서는 IPv4 만 연다.
 *     두 경우 모두 IPv4 loopback 은 받으므로 IPv4 를 고르면 어느 쪽이든 연결된다.
 *     "localhost" 가 ::1 로 먼저 풀리는 환경(Docker, macOS)에서 ::1 을 고르면 IPv4 만 연 백엔드에서
 *     연결 거절이 난다. IPv6 로만 보내고 싶으면 "[::1]:포트" 로 쓴다
 *     (#7 에 대한 요구: 백엔드 인스턴스는 loopback 의 IPv4 연결을 받아야 한다)
 */
int proxy_config_load(const char *path, proxy_config *cfg);

/*
 * 가장 긴 접두사가 맞는 라우트. 없으면 NULL.
 *   - target 은 원본 요청 대상(http_request.target, 디코딩 전)이고 '?' 앞 경로 부분만 비교한다
 *     (디코딩한 path 로 비교하면 "%2F" 같은 표기로 라우팅이 달라질 수 있다)
 *   - 경로 경계에서만 맞는다: prefix 가 '/' 로 끝나거나, target 의 다음 글자가 '/', '?', 끝
 *     ("/api" 는 "/api", "/api/x", "/api?x" 에 맞고 "/apix" 에는 맞지 않는다.
 *      "/api/" 는 "/api" 에 맞지 않으므로 둘 다 덮으려면 "/api" 로 쓴다. "/" 는 전부에 맞는다)
 *   - cfg, target 이 NULL 이거나 라우트가 없으면 NULL
 *   - 대소문자를 구분하고, 퍼센트 디코딩이나 "../" 정리를 하지 않는다 ("/%61pi/x" 는 "/api/" 에 안 맞는다)
 *   - 접두사를 떼지 않는다. 경로와 질의는 원본 target 그대로 백엔드로 간다
 *     (백엔드는 같은 바이너리라 "/api/..." 를 스스로 처리한다)
 */
proxy_route *proxy_match(proxy_config *cfg, const char *target);

/* 라우트의 다음 백엔드 (Round-Robin) */
const proxy_backend *proxy_next_backend(proxy_route *route);

#endif /* PROXY_CONFIG_H */
