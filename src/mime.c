/*
 * mime.c — 확장자 → Content-Type
 *
 * CS:APP 대응: get_filetype()
 *   Tiny는 strstr(filename, ".html") 처럼 "포함"으로 판단한다 → "a.html.png" 오판 가능.
 *   여기서는 마지막 '.' 뒤의 확장자만 표에서 찾는다.
 * 담당: 트랙 C · 이슈 #6 (단위 테스트: tests/unit/)
 */
#include "mime.h"

#include <string.h>

const char *mime_type(const char *path)
{
    /* TODO(#6): strrchr(path, '.') → 대소문자 무시 비교 표
     *  html htm css js json txt png jpg jpeg gif svg ico */
    (void)path;
    return "application/octet-stream";
}
