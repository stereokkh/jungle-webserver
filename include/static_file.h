/*
 * static_file.h — docroot 아래 정적 파일 열기와 나눠 보내기
 *
 * CS:APP 대응: tiny.c serve_static(), parse_uri() 의 정적 경로 부분, doit() 의 stat/권한 검사
 * 담당: 트랙 C · 이슈 #6
 * 현대식으로 바뀌는 점: mmap 후 한 번에 Rio_writen → 고정 버퍼로 조금씩 보내고 offset 저장,
 *                       docroot 탈출(.., 심볼릭 링크) 차단, 500 처리
 */
#ifndef STATIC_FILE_H
#define STATIC_FILE_H

#include <sys/types.h>

typedef struct {
    int fd;
    off_t size;
    off_t offset;      /* 지금까지 보낸 위치. EAGAIN 이면 여기서 이어서 보낸다 */
} static_file;

/* docroot + path(디코딩된 경로)를 연다. 디렉터리면 index.html.
 * 성공 0, 실패 -1 + *status (403, 404, 500) */
int static_open(const char *docroot, const char *path, static_file *out, int *status);

/* 소켓으로 다음 조각을 보낸다. 다 보냈으면 1, 아직 남았으면 0 (EAGAIN 포함), 오류 -1 */
int static_send_some(static_file *f, int sockfd);

void static_close(static_file *f);

#endif /* STATIC_FILE_H */
