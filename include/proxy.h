/*
 * proxy.h — 업스트림(백엔드) 중계
 *
 * CS:APP 대응: 없음 (Tiny에 없음. Proxy Lab은 Forward Proxy + 블로킹이라 구조가 다르다)
 * 담당: 트랙 B · 이슈 #8 (proxy_build_request: 요청 다시 쓰기), #9 (connect·중계, 이벤트 루프 통합은 A와 협업)
 *
 * 서버(server.c)에서 쓰는 순서 (#9 에서 A·B 가 함께 확정):
 *   1. handler_route → ROUTE_PROXY, route → proxy_next_backend(route)
 *   2. proxy_build_request(req, backend, client_ip, &up_out)   ← 입력 버퍼 consume 전에
 *   3. fd = proxy_connect_start(backend) → 실패면 502
 *   4. upstream POLLOUT → getsockopt(SO_ERROR) 확인 → up_out 송신
 *   5. upstream POLLIN → 받은 바이트를 클라이언트 out 버퍼로 중계
 *   - 응답 시작 전 실패 502, UPSTREAM_TIMEOUT_MS 초과 504, 시작 후 실패는 연결 종료
 *   - 업스트림 응답 해석(상태 줄 검사, 응답 끝 판단)은 #9 에서 함수를 추가한다
 */
#ifndef PROXY_H
#define PROXY_H

#include "buffer.h"
#include "http_request.h"
#include "proxy_config.h"

/* 백엔드로 보낼 요청 전체를 out 뒤에 덧붙인다. 바이트 순서:
 *   1. "<method_str> <target> HTTP/1.1\r\n"  (클라이언트가 1.0 이어도 1.1. target 은 원본 그대로:
 *      디코딩·접두사 제거·"../" 정리 없음)
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
 *   응답 코드는 #9 가 정한다) */
int proxy_build_request(const http_request *req, const proxy_backend *backend,
                        const char *client_ip, buffer *out);

/* 논블로킹 connect 를 시작한다. 성공(진행 중 포함): fd, 실패: -1.
 * 완료 여부는 POLLOUT 후 getsockopt(SO_ERROR) 로 확인한다. fd 는 서버가 닫는다 */
int proxy_connect_start(const proxy_backend *backend);

#endif /* PROXY_H */
