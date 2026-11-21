/*
 * static_file.h — docroot 아래 정적 파일 열기와 나눠 보내기
 *
 * CS:APP 대응: tiny.c serve_static(), parse_uri() 의 정적 경로 부분, doit() 의 stat/권한 검사
 * 담당: 트랙 C · 이슈 #6
 * 현대식으로 바뀌는 점: mmap 후 한 번에 Rio_writen → 고정 버퍼로 조금씩 보내고 offset 저장,
 *                       docroot 탈출(.., 심볼릭 링크) 차단, 500 처리
 *
 * 서버(server.c)에서 쓰는 순서:
 *   1. static_open → 실패면 http_append_error(out, *status, keep_alive, head_only, NULL)
 *   2. http_append_header(out, { 200, f.content_type, f.size, keep_alive, NULL })
 *   3. out 버퍼를 다 보낸 뒤, HEAD 가 아니면 static_send_some 을 1 이 나올 때까지 POLLOUT 마다 부른다
 *   4. 끝나거나 실패하면 static_close (연결을 닫을 때도 반드시)
 */
#ifndef STATIC_FILE_H
#define STATIC_FILE_H

#include <sys/types.h>

typedef struct {
    int fd;                    /* 열린 파일. 없으면 -1. static_close 가 닫는다 */
    off_t size;                /* Content-Length */
    off_t offset;              /* 지금까지 보낸 위치. EAGAIN 이면 여기서 이어서 보낸다 */
    const char *content_type;  /* 실제 연 파일 기준 mime_type() 결과 (디렉터리면 index.html 기준) */
} static_file;

#define STATIC_FILE_INIT { -1, 0, 0, NULL }

/* docroot + path(디코딩된 경로)를 연다. 디렉터리면 index.html.
 * 성공 0, 실패 -1 + *status (403, 404, 500). 실패해도 out->fd 는 -1
 *
 * 판정 예 (docroot = www):
 *   "/", "/docs", "/docs/"        → 0, 그 디렉터리의 index.html
 *   "//a.html", "/img/../a.html"  → 0, 안에서 움직이는 . .. // 는 정리
 *   "/../secret", "/a/../../x"    → 403, 루트 위로 가면 파일이 있든 없든 403
 *   docroot 밖을 가리키는 링크     → 403 (realpath 로 실제 경로 확인)
 *   "/nope", "/a.html/", 빈 디렉터리 → 404
 *   FIFO·장치 등 일반 파일 아님    → 403 (open 전에 검사, 멈추지 않음)
 *   읽기 권한 없음 → 403, docroot 자체가 없음 → 500
 * path 는 '/' 로 시작해야 한다 (아니면 403). % 디코딩은 파서 몫 */
int static_open(const char *docroot, const char *path, static_file *out, int *status);

/* 다음 조각을 fd(클라이언트 소켓, 테스트에서는 pipe)로 보낸다.
 * pread(f->fd, 지역 버퍼(16KB 이하), f->offset) → write → 실제로 쓴 만큼만 f->offset 증가.
 * 다 보냈으면 1, 아직 남았으면 0 (EAGAIN 포함), 오류 -1 (errno 유지) */
int static_send_some(static_file *f, int fd);

/* 파일을 닫고 fd = -1. 두 번 불러도 안전 */
void static_close(static_file *f);

#endif /* STATIC_FILE_H */
