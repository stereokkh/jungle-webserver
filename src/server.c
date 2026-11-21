/*
 * server.c — poll() 이벤트 루프, 연결 수명주기, 시그널/종료
 *
 * CS:APP 대응: tiny.c main()
 *   Tiny:  listenfd = Open_listenfd(port);
 *          while (1) { connfd = Accept(...); doit(connfd); Close(connfd); }
 *   → 한 번에 연결 하나. 요청을 다 보내지 않는 클라이언트 하나가 다른 모든 연결을 기다리게 한다.
 *   여기서는 poll 로 listen fd + 모든 연결 fd 를 감시하고, 연결마다 상태(conn)를 두어
 *   읽기/처리/쓰기를 "지금 할 수 있는 만큼만" 진행한 뒤 루프로 돌아온다.
 *
 * 루프 한 바퀴:
 *   1. 유휴 시간을 넘긴 연결을 닫고, poll 대기 시간을 가장 먼저 만료될 연결 기준으로 정한다
 *   2. pollfd 목록을 새로 만든다: [0] 시그널 파이프, [1] listen 소켓, [2..] 연결
 *      (연결 상태에 따라 POLLIN 또는 POLLOUT 중 하나만 감시)
 *   3. poll → 연결 이벤트 처리 → 마지막에 accept
 *      (accept 를 먼저 하면 방금 닫은 칸에 새 연결이 들어가서, 같은 칸을 가리키는
 *       이전 pollfd 의 revents 가 새 연결에 잘못 적용될 수 있다)
 *
 * 요청 하나의 흐름 (docs/interfaces.md 1~6단계):
 *   READING ─파서 DONE→ 응답을 out 에 만듦 → WRITING ─out 다 보냄→ (정적 파일이면) SENDING_FILE
 *     → keep-alive 면 READING (남은 입력에 다음 요청이 있으면 바로 처리 = 파이프라이닝)
 *     → 아니면 LINGERING (송신만 닫고 남은 입력을 읽어 버리다가 닫음)
 *   파서 오류 → 오류 응답 → CLOSING ─다 보냄→ LINGERING
 *
 * Tiny 와 다른 점: 요청마다 연결을 닫지 않는다 (keep-alive), 한 연결이 느려도 다른 연결은
 * 계속 처리된다, 요청 하나를 받는 시간에 상한이 있다, 오류 응답이 RST 로 사라지지 않게
 * lingering close 를 한다.
 *
 * 담당: 트랙 A · 이슈 #7
 */
#include "server.h"
#include "net.h"
#include "http_request.h"
#include "handler.h"
#include "buffer.h"
#include "static_file.h"
#include "http_response.h"
#include "app.h"
#include "proxy_config.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* read 한 번에 확보할 입력 버퍼 여유 */
#define READ_CHUNK 4096

/* accept 가 fd 고갈(EMFILE 등)로 실패하면 이 시간 동안 listen 소켓을 감시하지 않는다.
 * 그대로 두면 대기열의 연결 때문에 poll 이 계속 깨어나 CPU 를 다 쓴다 */
#define ACCEPT_PAUSE_MS 100

/* poll 한 바퀴에 accept 할 최대 연결 수. 대기열이 계속 차 있어도 기존 연결 처리·유휴 만료·
 * 종료 시그널 확인이 밀리지 않게 한다. 남은 연결은 다음 바퀴에 poll 이 다시 알려 준다 */
#define ACCEPT_PER_PASS 64

/* 연결 하나의 상태 (docs/interfaces.md 의 상태 표).
 * conn 은 calloc 으로 힙에 만든다 (http_request 가 수 KB). 이 파일 밖으로 내보내지 않는다 */
typedef enum {
    CONN_READING,        /* 요청을 읽는 중 (POLLIN). 파서가 NEED_MORE */
    CONN_WRITING,        /* out 버퍼를 보내는 중 (POLLOUT). EAGAIN 이면 다음 POLLOUT 까지 대기 */
    CONN_SENDING_FILE,   /* 헤더를 다 보내고 파일 조각을 보내는 중 (POLLOUT) */
    CONN_UPSTREAM,       /* 프록시: 백엔드 연결·송수신 중 (#9 에서 세분화) */
    CONN_CLOSING,        /* 오류 응답을 보내는 중 (POLLOUT). 다 보내면 LINGERING */
    CONN_LINGERING       /* 송신을 닫았다 (POLLIN). 받은 건 버리고 EOF·마감이면 닫는다 */
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
    int64_t last_active_ms;  /* CLIENT_IDLE_TIMEOUT_MS / UPSTREAM_TIMEOUT_MS 검사 */
    int64_t req_start_ms;    /* 지금 요청의 첫 바이트를 받은 시각, 없으면 0 (CLIENT_REQUEST_TIMEOUT_MS) */
    int64_t linger_end_ms;   /* LINGERING 의 마감 (CLIENT_LINGER_TIMEOUT_MS) */
    size_t need_len;         /* 파서 NEED_MORE 의 consumed: 이만큼 모이기 전엔 파서를 부르지 않음 */
} conn;

/* pollfd 목록의 고정 위치 */
enum { PFD_SIGNAL = 0, PFD_LISTEN = 1, PFD_FIRST_CONN = 2 };

/* 서버 전체 상태. 배열이 커서(수십 KB) 스택이 아니라 힙에 둔다 */
typedef struct {
    const server_config *cfg;
    int listen_fd;
    int64_t accept_resume_ms;                   /* 0 이 아니면 이 시각까지 accept 쉼 */
    conn *conns[SERVER_MAX_CONNS];              /* 빈 칸은 NULL */
    size_t nconns;
    struct pollfd pfds[PFD_FIRST_CONN + SERVER_MAX_CONNS];
    size_t pfd_slot[PFD_FIRST_CONN + SERVER_MAX_CONNS];   /* pfds[i] 가 가리키는 conns 칸 */
    proxy_config proxy;                         /* -c 가 있을 때만 (서버 수명 동안 유지) */
    int has_proxy;
} server;

/*
 * 시그널 처리: self-pipe.
 *   핸들러는 플래그를 세우고 파이프에 1바이트를 쓴다. 파이프 읽기 끝을 poll 에 넣어 두면
 *   "플래그 검사 → poll 진입" 사이에 시그널이 와도 poll 이 바로 깨어난다.
 *   (플래그만 쓰면 그 틈에 온 시그널은 poll 이 다음 이벤트·타임아웃으로 깰 때까지 묻힌다)
 *   핸들러 안에서는 async-signal-safe 함수(write)만 쓴다. errno 는 보존한다.
 */
static volatile sig_atomic_t stop_requested;
static int sig_pipe[2] = { -1, -1 };
/* 핸들러가 읽는 쓰기 끝. C11 에서 핸들러가 안전하게 접근할 수 있는 정적 객체는
 * volatile sig_atomic_t 뿐이라 sig_pipe[1] 과 따로 둔다 (fd 는 시작 직후 만든 작은 수) */
static volatile sig_atomic_t sig_wfd = -1;

static void on_stop_signal(int sig)
{
    int saved = errno;
    ssize_t r;

    (void)sig;
    stop_requested = 1;
    if (sig_wfd >= 0) {
        r = write(sig_wfd, "x", 1);   /* 파이프가 차서 EAGAIN 이어도 이미 깨울 바이트가 있다 */
        (void)r;
    }
    errno = saved;
}

static int setup_signals(void)
{
    struct sigaction sa;

    if (pipe(sig_pipe) < 0)
        return -1;
    if (net_set_nonblocking(sig_pipe[0]) < 0 || net_set_nonblocking(sig_pipe[1]) < 0)
        return -1;
    sig_wfd = sig_pipe[1];

    /* 끊긴 소켓에 write 하면 SIGPIPE 로 프로세스가 죽는다. 무시하고 EPIPE 로 받는다
     * (MSG_NOSIGNAL 은 Linux 전용이라 쓰지 않는다) */
    signal(SIGPIPE, SIG_IGN);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   /* SA_RESTART 없음: poll 이 EINTR 로 깨어나게 */
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;
    return 0;
}

static void close_signal_pipe(void)
{
    int r = sig_pipe[0], w = sig_pipe[1];

    sig_wfd = -1;
    sig_pipe[0] = sig_pipe[1] = -1;   /* 핸들러가 닫힌(재사용될) fd 에 쓰지 않게 먼저 지운다 */
    if (r >= 0)
        close(r);
    if (w >= 0)
        close(w);
}

static void drain_signal_pipe(void)
{
    char tmp[64];

    while (read(sig_pipe[0], tmp, sizeof tmp) > 0)
        ;
}

/* 단조 시계(ms). 벽시계(time)는 시스템 시간을 바꾸면 뒤로 갈 수 있다 */
/* long 이 32비트인 환경에서는 부팅 후 약 24일이면 tv_sec * 1000 이 넘쳐서 int64_t 를 쓴다 */
static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int would_block(int err)
{
    return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
}

/* ---------------------------------------------------------------
 * 연결 생성·정리
 * --------------------------------------------------------------- */

static int conn_open(server *s, int fd)
{
    size_t i;
    conn *c;

    if (s->nconns >= SERVER_MAX_CONNS || net_set_nonblocking(fd) < 0)
        return -1;
    for (i = 0; i < SERVER_MAX_CONNS && s->conns[i] != NULL; i++)
        ;
    c = calloc(1, sizeof *c);   /* in/out/up_out 은 0 으로 채워져 BUFFER_INIT 과 같다 */
    if (c == NULL)
        return -1;
    c->fd = fd;
    c->state = CONN_READING;
    c->file.fd = -1;            /* calloc 의 0 은 "fd 0 열림"으로 읽힌다 */
    c->upstream_fd = -1;
    c->last_active_ms = now_ms();
    s->conns[i] = c;
    s->nconns++;
    return 0;
}

/* 연결의 모든 자원을 정리하는 유일한 함수. 이미 닫힌 칸에 다시 불러도 아무 일도 하지 않는다 */
static void conn_close(server *s, size_t slot)
{
    conn *c = s->conns[slot];

    if (c == NULL)
        return;
    close(c->fd);
    if (c->upstream_fd >= 0)
        close(c->upstream_fd);
    static_close(&c->file);
    buffer_free(&c->in);
    buffer_free(&c->out);
    buffer_free(&c->up_out);
    free(c);
    s->conns[slot] = NULL;
    s->nconns--;
    s->accept_resume_ms = 0;    /* fd 가 하나 생겼으니 쉬던 accept 를 다시 시도한다 */
}

/* ---------------------------------------------------------------
 * 요청 처리 (docs/interfaces.md 2~4단계)
 * --------------------------------------------------------------- */

/* ROUTE_STATIC: GET/HEAD 만, 파일을 열고 200 헤더를 out 에. 본문은 SENDING_FILE 에서 보낸다.
 * 반환 0 (오류 응답 포함), -1 응답을 못 씀 */
static int respond_static(server *s, conn *c, int head)
{
    http_response_head h;
    int status;

    if (c->req.method != HTTP_GET && c->req.method != HTTP_HEAD)
        return http_append_error(&c->out, 405, c->keep_alive, head, "Allow: GET, HEAD\r\n");
    if (static_open(s->cfg->docroot, c->req.path, &c->file, &status) < 0)
        return http_append_error(&c->out, status, c->keep_alive, head, NULL);
    /* Content-Length 는 long (http_response_head). long 이 32비트인 환경에서 2GB 넘는 파일은
     * 길이가 잘려 응답 경계가 어긋나므로 500 으로 답한다 (CI 는 모두 64비트라 지금은 안 걸림) */
    if (c->file.size > (off_t)LONG_MAX) {
        static_close(&c->file);
        return http_append_error(&c->out, 500, c->keep_alive, head, NULL);
    }

    memset(&h, 0, sizeof h);
    h.status = 200;
    h.content_type = c->file.content_type;
    h.content_length = (long)c->file.size;
    h.keep_alive = c->keep_alive;
    if (http_append_header(&c->out, &h) < 0) {
        static_close(&c->file);
        return -1;
    }
    if (head)
        static_close(&c->file);   /* HEAD: 헤더(Content-Length 포함)만, 본문은 보내지 않는다 */
    return 0;
}

/*
 * 파서가 DONE 을 돌려준 요청 하나의 응답을 out 에 만든다. 처리 순서: 프록시 → 앱 → 정적.
 * 반환 0 (4xx·5xx 응답을 쓴 것도 성공), -1 응답을 못 씀 (메모리 부족 → 연결 닫기)
 */
static int respond(server *s, conn *c)
{
    proxy_route *route;
    int head = c->req.method == HTTP_HEAD;

    switch (handler_route(&c->req, s->has_proxy ? &s->proxy : NULL, &route)) {
    case ROUTE_PROXY:
        /* TODO(#9): proxy_build_request → proxy_connect_start → CONN_UPSTREAM.
         * #9 전까지는 프록시 경로에 502 로 답하고 닫는다 (#8 설정 로드가 머지되면 -c 로 여기 온다) */
        c->keep_alive = 0;
        return http_append_error(&c->out, 502, 0, head, NULL);
    case ROUTE_APP:
        if (app_handle(&c->req, s->cfg->port, &c->out) == 0)
            return 0;
        /* TODO(#10): app_handle 은 메모리 부족일 때만 -1 이지만, #10 전까지는 스텁이 항상 -1 이다.
         * 연결을 그냥 끊지 않고 500 으로 답한다 (out 은 app_handle 이 그대로 둠).
         * #10 뒤에도 남겨도 무해하다: 정말 메모리가 없으면 500 도 못 써서 -1 → 연결 닫기 */
        c->keep_alive = 0;
        return http_append_error(&c->out, 500, 0, head, NULL);
    case ROUTE_STATIC:
    default:
        return respond_static(s, c, head);
    }
}

/*
 * in 에 쌓인 바이트로 요청 하나를 처리해 본다 (READING 상태에서만 부른다).
 * 응답을 만들었으면 WRITING/CLOSING 으로 바뀐다. 반환 0 계속, -1 연결을 닫아야 함
 */
static int handle_request(server *s, conn *c)
{
    size_t pending = buffer_pending(&c->in), consumed = 0;
    int status = 0;
    parse_result r;

    if (pending == 0)
        return 0;
    /* 본문만 덜 온 요청: 다 모일 때까지 파서를 다시 부르지 않는다 (PR #24).
     * 바이트마다 헤더를 다시 파싱하지 않게. 소켓 읽기·타임아웃은 그대로 진행된다 */
    if (c->need_len != 0 && pending < c->need_len)
        return 0;

    r = http_parse_request(buffer_peek(&c->in), pending, &c->req, &consumed, &status);
    if (r == PARSE_NEED_MORE) {
        c->need_len = consumed;   /* 헤더가 아직이면 0: 다음 read 뒤에 다시 파싱 */
        return 0;
    }
    c->need_len = 0;              /* DONE·ERROR: 다음 요청에 남기지 않는다 */
    c->req_start_ms = 0;

    if (r == PARSE_ERROR) {
        /* 요청이 깨져 다음 요청이 어디서 시작하는지 모른다 → 오류 응답 후 닫는다.
         * HEAD 면 본문을 쓰지 않는다 (RFC 9110 9.3.2). 파서는 req 를 0(= HTTP_GET)으로 지우고
         * 요청 라인에서 "HEAD" 를 읽었을 때만 HTTP_HEAD 로 바꾸므로, 요청 라인 전에 난 오류(431 등)는
         * GET 으로 남아 본문까지 쓴다. 어차피 닫으므로 그래도 다음 응답과 섞이지 않는다.
         * (ERROR 때 method 는 http_request.h 계약에 아직 없다. PR #29 에서 상철님께 확인 중) */
        c->keep_alive = 0;
        if (http_append_error(&c->out, status, 0, c->req.method == HTTP_HEAD, NULL) < 0)
            return -1;
        c->state = CONN_CLOSING;
        return 0;
    }

    c->keep_alive = c->req.keep_alive;
    if (respond(s, c) < 0)
        return -1;
    /* 응답을 다 만든 뒤에 consume (req.headers/body 가 in 안을 가리킨다) */
    buffer_consume(&c->in, consumed);
    c->state = CONN_WRITING;
    return 0;
}

/* ---------------------------------------------------------------
 * 읽기·쓰기 (한 번 부를 때 시스템 콜 한 번. 막히면 바로 루프로 돌아간다)
 * --------------------------------------------------------------- */

/* 응답을 끝까지 보낸 뒤 연결을 닫는다: 바로 close 하지 않고 송신만 닫는다 (lingering close).
 * 클라이언트가 보낸 것을 다 읽지 않은 채 close 하면 커널이 RST 를 보내서, 클라이언트가 아직
 * 읽지 않은 응답(431, 413 등)까지 버려질 수 있다. FIN 을 보내 응답이 끝났음을 알리고,
 * 들어오는 건 읽어 버리다가 클라이언트가 닫거나(EOF) 마감이 되면 닫는다 */
static void conn_linger(server *s, size_t slot)
{
    conn *c = s->conns[slot];

    if (shutdown(c->fd, SHUT_WR) < 0) {
        conn_close(s, slot);          /* 이미 끊긴 연결 등 */
        return;
    }
    buffer_free(&c->in);              /* 더는 요청을 처리하지 않는다 */
    buffer_free(&c->out);
    c->state = CONN_LINGERING;
    c->linger_end_ms = now_ms() + CLIENT_LINGER_TIMEOUT_MS;
}

/* 응답 하나를 끝까지 보냈다 (out + 파일). keep-alive 면 다음 요청으로, 아니면 닫기 */
static void conn_response_done(server *s, size_t slot)
{
    conn *c = s->conns[slot];

    if (c->state == CONN_CLOSING || !c->keep_alive) {
        conn_linger(s, slot);
        return;
    }
    c->state = CONN_READING;
    /* 파이프라이닝: 클라이언트가 응답을 기다리지 않고 보낸 다음 요청이 in 에 이미 있으면
     * 새 바이트를 기다리지 않고 바로 처리한다 (안 그러면 poll 이 깨워 주지 않아 멈춘다) */
    if (buffer_pending(&c->in) > 0) {
        c->req_start_ms = now_ms();
        if (handle_request(s, c) < 0)
            conn_close(s, slot);
    }
}

/* LINGERING: 받은 바이트는 버린다. EOF·오류면 닫는다 */
static void conn_drain(server *s, size_t slot)
{
    char tmp[4096];
    ssize_t n = read(s->conns[slot]->fd, tmp, sizeof tmp);

    if (n == 0 || (n < 0 && !would_block(errno)))
        conn_close(s, slot);
}

static void conn_on_readable(server *s, size_t slot)
{
    conn *c = s->conns[slot];
    ssize_t n;

    if (c->state == CONN_LINGERING) {
        conn_drain(s, slot);
        return;
    }
    /* reserve 가 compact 로 데이터를 옮길 수 있지만, 여기 올 때는 in 을 가리키는 req 포인터를
     * 쓰는 중이 아니다 (응답을 만들고 consume 한 뒤에만 다시 읽는다) */
    if (buffer_reserve(&c->in, READ_CHUNK) < 0) {
        conn_close(s, slot);
        return;
    }
    n = read(c->fd, c->in.data + c->in.len, c->in.cap - c->in.len);
    if (n < 0) {
        if (!would_block(errno))
            conn_close(s, slot);   /* ECONNRESET 등: 이 연결만 닫는다 */
        return;
    }
    if (n == 0) {                  /* 클라이언트가 연결을 닫음 */
        conn_close(s, slot);
        return;
    }
    c->in.len += (size_t)n;
    c->last_active_ms = now_ms();
    if (c->req_start_ms == 0)
        c->req_start_ms = c->last_active_ms;   /* 새 요청의 첫 바이트 */
    if (handle_request(s, c) < 0)
        conn_close(s, slot);
}

/* SENDING_FILE: 파일 조각 하나 (static_send_some 이 pread + write 한 번) */
static void conn_send_file(server *s, size_t slot)
{
    conn *c = s->conns[slot];
    int r = static_send_some(&c->file, c->fd);

    if (r < 0) {
        conn_close(s, slot);       /* 응답 도중 실패: 이미 헤더를 보냈으니 닫는 수밖에 없다 */
        return;
    }
    c->last_active_ms = now_ms();
    if (r == 0)
        return;                    /* 남은 건 다음 POLLOUT 에 */
    /* 다 보냈으면 바로 닫는다. keep-alive 로 다음 요청의 static_open 이 덮어쓰면 fd 가 샌다
     * (PR #25 리뷰). conn_close 의 static_close 와 겹쳐도 안전하다 */
    static_close(&c->file);
    conn_response_done(s, slot);
}

static void conn_on_writable(server *s, size_t slot)
{
    conn *c = s->conns[slot];
    ssize_t n;

    if (c->state == CONN_SENDING_FILE) {
        conn_send_file(s, slot);
        return;
    }
    n = write(c->fd, buffer_peek(&c->out), buffer_pending(&c->out));
    if (n < 0) {
        if (!would_block(errno))
            conn_close(s, slot);   /* EPIPE, ECONNRESET: 이 연결만 닫는다 */
        return;
    }
    buffer_consume(&c->out, (size_t)n);
    c->last_active_ms = now_ms();
    if (buffer_pending(&c->out) > 0)
        return;                    /* 남은 건 다음 POLLOUT 에 */

    if (c->file.fd >= 0) {         /* 헤더를 다 보냈다. 본문은 파일에서 */
        c->state = CONN_SENDING_FILE;
        return;
    }
    conn_response_done(s, slot);
}

/* ---------------------------------------------------------------
 * accept
 * --------------------------------------------------------------- */

static void accept_ready(server *s)
{
    int n;

    for (n = 0; n < ACCEPT_PER_PASS && !stop_requested; n++) {
        int fd = accept(s->listen_fd, NULL, NULL);

        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            /* 대기열에 있던 연결이 그 사이 끊김 등: 다음 연결로 */
            if (errno == EINTR || errno == ECONNABORTED || errno == EPROTO)
                continue;
            /* EMFILE·ENFILE(fd 고갈), ENOBUFS·ENOMEM 등: Tiny 처럼 종료하지 않고 잠깐 쉰다 */
            fprintf(stderr, "webserver: accept: %s (pausing %d ms)\n",
                    strerror(errno), ACCEPT_PAUSE_MS);
            s->accept_resume_ms = now_ms() + ACCEPT_PAUSE_MS;
            return;
        }
        /* 연결 수 상한(SERVER_MAX_CONNS)·메모리 부족이면 받자마자 닫는다 */
        if (conn_open(s, fd) < 0)
            close(fd);
    }
}

/* ---------------------------------------------------------------
 * 이벤트 루프
 * --------------------------------------------------------------- */

/* 연결 하나가 닫혀야 하는 시각 (상태에 따라 가장 이른 마감) */
static int64_t conn_deadline(const conn *c)
{
    int64_t d;

    if (c->state == CONN_LINGERING)
        return c->linger_end_ms;
    d = c->last_active_ms + CLIENT_IDLE_TIMEOUT_MS;
    if (c->state == CONN_READING && c->req_start_ms != 0 &&
        c->req_start_ms + CLIENT_REQUEST_TIMEOUT_MS < d)
        d = c->req_start_ms + CLIENT_REQUEST_TIMEOUT_MS;
    return d;
}

/* 마감을 넘긴 연결을 닫고, 다음 poll 대기 시간(ms, 없으면 -1)을 돌려준다.
 * 마감: 유휴(바이트가 오가지 않음), 요청 하나를 받는 시간(slow loris), lingering */
static int expire_idle(server *s, int64_t now)
{
    int64_t next = -1, left;
    size_t i;

    for (i = 0; i < SERVER_MAX_CONNS; i++) {
        conn *c = s->conns[i];

        if (c == NULL)
            continue;
        left = conn_deadline(c) - now;
        if (left <= 0)
            conn_close(s, i);
        else if (next < 0 || left < next)
            next = left;
    }
    if (s->accept_resume_ms != 0) {
        left = s->accept_resume_ms - now;
        if (left <= 0)
            s->accept_resume_ms = 0;
        else if (next < 0 || left < next)
            next = left;
    }
    return (int)next;
}

static size_t build_pollfds(server *s)
{
    size_t n = PFD_FIRST_CONN, i;

    s->pfds[PFD_SIGNAL].fd = sig_pipe[0];
    s->pfds[PFD_SIGNAL].events = POLLIN;
    /* 음수 fd 는 poll 이 무시한다 (accept 를 쉬는 동안) */
    s->pfds[PFD_LISTEN].fd = s->accept_resume_ms != 0 ? -1 : s->listen_fd;
    s->pfds[PFD_LISTEN].events = POLLIN;

    for (i = 0; i < SERVER_MAX_CONNS; i++) {
        conn *c = s->conns[i];

        if (c == NULL)
            continue;
        s->pfds[n].fd = c->fd;
        s->pfds[n].events =
            (c->state == CONN_READING || c->state == CONN_LINGERING) ? POLLIN : POLLOUT;
        s->pfd_slot[n] = i;
        n++;
    }
    for (i = 0; i < n; i++)
        s->pfds[i].revents = 0;
    return n;
}

static void dispatch(server *s, size_t npfds)
{
    size_t i;

    for (i = PFD_FIRST_CONN; i < npfds; i++) {
        size_t slot = s->pfd_slot[i];
        short re = s->pfds[i].revents;
        conn *c = s->conns[slot];

        if (re == 0 || c == NULL)
            continue;
        if (re & (POLLERR | POLLNVAL)) {
            conn_close(s, slot);
        } else if (c->state == CONN_READING || c->state == CONN_LINGERING) {
            if (re & (POLLIN | POLLHUP))     /* HUP 이면 read 가 0(EOF)을 돌려줘 닫힌다 */
                conn_on_readable(s, slot);
        } else if (re & (POLLOUT | POLLHUP)) {
            conn_on_writable(s, slot);       /* HUP 이면 write 가 EPIPE 로 실패해 닫힌다 */
        }
    }
    if (s->pfds[PFD_SIGNAL].revents & POLLIN)
        drain_signal_pipe();
    if (s->pfds[PFD_LISTEN].fd >= 0 && (s->pfds[PFD_LISTEN].revents & POLLIN))
        accept_ready(s);   /* 연결 이벤트를 다 처리한 뒤에 (파일 맨 위 주석 3) */
}

static void log_listening(const server *s)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    int v6;

    memset(&ss, 0, sizeof ss);
    v6 = getsockname(s->listen_fd, (struct sockaddr *)&ss, &len) == 0 && ss.ss_family == AF_INET6;
    fprintf(stderr, "webserver: listening on port %s (%s), docroot %s%s%s\n",
            s->cfg->port, v6 ? "IPv6+IPv4" : "IPv4 only", s->cfg->docroot,
            s->has_proxy ? ", proxy config " : "", s->has_proxy ? s->cfg->proxy_conf : "");
}

/* 새 연결을 먼저 막고(listen 닫기) → 모든 연결·파일·업스트림 → 시그널 파이프 순으로 정리 */
static void shutdown_all(server *s)
{
    size_t i;

    if (s->listen_fd >= 0)
        close(s->listen_fd);
    s->listen_fd = -1;
    for (i = 0; i < SERVER_MAX_CONNS; i++)
        conn_close(s, i);
    close_signal_pipe();
}

int server_run(const server_config *cfg)
{
    server *s;
    int ret = 0;

    s = calloc(1, sizeof *s);
    if (s == NULL) {
        perror("webserver: calloc");
        return -1;
    }
    s->cfg = cfg;
    s->listen_fd = -1;
    stop_requested = 0;

    if (cfg->proxy_conf != NULL) {
        if (proxy_config_load(cfg->proxy_conf, &s->proxy) < 0) {   /* 구현: #8 */
            fprintf(stderr, "webserver: cannot load proxy config '%s'\n", cfg->proxy_conf);
            ret = -1;
            goto out;
        }
        s->has_proxy = 1;
    }
    if (setup_signals() < 0) {
        perror("webserver: signal setup");
        ret = -1;
        goto out;
    }
    s->listen_fd = net_listen(cfg->port);
    if (s->listen_fd < 0) {
        fprintf(stderr, "webserver: cannot listen on port %s: %s\n", cfg->port, strerror(errno));
        ret = -1;
        goto out;
    }
    log_listening(s);

    while (!stop_requested) {
        int timeout = expire_idle(s, now_ms());
        size_t npfds = build_pollfds(s);
        int rc = poll(s->pfds, (nfds_t)npfds, timeout);

        if (rc < 0) {
            if (errno == EINTR)
                continue;   /* 시그널: 루프 조건에서 stop_requested 확인 */
            perror("webserver: poll");
            ret = -1;
            break;
        }
        if (rc > 0)
            dispatch(s, npfds);
    }
    if (ret == 0)
        fprintf(stderr, "webserver: shutting down (%zu connections)\n", s->nconns);

out:
    shutdown_all(s);
    free(s);
    return ret;
}
