/*
 * ev_kqueue.c — ev.h 의 kqueue 백엔드 (macOS·BSD, make EVENT=kqueue)
 *
 * epoll 백엔드와 같은 구조: 등록을 커널에 남기고 fd 마다 "지금 걸린 필터·tag" 를 기억해 바뀐 것만 고친다.
 *   ev_want  : 기록과 같으면 시스템 콜 없음. 다르면 kevent() 한 번에 [옛 필터 EV_DELETE] + 새 필터 EV_ADD (결정 3: 그 자리에서)
 *   ev_pause : 걸린 필터 EV_DELETE
 *   ev_forget: 기록만 지운다 (close 가 그 fd 의 필터를 모두 지운다)
 * 레벨 트리거만 (EV_CLEAR·EV_ONESHOT 금지). fd 마다 필터 하나라 한 번의 ev_wait 에 fd 하나당 이벤트 하나다.
 * 명세: docs/event-backend.md
 */
#if !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__NetBSD__) && \
    !defined(__OpenBSD__) && !defined(__DragonFly__)
#error "ev_kqueue.c 는 macOS·BSD 전용이다. Linux 는 make EVENT=poll (기본) 또는 EVENT=epoll"
#endif

/* -D_POSIX_C_SOURCE 로 빌드하면 macOS 헤더가 BSD 확장(kqueue 등)을 숨긴다 */
#define _DARWIN_C_SOURCE

#include "ev.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* tag 를 udata(포인터)에 그대로 넣는다 (결정 4) */
_Static_assert(sizeof(void *) >= sizeof(uint64_t), "kqueue backend needs 64-bit pointers for the tag");

typedef struct {
    uint64_t tag;
    short filter;   /* EVFILT_READ 또는 EVFILT_WRITE */
    int on;
} ev_rec;

struct ev_loop {
    int kq;
    struct kevent *buf;
    ev_event *out;
    size_t max;
    ev_rec *rec;
    size_t nrec;
};

const char *ev_backend(void)
{
    return "kqueue";
}

ev_loop *ev_open(size_t max_watch)
{
    ev_loop *ev = calloc(1, sizeof *ev);

    if (ev == NULL)
        return NULL;
    ev->kq = -1;
    ev->max = max_watch ? max_watch : 1;
    ev->buf = calloc(ev->max, sizeof *ev->buf);
    ev->out = calloc(ev->max, sizeof *ev->out);
    if (ev->buf == NULL || ev->out == NULL) {
        ev_close(ev);
        errno = ENOMEM;
        return NULL;
    }
    ev->kq = kqueue();
    if (ev->kq < 0) {
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
    if (ev->kq >= 0)
        close(ev->kq);
    free(ev->buf);
    free(ev->out);
    free(ev->rec);
    free(ev);
}

void ev_begin(ev_loop *ev)
{
    (void)ev;
}

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
    struct kevent ch[2], res[2];
    short filter;
    ev_rec *r;
    int n = 0, got, i;

    if ((events != WS_EV_IN && events != WS_EV_OUT) || fd < 0) {
        errno = EINVAL;
        return -1;
    }
    r = rec_get(ev, fd);
    if (r == NULL) {
        errno = ENOMEM;
        return -1;
    }
    filter = events == WS_EV_IN ? EVFILT_READ : EVFILT_WRITE;
    if (r->on && r->filter == filter && r->tag == tag)
        return 0;
    /* EV_RECEIPT: 변경마다 결과를 EV_ERROR 항목(data = errno, 0 이면 성공)으로 받는다. 이벤트를 꺼내 오지 않는다 */
    if (r->on && r->filter != filter)
        EV_SET(&ch[n++], (uintptr_t)fd, r->filter, EV_DELETE | EV_RECEIPT, 0, 0, NULL);
    /* 이미 걸린 필터에 EV_ADD 하면 udata(tag)만 바뀐다 */
    EV_SET(&ch[n++], (uintptr_t)fd, filter, EV_ADD | EV_RECEIPT, 0, 0, (void *)(uintptr_t)tag);
    r->on = 0;   /* 아래에서 성공을 확인한 뒤에만 다시 켠다 */
    got = kevent(ev->kq, ch, n, res, n, NULL);
    if (got < 0)
        return -1;
    for (i = 0; i < got; i++) {
        if (!(res[i].flags & EV_ERROR) || res[i].data == 0)
            continue;
        if (res[i].filter != filter && res[i].data == ENOENT)
            continue;   /* 옛 필터가 이미 없다: 지우려던 것이니 괜찮다 */
        errno = (int)res[i].data;
        return -1;
    }
    r->on = 1;
    r->filter = filter;
    r->tag = tag;
    return 0;
}

void ev_pause(ev_loop *ev, int fd)
{
    struct kevent ch;

    if (fd < 0 || (size_t)fd >= ev->nrec || !ev->rec[fd].on)
        return;
    EV_SET(&ch, (uintptr_t)fd, ev->rec[fd].filter, EV_DELETE, 0, 0, NULL);
    (void)kevent(ev->kq, &ch, 1, NULL, 0, NULL);   /* 실패해도 기록은 지운다: 다음 ev_want 가 EV_ADD 부터 */
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
    struct timespec ts, *tp = NULL;
    int rc, i;

    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        tp = &ts;
    }
    rc = kevent(ev->kq, NULL, 0, ev->buf, (int)ev->max, tp);
    *out = ev->out;
    for (i = 0; i < rc; i++) {
        const struct kevent *k = &ev->buf[i];
        unsigned e = 0;

        if (k->flags & EV_ERROR) {
            e |= WS_EV_ERR;
        } else {
            e |= k->filter == EVFILT_READ ? WS_EV_IN : WS_EV_OUT;
            if (k->flags & EV_EOF) {
                e |= WS_EV_HUP;
                if (k->fflags != 0)
                    e |= WS_EV_ERR;   /* EV_EOF 와 함께 fflags 에 소켓 오류 번호가 온다 */
            }
        }
        ev->out[i].tag = (uint64_t)(uintptr_t)k->udata;
        ev->out[i].events = e;
    }
    return rc;
}
