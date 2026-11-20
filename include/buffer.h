/*
 * buffer.h — 늘어나는 바이트 버퍼 (연결의 입력·출력 버퍼, 응답·프록시 요청 조립)
 *
 * CS:APP 대응: rio_t 의 내부 버퍼(rio_buf, rio_bufptr, rio_cnt) 와 비슷한 역할
 *   Tiny 는 고정 크기 MAXLINE/MAXBUF 배열에 sprintf 로 쓰고 길이 검사를 하지 않는다.
 *   여기서는 필요한 만큼 늘리되 BUFFER_MAX 를 넘으면 -1 로 거절한다.
 * 담당: 트랙 A · 이슈 #4 (단위 테스트: tests/unit/test_buffer.c)
 *
 * 구조:
 *
 *   data: [ 이미 소비/송신함 | 아직 남은 바이트 | 빈 공간 ]
 *          0               off              len          cap
 *
 *   - 쓰는 쪽은 len 뒤에 덧붙인다 (buffer_append, buffer_appendf)
 *   - 읽는/보내는 쪽은 off 부터 len 까지 쓰고 buffer_consume 으로 off 를 민다
 *     (출력 버퍼에서는 off 가 "송신 진행 상태"다. write 가 EAGAIN 이면 여기서 이어서 보낸다)
 *
 * 소유권 (docs/interfaces.md):
 *   - 버퍼는 만든 쪽이 소유하고 buffer_free 로 해제한다. 연결의 in/out 은 conn(server.c)이 소유
 *   - 다른 모듈은 넘겨받은 buffer 에 덧붙이기만 하고 free 하지 않는다
 *   - data 를 가리키는 포인터(예: http_request.body)는 그 버퍼에 append / consume / compact
 *     하는 순간 무효가 될 수 있다 (realloc, memmove)
 */
#ifndef BUFFER_H
#define BUFFER_H

#include <stddef.h>

/* 한 버퍼의 최대 크기. 요청 본문 상한(HTTP_MAX_BODY, 1MB) + 헤더 여유 */
#define BUFFER_MAX ((size_t)4 << 20)

typedef struct {
    char  *data;
    size_t len;   /* 유효한 바이트의 끝 */
    size_t cap;   /* 할당된 크기 */
    size_t off;   /* 소비/송신한 위치. 0 <= off <= len <= cap */
} buffer;

#define BUFFER_INIT { NULL, 0, 0, 0 }

void buffer_init(buffer *b);

/* 메모리를 해제하고 빈 상태로 되돌린다. 두 번 불러도 안전 */
void buffer_free(buffer *b);

/* 내용만 비운다 (메모리는 유지, keep-alive 다음 응답에 재사용) */
void buffer_clear(buffer *b);

/* 뒤에 extra 바이트를 더 쓸 공간을 확보한다. 성공 0, BUFFER_MAX 초과·메모리 부족 -1 */
int buffer_reserve(buffer *b, size_t extra);

/* 뒤에 덧붙인다. 성공 0, 실패 -1 (실패하면 버퍼는 그대로) */
int buffer_append(buffer *b, const void *p, size_t n);
int buffer_append_str(buffer *b, const char *s);

/* printf 형식으로 덧붙인다 (상태 줄·헤더용). 성공 0, 실패 -1 (실패하면 버퍼는 그대로) */
int buffer_appendf(buffer *b, const char *fmt, ...);

/* 아직 남은 바이트의 시작과 길이 */
const char *buffer_peek(const buffer *b);
size_t buffer_pending(const buffer *b);

/* n 바이트를 소비/송신했다고 표시한다 (n 은 pending 이하). 다 소비하면 off = len = 0 으로 되돌린다 */
void buffer_consume(buffer *b, size_t n);

/* 남은 바이트를 맨 앞으로 옮긴다 (입력 버퍼에 read 하기 전, 앞쪽 공간 회수) */
void buffer_compact(buffer *b);

/*
 * 여러 번 나눠 덧붙이다 중간에 실패하면 시작 전 상태로 되돌린다:
 *
 *   size_t m = buffer_mark(out);
 *   if (buffer_appendf(out, ...) < 0 || buffer_append(out, ...) < 0) {
 *       buffer_rollback(out, m);
 *       return -1;
 *   }
 *
 * len 을 직접 저장했다 되돌리면 안 된다. 덧붙이는 도중 buffer_reserve 가 compact 해서
 * off·len 이 바뀔 수 있다. mark 는 pending 길이라 compact 와 상관없이 맞다 (PR #18 리뷰).
 * mark 와 rollback 사이에는 덧붙이기만 한다 (consume 하지 않는다).
 */
size_t buffer_mark(const buffer *b);
void buffer_rollback(buffer *b, size_t mark);

#endif /* BUFFER_H */
