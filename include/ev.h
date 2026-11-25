/*
 * ev.h — 이벤트 대기 계층 (poll / epoll / kqueue 중 빌드 때 하나)
 *
 * 명세: docs/event-backend.md (결정 1~5 는 그 문서 6장)
 * 상태: 제안 (트랙 A 합의 전). poll·epoll 은 구현됐고 kqueue 는 아직이다 (명세의 "구현 상태")
 *
 * server.c 는 poll·epoll·kqueue 를 직접 부르지 않고 이 함수들만 쓴다. 백엔드는 빌드 때 고른다:
 *   make               → src/ev_poll.c   (POSIX, 기본. macOS·Linux·musl 모두)
 *   make EVENT=epoll   → src/ev_epoll.c  (Linux)
 *   make EVENT=kqueue  → src/ev_kqueue.c (macOS, 아직 없음: 지금은 Makefile 이 멈춘다)
 * 세 백엔드는 같은 의미를 지킨다. 그래서 server.c 의 상태 기계는 어느 쪽에서도 똑같이 돈다.
 *
 * 쓰는 순서 (루프 한 바퀴):
 *   ev_begin → 감시 대상마다 ev_want 또는 ev_pause → ev_wait → 돌려받은 이벤트 처리
 *   - 매 바퀴 감시할 fd 를 모두 다시 알려 준다 (연결 상태에서 "지금 원하는 이벤트"를 계산하는 곳이 한 곳이 되게).
 *     poll 은 그대로 배열을 새로 만들고, epoll·kqueue 는 지난번 등록과 달라진 fd 만 고친다.
 *   - 감시하던 fd 를 close 하기 직전에는 반드시 ev_forget 을 부른다 (아래 설명).
 *
 * 레벨 트리거만 쓴다 (EPOLLET·EV_CLEAR 아님). 처리 함수는 이벤트 하나에 read/write 를 한 번만 하고
 * 남은 일은 다음 바퀴에 다시 깨워 주기를 기대한다. 엣지 트리거로 바꾸면 그 기대가 깨져 연결이 멈춘다.
 */
#ifndef EV_H
#define EV_H

#include <stddef.h>
#include <stdint.h>

/* 이벤트 비트. 접두사 WS_ 는 kqueue 의 <sys/event.h> (EV_ADD, EV_EOF, EV_SET ...) 와 섞이지 않게 (결정 5).
 * ev_want 에는 WS_EV_IN 또는 WS_EV_OUT 정확히 하나만 준다 (결정 2).
 * WS_EV_ERR·WS_EV_HUP 은 받기만 한다: 요청하지 않아도 온다 (poll·epoll·kqueue 모두).
 * poll 의 POLLNVAL(닫힌 fd 를 넣음)은 WS_EV_ERR 로 합친다. epoll·kqueue 에는 짝이 없고, 그런 fd 는 ev_want 가 실패한다.
 * POLLRDHUP/EPOLLRDHUP(상대가 송신만 닫음)는 쓰지 않는다: 한 백엔드에만 켜면 백엔드끼리 동작이 달라진다 */
#define WS_EV_IN   0x1u   /* 읽을 수 있음 (EOF 포함) */
#define WS_EV_OUT  0x2u   /* 쓸 수 있음 (논블로킹 connect 완료 포함) */
#define WS_EV_ERR  0x4u   /* 오류 */
#define WS_EV_HUP  0x8u   /* 끊김 (kqueue 는 EV_EOF) */

typedef struct ev_loop ev_loop;

typedef struct {
    uint64_t tag;      /* ev_want 에 넘긴 값 그대로 (server.c 가 칸·종류·세대를 담는다). kqueue 는 udata 에 넣는다 (결정 4) */
    unsigned events;   /* WS_EV_* */
} ev_event;

/* 백엔드 이름 ("poll" / "epoll" / "kqueue"). 시작 줄에 찍어 어느 바이너리인지 보이게 한다 */
const char *ev_backend(void);

/* max_watch: 한 바퀴에 감시할 fd 수의 상한 (한 번에 돌려받을 이벤트 수의 상한도 같다).
 * 실패하면 NULL (errno 설정) */
ev_loop *ev_open(size_t max_watch);

/* 모든 자원 정리. NULL 이어도 된다 */
void ev_close(ev_loop *ev);

/* 이번 바퀴의 감시 목록을 새로 시작한다 (poll: 배열을 비움, epoll·kqueue: 할 일 없음) */
void ev_begin(ev_loop *ev);

/* 이번 바퀴에 fd 를 감시한다. events 는 WS_EV_IN 또는 WS_EV_OUT 정확히 하나 (아니면 errno = EINVAL).
 * tag 는 이벤트가 왔을 때 그대로 돌려준다.
 * 등록은 이 자리에서 끝낸다: 실패하면 그 자리에서 -1 + errno (결정 3. kqueue 도 변경을 모아 두지 않는다) */
int ev_want(ev_loop *ev, int fd, uint64_t tag, unsigned events);

/* 이번 바퀴에 fd 를 아예 보지 않는다: WS_EV_ERR·WS_EV_HUP 도 받지 않는다. 등록돼 있지 않은 fd 에 불러도 된다.
 * poll 은 목록에서 빼고, epoll 은 EPOLL_CTL_DEL, kqueue 는 걸린 필터 EV_DELETE.
 * 쓰는 곳: accept 를 쉬는 listen 소켓, 역압력으로 읽기를 멈춘 업스트림, 프록시 중 보낼 것이 없는 클라이언트 (결정 1).
 * "이벤트 0 으로 등록"이 없는 이유: epoll 은 끊긴 fd 의 EPOLLHUP·EPOLLERR 가 레벨 트리거라 계속 와서 루프가 쉬지 못하고,
 * kqueue 는 필터 없이는 아무것도 오지 않아 세 백엔드의 뜻을 맞출 수 없다 */
void ev_pause(ev_loop *ev, int fd);

/* fd 를 close 하기 직전에 부른다. 등록 기록을 지운다 (poll: 할 일 없음).
 * 이것을 빠뜨리면 epoll·kqueue 쪽 기록이 남은 채로 같은 번호의 새 fd(페일오버의 새 업스트림 등)가 생기고,
 * "이미 같은 것으로 등록돼 있다"고 판단해 새 fd 를 등록하지 않아 그 연결이 마감까지 멈춘다.
 * 커널 쪽 등록은 close 가 지우므로 시스템 콜을 할지는 백엔드 자유. ev 가 NULL 이어도 된다 */
void ev_forget(ev_loop *ev, int fd);

/* 이벤트를 기다린다. timeout_ms 는 poll 과 같다 (-1 무한, 0 바로). 반환: 이벤트 수(0 이면 시간 초과),
 * -1 이면 errno (EINTR 포함). *out 은 다음 ev_wait/ev_begin 까지 유효하다.
 * 같은 fd 의 이벤트는 한 번에 하나다 (fd 마다 이벤트·필터를 하나만 걸므로).
 * 순서는 정하지 않는다 (poll 은 ev_want 순서, epoll·kqueue 는 커널이 준 순서) */
int ev_wait(ev_loop *ev, int timeout_ms, const ev_event **out);

#endif /* EV_H */
