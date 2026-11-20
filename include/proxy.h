/*
 * proxy.h — 업스트림(백엔드) 중계
 *
 * CS:APP 대응: 없음 (Tiny에 없음. Proxy Lab은 Forward Proxy + 블로킹이라 구조가 다르다)
 * 담당: 트랙 B · 이슈 #9 (이벤트 루프 통합은 A와 협업)
 */
#ifndef PROXY_H
#define PROXY_H

#include <stddef.h>
#include "http_request.h"
#include "proxy_config.h"

/* 백엔드로 보낼 요청을 만든다: 원본 target 유지, Host·X-Forwarded-For 재작성,
 * hop-by-hop 헤더 제거, Connection: close, 본문 그대로. 성공: 바이트 수, cap 부족: -1 */
int proxy_build_request(const http_request *req, const proxy_backend *backend,
                        const char *client_ip, char *out, size_t cap);

/* 논블로킹 connect 를 시작한다. 성공(진행 중 포함): fd, 실패: -1.
 * 완료 여부는 POLLOUT 후 getsockopt(SO_ERROR) 로 확인한다 */
int proxy_connect_start(const proxy_backend *backend);

#endif /* PROXY_H */
