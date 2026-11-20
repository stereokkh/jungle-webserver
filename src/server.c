/*
 * server.c — poll() 이벤트 루프, 연결 수명주기, 시그널/종료
 *
 * CS:APP 대응: tiny.c main()
 *   Tiny:  listenfd = Open_listenfd(port);
 *          while (1) { connfd = Accept(...); doit(connfd); Close(connfd); }
 *   → 느린 클라이언트 하나가 서버 전체를 막는다 (이슈 #3 재현 ①).
 *   여기서는 poll 로 listen fd + 모든 연결 fd 를 감시하고,
 *   연결마다 상태(conn)를 두어 읽기/처리/쓰기를 조금씩 진행한다.
 * 담당: 트랙 A · 이슈 #7
 */
#include "server.h"
#include "net.h"
#include "http_request.h"
#include "handler.h"

#include <poll.h>
#include <signal.h>
#include <stddef.h>

/* 연결 하나의 상태. 버퍼는 힙에 둔다 (musl 스레드 스택 규칙과 같은 이유로 큰 배열은 스택 금지).
 * TODO(#4): 필드와 버퍼 소유권을 확정한다 */
typedef enum {
    CONN_READING,       /* 요청을 읽는 중 (http_parse_request 가 NEED_MORE) */
    CONN_WRITING,       /* 응답을 보내는 중 (offset 저장, EAGAIN 이면 다음 POLLOUT 까지 대기) */
    CONN_UPSTREAM,      /* 프록시: 백엔드 연결/송수신 중 (#9) */
    CONN_CLOSING
} conn_state;

typedef struct {
    int fd;
    conn_state state;
    char *rbuf;  size_t rlen, rcap;     /* 읽은 바이트. 처리한 만큼 앞으로 당긴다 (keep-alive 잔여 보존) */
    char *wbuf;  size_t wlen, woff;     /* 보낼 바이트와 보낸 위치 */
    long last_active_ms;                /* 유휴 타임아웃 */
} conn;

int server_run(const server_config *cfg)
{
    /* TODO(#7):
     *  1. signal(SIGPIPE, SIG_IGN), sigaction(SIGINT/SIGTERM) → 종료 플래그
     *  2. listenfd = net_listen(cfg->port)
     *  3. 루프: poll(fds) → listenfd 면 accept 반복(EAGAIN 까지),
     *           연결이면 read → http_parse_request → handler_route → 응답 준비 → write
     *  4. 타임아웃 지난 연결 정리
     *  5. 종료 플래그면: 새 연결 중단 → 모든 conn/파일/업스트림 정리 → return 0 */
    (void)cfg;
    return -1;
}
