/*
 * test_contract.c — 모든 모듈이 구현 전후로 계속 지켜야 하는 공통 계약
 *
 * 지금은 스텁도 통과한다. 각 트랙은 구현하면서 이 계약을 깨지 않아야 하고,
 * 모듈별 동작 테스트는 test_<모듈>.c 에 따로 추가한다 (이슈 #5 #6 #8 #10).
 */
#include "test.h"

#include "app.h"
#include "buffer.h"
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

/* 덧붙이기에 실패하면 out 은 손대지 않은 상태 그대로다 (스텁은 항상 실패) */
static void test_append_failure_keeps_buffer(void)
{
    buffer out = BUFFER_INIT;
    http_response_head h = { 200, "text/plain", 0, 1, NULL };

    REQUIRE(buffer_append_str(&out, "keep") == 0);
    if (http_append_header(&out, &h) < 0)
        CHECK_MEM(buffer_peek(&out), buffer_pending(&out), "keep", 4);
    buffer_clear(&out);
    if (http_append_error(&out, 404, 0, 0, NULL) < 0)
        CHECK_INT(buffer_pending(&out), 0);
    buffer_free(&out);
}

static void set_get(http_request *req, const char *path)
{
    memset(req, 0, sizeof *req);
    req->method = HTTP_GET;
    strcpy(req->method_str, "GET");
    strcpy(req->target, path);
    strcpy(req->path, path);
    req->version_minor = 1;
    req->content_length = -1;
}

/* 프록시 설정이 없으면(백엔드 인스턴스) 앱 경로는 앱, 나머지는 정적 파일로 간다 */
static void test_route_without_proxy(void)
{
    static http_request req;   /* 수 KB 라서 스택에 두지 않는다 */
    static proxy_route dummy;
    proxy_route *route = &dummy;   /* NULL 로 바뀌는지 본다 */

    set_get(&req, "/index.html");
    CHECK_INT(handler_route(&req, NULL, &route), ROUTE_STATIC);
    CHECK(route == NULL);

    set_get(&req, "/api/add");
    CHECK_INT(handler_route(&req, NULL, &route), ROUTE_APP);
    CHECK(route == NULL);
}

/* 앱 경로는 "/api/" 접두사로만 판단한다 (Tiny 의 strstr("cgi-bin") 오판 방지) */
static void test_app_match_prefix_only(void)
{
    static http_request req;

    set_get(&req, "/api/echo");
    CHECK_INT(app_match(&req), 1);
    set_get(&req, "/index.html");
    CHECK_INT(app_match(&req), 0);
    set_get(&req, "/static/api/x");
    CHECK_INT(app_match(&req), 0);
    set_get(&req, "/api");
    CHECK_INT(app_match(&req), 0);

    /* app_match 는 target(파서가 만든 정규형)을 바이트 그대로 본다. "/%61pi/add" 같은 표기는
     * 파서가 "/api/add" 로 바꿔서 넘기므로 app_match 가 디코딩하지 않는다 (#31).
     * 파서를 거친 동작은 test_app.c test_add_normalized */
    set_get(&req, "/api/add");
    strcpy(req.target, "/api/add?a=1&b=2");
    CHECK_INT(app_match(&req), 1);
}

/* 정리 함수는 두 번 불러도 안전하다 (실패 경로에서 중복 정리 가능) */
static void test_static_close_idempotent(void)
{
    static_file f = STATIC_FILE_INIT;
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

/* Round-Robin (#9): 백엔드 셋이면 0, 1, 2, 0 ...  하나면 늘 0. 라우트마다 순번이 따로다 */
static void test_next_backend_round_robin(void)
{
    static proxy_route a, b;                    /* 수 KB 라서 스택에 두지 않는다 */
    size_t i;

    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.nbackends = 3;
    for (i = 0; i < 7; i++)
        CHECK(proxy_next_backend(&a) == &a.backends[i % 3]);
    CHECK_INT(a.next, 7 % 3);

    b.nbackends = 1;
    for (i = 0; i < 4; i++)
        CHECK(proxy_next_backend(&b) == &b.backends[0]);
    CHECK_INT(b.next, 0);

    b.nbackends = 2;                            /* 다른 라우트의 호출이 a 의 순번을 건드리지 않는다 */
    CHECK(proxy_next_backend(&b) == &b.backends[0]);
    CHECK(proxy_next_backend(&b) == &b.backends[1]);
    CHECK(proxy_next_backend(&a) == &a.backends[1]);

    a.next = 99;                                /* 범위 밖 순번도 나머지로 맞춘다 */
    CHECK(proxy_next_backend(&a) == &a.backends[99 % 3]);
    a.nbackends = 0;
    CHECK(proxy_next_backend(&a) == NULL);
}

int main(void)
{
    RUN(test_mime_never_null);
    RUN(test_reason_never_null);
    RUN(test_append_failure_keeps_buffer);
    RUN(test_route_without_proxy);
    RUN(test_app_match_prefix_only);
    RUN(test_static_close_idempotent);
    RUN(test_next_backend_null_safe);
    RUN(test_next_backend_round_robin);
    return test_report();
}
