/*
 * mime.h — 확장자 → Content-Type
 *
 * CS:APP 대응: tiny.c get_filetype() (html, gif, png, jpg, 그 외 text/plain)
 * 담당: 트랙 C · 이슈 #6
 * 현대식으로 바뀌는 점: css, js, json, svg, ico 추가, 대소문자 무시,
 *                       모르는 확장자는 application/octet-stream
 */
#ifndef MIME_H
#define MIME_H

/* "a/b/index.HTML" → "text/html; charset=utf-8". 항상 NULL 이 아닌 문자열 */
const char *mime_type(const char *path);

#endif /* MIME_H */
