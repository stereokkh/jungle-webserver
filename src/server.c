/*
 * server.c — 이벤트 루프 (ev.h: poll·epoll·kqueue 중 빌드 때 하나), 연결 수명주기, 시그널/종료
 *
 * CS:APP 대응: tiny.c main()
 *   Tiny:  listenfd = Open_listenfd(port);
 *          while (1) { connfd = Accept(...); doit(connfd); Close(connfd); }
 *   → 한 번에 연결 하나. 요청을 다 보내지 않는 클라이언트 하나가 다른 모든 연결을 기다리게 한다.
 *   여기서는 listen fd + 모든 연결 fd 를 감시하고, 연결마다 상태(conn)를 두어
 *   읽기/처리/쓰기를 "지금 할 수 있는 만큼만" 진행한 뒤 루프로 돌아온다.
 *
 * 루프 한 바퀴 (docs/event-backend.md):
 *   1. 유휴 시간을 넘긴 연결을 닫고, 대기 시간을 가장 먼저 만료될 연결 기준으로 정한다
 *   2. build_interest: 감시 대상마다 지금 원하는 이벤트를 ev_want(IN 또는 OUT 하나) 또는 ev_pause 로 알린다.
 *      프록시 중인 연결은 클라이언트 fd 와 업스트림 fd 둘. 이벤트마다 tag(fd·칸·종류·칸 세대)가 붙어 돌아온다
 *   3. ev_wait → 연결 이벤트 처리 → 마지막에 시그널·accept
 *      (accept 를 먼저 하면 방금 닫은 칸에 새 연결이 들어가서, 같은 칸을 가리키는
 *       이전 이벤트가 새 연결에 잘못 적용될 수 있다. 칸 세대도 이것을 막는다)
 *   감시하던 fd 는 close_watched(= ev_forget + close)로만 닫는다 (같은 번호의 새 fd 가 등록되지 않는 함정)
 *
 * 요청 하나의 흐름 (docs/interfaces.md 1~6단계):
 *   READING ─파서 DONE→ 응답을 out 에 만듦 → WRITING ─out 다 보냄→ (정적 파일이면) SENDING_FILE
 *     → keep-alive 면 READING (남은 입력에 다음 요청이 있으면 바로 처리 = 파이프라이닝)
 *     → 아니면 LINGERING (송신만 닫고 남은 입력을 읽어 버리다가 닫음)
 *   파서 오류 → 오류 응답 → CLOSING ─다 보냄→ LINGERING
 *   프록시 경로 (#9): 응답을 만들지 않고 업스트림 연결을 시작한다
 *     UP_CONNECTING ─연결됨→ UP_SENDING ─요청 다 보냄→ UP_RECEIVING ─응답 끝→ WRITING (남은 out 을 보내고 닫기)
 *     응답 시작 전 실패는 502/504 를 out 에 쓰고 WRITING, 시작 후 실패는 남은 out 만 보내고 닫는다.
 *     프록시 응답 뒤에는 항상 연결을 닫는다 (keep-alive 안 함). 연결 하나가 감시 fd 를 최대 둘 쓴다
 *     페일오버 (라우트에 failover= 가 있을 때만): UP_CONNECTING 에서 connect 가 실패하거나 시간이 넘으면 업스트림 fd 를
 *     닫고 다음 백엔드로 UP_CONNECTING 을 다시 한다. 요청 바이트는 UP_SENDING 부터 나가므로 그 전에만 되고,
 *     그래서 같은 요청(POST 포함)이 두 백엔드에서 실행되지 않는다. 시계(up_start_ms)는 첫 시도부터 한 번만 잰다
 *
 * Tiny 와 다른 점: 요청마다 연결을 닫지 않는다 (keep-alive), 한 연결이 느려도 다른 연결은
 * 계속 처리된다, 요청 하나를 받는 시간에 상한이 있다, 오류 응답이 RST 로 사라지지 않게
 * lingering close 를 한다.
 *
 * 담당: 트랙 A · 이슈 #7 (프록시 상태 통합은 #9)
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
#include "proxy.h"
#include "ev.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <limits.h>
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
    CONN_UP_CONNECTING,  /* 프록시: 업스트림 connect 진행 중 (업스트림 POLLOUT) */
    CONN_UP_SENDING,     /* 프록시: 다시 쓴 요청을 업스트림에 보내는 중 (업스트림 POLLOUT) */
    CONN_UP_RECEIVING,   /* 프록시: 응답을 읽어 out 으로 중계 중 (업스트림 POLLIN, 시작 후 out 이 있으면 클라이언트 POLLOUT) */
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
    buffer up_out;           /* 백엔드로 보낼 요청 (#9). out 처럼 off 가 송신 진행 상태. 시도마다 새로 만든다 (Host 가 백엔드 이름) */
    proxy_relay relay;       /* 업스트림 응답 머리 해석·본문 경계 (#9). calloc 0 이어도 free 해도 안전 */
    int64_t up_start_ms;     /* 요청을 프록시에 넘긴 시각 (응답 시작 전 마감의 기준). 페일오버해도 다시 잡지 않는다 */
    /* 페일오버 (이번 요청의 백엔드 선택 상태. 요청을 프록시에 넘길 때 respond_proxy 가 채운다) */
    proxy_route *up_route;   /* 이번 요청의 라우트. s->proxy 안을 가리키므로 서버 수명 동안 유효 */
    size_t up_rr_start;      /* proxy_rr_begin 이 돌려준 Round-Robin 시작 위치. 요청 하나에 한 번만 받는다 */
    unsigned up_tried;       /* 이미 시도한 백엔드 비트 (proxy_pick_backend 의 tried_mask) */
    size_t up_ntries;        /* 지금까지 시도한 백엔드 수 (route->tries 에 닿으면 멈춘다) */
    size_t up_backend;       /* 지금 connect 중인 백엔드의 인덱스 (실패하면 죽은 표시를 붙일 곳) */
    int64_t up_connect_ms;   /* 지금 시도의 connect 시작 시각 (PROXY_CONNECT_TIMEOUT_MS 의 기준) */
    int up_last_timeout;     /* 마지막 실패가 connect 시간 초과면 1, connect 오류면 0. 시도가 끝났을 때 504 / 502 를 가른다 */
    int64_t up_last_ms;      /* 시작 후: 마지막 업스트림 바이트 또는 클라이언트 쓰기 진행 시각 (역압력으로 쉰 시간은 안 센다) */
    char client_ip[INET6_ADDRSTRLEN];   /* X-Forwarded-For 용. accept 때 채운다 (C7) */
    int64_t last_active_ms;  /* CLIENT_IDLE_TIMEOUT_MS / UPSTREAM_TIMEOUT_MS 검사 */
    int64_t req_start_ms;    /* 지금 요청의 첫 바이트를 받은 시각, 없으면 0 (CLIENT_REQUEST_TIMEOUT_MS) */
    int64_t linger_end_ms;   /* LINGERING 의 마감 (CLIENT_LINGER_TIMEOUT_MS) */
    size_t need_len;         /* 파서 NEED_MORE 의 consumed: 이만큼 모이기 전엔 파서를 부르지 않음 */
    /* --access-log 용 (꺼져 있으면 log_active 는 늘 0 이고 나머지는 쓰지 않는다) */
    unsigned long long id;   /* 연결 번호 (서버 시작 후 1부터) */
    char client_port[8];
    int64_t open_ms;
    unsigned long nreq;      /* 이 연결에서 끝난 요청 수 */
    int log_active;          /* 처리 중인 요청이 있다: 응답이 끝나거나 연결이 닫힐 때 한 줄 */
    char log_req[192];       /* "METHOD target HTTP/1.x" (출력 가능한 ASCII 만) */
    int64_t log_start_ms;    /* 그 요청의 첫 바이트 시각 */
    int resp_status;         /* 클라이언트로 나간 상태 줄의 코드, 아직이면 0 */
    unsigned long long resp_bytes;   /* 클라이언트로 쓴 바이트 (머리 + 본문) */
    const char *close_why;   /* 닫는 이유. NULL 이면 닫을 때의 상태 이름 */
} conn;

/* ev tag 배치 (docs/event-backend.md 3-4): 0..31 fd, 32..47 conns 칸, 48..51 종류, 52..63 칸 세대 */
enum { TAG_CLIENT = 0, TAG_UPSTREAM = 1, TAG_LISTEN = 2, TAG_SIGNAL = 3 };
_Static_assert(SERVER_MAX_CONNS <= 65536, "conns slot must fit in 16 bits of the ev tag");

/* 한 바퀴에 감시하는 fd 상한: 시그널 + listen + 연결마다 클라이언트·업스트림 */
#define EV_MAX_WATCH (2 + 2 * (size_t)SERVER_MAX_CONNS)

/* respond() 의 반환: 응답을 out 에 만들었다 / 응답 대신 업스트림 연결을 시작했다 */
enum { RESPOND_OUT = 0, RESPOND_PROXY = 1 };

/* 서버 전체 상태. 배열이 커서(수십 KB) 스택이 아니라 힙에 둔다 */
typedef struct {
    const server_config *cfg;
    int listen_fd;
    int64_t accept_resume_ms;                   /* 0 이 아니면 이 시각까지 accept 쉼 */
    conn *conns[SERVER_MAX_CONNS];              /* 빈 칸은 NULL */
    size_t nconns;
    ev_loop *ev;                                /* 이벤트 백엔드 (ev.h) */
    unsigned gen[SERVER_MAX_CONNS];             /* 칸 세대: conn_close 마다 1 증가. 낡은 이벤트를 거른다 */
    proxy_config proxy;                        /* -c 가 있을 때만 (서버 수명 동안 유지) */
    int has_proxy;
    http_parse_options parse_opt;               /* 요청마다 같은 파서 옵션 (#31) */
    unsigned long long next_conn_id;            /* --access-log 의 연결 번호 */
} server;

static uint64_t tag_make(int fd, size_t slot, unsigned kind, unsigned gen)
{
    return (uint64_t)(uint32_t)fd | (uint64_t)(slot & 0xffffu) << 32 |
           (uint64_t)(kind & 0xfu) << 48 | (uint64_t)(gen & 0xfffu) << 52;
}

static int tag_fd(uint64_t t)        { return (int)(uint32_t)t; }
static size_t tag_slot(uint64_t t)   { return (size_t)(t >> 32 & 0xffffu); }
static unsigned tag_kind(uint64_t t) { return (unsigned)(t >> 48 & 0xfu); }
static unsigned tag_gen(uint64_t t)  { return (unsigned)(t >> 52 & 0xfffu); }

/* 감시 중일 수 있는 fd 는 이것으로만 닫는다: 백엔드의 등록 기록을 먼저 지운다 (ev.h ev_forget) */
static void close_watched(server *s, int fd)
{
    ev_forget(s->ev, fd);
    close(fd);
}

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
 * access log (--access-log, 기본 꺼짐)
 *   stderr 에 한 줄씩 쓴다. 기존 시작·종료 메시지와 같은 곳이라 `make run` 한 창에서 다 보인다.
 *     access: #3 open 127.0.0.1:51234 (active 1)
 *     access: #3 127.0.0.1:51234 "GET /healthz HTTP/1.1" 200 142 1ms
 *     access: #3 close idle-timeout requests=1 10012ms (active 0)
 *   요청 줄은 응답을 끝까지 보냈을 때(또는 그 전에 연결이 닫히면 "incomplete" 를 붙여) 쓴다.
 *   상태 코드는 모듈마다 응답을 out 에 쓰는 방식이 달라서(정적·앱·오류·프록시 중계) 따로 넘기지 않고,
 *   클라이언트로 처음 쓰는 상태 줄 "HTTP/1.x NNN" 에서 읽는다.
 *   stderr 는 버퍼링이 없어 줄이 바로 나가지만, 받는 쪽(파이프)이 막히면 poll 루프도 멈춘다.
 *   그래서 기본은 꺼 두고 관찰할 때만 켠다
 * --------------------------------------------------------------- */

static const char *conn_state_name(conn_state st)
{
    switch (st) {
    case CONN_READING:       return "reading";
    case CONN_WRITING:       return "writing";
    case CONN_SENDING_FILE:  return "sending-file";
    case CONN_UP_CONNECTING: return "upstream-connecting";
    case CONN_UP_SENDING:    return "upstream-sending";
    case CONN_UP_RECEIVING:  return "upstream-receiving";
    case CONN_CLOSING:       return "closing";
    case CONN_LINGERING:     return "lingering";
    default:                 return "?";
    }
}

/* 로그 한 줄에 요청에서 온 문자열을 넣을 때: 출력 가능한 ASCII 가 아니거나 '"' 면 '?' 로.
 * 요청 대상에 줄바꿈 등을 넣어 가짜 로그 줄을 만들지 못하게 한다 (파서가 대부분 막지만 한 번 더) */
static size_t log_put(char *dst, size_t size, size_t at, const char *src)
{
    for (; *src != '\0' && at + 1 < size; src++, at++) {
        unsigned char ch = (unsigned char)*src;

        dst[at] = (ch < 0x20 || ch > 0x7e || ch == '"') ? '?' : (char)ch;
    }
    dst[at] = '\0';
    return at;
}

/* 파서가 DONE/ERROR 를 돌려준 직후에 부른다 (req 가 아직 유효하고 req_start_ms 를 지우기 전) */
static void log_request_begin(server *s, conn *c, parse_result r)
{
    char ver[16];
    size_t at = 0;

    if (!s->cfg->access_log)
        return;
    c->log_active = 1;
    c->log_start_ms = c->req_start_ms != 0 ? c->req_start_ms : now_ms();
    c->resp_status = 0;
    c->resp_bytes = 0;
    if (r != PARSE_DONE) {
        /* 깨진 요청: 파서가 메서드(501 등)·대상까지 읽었으면 그만큼만, 아니면 "-".
         * 파서는 시작할 때 req 를 0 으로 지우므로 못 읽은 필드는 빈 문자열이다 */
        if (c->req.method_str[0] == '\0') {
            snprintf(c->log_req, sizeof c->log_req, "-");
            return;
        }
        at = log_put(c->log_req, sizeof c->log_req, at, c->req.method_str);
        if (c->req.target[0] != '\0') {
            at = log_put(c->log_req, sizeof c->log_req, at, " ");
            log_put(c->log_req, sizeof c->log_req, at, c->req.target);
        }
        return;
    }
    at = log_put(c->log_req, sizeof c->log_req, at, c->req.method_str);
    at = log_put(c->log_req, sizeof c->log_req, at, " ");
    at = log_put(c->log_req, sizeof c->log_req, at, c->req.target);
    snprintf(ver, sizeof ver, " HTTP/1.%d", c->req.version_minor);
    log_put(c->log_req, sizeof c->log_req, at, ver);
}

/* 클라이언트로 out 을 쓰기 직전에 부른다. 이번 응답의 첫 쓰기면 out 은 상태 줄로 시작한다 */
static void log_note_status(conn *c)
{
    const char *p = buffer_peek(&c->out);
    size_t n = buffer_pending(&c->out);

    if (!c->log_active || c->resp_status != 0 || n < 12 ||
        memcmp(p, "HTTP/1.", 7) != 0 || p[8] != ' ')
        return;
    if (p[9] >= '1' && p[9] <= '5' && p[10] >= '0' && p[10] <= '9' && p[11] >= '0' && p[11] <= '9')
        c->resp_status = (p[9] - '0') * 100 + (p[10] - '0') * 10 + (p[11] - '0');
}

/* 요청 하나가 끝났다 (응답을 다 보냄, 또는 note != NULL 이면 그 전에 연결이 닫힘) */
static void log_request_end(conn *c, const char *note)
{
    char status[8];

    if (!c->log_active)
        return;
    c->log_active = 0;
    c->nreq++;
    if (c->resp_status != 0)
        snprintf(status, sizeof status, "%d", c->resp_status);
    else
        snprintf(status, sizeof status, "-");
    fprintf(stderr, "access: #%llu %s:%s \"%s\" %s %llu %lldms%s%s\n",
            c->id, c->client_ip, c->client_port, c->log_req, status, c->resp_bytes,
            (long long)(now_ms() - c->log_start_ms), note != NULL ? " " : "", note != NULL ? note : "");
}

/* 상대 포트 (같은 IP 의 여러 연결을 ss 출력과 맞춰 보려고) */
static void fill_client_port(int fd, char *buf, size_t size)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;

    memset(&ss, 0, sizeof ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &len) < 0 ||
        getnameinfo((struct sockaddr *)&ss, len, NULL, 0, buf, (socklen_t)size, NI_NUMERICSERV) != 0)
        snprintf(buf, size, "?");
}

/* ---------------------------------------------------------------
 * 연결 생성·정리
 * --------------------------------------------------------------- */

/* 상대 주소를 숫자 문자열로 (X-Forwarded-For). 얻지 못하면 "unknown".
 * IPv6 소켓으로 들어온 IPv4 클라이언트("::ffff:1.2.3.4")는 "1.2.3.4" 로 쓴다 (C7) */
static void fill_client_ip(int fd, char *buf, size_t size)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;

    memset(&ss, 0, sizeof ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &len) < 0 ||
        getnameinfo((struct sockaddr *)&ss, len, buf, (socklen_t)size, NULL, 0, NI_NUMERICHOST) != 0) {
        snprintf(buf, size, "unknown");
        return;
    }
    {
        char *pct = strchr(buf, '%');               /* 링크 로컬 주소의 "%인터페이스" 꼬리는 헤더 값에 쓰지 않는다 */

        if (pct != NULL)
            *pct = '\0';
    }
    if (strncmp(buf, "::ffff:", 7) == 0 && strchr(buf, '.') != NULL)
        memmove(buf, buf + 7, strlen(buf + 7) + 1);
}

static int conn_open(server *s, int fd)
{
    size_t i;
    conn *c;

    if (s->nconns >= SERVER_MAX_CONNS || net_set_nonblocking(fd) < 0)
        return -1;
    /* 정적 파일은 머리와 본문을 따로 write 해서, Nagle 이 켜져 있으면 keep-alive 의 다음 응답마다
     * 상대의 지연 ACK 를 기다려 약 44ms 씩 멈췄다 (docs/benchmark.md). 실패해도 느릴 뿐이라 계속한다 */
    (void)net_set_nodelay(fd);
    for (i = 0; i < SERVER_MAX_CONNS && s->conns[i] != NULL; i++)
        ;
    c = calloc(1, sizeof *c);   /* in/out/up_out 은 0 으로 채워져 BUFFER_INIT 과 같다 */
    if (c == NULL)
        return -1;
    c->fd = fd;
    c->state = CONN_READING;
    c->file.fd = -1;            /* calloc 의 0 은 "fd 0 열림"으로 읽힌다 */
    c->upstream_fd = -1;
    fill_client_ip(fd, c->client_ip, sizeof c->client_ip);
    c->last_active_ms = now_ms();
    s->conns[i] = c;
    s->nconns++;
    if (s->cfg->access_log) {
        c->id = ++s->next_conn_id;
        c->open_ms = c->last_active_ms;
        fill_client_port(fd, c->client_port, sizeof c->client_port);
        fprintf(stderr, "access: #%llu open %s:%s (active %zu)\n",
                c->id, c->client_ip, c->client_port, s->nconns);
    }
    return 0;
}

/* 연결의 모든 자원을 정리하는 유일한 함수. 이미 닫힌 칸에 다시 불러도 아무 일도 하지 않는다 */
static void conn_close(server *s, size_t slot)
{
    conn *c = s->conns[slot];

    if (c == NULL)
        return;
    if (s->cfg->access_log) {
        log_request_end(c, "incomplete");   /* 응답을 끝내기 전에 닫힘 (끊김·시간 초과·송신 오류) */
        fprintf(stderr, "access: #%llu close %s requests=%lu %lldms (active %zu)\n",
                c->id, c->close_why != NULL ? c->close_why : conn_state_name(c->state), c->nreq,
                (long long)(now_ms() - c->open_ms), s->nconns - 1);
    }
    close_watched(s, c->fd);
    if (c->upstream_fd >= 0)
        close_watched(s, c->upstream_fd);
    static_close(&c->file);
    buffer_free(&c->in);
    buffer_free(&c->out);
    buffer_free(&c->up_out);
    proxy_relay_free(&c->relay);
    free(c);
    s->conns[slot] = NULL;
    s->gen[slot]++;             /* 이 칸을 가리키던 낡은 이벤트를 거른다 (dispatch) */
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

/* 업스트림 fd·요청·중계 상태를 정리한다 (conn_close 와 달리 클라이언트 연결은 둔다) */
static void up_release(server *s, conn *c)
{
    if (c->upstream_fd >= 0)
        close_watched(s, c->upstream_fd);
    c->upstream_fd = -1;
    buffer_free(&c->up_out);
    proxy_relay_free(&c->relay);
}

/* 응답 시작 전 전체 마감: 요청을 프록시에 넘긴 때(첫 시도)부터 UPSTREAM_TIMEOUT_MS. 페일오버해도 기준이 안 바뀐다 */
static int up_budget_spent(const conn *c, int64_t now)
{
    return now >= c->up_start_ms + UPSTREAM_TIMEOUT_MS;
}

/* 지금 시도의 connect 가 PROXY_CONNECT_TIMEOUT_MS 를 넘겼나. failover=timeout 인 라우트에서만 잰다
 * (없으면 connect 도 전체 마감 하나로 잰다). expire_idle 과 up_on_event 가 같은 판단을 쓰도록 한 곳에 둔다:
 * poll 이 연결 이벤트를 늦게 돌려줘도 마감을 넘긴 시도는 연결됐다고 받지 않고 시간 초과로 다룬다 */
static int up_connect_expired(const conn *c, int64_t now)
{
    return c->state == CONN_UP_CONNECTING && (c->up_route->failover & PROXY_FAILOVER_TIMEOUT) &&
           now >= c->up_connect_ms + PROXY_CONNECT_TIMEOUT_MS;
}

/* 업스트림 connect 가 지금 어디까지 왔나 (소켓에 물어본다). 전체 마감에 걸린 두 자리(expire_idle, up_on_event 맨 위)가
 * "그 백엔드를 죽었다고 표시할까"를 같은 판단으로 하도록 한 곳에 둔다.
 *   SO_ERROR != 0            → UP_CONN_FAILED  (거절·도달 불가 등)
 *   getpeername 성공         → UP_CONN_CONNECTED (연결은 이미 끝났고 POLLOUT 을 아직 처리하지 못했을 뿐: 멀쩡한 백엔드)
 *   getpeername ENOTCONN     → UP_CONN_PENDING (SYN 에 답이 없다)
 *   그 밖의 getpeername 오류 → UP_CONN_FAILED  (소켓이 이미 망가졌다)
 * 순서가 중요하다: 오류가 난 소켓도 getpeername 은 ENOTCONN 이라 SO_ERROR 를 먼저 봐야 PENDING 과 구별된다.
 * SO_ERROR 를 읽으면 그 오류는 지워진다. 이 함수를 부른 쪽은 fd 를 곧 닫으므로 괜찮다 */
typedef enum { UP_CONN_PENDING, UP_CONN_CONNECTED, UP_CONN_FAILED } up_conn_state;

static up_conn_state up_connect_state(int fd)
{
    struct sockaddr_storage peer;
    socklen_t len = sizeof peer;

    if (proxy_connect_result(fd) != 0)
        return UP_CONN_FAILED;
    if (getpeername(fd, (struct sockaddr *)&peer, &len) == 0)
        return UP_CONN_CONNECTED;
    return errno == ENOTCONN ? UP_CONN_PENDING : UP_CONN_FAILED;
}

/* 응답 시작 전 전체 마감에 걸린 연결의 현재 백엔드에 죽은 표시를 붙여야 하나.
 * connect 중이던 시도만 해당하고, 이미 연결된 백엔드(늦게 처리했을 뿐)는 아니다. 페일오버가 꺼진 라우트면 표시 자체가 no-op 이다 */
static int up_deadline_backend_failed(const conn *c)
{
    return c->state == CONN_UP_CONNECTING && up_connect_state(c->upstream_fd) != UP_CONN_CONNECTED;
}

/* 이번 요청의 다음 백엔드를 골라 up_out 을 새로 만들고 connect 를 시작한다. 첫 시도와 페일오버가 같은 함수를 쓴다.
 * 성공하면 0: upstream_fd 가 열려 있으니 UP_CONNECTING 에서 POLLOUT 을 기다리면 된다.
 * 실패하면 호출자가 클라이언트에게 보낼 상태 코드를 돌려준다. 이때 up_out 은 비우지 않고 둔다: 정리는 호출자의 up_release(up_fail 포함)가
 * 한 곳에서 한다 (실패 경로마다 책임이 같다. 다음 시도 앞의 buffer_free 는 새 요청을 만들 자리를 비우는 것일 뿐이다):
 *   431  다시 쓴 요청이 HTTP_MAX_HEADER 를 넘음 (결정 6). 첫 시도뿐 아니라 이름이 더 긴 백엔드로 넘어가다 넘어도 같다
 *   504  전체 마감을 넘겼다, 또는 더 시도할 수 없고 마지막 실패가 connect 시간 초과
 *   502  더 시도할 수 없고 마지막 실패가 connect 오류 (후보가 없거나 route->tries 에 닿은 것 포함)
 * proxy_connect_start 가 동기적으로 실패해도(fd 고갈·임시 포트 고갈은 PROXY_CONNECT_LOCAL, 즉시 거절은 -1) 한 번 실패로 세고, 페일오버가 켜져 있으면
 * 바로 다음 후보로 간다. 죽은 표시는 -1 일 때만 붙인다 (PROXY_CONNECT_LOCAL 은 이쪽 문제라 백엔드 탓이 아니다).
 * 반복은 시도마다 up_ntries 가 늘고 route->tries(<= PROXY_MAX_BACKENDS)에 닿으면 끝나므로 connect 를 무한히 돌지 않는다.
 * 전체 마감(첫 시도부터 UPSTREAM_TIMEOUT_MS)은 시도를 새로 열기 전, 요청을 다시 쓴 직후(connect 직전), 상태 코드를 정하기 전에 본다: 이미 넘었으면 새 connect 도
 * 502·431 도 아니고 504 다 (다른 곳은 expire_idle·up_on_event 가 같은 마감에 504 를 보내므로 같은 요청에 두 가지 답이 없게).
 * proxy_build_request 는 c->req 가 가리키는 in 을 읽는다. 그래서 handle_request 는 프록시 요청의 in 을 consume 하지 않는다 */
static int up_start_attempt(conn *c)
{
    proxy_route *route = c->up_route;

    while (c->up_ntries < route->tries) {
        int idx, fd;
        int64_t t;
        const proxy_backend *be;

        if (up_budget_spent(c, now_ms()))
            break;
        idx = proxy_pick_backend(route, now_ms(), c->up_tried, c->up_rr_start);
        if (idx < 0)
            break;
        be = &route->backends[idx];
        c->up_tried |= 1u << idx;
        c->up_ntries++;
        c->up_backend = (size_t)idx;
        buffer_free(&c->up_out);   /* 지난 시도의 요청은 Host 가 지난 백엔드 이름이다. 버리고 새로 만든다 */
        if (proxy_build_request(&c->req, be, c->client_ip, &c->up_out) < 0) {
            /* 파서·설정 로더가 막아 주므로 남는 실패는 "다시 쓴 머리가 HTTP_MAX_HEADER 초과"와 메모리 부족뿐.
             * 앞의 것이 현실적인 원인이라 431 로 답한다 */
            return up_budget_spent(c, now_ms()) ? 504 : 431;
        }
        /* 요청을 다시 쓰는 동안 전체 마감을 넘겼을 수 있다. 시각을 한 번만 읽어 마감을 보고, 남았으면 그 값이 곧 connect 시계의 시작이다.
         * 시각을 두 번 읽으면 그 사이에 마감이 지나도 connect 가 시작되고, 바로 실패하면 멀쩡한 백엔드를 죽었다고 표시한다.
         * 재작성 시간은 connect 시계가 아니라 전체 마감에만 든다 */
        t = now_ms();
        if (up_budget_spent(c, t))
            break;
        c->up_connect_ms = t;
        fd = proxy_connect_start(be);
        if (fd >= 0) {
            c->upstream_fd = fd;
            return 0;
        }
        c->up_last_timeout = 0;
        /* 이쪽 자원 문제(socket() 실패)는 connect 를 시도하지 않았으니 백엔드를 죽었다고 표시하지 않는다. 시도 횟수에는 센다.
         * 동기 connect 실패(ENETUNREACH, ECONNREFUSED 등)는 백엔드에 닿지 못한 것이라 표시한다 */
        if (fd != PROXY_CONNECT_LOCAL)
            proxy_mark_down(route, (size_t)idx, now_ms());   /* 페일오버가 꺼져 있으면 아무것도 하지 않는다 */
        if (!(route->failover & PROXY_FAILOVER_CONNECT))
            break;
    }
    return (c->up_last_timeout || up_budget_spent(c, now_ms())) ? 504 : 502;
}

/* ROUTE_PROXY: 다시 쓴 요청을 up_out 에 만들고 업스트림 connect 를 시작한다 (#9).
 * 프록시 응답 뒤에는 항상 연결을 닫으므로 keep_alive 는 0 (백엔드에도 Connection: close 를 보냈다).
 * 요청이 in 을 가리키는 동안(consume 전) 불러야 한다. 이 요청의 in 은 연결이 닫힐 때까지 consume 하지 않는다 (handle_request) */
static int respond_proxy(server *s, conn *c, proxy_route *route, int head)
{
    int64_t now = now_ms();   /* 결정 4a: 요청을 프록시에 넘긴 시각. 재구성·connect 시작 시간도 마감에 든다 */
    int code;

    c->keep_alive = 0;
    c->up_route = route;
    c->up_rr_start = proxy_rr_begin(route);   /* 순번은 요청당 한 칸. 페일오버가 다시 골라도 이 값으로 */
    c->up_tried = 0;
    c->up_ntries = 0;
    c->up_last_timeout = 0;
    proxy_relay_init(&c->relay, head);
    c->up_start_ms = now;
    c->up_last_ms = now;
    code = up_start_attempt(c);
    if (code != 0) {
        up_release(s, c);
        return http_append_error(&c->out, code, 0, head, NULL);
    }
    c->last_active_ms = now_ms();
    c->state = CONN_UP_CONNECTING;
    return RESPOND_PROXY;
}

/*
 * 파서가 DONE 을 돌려준 요청 하나의 응답을 out 에 만든다. 처리 순서: 헬스체크 → 프록시 → 앱 → 정적.
 * 반환 RESPOND_OUT (4xx·5xx 응답을 쓴 것도 성공), RESPOND_PROXY (업스트림 연결을 시작함: out 은 비어 있고
 * 상태는 UP_CONNECTING), -1 응답을 못 씀 (메모리 부족 → 연결 닫기)
 */
static int respond(server *s, conn *c)
{
    proxy_route *route;
    int head = c->req.method == HTTP_HEAD;

    switch (handler_route(&c->req, s->has_proxy ? &s->proxy : NULL, &route)) {
    case ROUTE_HEALTH:
        if (app_health(&c->req, s->cfg->port, &c->out) == 0)
            return RESPOND_OUT;
        c->keep_alive = 0;   /* 메모리 부족: 앱과 같이 500 (그것도 못 쓰면 -1 → 연결 닫기) */
        return http_append_error(&c->out, 500, 0, head, NULL);
    case ROUTE_PROXY:
        return respond_proxy(s, c, route, head);
    case ROUTE_APP:
        if (app_handle(&c->req, s->cfg->port, &c->out) == 0)
            return RESPOND_OUT;
        /* TODO(#10): app_handle 은 메모리 부족일 때만 -1 이지만, #10 전까지는 스텁이 항상 -1 이다.
         * 연결을 그냥 끊지 않고 500 으로 답한다 (out 은 app_handle 이 그대로 둠).
         * #10 뒤에도 남겨도 무해하다: 정말 메모리가 없으면 500 도 못 써서 -1 → 연결 닫기 */
        c->keep_alive = 0;
        return http_append_error(&c->out, 500, 0, head, NULL);
    case ROUTE_STATIC:
    default:
        return respond_static(s, c, head);   /* 0 == RESPOND_OUT */
    }
}

/*
 * in 에 쌓인 바이트로 요청 하나를 처리해 본다 (READING 상태에서만 부른다).
 * 응답을 만들었으면 WRITING/CLOSING 으로 바뀐다. 반환 0 계속, -1 연결을 닫아야 함
 */
static int handle_request(server *s, conn *c)
{
    size_t pending = buffer_pending(&c->in), consumed = 0;
    int status = 0, rr;
    parse_result r;

    if (pending == 0)
        return 0;
    /* 본문만 덜 온 요청: 다 모일 때까지 파서를 다시 부르지 않는다 (PR #24).
     * 바이트마다 헤더를 다시 파싱하지 않게. 소켓 읽기·타임아웃은 그대로 진행된다 */
    if (c->need_len != 0 && pending < c->need_len)
        return 0;

    r = http_parse_request(buffer_peek(&c->in), pending, &s->parse_opt, &c->req, &consumed, &status);
    if (r == PARSE_NEED_MORE) {
        c->need_len = consumed;   /* 헤더가 아직이면 0: 다음 read 뒤에 다시 파싱 */
        return 0;
    }
    c->need_len = 0;              /* DONE·ERROR: 다음 요청에 남기지 않는다 */
    log_request_begin(s, c, r);   /* req_start_ms 를 지우기 전에 (요청 처리 시간의 시작) */
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
    rr = respond(s, c);
    if (rr < 0)
        return -1;
    /* 프록시: respond_proxy 가 UP_CONNECTING 으로 정해 두었다. in 은 consume 하지 않는다. 페일오버가 요청을 다른
     * 백엔드용으로 다시 쓸 때 req.headers/body 가 가리키는 in 이 필요하기 때문이다. 프록시 중에는 클라이언트 입력을
     * 읽지 않으므로(C6) in 은 그대로 남고, 응답 뒤 conn_linger 가 비운다 (프록시 응답은 keep-alive 가 아니다) */
    if (rr == RESPOND_PROXY)
        return 0;
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

    log_request_end(c, NULL);     /* 파이프라이닝으로 다음 요청을 처리하기 전에 이번 요청을 쓴다 */
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

    if (n == 0 || (n < 0 && !would_block(errno))) {
        s->conns[slot]->close_why = "done";   /* 응답 뒤 클라이언트도 닫음: 정상 종료 */
        conn_close(s, slot);
    }
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
     * 쓰는 중이 아니다 (응답을 만들고 consume 한 뒤에만 다시 읽는다. 프록시 요청은 consume 하지 않지만 UP_* 상태에서는 이 함수가 불리지 않는다) */
    if (buffer_reserve(&c->in, READ_CHUNK) < 0) {
        conn_close(s, slot);
        return;
    }
    n = read(c->fd, c->in.data + c->in.len, c->in.cap - c->in.len);
    if (n < 0) {
        if (!would_block(errno)) {
            c->close_why = "read-error";
            conn_close(s, slot);   /* ECONNRESET 등: 이 연결만 닫는다 */
        }
        return;
    }
    if (n == 0) {                  /* 클라이언트가 연결을 닫음 */
        c->close_why = "client-closed";
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
        c->close_why = "write-error";
        conn_close(s, slot);       /* 응답 도중 실패: 이미 헤더를 보냈으니 닫는 수밖에 없다 */
        return;
    }
    c->last_active_ms = now_ms();
    if (r == 0)
        return;                    /* 남은 건 다음 POLLOUT 에 */
    c->resp_bytes += (unsigned long long)c->file.size;
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
    log_note_status(c);
    n = write(c->fd, buffer_peek(&c->out), buffer_pending(&c->out));
    if (n < 0) {
        if (!would_block(errno)) {
            c->close_why = "write-error";
            conn_close(s, slot);   /* EPIPE, ECONNRESET: 이 연결만 닫는다 */
        }
        return;
    }
    buffer_consume(&c->out, (size_t)n);
    c->resp_bytes += (unsigned long long)n;
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
 * 프록시: 업스트림 연결·송신·중계 (#9)
 * --------------------------------------------------------------- */

/* 응답 시작 전 실패(결정 2): 업스트림을 닫고 502/504 를 out 에 써서 WRITING 으로. 보낸 뒤 연결을 닫는다 */
static void up_fail(server *s, size_t slot, int code)
{
    conn *c = s->conns[slot];
    int head = c->relay.request_was_head;

    up_release(s, c);
    c->keep_alive = 0;
    c->last_active_ms = now_ms();   /* 클라이언트 유휴 타이머는 이 응답을 쓰는 시점부터 */
    if (http_append_error(&c->out, code, 0, head, NULL) < 0) {
        conn_close(s, slot);
        return;
    }
    c->state = CONN_WRITING;
}

/* 응답 시작 전 전체 마감(UPSTREAM_TIMEOUT_MS)을 넘겨 504 를 보낸다. expire_idle 과 up_on_event 가 같은 일을 하도록 한 곳에 둔다.
 * backend_failed 가 참이면 지금 시도 중인 백엔드에 죽은 표시를 붙인다 (페일오버가 꺼진 라우트면 proxy_mark_down 이 아무것도 안 한다).
 * connect 가 멈춘 채 마감까지 온 백엔드를 그냥 두면, failover=connect 만 켠 라우트(connect 시계가 없다)에서
 * 뒤이어 그 백엔드로 가는 요청마다 5초를 기다리고 504 를 받기 때문이다 */
static void up_fail_deadline(server *s, size_t slot, int backend_failed)
{
    conn *c = s->conns[slot];

    if (backend_failed)
        proxy_mark_down(c->up_route, c->up_backend, now_ms());
    up_fail(s, slot, 504);
}

/* 응답 끝(COMPLETE) 또는 시작 후 업스트림이 일찍 끊김(TRUNCATED): 업스트림을 닫고 out 에 남은 걸
 * 기존 WRITING 경로로 보낸 뒤 닫는다. out 이 이미 비었으면 바로 닫는 쪽으로 */
static void up_finish(server *s, size_t slot)
{
    conn *c = s->conns[slot];

    up_release(s, c);
    c->keep_alive = 0;
    c->state = CONN_WRITING;
    if (buffer_pending(&c->out) == 0)
        conn_response_done(s, slot);
}

/* UP_CONNECTING 에서 connect 가 실패했다 (timed_out == 0: SO_ERROR != 0) 또는 PROXY_CONNECT_TIMEOUT_MS 를 넘겼다 (1).
 * 어느 쪽이든 그 백엔드에 죽은 표시를 붙인다 (페일오버가 꺼진 라우트면 proxy_mark_down 이 아무것도 안 한다).
 * 이 종류의 페일오버가 켜져 있으면 업스트림 fd 를 닫고 다음 백엔드로 connect 를 다시 시작해 UP_CONNECTING 에 머문다.
 * 아니면, 또는 더 시도할 수 없으면 502 (connect 오류) / 504 (시간 초과, 또는 이미 전체 마감을 넘김).
 * 요청 바이트는 UP_SENDING 에서야 나가므로 여기서 다른 백엔드로 넘겨도 같은 요청이 두 번 실행되지 않는다.
 * 호출한 쪽은 이 함수가 돌아오면 upstream_fd 가 바뀌었을 수 있다고 보고 낡은 revents 로 더 처리하지 않는다 */
static void up_connect_failed(server *s, size_t slot, int timed_out)
{
    conn *c = s->conns[slot];
    proxy_route *route = c->up_route;
    unsigned kind = timed_out ? PROXY_FAILOVER_TIMEOUT : PROXY_FAILOVER_CONNECT;
    int code = timed_out ? 504 : 502;

    c->up_last_timeout = timed_out;
    close_watched(s, c->upstream_fd);   /* 다음 시도의 새 fd 가 같은 번호를 받기 쉽다 (ev_forget 이 없으면 등록이 빠진다) */
    c->upstream_fd = -1;
    proxy_mark_down(route, c->up_backend, now_ms());
    if (route->failover & kind) {
        code = up_start_attempt(c);
        if (code == 0)
            return;
    }
    /* 넘어갈 수 없는 경우(이 종류의 페일오버가 꺼짐, 시도 소진)도 최종 상태 코드를 정하는 이 자리에서 전체 마감을 다시 본다:
     * 마감이 이미 지났으면 502 가 아니라 504 다 (마감이 502·431 보다 앞선다. expire_idle 과 같은 답) */
    if (up_budget_spent(c, now_ms()))
        code = 504;
    up_fail(s, slot, code);
}

/* 클라이언트 out 에 더 쌓아도 되는 바이트 (PROXY_RELAY_MAX 까지) */
static size_t up_room(const conn *c)
{
    size_t pending = buffer_pending(&c->out);

    return pending < PROXY_RELAY_MAX ? PROXY_RELAY_MAX - pending : 0;
}

/* 역압력: relay 가 지금 더 받을 바이트가 0 (out 이 PROXY_RELAY_MAX 까지 찼거나 응답을 끝까지 받음).
 * 이때는 업스트림을 읽지 않고 업스트림 유휴 시계도 세지 않는다 (결정 4b) */
static int up_paused(const conn *c)
{
    return proxy_relay_want(&c->relay, up_room(c)) == 0;
}

static int is_up_state(conn_state st)
{
    return st == CONN_UP_CONNECTING || st == CONN_UP_SENDING || st == CONN_UP_RECEIVING;
}

/* UP_SENDING: up_out 을 업스트림으로. write 한 번, EAGAIN 이면 다음 POLLOUT 에 이어서 */
static void up_send(server *s, size_t slot)
{
    conn *c = s->conns[slot];
    ssize_t n = write(c->upstream_fd, buffer_peek(&c->up_out), buffer_pending(&c->up_out));

    if (n < 0) {
        if (!would_block(errno))
            up_fail(s, slot, 502);   /* EPIPE, ECONNRESET: 백엔드가 요청을 받지 못했다 */
        return;
    }
    buffer_consume(&c->up_out, (size_t)n);
    if (buffer_pending(&c->up_out) > 0)
        return;
    buffer_free(&c->up_out);
    c->state = CONN_UP_RECEIVING;
    c->up_last_ms = now_ms();
}

/* UP_RECEIVING: 업스트림에서 읽어 relay 에 먹인다. read 한 번 (남은 건 poll 이 다시 알려 준다) */
static void up_receive(server *s, size_t slot)
{
    conn *c = s->conns[slot];
    char tmp[16384];
    size_t want = proxy_relay_want(&c->relay, up_room(c));
    proxy_relay_result r;
    ssize_t n;

    if (want == 0)
        return;
    if (want > sizeof tmp)
        want = sizeof tmp;
    n = read(c->upstream_fd, tmp, want);
    if (n < 0 && would_block(errno))
        return;
    if (n > 0) {
        c->up_last_ms = now_ms();
        r = proxy_relay_input(&c->relay, tmp, (size_t)n, &c->out);
    } else {
        r = proxy_relay_eof(&c->relay);   /* n == 0 (EOF) 또는 read 오류: 둘 다 업스트림이 끝난 것 */
    }
    switch (r) {
    case PROXY_RELAY_MORE:
        return;
    case PROXY_RELAY_COMPLETE:
    case PROXY_RELAY_TRUNCATED:
        up_finish(s, slot);
        return;
    case PROXY_RELAY_NOMEM:
        if (c->relay.started)
            up_finish(s, slot);        /* 이미 응답이 나갔다: 대기 중인 out 은 보내고 닫는다 (결정 2) */
        else
            up_fail(s, slot, 502);
        return;
    case PROXY_RELAY_BAD:
    default:
        up_fail(s, slot, 502);
        return;
    }
}

/* 업스트림 fd 의 이벤트. 상태가 바뀌면 이어서 처리한다 (연결됨 → 바로 송신 시도) */
static void up_on_event(server *s, size_t slot)
{
    conn *c = s->conns[slot];

    /* 같은 poll 배치의 앞 이벤트를 처리하는 동안 마감이 지났을 수 있다. 시작 전이면 늦게 완성된 머리도 504 (결정 4a) */
    if (!c->relay.started && up_budget_spent(c, now_ms())) {
        /* 아직 connect 중이거나 실패한 백엔드는 죽은 것이다. 연결이 이미 끝났다면(POLLOUT 을 늦게 처리했을 뿐) 표시하지 않는다.
         * expire_idle 과 같은 판단이다 (up_deadline_backend_failed) */
        up_fail_deadline(s, slot, up_deadline_backend_failed(c));
        return;
    }
    /* 시작 후: 읽으려고 기다리던 마감이 이미 지났으면 늦게 온 바이트는 받지 않고 대기분만 보내고 닫는다 (결정 4b) */
    if (c->relay.started && c->state == CONN_UP_RECEIVING &&
        !up_paused(c) &&
        now_ms() >= c->up_last_ms + UPSTREAM_TIMEOUT_MS) {
        up_finish(s, slot);
        return;
    }
    if (c->state == CONN_UP_CONNECTING) {
        /* 이 시도의 connect 마감도 지났으면 (같은 배치의 앞 이벤트가 오래 걸렸거나 스케줄링이 밀렸을 때) 연결됐더라도
         * 요청을 보내지 않고 시간 초과로 다음 백엔드에 넘긴다. expire_idle 의 판단과 같다 (up_connect_expired) */
        if (up_connect_expired(c, now_ms())) {
            up_connect_failed(s, slot, 1);
            return;
        }
        /* POLLOUT·POLLERR·POLLHUP 어느 쪽이 와도 getsockopt(SO_ERROR) 만으로 판단 (결정 5) */
        if (proxy_connect_result(c->upstream_fd) != 0) {
            up_connect_failed(s, slot, 0);
            return;                /* 다른 백엔드의 새 fd 로 바뀌었을 수 있다: 이 revents 로 이어서 처리하지 않는다 */
        }
        /* 맨 위의 마감 검사와 여기 사이에 시간이 흘렀을 수 있다 (스케줄링이 밀림). 요청이 백엔드에 닿은 뒤에야 504 를 주면
         * 백엔드는 요청을 실행했는데 클라이언트는 실패로 본다. 첫 write 직전에 한 번 더 보고, 넘었으면 아무것도 쓰지 않는다.
         * 연결은 됐으니 그 백엔드는 죽은 표시 없이 */
        if (up_budget_spent(c, now_ms())) {
            up_fail_deadline(s, slot, 0);
            return;
        }
        c->state = CONN_UP_SENDING;
    }
    if (c->state == CONN_UP_SENDING) {
        up_send(s, slot);
        return;
    }
    if (c->state == CONN_UP_RECEIVING)
        up_receive(s, slot);   /* HUP 이어도 read 결과(남은 데이터 또는 0)로 판단 */
}

/* UP_* 상태의 클라이언트 fd 이벤트: 끊김은 모두 닫기, 응답 시작 후 쓰기 가능이면 out 을 보낸다.
 * 클라이언트 fd 는 out 에 바이트가 있을 때만 WS_EV_OUT 으로 감시하므로 (결정 1) 이벤트도 그때만 온다 */
static void up_on_client_event(server *s, size_t slot, unsigned re)
{
    conn *c = s->conns[slot];
    ssize_t n;
    int was_paused = up_paused(c);

    if (re & (WS_EV_ERR | WS_EV_HUP)) {
        c->close_why = "client-closed";
        conn_close(s, slot);       /* 업스트림 fd 도 같이 닫힌다 */
        return;
    }
    if (!(re & WS_EV_OUT) || buffer_pending(&c->out) == 0)
        return;
    log_note_status(c);
    n = write(c->fd, buffer_peek(&c->out), buffer_pending(&c->out));
    if (n < 0) {
        if (!would_block(errno)) {
            c->close_why = "write-error";
            conn_close(s, slot);
        }
        return;
    }
    buffer_consume(&c->out, (size_t)n);
    c->resp_bytes += (unsigned long long)n;
    c->last_active_ms = now_ms();
    /* 역압력으로 읽기를 쉬던 연결이 다시 읽게 된 순간에만 업스트림 유휴 시계를 새로 시작한다.
     * 쉬지 않았다면 클라이언트 쓰기는 업스트림 유휴와 상관없다 (결정 4b) */
    if (was_paused && !up_paused(c))
        c->up_last_ms = c->last_active_ms;
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
    if (is_up_state(c->state)) {
        /* 시계가 둘이다. 응답 시작 전: 요청을 넘긴 때부터 UPSTREAM_TIMEOUT_MS (넘으면 504).
         * 시작 후: 업스트림 유휴, 단 우리가 읽으려고 기다릴 때만 (역압력으로 멈춘 동안은 클라이언트 유휴가 맡는다) */
        int64_t u = -1;

        if (!c->relay.started)
            u = c->up_start_ms + UPSTREAM_TIMEOUT_MS;
        else if (!up_paused(c))
            u = c->up_last_ms + UPSTREAM_TIMEOUT_MS;
        if (u >= 0 && u < d)
            d = u;
        /* connect 시계: 라우트에 failover=timeout 이 있을 때만 (없으면 connect 도 전체 마감 UPSTREAM_TIMEOUT_MS 하나로 잰다) */
        if (c->state == CONN_UP_CONNECTING && (c->up_route->failover & PROXY_FAILOVER_TIMEOUT) &&
            c->up_connect_ms + PROXY_CONNECT_TIMEOUT_MS < d)
            d = c->up_connect_ms + PROXY_CONNECT_TIMEOUT_MS;
    }
    return d;
}

/* 마감을 넘긴 연결을 닫고, 남은 연결 중 가장 이른 마감 시각(now_ms 기준 절대 시각, 없으면 -1)을 돌려준다.
 * 마감: 유휴(바이트가 오가지 않음), 요청 하나를 받는 시간(slow loris), lingering.
 * 상대 시간(ms)이 아니라 절대 시각을 돌려주는 이유: 이 루프 안에서 페일오버 재작성·connect 가 시간을 쓰는데, 루프 시작의 now 로
 * 뺀 값을 poll 에 넘기면 그만큼 더 기다린다. poll_timeout_ms 가 poll 직전 시각으로 뺀다 */
static int64_t expire_idle(server *s, int64_t now)
{
    int64_t next = -1, d;
    size_t i;

    for (i = 0; i < SERVER_MAX_CONNS; i++) {
        conn *c = s->conns[i];

        if (c == NULL)
            continue;
        if (is_up_state(c->state) && !c->relay.started && up_budget_spent(c, now)) {
            /* 응답 시작 전 시간 초과: 닫지 않고 504 를 보낸다. 아직 connect 중이었다면 그 백엔드가 마감까지 답이 없던 것이다.
             * 단 소켓이 이미 연결됐다면(POLLOUT 이 아직 처리되지 않은 것) 표시하지 않는다 (up_deadline_backend_failed) */
            up_fail_deadline(s, i, up_deadline_backend_failed(c));
            c = s->conns[i];
            if (c == NULL)
                continue;
        }
        /* 전체 마감 검사 다음에: 이 시도의 connect 가 PROXY_CONNECT_TIMEOUT_MS 를 넘겼다. 다음 백엔드로 넘기거나 504.
         * 위의 마감과 아래 "마감이 지났으면 닫기" 사이에 둬야 한다. 그렇지 않으면 시간 초과한 연결이 응답 없이 닫힌다 */
        if (up_connect_expired(c, now)) {
            up_connect_failed(s, i, 1);
            c = s->conns[i];
            if (c == NULL)
                continue;
        }
        if (is_up_state(c->state) && c->relay.started &&
            !up_paused(c) &&
            now >= c->up_last_ms + UPSTREAM_TIMEOUT_MS) {
            up_finish(s, i);           /* 시작 후 업스트림 유휴: 이미 받아 둔 out 은 보내고 닫는다 (결정 2) */
            c = s->conns[i];
            if (c == NULL)
                continue;
        }
        d = conn_deadline(c);
        if (d <= now) {
            if (c->state == CONN_LINGERING)
                c->close_why = "linger-timeout";
            else if (c->state == CONN_READING && c->req_start_ms != 0 &&
                     now >= c->req_start_ms + CLIENT_REQUEST_TIMEOUT_MS)
                c->close_why = "request-timeout";   /* slow loris */
            else
                c->close_why = "idle-timeout";
            conn_close(s, i);
        } else if (next < 0 || d < next)
            next = d;
    }
    if (s->accept_resume_ms != 0) {
        if (s->accept_resume_ms <= now)
            s->accept_resume_ms = 0;
        else if (next < 0 || s->accept_resume_ms < next)
            next = s->accept_resume_ms;
    }
    return next;
}

/* expire_idle 이 돌려준 절대 마감 시각을 ev_wait 의 대기 시간(ms)으로 바꾼다. 마감이 없으면 -1(무한), 이미 지났으면 0.
 * 시각은 ev_wait 직전에 다시 읽는다: expire_idle 과 build_interest 가 쓴 시간이 대기 시간에서 빠진다 */
static int poll_timeout_ms(int64_t next_deadline)
{
    int64_t left;

    if (next_deadline < 0)
        return -1;
    left = next_deadline - now_ms();
    if (left <= 0)
        return 0;
    return left > INT_MAX ? INT_MAX : (int)left;
}

/* 감시 대상마다 지금 원하는 이벤트를 알린다 (docs/event-backend.md 4장 표). 매 바퀴 모든 대상에 ev_want 또는 ev_pause.
 *   READING·LINGERING → 클라이언트 IN / WRITING·SENDING_FILE·CLOSING → 클라이언트 OUT
 *   UP_CONNECTING·UP_SENDING → 클라이언트 pause, 업스트림 OUT
 *   UP_RECEIVING → 클라이언트는 out 에 바이트가 있으면 OUT 아니면 pause (결정 1), 업스트림은 역압력이면 pause 아니면 IN
 * 연결 fd 의 등록이 실패하면 그 연결만 닫는다 ("event-error"). 반환 -1: 시그널·listen 등록 실패 (서버를 끝낸다) */
static int build_interest(server *s)
{
    size_t i;

    ev_begin(s->ev);
    if (ev_want(s->ev, sig_pipe[0], tag_make(sig_pipe[0], 0, TAG_SIGNAL, 0), WS_EV_IN) < 0)
        return -1;
    if (s->accept_resume_ms != 0)
        ev_pause(s->ev, s->listen_fd);   /* accept 를 쉬는 동안 (fd 고갈) */
    else if (ev_want(s->ev, s->listen_fd, tag_make(s->listen_fd, 0, TAG_LISTEN, 0), WS_EV_IN) < 0)
        return -1;

    for (i = 0; i < SERVER_MAX_CONNS; i++) {
        conn *c = s->conns[i];
        unsigned g = s->gen[i];
        int rc = 0;

        if (c == NULL)
            continue;
        if (!is_up_state(c->state)) {
            rc = ev_want(s->ev, c->fd, tag_make(c->fd, i, TAG_CLIENT, g),
                         (c->state == CONN_READING || c->state == CONN_LINGERING) ? WS_EV_IN : WS_EV_OUT);
        } else {
            /* 프록시: 클라이언트 입력은 읽지 않는다 (C6) */
            if (c->state == CONN_UP_RECEIVING && buffer_pending(&c->out) > 0)
                rc = ev_want(s->ev, c->fd, tag_make(c->fd, i, TAG_CLIENT, g), WS_EV_OUT);
            else
                ev_pause(s->ev, c->fd);
            if (rc == 0 && c->state == CONN_UP_RECEIVING && up_paused(c))
                ev_pause(s->ev, c->upstream_fd);   /* 역압력: out 이 가득 차 읽을 바이트가 0 */
            else if (rc == 0)
                rc = ev_want(s->ev, c->upstream_fd, tag_make(c->upstream_fd, i, TAG_UPSTREAM, g),
                             c->state == CONN_UP_RECEIVING ? WS_EV_IN : WS_EV_OUT);
        }
        if (rc < 0) {
            fprintf(stderr, "webserver: event register: %s\n", strerror(errno));
            c->close_why = "event-error";
            conn_close(s, i);
        }
    }
    return 0;
}

/* 이벤트 처리. 순서는 백엔드마다 다르므로(ev.h) 기대지 않고, 낡은 이벤트는 tag 로 거른다:
 *   1. 칸이 비었다 (같은 배치의 앞 이벤트가 닫음)  2. tag 의 칸 세대가 지금과 다르다
 *   3. 업스트림 이벤트인데 tag 의 fd 가 지금 upstream_fd 가 아니거나 업스트림 상태가 아니다
 * 페일오버로 업스트림 fd 가 같은 번호로 바뀌어도 그 fd 의 이벤트는 한 배치에 하나뿐이고, 그것을 처리하는 중에 바뀐다.
 * 시그널·accept 는 표시만 해 두고 연결 이벤트를 다 처리한 뒤에 (파일 맨 위 주석 3) */
static void dispatch(server *s, const ev_event *evs, int nev)
{
    int i, sig = 0, acc = 0;

    for (i = 0; i < nev; i++) {
        uint64_t t = evs[i].tag;
        unsigned re = evs[i].events, kind = tag_kind(t);
        size_t slot = tag_slot(t);
        conn *c;

        if (kind == TAG_SIGNAL) {
            sig = 1;
            continue;
        }
        if (kind == TAG_LISTEN) {
            acc = 1;
            continue;
        }
        c = s->conns[slot];
        if (c == NULL || tag_gen(t) != (s->gen[slot] & 0xfffu))
            continue;
        if (kind == TAG_UPSTREAM) {
            if (c->upstream_fd != tag_fd(t) || !is_up_state(c->state))
                continue;
            up_on_event(s, slot);
        } else if (is_up_state(c->state)) {
            up_on_client_event(s, slot, re);
        } else if (re & WS_EV_ERR) {
            c->close_why = "client-closed";      /* RST 등 소켓 오류 (프록시 504 를 쓰려다 오기도 한다, 결정 1) */
            conn_close(s, slot);
        } else if (c->state == CONN_READING || c->state == CONN_LINGERING) {
            if (re & (WS_EV_IN | WS_EV_HUP))     /* HUP 이면 read 가 0(EOF)을 돌려줘 닫힌다 */
                conn_on_readable(s, slot);
        } else if (re & (WS_EV_OUT | WS_EV_HUP)) {
            conn_on_writable(s, slot);           /* HUP 이면 write 가 EPIPE 로 실패해 닫힌다 */
        }
    }
    if (sig)
        drain_signal_pipe();
    if (acc)
        accept_ready(s);
}

static void log_listening(const server *s)
{
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    int v6;

    memset(&ss, 0, sizeof ss);
    v6 = getsockname(s->listen_fd, (struct sockaddr *)&ss, &len) == 0 && ss.ss_family == AF_INET6;
    fprintf(stderr, "webserver: listening on port %s (%s, %s), docroot %s%s%s%s%s\n",
            s->cfg->port, v6 ? "IPv6+IPv4" : "IPv4 only", ev_backend(), s->cfg->docroot,
            s->has_proxy ? ", proxy config " : "", s->has_proxy ? s->cfg->proxy_conf : "",
            s->parse_opt.merge_slashes ? ", merge slashes" : "",
            s->cfg->access_log ? ", access log" : "");
}

/* 새 연결을 먼저 막고(listen 닫기) → 모든 연결·파일·업스트림 → 시그널 파이프 순으로 정리 */
static void shutdown_all(server *s)
{
    size_t i;

    if (s->listen_fd >= 0)
        close_watched(s, s->listen_fd);
    s->listen_fd = -1;
    for (i = 0; i < SERVER_MAX_CONNS; i++) {
        if (s->conns[i] != NULL)
            s->conns[i]->close_why = "shutdown";
        conn_close(s, i);
    }
    if (sig_pipe[0] >= 0)
        ev_forget(s->ev, sig_pipe[0]);
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
    s->parse_opt.merge_slashes = cfg->merge_slashes;
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
    s->ev = ev_open(EV_MAX_WATCH);
    if (s->ev == NULL) {
        fprintf(stderr, "webserver: cannot open %s: %s\n", ev_backend(), strerror(errno));
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
        int64_t next_deadline = expire_idle(s, now_ms());
        const ev_event *evs;
        int rc;

        if (build_interest(s) < 0) {
            perror("webserver: event register");
            ret = -1;
            break;
        }
        rc = ev_wait(s->ev, poll_timeout_ms(next_deadline), &evs);
        if (rc < 0) {
            if (errno == EINTR)
                continue;   /* 시그널: 루프 조건에서 stop_requested 확인 */
            fprintf(stderr, "webserver: %s: %s\n", ev_backend(), strerror(errno));
            ret = -1;
            break;
        }
        if (rc > 0)
            dispatch(s, evs, rc);
    }
    if (ret == 0)
        fprintf(stderr, "webserver: shutting down (%zu connections)\n", s->nconns);

out:
    shutdown_all(s);
    ev_close(s->ev);
    free(s);
    return ret;
}
