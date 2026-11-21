/*
 * static_file.c — docroot 아래 정적 파일 열기와 나눠 보내기
 *
 * CS:APP 대응:
 *   parse_uri()    : strcpy(filename, "."); strcat(filename, uri); → 경계 검사 없음
 *                    (평문 ".." 로 탈출 가능 — 이슈 #3 재현 ②)
 *   doit()         : stat → S_ISREG/S_IRUSR 검사 → 404/403
 *   serve_static() : Open → Mmap → Rio_writen 한 번에 전송
 * 담당: 트랙 C · 이슈 #6
 *
 * static_open 이 하는 일 (순서대로):
 *   1. path 를 '/' 로 쪼개 ".", "" 는 버리고 ".." 는 앞 조각을 지운다.
 *      루트 위로 올라가면 파일이 있든 없든 403 (존재 여부를 알려 주지 않는다)
 *   2. realpath 로 docroot 와 docroot/정리된경로 의 실제 경로를 구한다 (심볼릭 링크 풀림)
 *   3. 실제 경로가 docroot 밖이면 403 (docroot 밖을 가리키는 심볼릭 링크)
 *   4. 디렉터리면 그 안의 index.html 로 2~3 을 한 번 더
 *   5. 일반 파일이 아니면 403 (FIFO 를 open 하면 멈추므로 open 전에 검사)
 *   6. open → fstat 으로 크기, mime_type 으로 Content-Type
 */
#include "static_file.h"
#include "mime.h"

#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* realpath 로 이미 링크를 풀었으므로 없는 OS 에서는 빼도 동작은 같다 */
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define CHUNK_SIZE 16384   /* static_send_some 지역 버퍼 (스택 16KB 이하 규칙) */

/* realpath/open 실패 errno → 응답 상태 */
static int errno_status(int e)
{
    if (e == ENOENT || e == ENOTDIR)
        return 404;
    if (e == EACCES || e == ELOOP)   /* ELOOP: O_NOFOLLOW 가 링크를 만남 */
        return 403;
    return 500;
}

/* 1단계. "/a/./b//../c" → "a/c", "/" → "". 루트 위로 가면 NULL + *status = 403 */
static char *normalize(const char *path, int *status)
{
    char *out = malloc(strlen(path) + 1);
    size_t len = 0;
    const char *p = path;

    if (out == NULL) {
        *status = 500;
        return NULL;
    }
    while (*p != '\0') {
        const char *seg;
        size_t n;

        while (*p == '/')
            p++;
        seg = p;
        while (*p != '\0' && *p != '/')
            p++;
        n = (size_t)(p - seg);

        if (n == 0 || (n == 1 && seg[0] == '.'))
            continue;
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            if (len == 0) {             /* 루트 위로 올라가려 함 */
                free(out);
                *status = 403;
                return NULL;
            }
            while (len > 0 && out[len - 1] != '/')
                len--;
            if (len > 0)
                len--;                  /* 남은 '/' 도 지운다 */
            continue;
        }
        if (len > 0)
            out[len++] = '/';
        memcpy(out + len, seg, n);
        len += n;
    }
    out[len] = '\0';
    return out;
}

/* a + "/" + b 를 새로 할당. b 가 "" 이면 a 그대로 복사 */
static char *join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    char *s = malloc(la + 1 + lb + 1);

    if (s == NULL)
        return NULL;
    memcpy(s, a, la);
    if (lb > 0) {
        s[la] = '/';
        memcpy(s + la + 1, b, lb + 1);
    } else {
        s[la] = '\0';
    }
    return s;
}

/* real 이 root 자신이거나 root/ 아래인가 ("/www" 와 "/www2" 를 구별) */
static int inside(const char *root, const char *real)
{
    size_t n = strlen(root);

    if (n == 1 && root[0] == '/')
        return real[0] == '/';
    return strncmp(real, root, n) == 0 && (real[n] == '\0' || real[n] == '/');
}

/* 2~3단계. full 의 실제 경로를 돌려준다. 실패나 docroot 밖이면 NULL + *status */
static char *resolve(const char *root, const char *full, int *status)
{
    char *real = realpath(full, NULL);

    if (real == NULL) {
        *status = errno_status(errno);
        return NULL;
    }
    if (!inside(root, real)) {
        free(real);
        *status = 403;
        return NULL;
    }
    return real;
}

int static_open(const char *docroot, const char *path, static_file *out, int *status)
{
    char *root = NULL, *rel = NULL, *full = NULL, *real = NULL;
    struct stat st;
    int fd, want_dir;

    out->fd = -1;
    out->size = 0;
    out->offset = 0;
    out->content_type = NULL;
    *status = 500;

    if (docroot == NULL || path == NULL)
        return -1;
    if (path[0] != '/') {               /* 파서가 이미 막지만 직접 부를 때 대비 */
        *status = 403;
        return -1;
    }
    want_dir = path[strlen(path) - 1] == '/';

    if ((rel = normalize(path, status)) == NULL)
        goto fail;
    if ((root = realpath(docroot, NULL)) == NULL)
        goto fail;                      /* 서버 설정 문제 → 500 */
    if ((full = join(root, rel)) == NULL)
        goto fail;
    if ((real = resolve(root, full, status)) == NULL)
        goto fail;
    if (stat(real, &st) < 0) {
        *status = errno_status(errno);
        goto fail;
    }

    /* 4단계 */
    if (S_ISDIR(st.st_mode)) {
        free(full);
        if ((full = join(real, "index.html")) == NULL)
            goto fail;
        free(real);
        if ((real = resolve(root, full, status)) == NULL)
            goto fail;                  /* index.html 없는 디렉터리 → 404 */
        if (stat(real, &st) < 0) {
            *status = errno_status(errno);
            goto fail;
        }
    } else if (want_dir) {              /* "/index.html/" 처럼 파일 뒤에 '/' */
        *status = 404;
        goto fail;
    }

    /* 5단계 */
    if (!S_ISREG(st.st_mode)) {
        *status = 403;
        goto fail;
    }

    /* 6단계 */
    fd = open(real, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        *status = errno_status(errno);
        goto fail;
    }
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {   /* stat 과 open 사이에 바뀐 경우 */
        *status = S_ISREG(st.st_mode) ? 500 : 403;
        close(fd);
        goto fail;
    }

    out->fd = fd;
    out->size = st.st_size;
    out->content_type = mime_type(real);   /* 상수 문자열이라 real 을 free 해도 된다 */
    free(root);
    free(rel);
    free(full);
    free(real);
    return 0;

fail:
    free(root);
    free(rel);
    free(full);
    free(real);
    return -1;
}

int static_send_some(static_file *f, int fd)
{
    char chunk[CHUNK_SIZE];
    off_t left;
    size_t want;
    ssize_t got, wrote;

    if (f->offset >= f->size)
        return 1;

    left = f->size - f->offset;
    want = left < CHUNK_SIZE ? (size_t)left : CHUNK_SIZE;

    got = pread(f->fd, chunk, want, f->offset);
    if (got < 0)
        return errno == EINTR ? 0 : -1;
    if (got == 0) {                     /* 연 뒤에 파일이 줄었다: Content-Length 를 못 채운다 */
        errno = EIO;
        return -1;
    }

    /* 한 번만 쓴다. 일부만 써져도 다음 호출이 offset 부터 다시 pread 하므로 바이트를 잃지 않는다 */
    wrote = write(fd, chunk, (size_t)got);
    if (wrote < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0;                   /* 소켓이 가득 참 → 다음 POLLOUT 에 이어서 */
        return -1;
    }
    f->offset += wrote;
    return f->offset >= f->size ? 1 : 0;
}

void static_close(static_file *f)
{
    if (f->fd >= 0) {
        close(f->fd);
        f->fd = -1;
    }
}
