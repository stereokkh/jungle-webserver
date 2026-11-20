/*
 * test_contract.c — 모든 모듈이 구현 전후로 계속 지켜야 하는 공통 계약
 *
 * 지금은 스텁도 통과한다. 각 트랙은 구현하면서 이 계약을 깨지 않아야 하고,
 * 모듈별 동작 테스트는 test_<모듈>.c 에 따로 추가한다 (이슈 #5 #6 #8 #10).
 */
#include "test.h"

#include "app.h"
#include "handler.h"
#include "http_request.h"
#include "http_response.h"
#include "mime.h"
#include "proxy_config.h"
#include "static_file.h"

/* mime_type 은 어떤 입력에도 NULL 을 돌려주지 않는다 */
static void test_mime_never_null(void)
{
    CHECK(mime_type("") != NULL);
    CHECK(mime_type("noext") != NULL);
    CHECK(mime_type("dir.d/noext") != NULL);
    CHECK(mime_type("a.HTML") != NULL);
    CHECK(mime_type(".") != NULL);
}

/* http_reason 은 모르는 코드에도 NULL 을 돌려주지 않는다 */
static void test_reason_never_null(void)
{
    CHECK(http_reason(200) != NULL);
    CHECK(http_reason(404) != NULL);
    CHECK(http_reason(999) != NULL);
    CHECK(http_reason(-1) != NULL);
}

/* 버퍼가 모자라면 -1 이고 cap 을 넘어 쓰지 않는다 */
static void test_build_respects_cap(void)
{
    char buf[1];
    CHECK_INT(http_build_header(buf, 0, 200, "text/plain", 0, 1), -1);
    CHECK_INT(http_build_error(buf, 0, 404, 0), -1);
}

/* 프록시 설정이 없으면(백엔드 인스턴스) 일반 경로는 정적 파일로 간다 */
static void test_route_without_proxy(void)
{
    static http_request req;   /* 수 KB 라서 스택에 두지 않는다 */
    memset(&req, 0, sizeof req);
    req.method = HTTP_GET;
    strcpy(req.target, "/index.html");
    strcpy(req.path, "/index.html");
    req.content_length = -1;

    CHECK_INT(handler_route(&req, NULL), ROUTE_STATIC);
}

/* 앱 경로가 아니면 0 을 돌려주고 아무것도 쓰지 않는다 */
static void test_app_ignores_other_paths(void)
{
    static http_request req;
    char out[64];
    size_t out_len = 123;

    memset(&req, 0, sizeof req);
    req.method = HTTP_GET;
    strcpy(req.target, "/index.html");
    strcpy(req.path, "/index.html");
    req.content_length = -1;

    CHECK_INT(app_handle(&req, "8080", out, sizeof out, &out_len), 0);
    CHECK_INT(out_len, 0);
}

/* 정리 함수는 두 번 불러도 안전하다 (실패 경로에서 중복 정리 가능) */
static void test_static_close_idempotent(void)
{
    static_file f = { .fd = -1, .size = 0, .offset = 0 };
    static_close(&f);
    static_close(&f);
    CHECK_INT(f.fd, -1);
}

static void test_next_backend_null_safe(void)
{
    proxy_route route;
    memset(&route, 0, sizeof route);
    CHECK(proxy_next_backend(NULL) == NULL);
    CHECK(proxy_next_backend(&route) == NULL);
}

int main(void)
{
    RUN(test_mime_never_null);
    RUN(test_reason_never_null);
    RUN(test_build_respects_cap);
    RUN(test_route_without_proxy);
    RUN(test_app_ignores_other_paths);
    RUN(test_static_close_idempotent);
    RUN(test_next_backend_null_safe);
    return test_report();
}
