/*
 * ev_epoll.c — ev.h 의 epoll 백엔드 (Linux 전용, make EVENT=epoll)
 *
 * 등록을 커널에 남겨 두고 바뀐 fd 만 고친다. 그러려고 fd 마다 "지금 등록된 이벤트·tag" 를 기억한다 (rec).
 *   ev_want  : 기록과 같으면 시스템 콜 없음, 다르면 ADD 또는 MOD
 *   ev_pause : 등록돼 있으면 DEL (이벤트 0 으로 남기면 끊긴 fd 의 HUP·ERR 가 레벨 트리거로 계속 온다)
 *   ev_forget: 기록만 지운다 (close 가 커널 쪽 등록을 지운다)
 * 레벨 트리거만 (EPOLLET 금지). 명세: docs/event-backend.md
 */
#ifndef __linux__
#error "ev_epoll.c 는 Linux 전용이다. 다른 OS 에서는 make EVENT=poll (기본) 또는 EVENT=kqueue"
#endif

#include "ev.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

typedef struct {
    uint64_t tag;
    unsigned events;   /* WS_EV_IN 또는 WS_EV_OUT */
    int on;            /* 커널에 등록돼 있다 */
} ev_rec;

struct ev_loop {
    int epfd;
    struct epoll_event *buf;   /* epoll_wait 결과 */
    ev_event *out;
    size_t max;
    ev_rec *rec;               /* fd 번호로 찾는다. 필요할 때 늘린다 */
    size_t nrec;
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
    ev->max = max_watch ? max_watch : 1;
    ev->buf = calloc(ev->max, sizeof *ev->buf);
    ev->out = calloc(ev->max, sizeof *ev->out);
    if (ev->buf == NULL || ev->out == NULL) {
        ev_close(ev);
        errno = ENOMEM;
        return NULL;
    }
    ev->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (ev->epfd < 0) {
        int e = errno;

        ev_close(ev);
        errno = e;
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
    free(ev->buf);
    free(ev->out);
    free(ev->rec);
    free(ev);
}

void ev_begin(ev_loop *ev)
{
    (void)ev;
}

/* fd 의 기록. 없으면 배열을 늘린다 (실패하면 NULL) */
static ev_rec *rec_get(ev_loop *ev, int fd)
{
    if ((size_t)fd >= ev->nrec) {
        size_t n = ev->nrec ? ev->nrec : 64;
        ev_rec *p;

        while (n <= (size_t)fd)
            n *= 2;
        p = realloc(ev->rec, n * sizeof *p);
        if (p == NULL)
            return NULL;
        for (size_t i = ev->nrec; i < n; i++)
            p[i].on = 0;
        ev->rec = p;
        ev->nrec = n;
    }
    return &ev->rec[fd];
}

int ev_want(ev_loop *ev, int fd, uint64_t tag, unsigned events)
{
    struct epoll_event e;
    ev_rec *r;
    int op;

    if ((events != WS_EV_IN && events != WS_EV_OUT) || fd < 0) {
        errno = EINVAL;
        return -1;
    }
    r = rec_get(ev, fd);
    if (r == NULL) {
        errno = ENOMEM;
        return -1;
    }
    if (r->on && r->tag == tag && r->events == events)
        return 0;
    e.events = events == WS_EV_IN ? EPOLLIN : EPOLLOUT;
    e.data.u64 = tag;
    op = r->on ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (epoll_ctl(ev->epfd, op, fd, &e) < 0) {
        /* 기록과 커널이 어긋났을 때만 (정상 경로에서는 오지 않는다): 반대 연산으로 한 번 더 */
        if (op == EPOLL_CTL_MOD && errno == ENOENT)
            op = EPOLL_CTL_ADD;
        else if (op == EPOLL_CTL_ADD && errno == EEXIST)
            op = EPOLL_CTL_MOD;
        else {
            r->on = 0;
            return -1;
        }
        if (epoll_ctl(ev->epfd, op, fd, &e) < 0) {
            r->on = 0;
            return -1;
        }
    }
    r->on = 1;
    r->tag = tag;
    r->events = events;
    return 0;
}

void ev_pause(ev_loop *ev, int fd)
{
    if (fd < 0 || (size_t)fd >= ev->nrec || !ev->rec[fd].on)
        return;
    (void)epoll_ctl(ev->epfd, EPOLL_CTL_DEL, fd, NULL);   /* 실패해도 기록은 지운다: 다음 ev_want 가 ADD 부터 */
    ev->rec[fd].on = 0;
}

void ev_forget(ev_loop *ev, int fd)
{
    if (ev == NULL || fd < 0 || (size_t)fd >= ev->nrec)
        return;
    ev->rec[fd].on = 0;
}

int ev_wait(ev_loop *ev, int timeout_ms, const ev_event **out)
{
    int rc = epoll_wait(ev->epfd, ev->buf, (int)ev->max, timeout_ms);
    int i;

    *out = ev->out;
    for (i = 0; i < rc; i++) {
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
    return rc;
}
