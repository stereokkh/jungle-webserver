# CS:APP Tiny 관찰 → 개선 (#3)

CS:APP 3판 11장 Tiny 원본을 재현한 결과를 우리 서버의 어느 이슈·PR 에서 고쳤는지로 묶은 요약 표다. 재현 명령·실제 출력·코드 줄 번호는 프로젝트 노션 "3. 우리가 직접 찾은 것" 에 있다.

- 원본: `make classic` 으로 받은 `tiny.c`·`csapp.c` (URL·sha256 은 `classic/SOURCES.txt`, 커밋하지 않음)
- 재현: WSL Ubuntu, 격리된 폴더에 `tiny` 와 시험 파일만 두고 실행 (11-21 각자, 11-23 혜성 추가분)
- **관찰** 은 실제로 본 출력, **해석** 은 팀이 붙인 이유다. 성능 비교는 하지 않았다 (CLAUDE.md: 인과 주장 금지)

## 재현한 것

| # | 관찰 (Tiny) | 해석 | 개선 (우리 서버) | 재현 |
|---|---|---|---|---|
| 1 | 아무것도 안 보내는 `nc` 연결 하나를 열어 두면 다른 `curl` 이 응답을 못 받는다 (`--max-time 5` 면 타임아웃). `nc` 를 끊은 뒤에야 200 | `main` 이 `accept → doit → close` 를 하나씩만 하고, `doit` 의 `Rio_readlineb` 가 블로킹 `read` 에서 줄바꿈을 기다림 | #7 `poll` 루프 (느린 연결 격리), 파서는 `NEED_MORE` 로 바로 돌아옴, 요청 타임아웃 (PR #25, #29). `run.sh` 멈춘 연결 검사 | 상철, 건호 |
| 2 | 1번에서 기다리다 포기한 `curl` 이 있으면, `nc` 가 끊긴 뒤 Tiny 가 그 닫힌 연결에 응답을 쓰다가 종료 (상태 141 = SIGPIPE). 이후 연결 거부. 20MB 파일을 받는 도중 `curl` 을 끊으면 `Rio_writen error: Connection reset by peer` 후 종료 (상태 0, 4번 반복 모두 같음) | SIGPIPE 를 무시하지 않아 끊긴 소켓에 `write` 하는 순간 프로세스가 죽음. 큰 파일은 상대가 RST 를 먼저 보내 `write` 가 `ECONNRESET` 으로 실패하고 3번과 같은 래퍼 경로로 끝남. 어느 쪽이든 클라이언트 하나가 서버 전체를 끝냄 | #7 `signal(SIGPIPE, SIG_IGN)`, `EPIPE`·`ECONNRESET` 이면 그 연결만 닫음 (`server.c`). `run.sh` "server survives client that hangs up" | 건호, 혜성 |
| 3 | SIGPIPE 를 무시하고 띄워도 같은 절차에서 `Rio_writen error: Broken pipe` 후 종료 (상태 0) | 대문자 래퍼가 오류면 `unix_error` → `exit(0)`. 연결 하나의 오류가 서버 종료가 되고, 종료 코드가 0 이라 감시 도구도 못 알아챔 | 함수는 -1 을 돌려주고 그 연결만 닫음. 프로세스 종료는 시작 단계 실패뿐 (`docs/interfaces.md` 반환값 규칙) | 건호 |
| 4 | `http://[::1]:포트/` → 연결 거절, `127.0.0.1` 은 200. `ss` 로 보면 `0.0.0.0` 하나만 LISTEN | `open_listenfd` 가 bind 에 처음 성공한 주소 하나만 엶. `localhost` 를 `::1` 로 먼저 시도하는 클라이언트는 거절당함 | #7 IPv6 소켓 하나로 IPv4 까지 받음 (`IPV6_V6ONLY=0` 명시, PR #25) | 건호 |
| 5 | `GET /../../파일` (`--path-as-is`) → 200, docroot 밖 파일 내용. 옵션 없이 보내면 curl 이 `../` 를 지워 `GET /파일` 을 보내고 404 | `parse_uri` 가 `"."` 에 URI 를 붙이기만 하고 `..` 를 검사하지 않음. 옵션 없는 404 는 Tiny 가 아니라 curl 이 막은 것 (6번과 같은, 막힌 것처럼 보이는 404) | #5 파서가 `path` 를 디코딩하고 `.`·`..` 조각을 400 (PR #27). 그 뒤 안전장치로 #6 `static_open` 이 경로를 정리해 루트 위로 가면 파일 유무와 상관없이 403, `realpath` 로 심볼릭 링크 탈출 403 (PR #22). `run.sh` `..`·링크 검사. 빈 조각·퍼센트 인코딩 때문에 라우팅과 정적 파일이 서로 다른 경로를 보는 문제는 #31 진행 중 | 상철 |
| 6 | `/%2e%2e/secret.txt` → 404 | 디코딩을 안 해서 `%2e%2e` 라는 폴더를 찾다 실패한 것. 막은 게 아니라 우연히 실패, 경계 검사는 여전히 없음 | 5번과 같음 (디코딩 뒤 `..` 이면 막음) | 혜성 |
| 7 | `HOME.HTML` → `text/plain`, `report.html.txt` → `text/html`, `style.css` → `text/plain` | `get_filetype` 이 `strstr` 로 이름 어디든 찾고, 대소문자 구분, css 없음. `.txt` 가 HTML 로 렌더링될 수 있음 | #6 `mime_type`: 마지막 `.` 뒤만, 대소문자 무시, 모르면 `application/octet-stream`, text 계열은 `charset=utf-8` (PR #20). `run.sh` www/ html·css·png 검사 (PR #32) | 혜성 |
| 8 | `GET /<script>alert(1)</script>` 의 404 본문에 요청 경로가 그대로 들어감. `Content-length` 없음 | `clienterror` 가 요청 값을 이스케이프 없이 HTML 에 넣어 반사형 XSS. 길이가 없어 keep-alive 불가 | #6 `http_append_error` 본문은 `<h1>상태 사유</h1>` 고정, `Content-Length` 항상 (PR #21) | 혜성 |
| 9 | HTTP/1.1 요청에도 응답 첫 줄이 `HTTP/1.0 200 OK` | `doit` 이 version 을 읽기만 하고 안 씀 (응답은 1.0 고정). 검사가 없어 `HTTP/9.9` 도 안 걸러짐 (코드로만 확인) | #5 `version_minor` 저장, 1.0·1.1 외 505 (PR #24) | 상철 |
| 10 | 대상 9000바이트 GET → 응답 일부만 오고 Tiny 가 종료 (상태 134 = SIGABRT) | 고정 `MAXLINE`(8192) 버퍼에 길이 검사가 없음 | #5 대상 2048 초과 414, 머리 8192 초과 431 (PR #24). 파서가 머리 8KB 초과를 먼저 판정해서 9000바이트 대상은 431 (3000바이트면 414). `run.sh` 는 9KB 헤더로 431 검사 | 혜성 |
| 11 | 일반 파일 `notes-cgi-bin.html` → 403 | `parse_uri` 가 URI 어디든 `cgi-bin` 이 있으면 CGI 로 봄 | #8 라우팅은 접두사 비교 (PR #26), #10 앱은 `/api/add`·`/api/echo` 경로 전체 비교 (PR #30) | 혜성 |

## 코드에서만 본 것 (재현 안 함)

| # | 관찰 (Tiny) | 해석 | 개선 (우리 서버) |
|---|---|---|---|
| 12 | `read_requesthdrs` 가 헤더를 읽고 버림. `Content-Length` 본문을 읽지 않고 GET 만 받음 (그 외 501) | 요청 경계를 몰라 keep-alive·파이프라이닝·POST 가 불가능 | #5 증분 파서 (`Content-Length` 본문, 413/431/501, PR #24), #7 keep-alive·파이프라이닝 (PR #29) |
| 13 | `serve_static` 이 파일 전체를 `mmap` 해서 `Rio_writen` 으로 다 쓸 때까지 막힘 | 큰 파일·느린 클라이언트를 보내는 동안 다른 연결을 못 받음 | #6 16KB 씩 나눠 보내고 막히면 `poll` 로 돌아감 (`static_send_some`, PR #22). `run.sh` 1MB 일치 검사 |
| 14 | `serve_dynamic` 이 요청마다 `fork`·`execve`, `adder.c` 는 `atoi`·범위 검사 없음 | `poll` 루프 안에서 자식을 기다리면 서버 전체가 멈춤. `a=abc` 가 0 으로 계산됨 | #10 서버 안 함수 `/api/add`·`/api/echo`, 숫자·범위·오버플로 400 (PR #30) |
| 15 | Reverse Proxy 없음 | Tiny 에 없는 기능 | #8 설정·라우팅 (PR #26), #9 중계·502/504·Round-Robin (진행 중) |

## 미확인
- 2번의 종료 원인이 SIGPIPE 라는 것은 종료 상태(141 = 128 + 13)로만 판단했다
- 4번에서 IPv4 가 먼저 잡힌 이유가 `getaddrinfo` 정렬 순서인지 `AI_ADDRCONFIG` 인지
- 10번의 SIGABRT 가 어느 버퍼에서 났는지
