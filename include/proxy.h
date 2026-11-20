/*
 * proxy.h — 업스트림(백엔드) 중계
 *
 * CS:APP 대응: 없음 (Tiny에 없음. Proxy Lab은 Forward Proxy + 블로킹이라 구조가 다르다)
 * 담당: 트랙 B · 이슈 #9 (이벤트 루프 통합은 A와 협업)
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

/* 백엔드로 보낼 요청 전체를 out 뒤에 덧붙인다:
 *   요청 라인 "<method_str> <target> HTTP/1.1" (원본 target 유지),
 *   req->headers 에서 hop-by-hop 헤더(Connection, Keep-Alive, Proxy-*, TE, Trailer,
 *   Transfer-Encoding, Upgrade)와 Host 를 빼고 전달, Host: <backend name>,
 *   X-Forwarded-For: <client_ip> 추가, Connection: close, 본문 그대로.
 * 성공 0, 실패 -1 (out 은 그대로) */
int proxy_build_request(const http_request *req, const proxy_backend *backend,
                        const char *client_ip, buffer *out);

/* 논블로킹 connect 를 시작한다. 성공(진행 중 포함): fd, 실패: -1.
 * 완료 여부는 POLLOUT 후 getsockopt(SO_ERROR) 로 확인한다. fd 는 서버가 닫는다 */
int proxy_connect_start(const proxy_backend *backend);

#endif /* PROXY_H */
