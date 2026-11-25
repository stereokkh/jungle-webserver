/*
 * ev_epoll.c — ev.h 의 epoll 백엔드 (Linux 전용, make EVENT=epoll)
 *
 * 이 저장소에서 Linux 전용 코드는 이 파일 하나에만 있다 (CLAUDE.md 이식성 규칙: OS 분기는 한 곳에).
 * Makefile 이 EVENT=epoll 일 때만 이 파일을 빌드하고, 기본(poll) 빌드에는 들어가지 않는다.
 *
 * poll 과 다른 점: 감시 목록이 커널에 남아 있다. 그래서 fd 마다 "지금 커널에 무엇으로 등록돼 있나"를
 * regs[fd] 에 기록해 두고, ev_want 가 다른 값(이벤트나 tag)을 요구할 때만 epoll_ctl 을 부른다.
 * 상태가 바뀌지 않은 연결은 바퀴마다 시스템 콜이 없다. 대신 상태가 바뀔 때마다(읽기 ↔ 쓰기) MOD 한 번이 든다.
 *
 * 함정 세 가지:
 *   1. 이벤트 0 으로 등록한 fd 도 EPOLLHUP·EPOLLERR 는 온다. poll 의 "음수 fd = 아예 안 봄"에 해당하는 것은
 *      등록을 지우는 것(DEL)뿐이다 → ev_pause. 0 으로 남겨 두면 끊긴 fd 가 레벨 트리거로 계속 깨워 루프가 돈다.
 *      그래서 ev_want 는 IN 이나 OUT 하나만 받는다 (명세 결정 2).
 *   2. close 하면 커널은 그 fd 를 감시 목록에서 저절로 뺀다 (fd 를 dup·fork 로 나누지 않으므로). 그래서 close 앞에
 *      DEL 시스템 콜은 하지 않고 regs 기록만 지운다 (ev_forget. nginx 도 닫을 fd 는 DEL 하지 않는다).
 *      기록을 지우지 않으면 같은 번호의 새 fd 를 "이미 등록됨"으로 보고 넘어가 그 fd 가 감시되지 않는다.
 *   3. POLLNVAL 에 해당하는 이벤트가 없다. 닫힌 fd 는 ev_want 의 epoll_ctl 이 EBADF 로 실패한다 (호출자가 처리).
 * 레벨 트리거만 쓴다 (EPOLLET 없음). 이유는 ev.h 맨 위.
 */
#if !defined(__linux__)
#error "ev_epoll.c is Linux only: build with the default EVENT=poll on this OS"
#endif

#include "ev.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

/* fd 하나의 현재 등록 상태 */
typedef struct {
    uint64_t tag;
    uint32_t events;   /* EPOLLIN / EPOLLOUT 조합 */
    int on;            /* 커널 감시 목록에 있다 */
} ev_reg;

struct ev_loop {
    int epfd;
    ev_reg *regs;      /* fd 번호로 찾는다. 필요할 때 늘린다 */
    size_t nregs;
    struct epoll_event *buf;
    ev_event *out;
    size_t cap;
};

const char *ev_backend(void)
{
    return "epoll";
}

ev_loop *ev_open(size_t max_watch)
{
    ev_loop *ev = calloc(1, sizeof *ev);

    if (ev == NULL)
        return NULL;
    ev->epfd = -1;
    ev->buf = calloc(max_watch, sizeof *ev->buf);
    ev->out = calloc(max_watch, sizeof *ev->out);
    if (ev->buf == NULL || ev->out == NULL) {
        ev_close(ev);
        errno = ENOMEM;
        return NULL;
    }
    ev->cap = max_watch;
    ev->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (ev->epfd < 0) {
        int saved = errno;

        ev_close(ev);
        errno = saved;
        return NULL;
    }
    return ev;
}

void ev_close(ev_loop *ev)
{
    if (ev == NULL)
        return;
    if (ev->epfd >= 0)
        close(ev->epfd);
    free(ev->regs);
    free(ev->buf);
    free(ev->out);
    free(ev);
}

void ev_begin(ev_loop *ev)
{
    (void)ev;   /* 등록은 커널에 남아 있다. 바뀐 것만 ev_want·ev_pause 가 고친다 */
}

/* regs 를 fd 번호까지 늘린다. 새 칸은 0 (등록 안 됨) */
static int regs_reserve(ev_loop *ev, int fd)
{
    size_t need = (size_t)fd + 1, n;
    ev_reg *p;

    if (need <= ev->nregs)
        return 0;
    n = ev->nregs != 0 ? ev->nregs : 64;
    while (n < need)
        n *= 2;
    p = realloc(ev->regs, n * sizeof *p);
    if (p == NULL) {
        errno = ENOMEM;
        return -1;
    }
    memset(p + ev->nregs, 0, (n - ev->nregs) * sizeof *p);
    ev->regs = p;
    ev->nregs = n;
    return 0;
}

int ev_want(ev_loop *ev, int fd, uint64_t tag, unsigned events)
{
    uint32_t want = events == WS_EV_IN ? (uint32_t)EPOLLIN : (uint32_t)EPOLLOUT;
    struct epoll_event e;
    ev_reg *r;

    if (events != WS_EV_IN && events != WS_EV_OUT) {   /* 정확히 하나 (명세 결정 2) */
        errno = EINVAL;
        return -1;
    }
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (regs_reserve(ev, fd) < 0)
        return -1;
    r = &ev->regs[fd];
    if (r->on && r->events == want && r->tag == tag)
        return 0;                    /* 지난 바퀴와 같다: 시스템 콜 없음 */
    memset(&e, 0, sizeof e);
    e.events = want;
    e.data.u64 = tag;
    if (epoll_ctl(ev->epfd, r->on ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, fd, &e) < 0)
        return -1;
    r->on = 1;
    r->events = want;
    r->tag = tag;
    return 0;
}

void ev_pause(ev_loop *ev, int fd)
{
    ev_reg *r;

    if (fd < 0 || (size_t)fd >= ev->nregs || !ev->regs[fd].on)
        return;
    r = &ev->regs[fd];
    /* 실패할 수 있는 경우는 이미 목록에 없을 때(ENOENT)뿐이다. 어느 쪽이든 이제 등록되지 않은 것으로 본다 */
    (void)epoll_ctl(ev->epfd, EPOLL_CTL_DEL, fd, NULL);
    r->on = 0;
}

void ev_forget(ev_loop *ev, int fd)
{
    if (ev == NULL || fd < 0 || (size_t)fd >= ev->nregs)
        return;
    ev->regs[fd].on = 0;   /* 커널 쪽은 곧 할 close 가 지운다 (맨 위 함정 2) */
}

int ev_wait(ev_loop *ev, int timeout_ms, const ev_event **out)
{
    int i, n = epoll_wait(ev->epfd, ev->buf, (int)ev->cap, timeout_ms);

    *out = ev->out;
    for (i = 0; i < n; i++) {
        uint32_t re = ev->buf[i].events;
        unsigned e = 0;

        if (re & EPOLLIN)
            e |= WS_EV_IN;
        if (re & EPOLLOUT)
            e |= WS_EV_OUT;
        if (re & EPOLLERR)
            e |= WS_EV_ERR;
        if (re & EPOLLHUP)
            e |= WS_EV_HUP;
        ev->out[i].tag = ev->buf[i].data.u64;
        ev->out[i].events = e;
    }
    return n;
}
