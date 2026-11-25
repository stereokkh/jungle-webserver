/*
 * ev_poll.c — ev.h 의 poll() 백엔드 (기본)
 *
 * 매 바퀴 pollfd 배열을 처음부터 만든다. ev_want 순서가 곧 배열 순서이고 이벤트도 그 순서로 돌려준다.
 * ev_pause 는 배열에 넣지 않는 것이다 (예전 server.c 가 음수 fd 를 넣어 poll 이 무시하게 한 것과 같다).
 * 커널이 매번 배열 전체를 훑으므로 비용은 감시하는 fd 수에 비례한다.
 *
 * Makefile 이 EVENT=poll(기본)일 때만 이 파일을 빌드한다.
 */
#include "ev.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>

struct ev_loop {
    struct pollfd *pfds;
    uint64_t *tags;          /* tags[i] 는 pfds[i] 의 tag */
    ev_event *out;
    size_t n, cap;
};

const char *ev_backend(void)
{
    return "poll";
}

ev_loop *ev_open(size_t max_watch)
{
    ev_loop *ev = calloc(1, sizeof *ev);

    if (ev == NULL)
        return NULL;
    ev->pfds = calloc(max_watch, sizeof *ev->pfds);
    ev->tags = calloc(max_watch, sizeof *ev->tags);
    ev->out = calloc(max_watch, sizeof *ev->out);
    if (ev->pfds == NULL || ev->tags == NULL || ev->out == NULL) {
        ev_close(ev);
        errno = ENOMEM;
        return NULL;
    }
    ev->cap = max_watch;
    return ev;
}

void ev_close(ev_loop *ev)
{
    if (ev == NULL)
        return;
    free(ev->pfds);
    free(ev->tags);
    free(ev->out);
    free(ev);
}

void ev_begin(ev_loop *ev)
{
    ev->n = 0;
}

int ev_want(ev_loop *ev, int fd, uint64_t tag, unsigned events)
{
    struct pollfd *p;

    if (events != WS_EV_IN && events != WS_EV_OUT) {   /* 정확히 하나 (명세 결정 2) */
        errno = EINVAL;
        return -1;
    }
    if (ev->n >= ev->cap) {
        errno = ENOSPC;
        return -1;
    }
    p = &ev->pfds[ev->n];
    p->fd = fd;
    p->events = events == WS_EV_IN ? POLLIN : POLLOUT;
    p->revents = 0;
    ev->tags[ev->n] = tag;
    ev->n++;
    return 0;
}

void ev_pause(ev_loop *ev, int fd)
{
    (void)ev;
    (void)fd;   /* 목록에 넣지 않으면 poll 은 이 fd 를 보지 않는다 */
}

void ev_forget(ev_loop *ev, int fd)
{
    (void)ev;
    (void)fd;   /* 매 바퀴 새로 만드므로 지울 기록이 없다 */
}

int ev_wait(ev_loop *ev, int timeout_ms, const ev_event **out)
{
    size_t i, k = 0;
    int rc = poll(ev->pfds, (nfds_t)ev->n, timeout_ms);

    *out = ev->out;
    if (rc <= 0)
        return rc;
    for (i = 0; i < ev->n; i++) {
        short re = ev->pfds[i].revents;
        unsigned e = 0;

        if (re == 0)
            continue;
        if (re & POLLIN)
            e |= WS_EV_IN;
        if (re & POLLOUT)
            e |= WS_EV_OUT;
        if (re & (POLLERR | POLLNVAL))
            e |= WS_EV_ERR;
        if (re & POLLHUP)
            e |= WS_EV_HUP;
        ev->out[k].tag = ev->tags[i];
        ev->out[k].events = e;
        k++;
    }
    return (int)k;
}
