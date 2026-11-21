/*
 * mime.c — 확장자 → Content-Type
 *
 * CS:APP 대응: get_filetype()
 *   Tiny는 strstr(filename, ".html") 처럼 "포함"으로 판단하고 대소문자를 구분한다.
 *   이슈 #3 관찰: report.html.txt → text/html, HOME.HTML → text/plain, style.css → text/plain.
 *   여기서는 파일 이름(마지막 '/' 뒤)의 마지막 '.' 뒤 확장자만 대소문자 무시로 표에서 찾는다.
 *   모르는 확장자는 text/plain 이 아니라 application/octet-stream (RFC 9110 8.3, nginx 기본값과 같음).
 * 담당: 트랙 C · 이슈 #6 (단위 테스트: tests/unit/test_mime.c)
 *
 * 판정 순서 (mime_type):
 *   1. 마지막 '/' 뒤만 남긴다          "/a.b/README"     → "README"
 *   2. 그 안에서 마지막 '.' 을 찾는다  "report.html.txt" → ".txt"
 *   3. '.' 이 없거나, 맨 앞이거나(.bashrc), 맨 끝이면(file.) → 기본값
 *   4. '.' 뒤를 mime_table 에서 대소문자 무시로 찾는다. 없으면 기본값
 */
#include "mime.h"

#include <stddef.h>
#include <string.h>
#include <strings.h>   /* strcasecmp (POSIX) */

/* 표에 없을 때. 브라우저가 화면에 그리지 않고 다운로드로 처리한다 */
#define MIME_DEFAULT "application/octet-stream"

/*
 * 확장자('.' 없이, 소문자) → Content-Type.
 * 추가할 때는 여기에 한 줄 넣고 tests/unit/test_mime.c 에 CHECK 를 한 줄 넣는다.
 * text 계열은 한글이 깨지지 않도록 charset 을 붙인다.
 */
static const struct {
    const char *ext;
    const char *type;
} mime_table[] = {
    { "html", "text/html; charset=utf-8" },
    { "htm",  "text/html; charset=utf-8" },
    { "css",  "text/css; charset=utf-8" },
    { "js",   "text/javascript; charset=utf-8" },
    { "txt",  "text/plain; charset=utf-8" },
    { "json", "application/json" },          /* JSON 은 규격상 항상 UTF-8 이라 charset 불필요 */
    { "png",  "image/png" },
    { "jpg",  "image/jpeg" },
    { "jpeg", "image/jpeg" },
    { "gif",  "image/gif" },
    { "svg",  "image/svg+xml" },
    { "ico",  "image/x-icon" },
};

const char *mime_type(const char *path)
{
    const char *name, *dot;
    size_t i;

    if (path == NULL)
        return MIME_DEFAULT;

    /* 1. 파일 이름만 본다: "/a.b/README" 의 폴더 쪽 '.' 에 속지 않게 */
    name = strrchr(path, '/');
    name = name ? name + 1 : path;

    /* 2~3. 마지막 '.' (Tiny 의 strstr 처럼 중간의 ".html" 을 보지 않는다).
     *      ".bashrc" 처럼 맨 앞의 '.' 은 확장자가 아니라 숨김 파일 표시, "file." 은 빈 확장자 */
    dot = strrchr(name, '.');
    if (dot == NULL || dot == name || dot[1] == '\0')
        return MIME_DEFAULT;

    /* 4. 대소문자 무시 비교 (Tiny 는 HOME.HTML 을 text/plain 으로 오판) */
    for (i = 0; i < sizeof mime_table / sizeof mime_table[0]; i++)
        if (strcasecmp(dot + 1, mime_table[i].ext) == 0)
            return mime_table[i].type;

    return MIME_DEFAULT;
}
