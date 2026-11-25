/*
 * server.h — poll() 이벤트 루프, 연결 수명주기, 시그널/종료
 *
 * CS:APP 대응: tiny.c main() 의 while(1) { Accept; doit; Close; } 반복 루프
 * 담당: 트랙 A · 이슈 #7 (업스트림 상태 통합은 #9, B와 협업)
 * 현대식으로 바뀌는 점: 한 번에 한 연결 → poll 로 여러 연결, keep-alive, 타임아웃,
 *                       SIGINT/SIGTERM 시 모든 자원 정리 후 종료
 *
 * 모듈을 어떻게 이어 붙이는지(호출 순서, 버퍼 소유권, 정리 책임)는 docs/interfaces.md
 */
#ifndef SERVER_H
#define SERVER_H

/* 동시 연결 상한. 넘으면 accept 후 바로 닫는다 */
#define SERVER_MAX_CONNS        1024

/* 클라이언트가 이 시간 동안 아무것도 보내지/받지 않으면 연결을 닫는다
 * (요청을 반만 보내고 멈춘 느린 클라이언트, keep-alive 유휴 연결 모두) */
#define CLIENT_IDLE_TIMEOUT_MS  10000

/* 요청 하나를 다 받을 때까지의 상한 (그 요청의 첫 바이트부터). 유휴 타이머는 바이트가 올 때마다
 * 리셋되므로, 헤더·본문을 1바이트씩 보내는 느린 클라이언트(slow loris)는 이것으로 끊는다.
 * 넘으면 응답 없이 닫는다. #ifndef 는 수동 검증 때 -D 로 줄여 빌드하기 위한 것 */
#ifndef CLIENT_REQUEST_TIMEOUT_MS
#define CLIENT_REQUEST_TIMEOUT_MS 30000
#endif

/* 응답을 보내고 닫을 때 송신만 먼저 닫고(shutdown), 클라이언트가 보낸 나머지를 읽어 버리며
 * 기다리는 최대 시간 (lingering close). 읽지 않은 입력을 남긴 채 close 하면 커널이 RST 를 보내
 * 클라이언트가 아직 읽지 않은 응답(413, 431 등)까지 버려질 수 있다 */
#define CLIENT_LINGER_TIMEOUT_MS  2000

/* 업스트림 시간 상한. 시계가 둘이다:
 *   응답 시작 전: 요청을 프록시에 넘긴 때부터 머리가 다 올 때까지. 넘으면 504
 *   응답 시작 후: 업스트림에서 읽으려고 기다리는 동안 바이트가 이만큼 안 오면 연결을 닫는다.
 *                 느린 클라이언트 때문에 읽기를 멈춘 시간은 세지 않는다 (그건 CLIENT_IDLE_TIMEOUT_MS)
 * 그래서 전체 전송 시간이 이보다 길어도 바이트 사이 간격이 짧으면 끝까지 중계된다.
 * 페일오버(라우트의 failover=)로 다른 백엔드를 시도해도 응답 시작 전 시계는 처음 시도부터 한 번만 간다
 * (다시 잡지 않는다). 그래서 백엔드를 몇 개 거치든 응답 시작 전 전체 시간은 이 값으로 막힌다.
 * #ifndef 는 수동 검증(느리게 찔끔찔끔 보내는 업스트림) 때 -D 로 줄여 빌드하기 위한 것 */
#ifndef UPSTREAM_TIMEOUT_MS
#define UPSTREAM_TIMEOUT_MS     5000
#endif
/* 응답 시작 전에는 클라이언트 유휴 시계(CLIENT_IDLE_TIMEOUT_MS)도 같이 돈다. 업스트림 상한이 같거나 길면
 * 클라이언트 쪽이 먼저 만료돼 504 대신 응답 없이 닫히므로 빌드에서 막는다 */
#if UPSTREAM_TIMEOUT_MS >= CLIENT_IDLE_TIMEOUT_MS
#error "UPSTREAM_TIMEOUT_MS must be smaller than CLIENT_IDLE_TIMEOUT_MS"
#endif

/* 업스트림 connect 한 번에 줄 시간. 라우트에 failover=timeout 이 있을 때만 재고, 넘으면 그 connect 를 닫고
 * 다음 백엔드로 넘어간다 (없으면 0 이 아니라 "재지 않는다": connect 도 UPSTREAM_TIMEOUT_MS 하나로 잰다).
 * 전체 마감(UPSTREAM_TIMEOUT_MS)보다 짧아야 다음 백엔드를 시도할 시간이 남고, 0 보다 커야 한다 (0 이하면 connect 마다 시작하자마자
 * 마감이 지난 것이 되어 failover=timeout 이 모든 connect 를 시간 초과로 닫는다). 둘 다 빌드에서 막는다.
 * #ifndef 는 -D 로 줄여 빌드해 보기 위한 것. 안 주면 기본값은 아래 */
#ifndef PROXY_CONNECT_TIMEOUT_MS
/* 따로 주지 않으면 기본값을 UPSTREAM_TIMEOUT_MS 에서 정한다: 보통은 1000, 전체 마감이 1000 이하로 줄어 있으면 그 절반.
 * 그래서 -DUPSTREAM_TIMEOUT_MS=1000 하나만 줘도 빌드된다. 둘 다 -D 로 주면 아래 검사가 그대로 적용된다 */
#if UPSTREAM_TIMEOUT_MS < 2
#error "UPSTREAM_TIMEOUT_MS must be at least 2 (the connect timeout must be at least 1 and smaller than it)"
#elif UPSTREAM_TIMEOUT_MS > 1000
#define PROXY_CONNECT_TIMEOUT_MS 1000
#else
#define PROXY_CONNECT_TIMEOUT_MS (UPSTREAM_TIMEOUT_MS / 2)
#endif
#endif
#if PROXY_CONNECT_TIMEOUT_MS <= 0
#error "PROXY_CONNECT_TIMEOUT_MS must be greater than 0"
#endif
#if PROXY_CONNECT_TIMEOUT_MS >= UPSTREAM_TIMEOUT_MS
#error "PROXY_CONNECT_TIMEOUT_MS must be smaller than UPSTREAM_TIMEOUT_MS"
#endif

typedef struct {
    const char *port;        /* -p */
    const char *docroot;     /* -r */
    const char *proxy_conf;  /* -c (없으면 NULL → 프록시 없는 백엔드 인스턴스) */
    int merge_slashes;       /* --merge-slashes: 경로의 빈 조각 "//" 를 400 대신 "/" 로 합친다 (#31) */
    int access_log;          /* --access-log: 연결 열림·요청·닫힘을 stderr 에 한 줄씩 (기본 꺼짐) */
} server_config;

/* 이벤트 루프를 돌린다. 시그널로 정상 종료하면 0, 초기화 실패 시 -1 */
int server_run(const server_config *cfg);

#endif /* SERVER_H */
