/*
 * ev_poll.c — ev.h 의 poll 백엔드 (기본. POSIX 라 macOS·glibc·musl 모두)
 *
 * 등록을 기억하지 않는다: ev_begin 이 배열을 비우고 ev_want 가 한 칸씩 붙인다. 그래서 ev_pause·ev_forget 은 할 일이 없다
 * (그 바퀴에 ev_want 를 부르지 않으면 배열에 없다).
 * 명세: docs/event-backend.md
 */
#include "ev.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>

struct ev_loop {
    struct pollfd *pfds;
    uint64_t *tags;       /* pfds[i] 의 tag */
    ev_event *out;        /* ev_wait 가 돌려주는 배열 */
    size_t n, max;
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
    ev->max = max_watch;
    ev->pfds = calloc(max_watch ? max_watch : 1, sizeof *ev->pfds);
    ev->tags = calloc(max_watch ? max_watch : 1, sizeof *ev->tags);
    ev->out = calloc(max_watch ? max_watch : 1, sizeof *ev->out);
    if (ev->pfds == NULL || ev->tags == NULL || ev->out == NULL) {
        ev_close(ev);
        errno = ENOMEM;
        return NULL;
    }
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
    if ((events != WS_EV_IN && events != WS_EV_OUT) || fd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (ev->n >= ev->max) {
        errno = ENOSPC;
        return -1;
    }
    ev->pfds[ev->n].fd = fd;
    ev->pfds[ev->n].events = events == WS_EV_IN ? POLLIN : POLLOUT;
    ev->pfds[ev->n].revents = 0;
    ev->tags[ev->n] = tag;
    ev->n++;
    return 0;
}

void ev_pause(ev_loop *ev, int fd)
{
    (void)ev;
    (void)fd;
}

void ev_forget(ev_loop *ev, int fd)
{
    (void)ev;
    (void)fd;
}

int ev_wait(ev_loop *ev, int timeout_ms, const ev_event **out)
{
    int rc = poll(ev->pfds, (nfds_t)ev->n, timeout_ms);
    size_t i;
    int k = 0;

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
    return k;
}
