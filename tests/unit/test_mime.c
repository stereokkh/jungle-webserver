/*
 * test_mime.c — mime 모듈 단위 테스트 (이슈 #6)
 *
 * Tiny get_filetype() 에서 관찰한 오판(이슈 #3)이 다시 생기지 않는지 확인한다.
 */
#include "test.h"

#include "mime.h"

#define HTML "text/html; charset=utf-8"
#define TEXT "text/plain; charset=utf-8"
#define BIN  "application/octet-stream"

static void test_known_types(void)
{
    CHECK_STR(mime_type("/index.html"), HTML);
    CHECK_STR(mime_type("/index.htm"), HTML);
    CHECK_STR(mime_type("/style.css"), "text/css; charset=utf-8");
    CHECK_STR(mime_type("/app.js"), "text/javascript; charset=utf-8");
    CHECK_STR(mime_type("/data.json"), "application/json");
    CHECK_STR(mime_type("/note.txt"), TEXT);
    CHECK_STR(mime_type("/img/a.png"), "image/png");
    CHECK_STR(mime_type("/img/a.jpg"), "image/jpeg");
    CHECK_STR(mime_type("/img/a.jpeg"), "image/jpeg");
    CHECK_STR(mime_type("/img/a.gif"), "image/gif");
    CHECK_STR(mime_type("/img/a.svg"), "image/svg+xml");
    CHECK_STR(mime_type("/favicon.ico"), "image/x-icon");
}

/* 이슈 #3 재현에서 Tiny 가 틀린 경우들 */
static void test_tiny_mistakes(void)
{
    CHECK_STR(mime_type("/report.html.txt"), TEXT);   /* Tiny: text/html (strstr) */
    CHECK_STR(mime_type("/HOME.HTML"), HTML);          /* Tiny: text/plain (대소문자) */
    CHECK_STR(mime_type("/img/CAT.PNG"), "image/png");
    CHECK_STR(mime_type("/style.css"), "text/css; charset=utf-8"); /* Tiny: text/plain */
    CHECK_STR(mime_type("/a.zip"), BIN);               /* Tiny: text/plain */
}

/* 확장자가 없거나 애매한 경우는 모두 기본값 */
static void test_edge_cases(void)
{
    CHECK_STR(mime_type("/a.b/README"), BIN);   /* 폴더 이름의 '.' */
    CHECK_STR(mime_type("/.bashrc"), BIN);      /* 숨김 파일: 확장자 아님 */
    CHECK_STR(mime_type("/file."), BIN);        /* 빈 확장자 */
    CHECK_STR(mime_type("/noext"), BIN);
    CHECK_STR(mime_type("/"), BIN);
    CHECK_STR(mime_type(""), BIN);
    CHECK_STR(mime_type("index.html"), HTML);   /* '/' 없는 이름 */
    CHECK_STR(mime_type("/a.html/"), BIN);      /* '/' 로 끝나면 파일 이름이 비어 있음 */
    CHECK_STR(mime_type("/x.htmlx"), BIN);      /* 앞부분만 같은 확장자 */
    CHECK_STR(mime_type(NULL), BIN);
}

int main(void)
{
    RUN(test_known_types);
    RUN(test_tiny_mistakes);
    RUN(test_edge_cases);
    return test_report();
}
