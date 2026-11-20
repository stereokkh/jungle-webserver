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
#include "buffer.h"
#include "static_file.h"

#include <poll.h>
#include <signal.h>
#include <stddef.h>

/* 연결 하나의 상태 (docs/interfaces.md 의 상태 표).
 * conn 은 calloc 으로 힙에 만든다 (http_request 가 수 KB). 이 파일 밖으로 내보내지 않는다 */
typedef enum {
    CONN_READING,        /* 요청을 읽는 중 (POLLIN). 파서가 NEED_MORE */
    CONN_WRITING,        /* out 버퍼를 보내는 중 (POLLOUT). EAGAIN 이면 다음 POLLOUT 까지 대기 */
    CONN_SENDING_FILE,   /* 헤더를 다 보내고 파일 조각을 보내는 중 (POLLOUT) */
    CONN_UPSTREAM,       /* 프록시: 백엔드 연결·송수신 중 (#9 에서 세분화) */
    CONN_CLOSING         /* 다 보내면 닫는다 (keep_alive == 0 또는 오류 응답 후) */
} conn_state;

typedef struct {
    int fd;
    conn_state state;
    buffer in;               /* 받은 바이트. 파서는 buffer_peek(&in) 부터 본다 */
    buffer out;              /* 보낼 바이트. out.off 가 송신 진행 상태 */
    http_request req;        /* 파서 결과. headers/body 는 in 안을 가리킴 → 처리 후 consume */
    int keep_alive;          /* 이번 응답 후 연결 유지 여부 */
    static_file file;        /* CONN_SENDING_FILE 일 때 */
    int upstream_fd;         /* 프록시 백엔드 소켓, 없으면 -1 (#9) */
    buffer up_out;           /* 백엔드로 보낼 요청 (#9) */
    long last_active_ms;     /* CLIENT_IDLE_TIMEOUT_MS / UPSTREAM_TIMEOUT_MS 검사 */
} conn;

/* TODO(#7): conn_close(conn *c) — 모든 자원을 정리하는 유일한 함수. 두 번 불러도 안전하게.
 *   close(fd), close(upstream_fd), static_close(&file),
 *   buffer_free(&in/&out/&up_out), poll 목록에서 제거, free(c) */

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
