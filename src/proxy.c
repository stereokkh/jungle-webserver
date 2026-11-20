/*
 * proxy.c — 업스트림(백엔드) 중계
 *
 * CS:APP 대응: 없음
 * 흐름 (#9): handler_route → ROUTE_PROXY → proxy_next_backend → proxy_connect_start
 *   → POLLOUT 에서 SO_ERROR 확인 → 요청 송신 → 응답 수신 → 클라이언트로 중계
 *   - 응답 시작 전 실패: 502 (연결 실패) / 504 (타임아웃)
 *   - 응답 시작 후 실패: 연결 종료 + 자원 회수
 *   - 자동 재시도 없음
 * 담당: 트랙 B · 이슈 #9
 */
#include "proxy.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

int proxy_build_request(const http_request *req, const proxy_backend *backend,
                        const char *client_ip, char *out, size_t cap)
{
    /* TODO(#9): 요청 라인(method target HTTP/1.1) + 헤더 재작성 + 본문 (단위 테스트 대상) */
    (void)req; (void)backend; (void)client_ip; (void)out; (void)cap;
    return -1;
}

int proxy_connect_start(const proxy_backend *backend)
{
    /* TODO(#9): socket → net_set_nonblocking → connect. EINPROGRESS 면 fd 반환 */
    (void)backend;
    return -1;
}
