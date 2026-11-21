/*
 * mime.h — 확장자 → Content-Type
 *
 * CS:APP 대응: tiny.c get_filetype() (html, gif, png, jpg, 그 외 text/plain)
 * 담당: 트랙 C · 이슈 #6 (단위 테스트: tests/unit/test_mime.c)
 * 현대식으로 바뀌는 점: css, js, json, svg, ico 추가, 대소문자 무시,
 *                       파일 이름의 마지막 '.' 뒤만 확장자로 본다 (Tiny 는 strstr 로 이름 어디서든 찾음),
 *                       text 계열은 "; charset=utf-8" 을 붙인다,
 *                       모르는 확장자·확장자 없음·".bashrc" 같은 숨김 파일은 application/octet-stream
 *
 * 누가 쓰나:
 *   - static_open (static_file.c) 이 실제로 연 파일 경로로 불러 static_file.content_type 에 넣는다
 *     (디렉터리 요청이면 "docs/" 가 아니라 "docs/index.html" 로 부른다)
 *   - 그 값을 http_append_header 의 http_response_head.content_type 에 그대로 넘긴다
 *
 * 사용 예:
 *
 *   const char *type = mime_type("/var/www/img/CAT.PNG");   // "image/png"
 *   http_response_head h = { 200, type, size, keep_alive, NULL };
 *   http_append_header(out, &h);
 *
 * 판정 예:
 *
 *   "a/b/index.HTML"   → "text/html; charset=utf-8"   (대소문자 무시)
 *   "report.html.txt"  → "text/plain; charset=utf-8"  (마지막 '.' 뒤만 봄)
 *   "a.b/README"       → "application/octet-stream"   (폴더 이름의 '.' 은 무시)
 *   ".bashrc", "file." → "application/octet-stream"   (확장자 아님)
 *
 * 확장자를 추가하려면: mime.c 의 mime_table 에 한 줄 + test_mime.c 에 CHECK 한 줄
 */
#ifndef MIME_H
#define MIME_H

/*
 * path 의 확장자로 Content-Type 문자열을 돌려준다.
 *   - path: 파일 경로 (전체 경로든 이름만이든 상관없음). NULL 이어도 된다
 *   - 반환: 항상 NULL 이 아닌 문자열. 프로그램 안의 상수라서
 *           free 하지 않고, 고치지 않고, 서버가 도는 동안 계속 써도 된다
 *   - 파일을 열거나 디스크를 보지 않는다 (이름만 보는 순수 함수). 여러 연결에서 동시에 불러도 안전
 */
const char *mime_type(const char *path);

#endif /* MIME_H */
