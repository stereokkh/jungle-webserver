/*
 * buffer.c — 늘어나는 바이트 버퍼
 *
 * CS:APP 대응: rio_t 의 내부 버퍼
 *   Tiny 의 sprintf(buf, "%s...", buf) 는 같은 버퍼를 읽으면서 쓰는 정의되지 않은 동작이고
 *   길이 검사도 없다 (이슈 #3 관찰). 여기서는 vsnprintf 로 길이를 먼저 재고 공간을 확보한다.
 * 담당: 트랙 A · 이슈 #4
 */
#include "buffer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_MIN_CAP 1024

void buffer_init(buffer *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->off = 0;
}

void buffer_free(buffer *b)
{
    free(b->data);
    buffer_init(b);
}

void buffer_clear(buffer *b)
{
    b->len = 0;
    b->off = 0;
}

int buffer_reserve(buffer *b, size_t extra)
{
    size_t need, ncap;
    char *p;

    if (extra > BUFFER_MAX - b->len)
        return -1;
    need = b->len + extra;
    if (need <= b->cap)
        return 0;

    /* 앞쪽에 소비한 공간이 충분하면 늘리지 않고 당겨서 쓴다 */
    if (b->off > 0 && need - b->off <= b->cap) {
        buffer_compact(b);
        return 0;
    }

    ncap = b->cap ? b->cap : BUFFER_MIN_CAP;
    while (ncap < need)
        ncap = ncap > BUFFER_MAX / 2 ? BUFFER_MAX : ncap * 2;

    p = realloc(b->data, ncap);
    if (p == NULL)
        return -1;
    b->data = p;
    b->cap = ncap;
    return 0;
}

int buffer_append(buffer *b, const void *p, size_t n)
{
    if (n == 0)
        return 0;
    if (buffer_reserve(b, n) < 0)
        return -1;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 0;
}

int buffer_append_str(buffer *b, const char *s)
{
    return buffer_append(b, s, strlen(s));
}

int buffer_appendf(buffer *b, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;

    /* vsnprintf 는 NUL 까지 쓰므로 1 바이트 더 확보한다 (len 에는 포함하지 않음) */
    if (buffer_reserve(b, (size_t)n + 1) < 0)
        return -1;

    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
    return 0;
}

const char *buffer_peek(const buffer *b)
{
    return b->data ? b->data + b->off : "";
}

size_t buffer_pending(const buffer *b)
{
    return b->len - b->off;
}

void buffer_consume(buffer *b, size_t n)
{
    if (n > b->len - b->off)
        n = b->len - b->off;
    b->off += n;
    if (b->off == b->len) {
        b->off = 0;
        b->len = 0;
    }
}

void buffer_compact(buffer *b)
{
    if (b->off == 0)
        return;
    memmove(b->data, b->data + b->off, b->len - b->off);
    b->len -= b->off;
    b->off = 0;
}

size_t buffer_mark(const buffer *b)
{
    return b->len - b->off;
}

void buffer_rollback(buffer *b, size_t mark)
{
    if (mark < b->len - b->off)
        b->len = b->off + mark;
}
