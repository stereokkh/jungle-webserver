/*
 * proxy.h — 업스트림(백엔드) 중계
 *
 * CS:APP 대응: 없음 (Tiny에 없음. Proxy Lab은 Forward Proxy + 블로킹이라 구조가 다르다)
 * 담당: 트랙 B · 이슈 #8 (proxy_build_request: 요청 다시 쓰기), #9 (connect·중계, 이벤트 루프 통합은 A와 협업)
 *
 * 서버(server.c)에서 쓰는 순서 (#9 에서 A·B 가 함께 확정):
 *   1. handler_route → ROUTE_PROXY, route → proxy_rr_begin(route) 로 Round-Robin 시작 위치 (C9),
 *      proxy_pick_backend(route, now, tried, rr_start) 로 백엔드 선택 (둘 다 proxy_config.h)
 *   2. proxy_build_request(req, backend, client_ip, &up_out)   ← 입력 버퍼 consume 전에. 실패면 431 (결정 6)
 *   3. fd = proxy_connect_start(backend) → 실패면 (페일오버가 꺼져 있으면) 502
 *   4. upstream POLLOUT·POLLERR·POLLHUP → proxy_connect_result(fd) 가 0 이 아니면 (페일오버가 꺼져 있으면) 502 → up_out 송신
 *   5. upstream POLLIN → proxy_relay_want 만큼 read → proxy_relay_input 으로 클라이언트 out 버퍼에 중계
 *      (EOF·read 오류는 proxy_relay_eof)
 *   - 응답 시작 전 실패 502, UPSTREAM_TIMEOUT_MS 초과 504, 시작 후 실패는 남은 out 을 보내고 연결 종료
 *   - 페일오버 (라우트에 failover= 가 있을 때만, 결정 2026-11-25): 3·4 의 connect 실패와 connect 시간 초과
 *     (PROXY_CONNECT_TIMEOUT_MS)에서만 다른 백엔드로 2 부터 다시 한다. 요청 바이트를 보내기 시작한 뒤(5 이후, 4 의 송신 포함)의
 *     실패는 예전 그대로 502/504/연결 종료다. 그래서 같은 요청이 두 백엔드에서 실행되는 일이 없다.
 *     2 를 다시 하므로 Host 가 새 백엔드 이름으로 바뀐다. 요청이 끝난 뒤의 자동 재시도는 하지 않는다
 *
 * 결정 1-6 은 상철님이 11-23 설계 문답에서 정했고, C1-C9 는 구현 중 정해 11-23 확인했다.
 *   C1-C5: 아래 "응답 중계" 블록. C6-C8: server.c 쪽 (docs/decisions.md). C9: proxy_rr_begin 이
 *   라우트마다 next 를 돌려 Round-Robin (proxy_config.c)
 */
#ifndef PROXY_H
#define PROXY_H

#include "buffer.h"
#include "http_request.h"
#include "proxy_config.h"

/* 백엔드로 보낼 요청 전체를 out 뒤에 덧붙인다. 바이트 순서:
 *   1. "<method_str> <target> HTTP/1.1\r\n"  (클라이언트가 1.0 이어도 1.1. target 은 파서가 만든
 *      정규화된 요청 대상 그대로: 정규형 경로 + 원본 ?query, 접두사는 떼지 않음. #31, docs/interfaces.md "경로 정규형")
 *   2. req->headers 의 줄 중 아래에 해당하지 않는 것을 원래 순서, 원래 바이트(이름 대소문자, 공백 포함)
 *      그대로
 *   3. "Host: <backend->name>\r\n"
 *   4. "X-Forwarded-For: <client_ip>\r\n"   (덧붙이지 않고 대체. 클라이언트가 보낸 값은 버린다)
 *   4b. "Via: 1.1 jungle-webserver\r\n"    (RFC 9110 7.6.3: 게이트웨이는 요청마다 Via 를 보낸다. 가명 사용.
 *      클라이언트가 보낸 Via 줄은 2 에서 그대로 전달돼 앞에 남는다)
 *   5. "Content-Length: <body_len>\r\n"    (req->content_length >= 0 일 때만. POST 의 0 도 쓴다)
 *   6. "Connection: close\r\n", 빈 줄, 본문 body_len 바이트 (뒤에 이어 온 파이프라인 요청은 복사하지 않는다)
 *
 * 2 에서 빼는 줄 (이름은 대소문자 무시, 이름 전체가 같아야 한다):
 *   - hop-by-hop: Connection, Keep-Alive, TE, Trailer, Transfer-Encoding, Upgrade,
 *     그리고 "Proxy-" 로 시작하는 것 (Proxy-Connection, Proxy-Authorization). 하이픈 없는 "Proxy" 는 둔다
 *   - 블록 안 어느 Connection 줄이든 그 값(쉼표로 나눈 토큰, 앞뒤 SP/TAB 제거, 빈 토큰 무시)에 적힌 이름.
 *     그 줄보다 뒤에 있는 Connection 줄도 센다 ("Connection: close, X-Foo" 면 X-Foo 도 뺀다.
 *     RFC 9110 7.6.1: 중개자는 Connection 에 적힌 필드와 Connection 자체를 지워야 한다)
 *   - Host, X-Forwarded-For (3, 4 에서 새로 쓴다), Content-Length (5 에서 다시 쓴다), Expect
 *   이름이 비슷하기만 한 것(Connection-Extra, TEx, Expected, Content-Length-X ...)은 둔다.
 *   Forwarded, X-Real-IP, 클라이언트가 보낸 Via 는 그대로 전달한다
 *
 * PR #24 에서 미뤘던 두 가지의 결정:
 *   - "Connection: Content-Length" (Host, X-Forwarded-For, Connection 도 같다):
 *     Connection 에 적힌 이름은 클라이언트 줄에만 작용한다. 3-6 의 줄은 필터가 끝난 뒤에 만들므로
 *     클라이언트 토큰으로 지울 수 없다. 클라이언트의 Content-Length 줄은 규칙대로 지우고(RFC 9110 7.6.1),
 *     이미 받아 둔 본문 길이(body_len)로 새로 쓴다. 메시지 길이 지정은 홉마다 보내는 쪽 몫이다
 *     (RFC 9112 6.3). 그래서 본문이 있는데 길이가 없는 메시지는 만들지 않는다
 *   - Expect: 값에 상관없이 항상 뺀다 (정의된 기대는 100-continue 뿐). 이 함수는 PARSE_DONE 뒤에만
 *     부르므로 본문이 이미 전부 버퍼에 있어 기다릴 것이 없다. 전달하면 백엔드가 "100 Continue" 를
 *     먼저 보내 #9 의 응답 처리가 그것을 건너뛰어야 한다. RFC 9110 10.1.1 에서 프록시는
 *     417 로 답하거나 요청을 전달할 수 있는데, 기대를 뺀 채 전달한다.
 *     클라이언트가 100 을 기다리는 쪽(서버가 본문을 기다리며 100 을 보낼지)은 여기서 정하지 않고
 *     #7 의 서버 쪽 일이다
 *
 * 성공 0. 실패 -1 이고 out 은 호출 전 그대로다 (buffer_mark/rollback, BUFFER_MAX 초과 포함):
 *   인자가 NULL, method_str 이 비었거나 target 이 '/' 로 시작하지 않음,
 *   method_str·target 이 NUL 로 끝나지 않거나 0x21-0x7E 밖의 바이트가 있음,
 *   content_length < 0 인데 body_len > 0 (길이 없는 본문),
 *   backend->name 이 비었거나 NUL 이 없거나 0x21-0x7E 밖의 바이트가 있음, client_ip 가 비었거나
 *   0x21-0x7E 밖의 바이트가 있음 (헤더 주입 차단), content_length >= 0 인데 body_len 과 다름,
 *   body_len > 0 인데 body 가 NULL, headers_len > 0 인데 headers 가 NULL,
 *   헤더 블록 줄이 깨짐 (파서가 400 으로 막는 모양: 이름이 토큰 글자가 아님, 값에 제어 문자, CRLF 없음 등.
 *   http_parse_request 가 만든 블록은 통과한다),
 *   다시 쓴 머리(1 ~ 6 의 빈 줄까지)가 HTTP_MAX_HEADER 바이트를 넘음 (프런트 한도에 가까운 요청은
 *   Host 이름 교체와 줄 추가로 길어져 같은 바이너리의 백엔드가 431 로 거절한다. 보내지 않고 실패시키며
 *   서버는 431 로 답한다. 결정 6) */
int proxy_build_request(const http_request *req, const proxy_backend *backend,
                        const char *client_ip, buffer *out);

/* proxy_connect_start 의 실패 두 가지 (둘 다 errno 를 남긴다).
 *   -1                    connect 가 동기적으로 실패했다 (ENETUNREACH, ECONNREFUSED 등): 백엔드에 닿지 못한 것
 *   PROXY_CONNECT_LOCAL   이쪽 문제로 연결하지 못했다: socket() 이 EMFILE·ENFILE·ENOBUFS·ENOMEM 등으로 실패, 논블로킹 설정 실패,
 *                         또는 connect 가 EADDRNOTAVAIL (리눅스에서 임시 포트가 바닥남. 로컬 주소를 못 얻은 것).
 *                         백엔드 탓이 아니므로 호출자는 이 백엔드에 죽은 표시를 붙이면 안 된다 (시도 횟수에는 센다) */
#define PROXY_CONNECT_LOCAL (-2)

/* 논블로킹 connect 시작. 성공(진행 중 포함): fd (0 이상), 실패: -1 또는 PROXY_CONNECT_LOCAL (errno 유지, 위).
 * connect 가 0 이어도 fd 를 돌려주고 서버는 똑같이 POLLOUT 을 기다린다. EINPROGRESS·EINTR 은 진행 중으로 본다.
 * (EINTR 뒤에는 connect 를 다시 부르지 않는다. 연결은 커널이 이어서 하고, 다시 부르면 EALREADY 가 난다)
 * fd 는 서버가 닫는다 */
int proxy_connect_start(const proxy_backend *backend);

/* connect 결과: getsockopt(SO_ERROR). 연결됨 0, 실패면 그 errno (getsockopt 자체가 실패하면 그 errno).
 * 업스트림 fd 의 POLLOUT·POLLERR·POLLHUP 어느 쪽이 와도 이 값만으로 판단한다 (결정 5) */
int proxy_connect_result(int fd);

/* ---- 응답 중계 (소켓 없는 순수 함수. 업스트림에서 읽은 바이트를 먹이면 클라이언트 out 에 덧붙인다) ----
 *
 * 결정 (상철, 11-23): 응답은 머리(상태 줄 + Content-Length)만 해석하고 받은 바이트를 그대로 중계한다
 * (머리를 고쳐 쓰지 않는다). 백엔드에는 늘 "Connection: close" 를 보내므로(#8) 중계한 뒤 클라이언트
 * 연결은 항상 닫는다. "응답 시작" = 머리 해석이 끝나 상태 줄 검사를 통과하고 머리 바이트를 out 에 넣은 때.
 * 시작 전 실패는 502, 시작 후 실패는 이미 나간 바이트가 있어 연결 종료뿐이다.
 *
 * 본문 없음: 요청이 HEAD 이거나 상태가 204·304 이면 Content-Length 가 있어도 본문이 없다
 * (우리 http_response.c 는 HEAD 응답에도 GET 의 길이를 쓴다). 그래서 요청 메서드를 기억한다.
 *
 * 구현 중 정한 것 (11-23 확인):
 *   C1. 1xx 는 머리가 깨진 것으로 본다 → BAD (요청에서 Expect·Upgrade 를 빼므로 올 일이 없다)
 *   C2. Transfer-Encoding 이 있으면(값 무관) 업스트림 EOF 까지가 본문이다. Content-Length 는 무시하고
 *       바이트는 그대로 중계한다 (chunked 틀도 그대로 지나간다. 어차피 연결은 뒤에 닫는다)
 *   C3. 중계 상한 PROXY_RELAY_MAX: 클라이언트 out 대기 바이트가 이보다 적을 때만 업스트림을 읽는다
 *   C4. 머리 문법은 엄격하게: 줄 끝은 CRLF (맨 LF·맨 CR 은 BAD), obs-fold(줄 맨 앞 SP/TAB)는 BAD,
 *       머리가 HTTP_MAX_HEADER(빈 줄까지 8192바이트)를 넘으면 BAD, 헤더 이름은 토큰 글자, 값에 제어 문자
 *       (TAB 제외) 없음. 상태 줄: "HTTP/1." 숫자 SP 숫자 세 자리, 그 뒤 CRLF 또는 SP + 이유 문구 + CRLF.
 *       상태는 100..599
 *   C5. Content-Length 값: 앞뒤 SP/TAB 을 뗀 뒤 숫자만 1자 이상 (부호·쉼표 목록 불가). 2^53 이나 size_t 를
 *       넘으면 BAD. 줄이 여러 개면 값이 모두 같아야 한다 (숫자로 비교) */
#define PROXY_RELAY_MAX ((size_t)64 << 10)   /* 클라이언트 out 에 쌓아 둘 중계 바이트 상한 (역압력) */

typedef enum { PROXY_BODY_NONE, PROXY_BODY_LENGTH, PROXY_BODY_UNTIL_EOF } proxy_body_mode;

typedef enum {
    PROXY_RELAY_MORE,       /* 업스트림 바이트가 더 필요하다 */
    PROXY_RELAY_COMPLETE,   /* 응답 전체를 out 에 넣었다 → 업스트림 닫고, out 을 다 보낸 뒤 클라이언트 연결 종료 */
    PROXY_RELAY_BAD,        /* 응답 시작 전: 머리가 깨졌거나 너무 길거나 EOF·오류 → 502 */
    PROXY_RELAY_TRUNCATED,  /* 응답 시작 후 업스트림이 일찍 끝났다(EOF·오류) → out 을 다 보내고 연결 종료 */
    PROXY_RELAY_NOMEM       /* buffer 덧붙이기 실패. 시작 전이면 502, 시작 후면 연결 종료 */
} proxy_relay_result;

typedef struct {
    int request_was_head;   /* HEAD 요청이면 본문 없음 */
    int started;            /* 응답 시작 (머리를 out 에 넣음) */
    buffer head;            /* 시작 전 머리를 모으는 대기 버퍼 */
    int status;
    proxy_body_mode body_mode;
    size_t body_left;       /* PROXY_BODY_LENGTH 일 때 남은 본문 바이트 */
} proxy_relay;

void proxy_relay_init(proxy_relay *r, int request_was_head);
/* head 버퍼를 해제한다. 두 번 불러도 안전 */
void proxy_relay_free(proxy_relay *r);

/* 업스트림에서 읽은 n(>0) 바이트를 먹인다. 시작 전에는 head 에 모아 머리를 해석하고, 머리가 끝나면 머리 +
 * 이미 받은 본문을 out 에 덧붙인다(시작). 본문 끝을 넘는 바이트는 버린다.
 * 시작할 때 out 에 넣는 덧붙이기는 한 번에 롤백되므로, NOMEM 이면 out 은 그대로이고 started 는 0 이다.
 * 시작과 동시에 본문이 끝나면(본문 없음, Content-Length 0, 이미 다 받음) 바로 COMPLETE */
proxy_relay_result proxy_relay_input(proxy_relay *r, const char *data, size_t n, buffer *out);

/* 업스트림 EOF 또는 read 오류: 시작 전이면 BAD, UNTIL_EOF 면 COMPLETE, LENGTH 에서 남았으면 TRUNCATED,
 * 이미 끝났으면(NONE / body_left == 0) COMPLETE */
proxy_relay_result proxy_relay_eof(proxy_relay *r);

/* 지금 업스트림에서 최대 몇 바이트를 읽을지. 시작 전: HTTP_MAX_HEADER + 1 - 모은 머리 길이 (한도 초과 판정에
 * 1바이트 여유). 시작 후: room(= 서버가 계산한 PROXY_RELAY_MAX - out 대기 바이트)과 LENGTH 의 body_left 중
 * 작은 값 (UNTIL_EOF 는 room). 0 이면 지금은 읽지 않는다 (본문이 없는 모드는 읽을 것이 없으므로 0) */
size_t proxy_relay_want(const proxy_relay *r, size_t room);

#endif /* PROXY_H */
