/*
 * proxy_config.h — Reverse Proxy 설정 파일과 라우팅 정책 (순수 함수)
 *
 * CS:APP 대응: 없음 (Tiny에 없는 기능)
 * 담당: 트랙 B · 이슈 #8
 */
#ifndef PROXY_CONFIG_H
#define PROXY_CONFIG_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>

#define PROXY_MAX_ROUTES   16
#define PROXY_MAX_BACKENDS 8

/* 백엔드가 죽었다고 잠깐 기억하는 시간 (ms). connect 실패·시간 초과가 난 백엔드를 이 시간 동안 고르지 않는다.
 * nginx 의 max_fails/fail_timeout 처럼 "일정 시간 쉬게 한다" 이고, 시간이 지나면 저절로 풀린다 (결정: docs/decisions.md 2026-11-25).
 * #ifndef 는 UPSTREAM_TIMEOUT_MS 처럼 -D 로 줄여 빌드해 보기 위한 것 */
#ifndef PROXY_BACKEND_DOWN_MS
#define PROXY_BACKEND_DOWN_MS 10000
#endif

/* proxy_route.failover 의 비트. 0 이면 페일오버 꺼짐 ("failover=off", 설정에 없을 때도 같다) */
#define PROXY_FAILOVER_CONNECT 0x1u   /* connect 실패 → 다음 백엔드 */
#define PROXY_FAILOVER_TIMEOUT 0x2u   /* PROXY_CONNECT_TIMEOUT_MS 안에 connect 가 안 끝남 → 다음 백엔드 */

/* 모두 0 인 값이 "기본 주 백엔드, 안 죽음" 이다 (테스트가 memset 0 으로 만든 라우트도 그대로 동작) */
typedef struct {
    struct sockaddr_storage addr;   /* 설정 로드 시 getaddrinfo 로 한 번 해석 */
    socklen_t addrlen;
    char name[64];                  /* "127.0.0.1:8001" (로그·Host 헤더용). "backup=" 같은 옵션 이름은 안 붙는다 */
    int is_backup;                  /* 1 이면 "backup=host:port" 로 적은 백업 백엔드 (시도하지 않은 정상 주 백엔드가 없을 때만 쓴다) */
    int64_t down_until_ms;          /* 이 시각(단조 시계, ms) 전까지는 죽은 것으로 본다. 0 이면 표시 없음 (proxy_mark_down 이 쓴다) */
} proxy_backend;

typedef struct {
    char prefix[128];               /* "/api/" */
    proxy_backend backends[PROXY_MAX_BACKENDS];   /* 설정 줄의 등장 순서. 주 백엔드와 백업이 섞여 있을 수 있다 (is_backup 으로 구분) */
    size_t nbackends;               /* 주 + 백업 합계 (PROXY_MAX_BACKENDS 이하) */
    size_t next;                    /* Round-Robin 다음 순번: "주 백엔드들 중" 몇 번째인지 (proxy_rr_begin 이 돌린다) */
    unsigned failover;              /* PROXY_FAILOVER_* 비트. 0 = 꺼짐 (이때 백업·tries 는 쓰이지 않는다) */
    size_t tries;                   /* 요청 하나가 시도할 수 있는 백엔드 수 (첫 시도 포함). 로더는 1..PROXY_MAX_BACKENDS,
                                     * 설정에 없으면 nbackends 로 채운다. 이 한도를 지키는 쪽은 호출자(server.c)다 */
} proxy_route;

typedef struct {
    proxy_route routes[PROXY_MAX_ROUTES];
    size_t nroutes;
} proxy_config;

/*
 * 설정 파일을 읽는다. 형식 오류·주소 해석 실패면 -1 (서버 시작 거절).
 *   - 한 줄에 라우트 하나: "<접두사> <host:port> [<host:port>...]", '#' 로 시작하는 토큰부터 줄 끝까지 주석.
 *     host 는 이름·IPv4, IPv6 는 "[::1]:8001". 공백은 SP/TAB, 줄 끝은 LF·CRLF
 *   - 접두사 뒤에는 옵션 토큰을 어디에나 둘 수 있다 (페일오버, 2026-11-25. failover=, tries= 는 줄에 한 번만, backup= 은 여러 번. 자세한 형식은 docs/api.md):
 *       backup=<host:port>     백업 백엔드 (여러 번 가능. 아직 시도하지 않은 정상(쉬는 중이 아닌) 주 백엔드가 없을 때만 쓴다). 주 백엔드가 하나는 있어야 한다
 *       failover=<목록>        쉼표로 구분한 off | connect | timeout (off 는 혼자만, 빈 항목·중복·모르는 값은 오류). 없으면 off
 *       tries=<N>              요청 하나가 시도할 백엔드 수 (첫 시도 포함). 앞자리 0 없는 10진수 1..PROXY_MAX_BACKENDS. 없으면 백엔드 전체 수
 *     failover 가 off 이면 backup·tries 는 읽기만 하고 쓰이지 않는다 (오류 아님). '=' 가 있는 토큰은 옵션, 없으면 주 백엔드
 *   - 접두사는 '/' 로 시작, 글자는 영숫자와 - . _ ~ ! $ & ' ( ) * + , ; = : @ / 만
 *     (% ? # 제어 문자 비 ASCII 는 오류. 정규화된 target 과 바이트 그대로 비교하기 때문.
 *      비예약 문자는 정규형에서 글자로 오므로 그대로 쓰면 되고, 예약 문자를 %XX 로 보낸 요청은
 *      정규형에서도 %XX 라 글자로 쓴 접두사와 맞지 않는다 (RFC 3986 에서 다른 URI))
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
 *     페일오버는 "백엔드 사이"이지 한 이름의 IPv4→IPv6 주소 사이가 아니므로, 이름이 IPv4 와 IPv6 로 모두 해석되면 IPv4 를 고른다.
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
 *   - target 은 파서가 만든 정규화된 요청 대상(http_request.target)이고 '?' 앞 경로 부분만 비교한다.
 *     정규형에는 빈 조각·점 조각·%2F 가 없고 비예약 문자의 %XX 는 글자라, 표기만 바꿔 라우트를
 *     건너뛸 수 없다 (#31). 정적 파일의 path 도 같은 정규형에서 나온다
 *   - 경로 경계에서만 맞는다: prefix 가 '/' 로 끝나거나, target 의 다음 글자가 '/', '?', 끝
 *     ("/api" 는 "/api", "/api/x", "/api?x" 에 맞고 "/apix" 에는 맞지 않는다.
 *      "/api/" 는 "/api" 에 맞지 않으므로 둘 다 덮으려면 "/api" 로 쓴다. "/" 는 전부에 맞는다)
 *   - cfg, target 이 NULL 이거나 라우트가 없으면 NULL
 *   - 대소문자를 구분한다. 디코딩·정리는 파서가 이미 했으므로 여기서는 하지 않는다
 *     ("/%61pi/x" 는 파서가 "/api/x" 로 바꿔 오므로 "/api/" 에 맞는다)
 *   - 접두사를 떼지 않는다. 경로와 질의는 정규화된 target 그대로 백엔드로 간다
 *     (백엔드는 같은 바이너리라 "/api/..." 를 스스로 처리한다)
 */
proxy_route *proxy_match(proxy_config *cfg, const char *target);

/*
 * ---- 백엔드 고르기 (소켓·시계 없는 순수 함수. 현재 시각은 now_ms 로 받는다) ----
 *
 * 요청 하나의 흐름 (server.c):
 *   rr = proxy_rr_begin(route);              요청 시작에 한 번
 *   tried = 0;
 *   loop: i = proxy_pick_backend(route, now, tried, rr);  → -1 이면 더 고를 백엔드가 없다
 *         tried |= 1u << i;  시도 횟수가 route->tries 에 닿으면 멈춘다 (호출자 몫: 이 함수는 tries 를 모른다)
 *         connect 실패·시간 초과면 proxy_mark_down(route, i, now) 하고 loop
 */

/* 요청 시작 때 한 번 부른다. Round-Robin 시작 위치(주 백엔드들 중 몇 번째, 0 부터)를 돌려주고 route->next 를 한 칸 민다.
 * 페일오버로 같은 요청이 다시 고를 때는 이 값을 그대로 쓰고 다시 부르지 않는다 (순번은 요청당 한 칸).
 * 주 백엔드가 하나면 늘 0, 없으면(손으로 채운 route) 0. route 가 NULL 이어도 0.
 * 페일오버가 꺼진 라우트에서 고르는 주 백엔드의 순서는 #9 의 Round-Robin 과 같다: 0, 1, ..., n-1, 0 ...
 * next 가 범위 밖이어도 (손으로 채운 route) 나머지 연산으로 맞춘다 */
size_t proxy_rr_begin(proxy_route *route);

/* 이 요청이 다음에 시도할 백엔드의 인덱스 (route->backends[]). 없으면 -1 (route 가 NULL, 백엔드 없음 포함).
 * 후보 순서: 주 백엔드를 rr_start 번째부터 한 바퀴(Round-Robin), 그다음 백업을 적은 순서대로.
 *   1. tried_mask(비트 i = backends[i] 는 이미 시도함)에 없고 now_ms 에 죽은 것으로 표시되지 않은 첫 후보
 *   2. 없고 라우트의 백엔드가 전부 죽은 표시 중이면(이미 시도한 것도 센다) 죽은 표시를 무시하고 tried_mask 에 없는 첫 후보.
 *      그래서 전부 죽은 것으로 표시돼도 기다리지 않고 다음 후보를 시도해 살아난 백엔드를 바로 찾는다.
 *      하나라도 죽지 않았으면(이미 시도한 것이어도) 표시를 무시하지 않는다: 남은 후보가 죽은 표시뿐이면 -1
 *   3. 둘 다 없으면 -1
 * 주 백엔드가 남아 있는 동안 백업은 고르지 않는다. 죽은 표시는 시간으로만 풀린다: now_ms >= down_until_ms 이면 살아난 것.
 * route 를 바꾸지 않는다. 페일오버가 꺼져 있으면(route->failover == 0) 위 순서를 쓰지 않고 후보는 rr_start 번째 주 백엔드 하나뿐이다:
 * 백업은 고르지 않고, 죽은 표시도 보지 않는다 (proxy_mark_down 이 표시를 안 한다). 그 백엔드가 tried_mask 에 있으면 -1, 주 백엔드가 없어도 -1.
 * tries 한도는 보지 않는다 (호출자가 시도 횟수를 센다) */
int proxy_pick_backend(const proxy_route *route, int64_t now_ms, unsigned tried_mask, size_t rr_start);

/* backends[idx] 를 now_ms + PROXY_BACKEND_DOWN_MS 까지 죽은 것으로 표시한다. route->failover 가 0 이면 아무것도 안 한다
 * (페일오버를 껐으면 기억도 하지 않는다: 예전 동작 그대로). idx 가 범위 밖이거나 route 가 NULL 이어도 아무것도 안 한다.
 * 이미 표시된 것을 다시 부르면 만료 시각이 now_ms 기준으로 새로 잡힌다 */
void proxy_mark_down(proxy_route *route, size_t idx, int64_t now_ms);

#endif /* PROXY_CONFIG_H */
