/*
 * test_ev.c — 이벤트 대기 계층(ev.h) 단위 테스트 (docs/event-backend.md 7장)
 *
 * 어느 백엔드로 빌드하든(make EVENT=poll|epoll|kqueue) 이 파일이 그대로 돈다. 백엔드끼리 약속이 같은지 보는 시험이다.
 * fd 는 socketpair 로 만든다: 한쪽에 쓰면 다른 쪽이 읽기 가능, 한쪽을 닫으면 다른 쪽은 EOF.
 * 매 "바퀴"는 서버처럼 ev_begin → ev_want/ev_pause → ev_wait 순서로 부른다 (epoll 은 등록이 남지만 poll 은 매번 새로 만든다).
 */
#include "test.h"

#include "ev.h"

#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#define TAG_A ((uint64_t)0x1234567890abcdefULL)   /* 64비트를 다 쓰는 값: tag 가 잘리지 않는지 */
#define TAG_B ((uint64_t)0xfedcba0987654321ULL)
#define WAIT_MS 1000                               /* 준비된 이벤트는 바로 오므로 실제로는 기다리지 않는다 */

/* evs 에서 tag 의 이벤트 비트. 없으면 0 */
static unsigned events_of(const ev_event *evs, int n, uint64_t tag)
{
    int i;

    for (i = 0; i < n; i++)
        if (evs[i].tag == tag)
            return evs[i].events;
    return 0;
}

/* 한 바퀴: fd 하나를 events 로 감시하고 기다린다. 반환은 ev_wait 의 값 */
static int round_want(ev_loop *ev, int fd, uint64_t tag, unsigned events, int timeout_ms, const ev_event **evs)
{
    ev_begin(ev);
    if (ev_want(ev, fd, tag, events) < 0)
        return -100;
    return ev_wait(ev, timeout_ms, evs);
}

static void test_backend_name(void)
{
    const char *name = ev_backend();

    REQUIRE(name != NULL);
    CHECK(strcmp(name, "poll") == 0 || strcmp(name, "epoll") == 0 || strcmp(name, "kqueue") == 0);
}

/* 1. 읽기를 원하고 상대가 쓰면 이벤트 하나, tag 는 그대로 */
static void test_in_ready(void)
{
    ev_loop *ev = ev_open(8);
    const ev_event *evs;
    int sv[2], n;

    REQUIRE(ev != NULL);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK_INT(round_want(ev, sv[0], TAG_A, WS_EV_IN, 0, &evs), 0);   /* 아직 아무것도 안 왔다 */
    REQUIRE(write(sv[1], "x", 1) == 1);
    n = round_want(ev, sv[0], TAG_A, WS_EV_IN, WAIT_MS, &evs);
    CHECK_INT(n, 1);
    if (n == 1) {
        CHECK(evs[0].tag == TAG_A);
        CHECK(evs[0].events & WS_EV_IN);
        CHECK(!(evs[0].events & WS_EV_OUT));
    }
    close(sv[0]);
    close(sv[1]);
    ev_close(ev);
}

/* fd 둘이 함께 준비되면 이벤트 둘, 각자의 tag */
static void test_two_fds(void)
{
    ev_loop *ev = ev_open(8);
    const ev_event *evs;
    int a[2], b[2], n;

    REQUIRE(ev != NULL);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, a) == 0);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, b) == 0);
    REQUIRE(write(a[1], "x", 1) == 1);
    ev_begin(ev);
    REQUIRE(ev_want(ev, a[0], TAG_A, WS_EV_IN) == 0);
    REQUIRE(ev_want(ev, b[0], TAG_B, WS_EV_OUT) == 0);   /* 소켓은 처음부터 쓰기 가능 */
    n = ev_wait(ev, WAIT_MS, &evs);
    CHECK_INT(n, 2);
    CHECK(events_of(evs, n, TAG_A) & WS_EV_IN);
    CHECK(events_of(evs, n, TAG_B) & WS_EV_OUT);
    close(a[0]);
    close(a[1]);
    close(b[0]);
    close(b[1]);
    ev_close(ev);
}

/* 2. ev_pause 한 fd 는 상대가 쓰거나 닫아도 아무것도 안 온다 (HUP·ERR 도). 다시 ev_want 하면 온다 */
static void test_pause(void)
{
    ev_loop *ev = ev_open(8);
    const ev_event *evs;
    int sv[2], n;

    REQUIRE(ev != NULL);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK_INT(round_want(ev, sv[0], TAG_A, WS_EV_IN, 0, &evs), 0);   /* epoll 이면 여기서 등록된다 */
    REQUIRE(write(sv[1], "x", 1) == 1);
    close(sv[1]);                                                     /* 데이터 + EOF */

    ev_begin(ev);
    ev_pause(ev, sv[0]);
    CHECK_INT(ev_wait(ev, 0, &evs), 0);
    ev_begin(ev);
    ev_pause(ev, sv[0]);                                              /* 이미 멈춘 fd 를 또 멈춰도 된다 */
    CHECK_INT(ev_wait(ev, 0, &evs), 0);

    n = round_want(ev, sv[0], TAG_A, WS_EV_IN, WAIT_MS, &evs);
    CHECK_INT(n, 1);
    CHECK(events_of(evs, n, TAG_A) & WS_EV_IN);
    close(sv[0]);
    ev_close(ev);
}

/* 등록한 적 없는 fd 를 ev_pause 해도 된다 (서버는 바퀴마다 상태만 보고 부른다) */
static void test_pause_unregistered(void)
{
    ev_loop *ev = ev_open(8);
    const ev_event *evs;
    int sv[2];

    REQUIRE(ev != NULL);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    ev_begin(ev);
    ev_pause(ev, sv[0]);
    ev_pause(ev, 1000);   /* 열려 있지도 않은 번호 */
    CHECK_INT(ev_wait(ev, 0, &evs), 0);
    close(sv[0]);
    close(sv[1]);
    ev_close(ev);
}

/* 3. ev_forget → close → 같은 번호의 새 fd 를 같은 tag·같은 이벤트로 감시해도 이벤트가 온다.
 * 페일오버가 업스트림 fd 를 닫자마자 새로 열 때의 모양이다. 백엔드가 "fd N 은 이미 같은 것으로 등록됨"이라는
 * 기록을 지우지 않으면 새 fd 를 등록하지 않고 넘어가서 여기서 0 이 나온다 */
static void test_forget_reuse(void)
{
    ev_loop *ev = ev_open(8);
    const ev_event *evs;
    int a[2], b[2], num, n;

    REQUIRE(ev != NULL);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, a) == 0);
    num = a[0];
    CHECK_INT(round_want(ev, num, TAG_A, WS_EV_IN, 0, &evs), 0);   /* 등록 */
    ev_forget(ev, num);
    close(a[0]);
    close(a[1]);

    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, b) == 0);
    if (b[0] != num) {                     /* 같은 번호를 확실히 쓰게 옮긴다 */
        REQUIRE(dup2(b[0], num) == num);
        close(b[0]);
        b[0] = num;
    }
    REQUIRE(write(b[1], "y", 1) == 1);
    n = round_want(ev, num, TAG_A, WS_EV_IN, WAIT_MS, &evs);
    CHECK_INT(n, 1);
    CHECK(events_of(evs, n, TAG_A) & WS_EV_IN);
    close(b[0]);
    close(b[1]);
    ev_close(ev);
}

/* 4. 레벨 트리거: 읽지 않고 두면 다음 바퀴에도 또 온다 */
static void test_level_triggered(void)
{
    ev_loop *ev = ev_open(8);
    const ev_event *evs;
    int sv[2];

    REQUIRE(ev != NULL);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    REQUIRE(write(sv[1], "x", 1) == 1);
    CHECK_INT(round_want(ev, sv[0], TAG_A, WS_EV_IN, WAIT_MS, &evs), 1);
    CHECK_INT(round_want(ev, sv[0], TAG_A, WS_EV_IN, WAIT_MS, &evs), 1);
    CHECK_INT(round_want(ev, sv[0], TAG_A, WS_EV_IN, WAIT_MS, &evs), 1);
    close(sv[0]);
    close(sv[1]);
    ev_close(ev);
}

/* 5. IN → OUT 으로 바꾸면 OUT 만 온다 (읽을 바이트가 있어도). kqueue 면 옛 READ 필터가 지워졌는지 */
static void test_switch_in_to_out(void)
{
    ev_loop *ev = ev_open(8);
    const ev_event *evs;
    int sv[2], n;
    unsigned e;

    REQUIRE(ev != NULL);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    REQUIRE(write(sv[1], "x", 1) == 1);
    n = round_want(ev, sv[0], TAG_A, WS_EV_IN, WAIT_MS, &evs);
    CHECK_INT(n, 1);
    n = round_want(ev, sv[0], TAG_B, WS_EV_OUT, WAIT_MS, &evs);   /* tag 도 바꾼다 */
    CHECK_INT(n, 1);
    e = events_of(evs, n, TAG_B);
    CHECK(e & WS_EV_OUT);
    CHECK(!(e & WS_EV_IN));
    CHECK_INT(events_of(evs, n, TAG_A), 0);                       /* 옛 tag 로는 안 온다 */
    close(sv[0]);
    close(sv[1]);
    ev_close(ev);
}

/* 6. events 는 IN 이나 OUT 정확히 하나 (명세 결정 2) */
static void test_invalid_events(void)
{
    ev_loop *ev = ev_open(8);
    int sv[2];

    REQUIRE(ev != NULL);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    ev_begin(ev);
    errno = 0;
    CHECK_INT(ev_want(ev, sv[0], TAG_A, 0), -1);
    CHECK_INT(errno, EINVAL);
    errno = 0;
    CHECK_INT(ev_want(ev, sv[0], TAG_A, WS_EV_IN | WS_EV_OUT), -1);
    CHECK_INT(errno, EINVAL);
    errno = 0;
    CHECK_INT(ev_want(ev, sv[0], TAG_A, WS_EV_HUP), -1);          /* 받기만 하는 비트 */
    CHECK_INT(errno, EINVAL);
    close(sv[0]);
    close(sv[1]);
    ev_close(ev);
}

/* 감시 목록이 비어도 timeout 0 이면 바로 0 */
static void test_empty_timeout(void)
{
    ev_loop *ev = ev_open(8);
    const ev_event *evs;

    REQUIRE(ev != NULL);
    ev_begin(ev);
    CHECK_INT(ev_wait(ev, 0, &evs), 0);
    ev_close(ev);
    ev_close(NULL);         /* NULL 도 된다 */
    ev_forget(NULL, 3);
}

int main(void)
{
    printf("  (event backend: %s)\n", ev_backend());
    RUN(test_backend_name);
    RUN(test_in_ready);
    RUN(test_two_fds);
    RUN(test_pause);
    RUN(test_pause_unregistered);
    RUN(test_forget_reuse);
    RUN(test_level_triggered);
    RUN(test_switch_in_to_out);
    RUN(test_invalid_events);
    RUN(test_empty_timeout);
    return test_report();
}
