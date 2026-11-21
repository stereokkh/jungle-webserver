/*
 * test_static_file.c — static_file 모듈 단위 테스트 (이슈 #6)
 *
 * /tmp 아래 임시 디렉터리에 docroot(www/)와 그 밖의 비밀 파일을 만들어 놓고
 *   - 상태 코드: 이슈 #3 에서 Tiny 가 뚫린 ".." 탈출, 심볼릭 링크 탈출, FIFO 등
 *   - 나눠 보내기: non-blocking pipe 를 소켓 대신 써서 EAGAIN·이어 보내기·SIGPIPE
 * 를 확인한다.
 */
#include "test.h"

#include "static_file.h"

#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#define HTML "text/html; charset=utf-8"
#define INDEX "<h1>hi</h1>\n"
#define DOCS_INDEX "<h1>docs</h1>\n"
#define BIG_SIZE 200000   /* pipe 용량(리눅스 64KB)보다 커야 EAGAIN 이 난다 */

static char base[64];     /* /tmp/sf_XXXXXX */
static char www[128];     /* base/www = docroot */

static void path_of(char *dst, size_t n, const char *rel)
{
    snprintf(dst, n, "%s/%s", base, rel);
}

static void put(const char *rel, const char *data, size_t len)
{
    char p[256];
    FILE *fp;

    path_of(p, sizeof p, rel);
    fp = fopen(p, "wb");
    if (fp == NULL) {
        perror(p);
        exit(1);
    }
    fwrite(data, 1, len, fp);
    fclose(fp);
}

static void mk(const char *rel)
{
    char p[256];

    path_of(p, sizeof p, rel);
    if (mkdir(p, 0755) < 0) {
        perror(p);
        exit(1);
    }
}

static void ln(const char *target, const char *rel)
{
    char p[256];

    path_of(p, sizeof p, rel);
    if (symlink(target, p) < 0) {
        perror(p);
        exit(1);
    }
}

static unsigned char big_byte(long i)
{
    return (unsigned char)(i * 7 + i / 251);
}

static void setup(void)
{
    char p[256];
    unsigned char *big = malloc(BIG_SIZE);
    long i;

    snprintf(base, sizeof base, "/tmp/sf_XXXXXX");
    if (big == NULL || mkdtemp(base) == NULL) {
        perror("setup");
        exit(1);
    }
    snprintf(www, sizeof www, "%s/www", base);

    put("secret.txt", "secret\n", 7);              /* docroot 밖 */
    mk("www2");
    put("www2/x.txt", "x\n", 2);                   /* 이름이 www 로 시작하는 이웃 */
    mk("www");
    put("www/index.html", INDEX, strlen(INDEX));
    put("www/style.css", "body{}\n", 7);
    put("www/empty.txt", "", 0);
    mk("www/img");
    put("www/img/CAT.PNG", "png", 3);
    mk("www/docs");
    put("www/docs/index.html", DOCS_INDEX, strlen(DOCS_INDEX));
    mk("www/emptydir");
    ln("../secret.txt", "www/link_out");
    ln("../www2/x.txt", "www/link_sib");
    ln("index.html", "www/link_in");
    path_of(p, sizeof p, "www/fifo");
    if (mkfifo(p, 0644) < 0) {
        perror(p);
        exit(1);
    }
    put("www/locked.txt", "no\n", 3);
    path_of(p, sizeof p, "www/locked.txt");
    chmod(p, 0);

    for (i = 0; i < BIG_SIZE; i++)
        big[i] = big_byte(i);
    put("www/big.bin", (const char *)big, BIG_SIZE);
    free(big);
}

static void teardown(void)
{
    char cmd[128];

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", base);
    if (system(cmd) != 0)
        fprintf(stderr, "  (임시 디렉터리 정리 실패: %s)\n", base);
}

/* 성공이면 200, 실패면 *status. 실패 시 fd 가 -1 인지도 확인한다 */
static int try_open(const char *path)
{
    static_file f = STATIC_FILE_INIT;
    int status = 0;

    if (static_open(www, path, &f, &status) == 0) {
        CHECK(f.fd >= 0);
        static_close(&f);
        return 200;
    }
    CHECK_INT(f.fd, -1);
    return status;
}

static void test_open_ok(void)
{
    static_file f = STATIC_FILE_INIT;
    int status = 0;

    REQUIRE(static_open(www, "/index.html", &f, &status) == 0);
    CHECK_INT(f.size, strlen(INDEX));
    CHECK_INT(f.offset, 0);
    CHECK_STR(f.content_type, HTML);
    static_close(&f);

    REQUIRE(static_open(www, "/img/CAT.PNG", &f, &status) == 0);
    CHECK_STR(f.content_type, "image/png");
    static_close(&f);

    REQUIRE(static_open(www, "/style.css", &f, &status) == 0);
    CHECK_STR(f.content_type, "text/css; charset=utf-8");
    static_close(&f);
}

/* 디렉터리면 index.html. Content-Type 도 index.html 기준 */
static void test_open_index(void)
{
    static_file f = STATIC_FILE_INIT;
    int status = 0;

    REQUIRE(static_open(www, "/", &f, &status) == 0);
    CHECK_INT(f.size, strlen(INDEX));
    CHECK_STR(f.content_type, HTML);
    static_close(&f);

    REQUIRE(static_open(www, "/docs", &f, &status) == 0);
    CHECK_INT(f.size, strlen(DOCS_INDEX));
    static_close(&f);

    REQUIRE(static_open(www, "/docs/", &f, &status) == 0);
    CHECK_INT(f.size, strlen(DOCS_INDEX));
    static_close(&f);

    CHECK_INT(try_open("/emptydir/"), 404);   /* index.html 없는 디렉터리 */
}

/* 경로 정리: 안에서 움직이는 "." ".." "//" 는 허용 */
static void test_open_normalize(void)
{
    CHECK_INT(try_open("//index.html"), 200);
    CHECK_INT(try_open("/./index.html"), 200);
    CHECK_INT(try_open("/img/../index.html"), 200);
    CHECK_INT(try_open("/docs/./../img/CAT.PNG"), 200);
}

/* 이슈 #3 재현 ②: Tiny 는 "." + uri 라서 ".." 로 밖에 나갔다.
 * 우리는 파일이 있든 없든 403 (존재 여부를 알려 주지 않는다) */
static void test_open_traversal(void)
{
    CHECK_INT(try_open("/../secret.txt"), 403);
    CHECK_INT(try_open("/../nothing.txt"), 403);
    CHECK_INT(try_open("/a/../../secret.txt"), 403);
    CHECK_INT(try_open("/img/../../secret.txt"), 403);
    CHECK_INT(try_open("/.."), 403);
    CHECK_INT(try_open("index.html"), 403);          /* '/' 로 시작하지 않음 */
    /* 디코딩은 파서 몫. 여기 들어온 "%2e%2e" 는 그냥 그런 이름의 파일 → 없음 */
    CHECK_INT(try_open("/%2e%2e/secret.txt"), 404);
}

/* 심볼릭 링크: 안을 가리키면 OK, 밖(이웃 www2 포함)을 가리키면 403 */
static void test_open_symlink(void)
{
    CHECK_INT(try_open("/link_in"), 200);
    CHECK_INT(try_open("/link_out"), 403);
    CHECK_INT(try_open("/link_sib"), 403);
}

static void test_open_errors(void)
{
    static_file f = STATIC_FILE_INIT;
    int status = 0;

    CHECK_INT(try_open("/nope.html"), 404);
    CHECK_INT(try_open("/index.html/"), 404);       /* 파일 뒤에 '/' */
    CHECK_INT(try_open("/index.html/x"), 404);
    CHECK_INT(try_open("/fifo"), 403);              /* 일반 파일 아님 (open 하면 멈춤) */
    if (geteuid() != 0)                             /* root 는 권한 검사를 무시한다 */
        CHECK_INT(try_open("/locked.txt"), 403);

    CHECK_INT(static_open("/no/such/docroot", "/", &f, &status), -1);
    CHECK_INT(status, 500);
    CHECK_INT(f.fd, -1);
}

/* 빈 파일은 보낼 게 없으니 바로 1 */
static void test_send_empty(void)
{
    static_file f = STATIC_FILE_INIT;
    int status = 0, p[2];

    REQUIRE(static_open(www, "/empty.txt", &f, &status) == 0);
    REQUIRE(pipe(p) == 0);
    CHECK_INT(f.size, 0);
    CHECK_INT(static_send_some(&f, p[1]), 1);
    static_close(&f);
    close(p[0]);
    close(p[1]);
}

/* pipe 에서 지금 읽을 수 있는 만큼 got 에 붙인다 */
static void drain(int rfd, unsigned char *got, long *n)
{
    ssize_t r;

    while (*n < BIG_SIZE && (r = read(rfd, got + *n, (size_t)(BIG_SIZE - *n))) > 0)
        *n += r;
}

/* 큰 파일: 안 비우면 EAGAIN 으로 0 이 나오고 offset 은 그대로,
 * 비우면서 계속 부르면 1 로 끝나고 바이트가 원본과 같다 */
static void test_send_big(void)
{
    static_file f = STATIC_FILE_INIT;
    unsigned char *got = malloc(BIG_SIZE);
    int status = 0, p[2], r = 0, stuck = 0, i;
    long n = 0, bad = -1;
    off_t before;

    REQUIRE(got != NULL);
    REQUIRE(static_open(www, "/big.bin", &f, &status) == 0);
    REQUIRE(pipe(p) == 0);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    CHECK_INT(f.size, BIG_SIZE);
    CHECK_STR(f.content_type, "application/octet-stream");

    /* 1. 비우지 않고 계속 → pipe 가 차면 0 이고 offset 이 안 움직인다 */
    for (i = 0; i < 100 && !stuck; i++) {
        before = f.offset;
        r = static_send_some(&f, p[1]);
        CHECK(r == 0);
        stuck = f.offset == before;
    }
    CHECK(stuck);
    CHECK(f.offset > 0 && f.offset < BIG_SIZE);

    /* 2. 비우면서 끝까지 */
    for (i = 0; i < 10000 && r != 1; i++) {
        drain(p[0], got, &n);
        r = static_send_some(&f, p[1]);
        CHECK(r >= 0);
    }
    drain(p[0], got, &n);
    CHECK_INT(r, 1);
    CHECK_INT(f.offset, BIG_SIZE);
    CHECK_INT(n, BIG_SIZE);
    for (i = 0; i < n && bad < 0; i++)
        if (got[i] != big_byte(i))
            bad = i;
    CHECK_INT(bad, -1);

    /* 3. 다 보낸 뒤 또 불러도 1 */
    CHECK_INT(static_send_some(&f, p[1]), 1);

    static_close(&f);
    close(p[0]);
    close(p[1]);
    free(got);
}

/* 클라이언트가 먼저 끊으면 -1 + EPIPE (SIGPIPE 는 서버처럼 무시) */
static void test_send_closed_peer(void)
{
    static_file f = STATIC_FILE_INIT;
    int status = 0, p[2];

    REQUIRE(static_open(www, "/index.html", &f, &status) == 0);
    REQUIRE(pipe(p) == 0);
    close(p[0]);
    errno = 0;
    CHECK_INT(static_send_some(&f, p[1]), -1);
    CHECK_INT(errno, EPIPE);
    CHECK_INT(f.offset, 0);
    static_close(&f);
    close(p[1]);
}

static void test_close_twice(void)
{
    static_file f = STATIC_FILE_INIT;
    int status = 0;

    REQUIRE(static_open(www, "/index.html", &f, &status) == 0);
    static_close(&f);
    CHECK_INT(f.fd, -1);
    static_close(&f);
    CHECK_INT(f.fd, -1);
}

int main(void)
{
    int rc;

    signal(SIGPIPE, SIG_IGN);
    setup();
    RUN(test_open_ok);
    RUN(test_open_index);
    RUN(test_open_normalize);
    RUN(test_open_traversal);
    RUN(test_open_symlink);
    RUN(test_open_errors);
    RUN(test_send_empty);
    RUN(test_send_big);
    RUN(test_send_closed_peer);
    RUN(test_close_twice);
    rc = test_report();
    teardown();
    return rc;
}
