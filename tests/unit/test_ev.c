/*
 * test_ev.c — 이벤트 백엔드 단위 테스트 (docs/event-backend.md 7장)
 *
 * 어느 백엔드로 빌드하든 (make test, make test EVENT=epoll, EVENT=kqueue) 같은 파일이 돈다.
 * 루프 한 바퀴 = ev_begin → ev_want/ev_pause → ev_wait(0). socketpair 로 이벤트를 만든다.
 */
#include "test.h"

#include "ev.h"

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#define BIG_TAG 0xabc1234500000000ull   /* 위쪽 비트까지 그대로 돌아오는지 */

static int pair(int sv[2])
{
    return socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
}

/* 한 바퀴: fd 하나를 events 로 감시하고 바로 기다린다 */
static int pass_want(ev_loop *ev, int fd, uint64_t tag, unsigned events, const ev_event **out)
{
    ev_begin(ev);
    if (ev_want(ev, fd, tag, events) < 0)
        return -1;
    return ev_wait(ev, 0, out);
}

/* 1. IN 을 원하고 상대가 쓰면 이벤트 하나, tag 그대로 */
static void test_in_event_tag(void)
{
    ev_loop *ev = ev_open(4);
    const ev_event *out;
    int sv[2];

    REQUIRE(ev != NULL);
    REQUIRE(pair(sv) == 0);
    CHECK_INT(pass_want(ev, sv[0], BIG_TAG | 7, WS_EV_IN, &out), 0);   /* 아직 아무것도 안 옴 */
    REQUIRE(write(sv[1], "x", 1) == 1);
    REQUIRE(pass_want(ev, sv[0], BIG_TAG | 7, WS_EV_IN, &out) == 1);
    CHECK(out[0].tag == (BIG_TAG | 7));
    CHECK(out[0].events & WS_EV_IN);
    CHECK(!(out[0].events & WS_EV_OUT));
    ev_forget(ev, sv[0]);
    close(sv[0]);
    close(sv[1]);
    ev_close(ev);
}

/* 2. ev_pause 한 fd 는 상대가 쓰거나 닫아도 조용하다. 다시 ev_want 하면 온다 */
static void test_pause(void)
{
    ev_loop *ev = ev_open(4);
    const ev_event *out;
    int sv[2];

    REQUIRE(ev != NULL);
    REQUIRE(pair(sv) == 0);
    CHECK_INT(pass_want(ev, sv[0], 1, WS_EV_IN, &out), 0);   /* 등록된 상태에서 pause 로 */
    REQUIRE(write(sv[1], "x", 1) == 1);
    ev_begin(ev);
    ev_pause(ev, sv[0]);
    CHECK_INT(ev_wait(ev, 0, &out), 0);
    close(sv[1]);                                            /* 끊겨도 HUP 이 오면 안 된다 */
    ev_begin(ev);
    ev_pause(ev, sv[0]);
    CHECK_INT(ev_wait(ev, 0, &out), 0);
    ev_begin(ev);
    ev_pause(ev, 12345);                                     /* 등록 안 된 fd 에 불러도 된다 */
    CHECK_INT(ev_wait(ev, 0, &out), 0);
    REQUIRE(pass_want(ev, sv[0], 2, WS_EV_IN, &out) == 1);
    CHECK(out[0].tag == 2);
    ev_forget(ev, sv[0]);
    close(sv[0]);
    ev_close(ev);
}

/* 3. ev_forget → close → 같은 번호의 새 fd → 같은 tag·events 로 ev_want → 이벤트가 온다 (3-3 의 함정) */
static void test_forget_reuse(void)
{
    ev_loop *ev = ev_open(4);
    const ev_event *out;
    int a[2], b[2], fd;

    REQUIRE(ev != NULL);
    REQUIRE(pair(a) == 0);
    fd = a[0];
    CHECK_INT(pass_want(ev, fd, 9, WS_EV_IN, &out), 0);
    ev_forget(ev, fd);
    close(fd);
    close(a[1]);
    REQUIRE(pair(b) == 0);
    if (b[0] != fd) {                                        /* 번호를 일부러 같게 (이미 같으면 그대로) */
        REQUIRE(dup2(b[0], fd) == fd);
        close(b[0]);
    }
    REQUIRE(write(b[1], "x", 1) == 1);
    REQUIRE(pass_want(ev, fd, 9, WS_EV_IN, &out) == 1);
    CHECK(out[0].tag == 9);
    ev_forget(ev, fd);
    close(fd);
    close(b[1]);
    ev_close(ev);
}

/* 4. ev_want 를 바꾸지 않고 두 바퀴 돌아도 계속 온다 (레벨 트리거) */
static void test_level_triggered(void)
{
    ev_loop *ev = ev_open(4);
    const ev_event *out;
    int sv[2];

    REQUIRE(ev != NULL);
    REQUIRE(pair(sv) == 0);
    REQUIRE(write(sv[1], "x", 1) == 1);
    CHECK_INT(pass_want(ev, sv[0], 3, WS_EV_IN, &out), 1);
    CHECK_INT(pass_want(ev, sv[0], 3, WS_EV_IN, &out), 1);   /* 읽지 않았으니 또 */
    ev_forget(ev, sv[0]);
    close(sv[0]);
    close(sv[1]);
    ev_close(ev);
}

/* 5. IN → OUT 으로 바꾸면 OUT 만 온다 (읽을 바이트가 남아 있어도. kqueue 는 옛 필터가 지워졌는지) */
static void test_switch_in_to_out(void)
{
    ev_loop *ev = ev_open(4);
    const ev_event *out;
    int sv[2];

    REQUIRE(ev != NULL);
    REQUIRE(pair(sv) == 0);
    REQUIRE(write(sv[1], "x", 1) == 1);
    REQUIRE(pass_want(ev, sv[0], 4, WS_EV_IN, &out) == 1);
    CHECK(out[0].events & WS_EV_IN);
    REQUIRE(pass_want(ev, sv[0], 5, WS_EV_OUT, &out) == 1);
    CHECK(out[0].tag == 5);
    CHECK(out[0].events & WS_EV_OUT);
    CHECK(!(out[0].events & WS_EV_IN));
    ev_forget(ev, sv[0]);
    close(sv[0]);
    close(sv[1]);
    ev_close(ev);
}

/* 6. events 는 IN 또는 OUT 정확히 하나 */
static void test_einval(void)
{
    ev_loop *ev = ev_open(4);
    int sv[2];

    REQUIRE(ev != NULL);
    REQUIRE(pair(sv) == 0);
    ev_begin(ev);
    errno = 0;
    CHECK_INT(ev_want(ev, sv[0], 1, 0), -1);
    CHECK_INT(errno, EINVAL);
    errno = 0;
    CHECK_INT(ev_want(ev, sv[0], 1, WS_EV_IN | WS_EV_OUT), -1);
    CHECK_INT(errno, EINVAL);
    errno = 0;
    CHECK_INT(ev_want(ev, sv[0], 1, WS_EV_HUP), -1);
    CHECK_INT(errno, EINVAL);
    ev_forget(ev, sv[0]);
    close(sv[0]);
    close(sv[1]);
    ev_close(ev);
    ev_forget(NULL, 3);   /* NULL 이어도 된다 */
    ev_close(NULL);
}

int main(void)
{
    printf("  ev backend: %s\n", ev_backend());
    RUN(test_in_event_tag);
    RUN(test_pause);
    RUN(test_forget_reuse);
    RUN(test_level_triggered);
    RUN(test_switch_in_to_out);
    RUN(test_einval);
    return test_report();
}
