/*
 * static_file.c — docroot 아래 정적 파일 열기와 나눠 보내기
 *
 * CS:APP 대응:
 *   parse_uri()    : strcpy(filename, "."); strcat(filename, uri); → 경계 검사 없음
 *                    (평문 ".." 로 탈출 가능 — 이슈 #3 재현 ②)
 *   doit()         : stat → S_ISREG/S_IRUSR 검사 → 404/403
 *   serve_static() : Open → Mmap → Rio_writen 한 번에 전송
 * 담당: 트랙 C · 이슈 #6
 */
#include "static_file.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

int static_open(const char *docroot, const char *path, static_file *out, int *status)
{
    /* TODO(#6):
     *  - docroot + path 결합 (길이 검사), 디렉터리면 /index.html
     *  - realpath 로 실제 경로가 docroot 안인지 확인 → 아니면 403
     *  - open/fstat: 없으면 404, 권한 없으면 403, 그 외 500 */
    (void)docroot; (void)path;
    out->fd = -1;
    out->size = 0;
    out->offset = 0;
    out->content_type = NULL;   /* 성공 시 mime_type(실제 연 경로) */
    *status = 500;
    return -1;
}

int static_send_some(static_file *f, int fd)
{
    /* TODO(#6): char chunk[16384] (지역 버퍼) 에 pread(f->fd, chunk, n, f->offset)
     *  → write(fd, chunk, got) → 실제로 쓴 만큼만 f->offset += written.
     *  (일부만 써져도 다음 호출이 offset 부터 다시 pread 하므로 바이트를 잃지 않는다)
     *  EAGAIN 이면 0 반환해서 이벤트 루프에 제어권을 돌려준다.
     *  (sendfile 은 OS마다 시그니처가 달라 쓰지 않는다) */
    (void)f; (void)fd;
    return -1;
}

void static_close(static_file *f)
{
    if (f->fd >= 0) {
        close(f->fd);
        f->fd = -1;
    }
}
