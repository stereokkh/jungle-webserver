# 결정 기록

계획·범위·규칙이 바뀔 때마다 **맨 위에** 항목을 추가한다. CLAUDE.md 는 "지금 상태"만, 이 파일은 "언제, 왜 바뀌었나"를 남긴다.

- 결정을 바꾸는 PR은 이 파일 항목 + CLAUDE.md 수정을 **같은 PR**에 넣는다
- 형식: 날짜 · 제목 / 결정 / 이유 / 영향 (파일·이슈)
- 각 세션의 Claude는 동기화 훅(`scripts/claude-sync.sh`)이 이 파일 변경을 알려 주면 새 항목을 읽고 따른다

---

## 2026-11-21 · 프록시 설정 형식과 요청 재작성 규칙 (이슈 #8)

- **결정**:
  1. 설정 파일: 한 줄에 `접두사 host:port...`, `#` 주석, 줄 1024바이트·접두사 127·이름 63·라우트 16·라우트당 백엔드 8, 포트 1-65535, 같은 접두사 중복과 빈 파일은 오류. 오류는 -1 + stderr `파일:줄: 이유`, cfg 는 전부 0
  2. 이름은 로드 시 한 번 해석하고 IPv4 를 먼저 고른다. IPv6 는 `[::1]:포트` 로 명시 (`AI_ADDRCONFIG` 는 쓰지 않음)
  3. 접두사를 떼지 않고 원본 `target` 을 그대로 전달한다
  4. `Content-Length` 는 클라이언트 줄을 버리고 본문 길이로 다시 쓴다 (원래 있었을 때만). `Connection` 에 적힌 이름은 클라이언트 줄에만 작용하고 프록시가 쓰는 줄(Host, X-Forwarded-For, Via, Content-Length, Connection)은 지우지 못한다
  5. `Expect` 는 항상 뺀다
  6. `proxy_build_request` 는 #9 가 아니라 #8 에서 구현한다
  7. (리뷰 반영) 포트와 숫자 IPv4 는 앞자리 0 을 금지한다 (`08001`, `127.0.0.010` 은 오류). 줄 없는 파일 전체 오류는 `파일: 이유` 로 예외. 줄 끝 CR 은 LF 앞에서만 인정. 다시 쓴 머리가 `HTTP_MAX_HEADER` 를 넘으면 `proxy_build_request` 가 -1, 깨진 헤더 줄도 파서와 같은 기준으로 -1
- **이유**: 2 주소를 하나만 저장하고 재시도가 없어서 family 가 틀리면 복구 없이 502 가 된다. #7 `net_listen`(PR #25)은 dual-stack(`IPV6_V6ONLY=0`)이고 IPv6 를 못 쓰면 IPv4 만 연다. 어느 쪽이든 IPv4 loopback 은 받으므로 IPv4 가 안전하고, ::1 을 고르면 IPv4 만 연 백엔드에서 거절된다 (노션 #3 에서 Tiny 의 `open_listenfd` 가 0.0.0.0 만 묶은 것도 같은 경우). 4 RFC 9110 7.6.1 은 중개자가 Connection 에 적힌 필드를 지우라고 하고, 메시지 길이는 홉마다 보내는 쪽이 정한다 (RFC 9112 6.3). 본문이 있는데 길이가 없는 메시지는 만들지 않는다. 5 본문이 이미 전부 버퍼에 있어 기다릴 것이 없고, 전달하면 백엔드의 `100 Continue` 를 #9 가 건너뛰어야 한다. 4·5 는 PR #24 에서 #9 로 미룬 두 항목이다. 7 glibc `getaddrinfo` 는 앞자리 0 을 8진수로 읽어 `127.0.0.010` 을 127.0.0.8 로 풀기 때문에 검사한 값과 저장한 주소가 달라지고, 포트 토큰은 그대로 Host 헤더 값이 되므로 표기를 하나로 둔다. 프런트 한도에 꽉 찬 요청은 Host·XFF·Via·Connection 줄이 붙으면 백엔드의 8192바이트 한도를 넘는다
- **영향**: `proxy_config.h/.c`, `proxy.h/.c`, `docs/interfaces.md`, `CLAUDE.md`, `tests/unit/test_proxy_config.c`, `tests/unit/test_proxy.c`. #7 (백엔드 인스턴스는 IPv4 loopback 을 받아야 함), #9 (요청 생성은 끝났고 연결·중계가 남음)

## 2026-11-21 · 서버 모듈 연결·keep-alive·요청 타임아웃·lingering close (이슈 #7 두 번째 PR)

결정
1. `handle_request` 는 `http_parse_request` → `handler_route` → (정적) `static_open` + `http_append_header` 순서로 응답을 만들고, 응답을 다 만든 뒤에 `buffer_consume` 한다. 파일 본문은 `CONN_SENDING_FILE` 에서 `static_send_some` 으로 나눠 보내고, 다 보내면 바로 `static_close` 한다 (keep-alive 에서 다음 `static_open` 이 fd 를 덮어쓰지 않게, PR #25 혜성 리뷰)
2. 파서 `NEED_MORE` 의 `consumed` 가 0 이 아니면 `need_len` 에 기억하고 `buffer_pending(&in)` 이 그만큼 찰 때까지 파서를 부르지 않는다. DONE·ERROR 에서 지운다 (PR #24 계약)
3. keep-alive·파이프라이닝: 응답을 끝까지 보낸 뒤 `in` 에 남은 바이트가 있으면 새 바이트를 기다리지 않고 바로 다음 요청을 처리한다. 응답을 보내는 동안은 읽지 않는다
4. 요청 타임아웃 `CLIENT_REQUEST_TIMEOUT_MS` = 30초 (요청의 첫 바이트부터 완성까지). 넘으면 응답 없이 닫는다
5. 연결을 닫는 응답은 `shutdown(SHUT_WR)` 후 `CONN_LINGERING` 에서 남은 입력을 읽어 버리다가 EOF 나 2초(`CLIENT_LINGER_TIMEOUT_MS`)에 닫는다
6. 파서 오류(400, 413, 414, 431, 501, 505)는 `keep_alive = 0` 으로 오류 응답 후 닫는다. HEAD 면 본문을 쓰지 않는다 (`req.method == HTTP_HEAD`. 요청 라인 전에 난 431 등은 method 가 기본값 GET 이라 본문까지 씀). 앱 스텁(-1)은 500, 프록시 경로는 502 로 임시 처리
7. 파일 크기가 `LONG_MAX` 를 넘으면 500 (`http_response_head.content_length` 가 `long`)

이유
- 유휴 타이머는 바이트가 올 때마다 리셋돼서, 헤더나 본문을 1바이트씩 보내는 클라이언트는 연결 하나를 무기한 붙잡을 수 있다. 30초는 1MB 본문 상한을 약 35KB/s 로도 받을 수 있는 값
- 읽지 않은 입력을 남긴 채 `close` 하면 커널이 RST 를 보내고, 클라이언트는 아직 읽지 않은 응답(413, 431 등)을 버릴 수 있다 (RFC 9112 9.6). Linux 루프백에서는 lingering 없이도 재현되지 않았지만(20/20) 네트워크·OS 에 따라 달라서 표준 방식을 따른다
- 파서 오류는 요청 경계를 믿을 수 없어 다음 요청을 이어 처리할 수 없다. HEAD 를 파서가 읽기 전에 난 오류는 본문이 붙지만, 응답 뒤에 닫으므로 다음 응답과 섞이지 않는다. ERROR 때 `method` 동작은 `http_request.h` 계약에 아직 없어 PR #29 에서 상철님께 확인 중
- `long` 이 32비트인 환경에서 2GB 넘는 파일은 Content-Length 가 잘려 응답 경계가 어긋난다

영향
- `src/server.c`, `include/server.h` (`CLIENT_REQUEST_TIMEOUT_MS`, `CLIENT_LINGER_TIMEOUT_MS`), `docs/interfaces.md` 상태·타임아웃 표, `tests/run.sh` (정적 200·1MB·404·403(링크)·`..` 차단(403 또는 #27 뒤 400)·405·431·413·HEAD·keep-alive·1.0/1.1 기본 Connection·파이프라이닝(한꺼번에, 반쪽)·400·파서 오류 HEAD)
- #9 는 `respond()` 의 `ROUTE_PROXY` 분기, #10 은 `ROUTE_APP` 의 500 대체 처리를 걷어 낸다

## 2026-11-21 · 서버 CLI·listen 소켓·종료 방식 (이슈 #7 첫 PR)

- **결정**:
  1. CLI 는 `./webserver -p <port> -r <docroot> [-c <proxy.conf>]`. 포트는 1–65535 10진수, docroot 는 존재하는 디렉터리여야 한다. 아니면 서버를 띄우지 않고 exit 1. `-c` 가 있으면 시작할 때 `proxy_config_load` 하고 실패하면 exit 1 (#8 구현 전에는 항상 실패)
  2. listen 소켓은 **IPv6 소켓 하나에 `IPV6_V6ONLY=0` 을 명시**해서 IPv4 도 받는다. IPv6 를 쓸 수 없으면(커널·컨테이너에서 꺼짐) IPv4 로 물러난다. 단 `EADDRINUSE`·`EACCES` 는 물러나지 않고 실패한다
  3. `SIGINT`/`SIGTERM` 은 self-pipe 로 poll 을 깨운다. 받으면 listen 소켓 닫기 → 모든 연결 정리 → `server_run` 0 반환 → exit 0. 보내던 응답을 마저 보내지는 않는다
  4. `accept` 가 fd 고갈(`EMFILE` 등)로 실패하면 종료하지 않고 100ms 동안 listen 소켓 감시를 멈춘다. 연결이 하나 닫히면 바로 재개
  5. `tests/run.sh` 는 서버를 백그라운드로 띄우고 curl 이 성공할 때까지 최대 5초 기다린다. 종료는 감시 프로세스로 제한 시간을 잰다 (macOS 에 `timeout` 없음)
- **이유**: 2 `net_listen` 이 fd 하나를 돌려주는 계약을 유지하면서 `localhost` 가 `::1` 로 풀리는 환경도 받기 위해 (Tiny 는 0.0.0.0 하나만 열어 `::1` 접속이 거부됨, #3). `IPV6_V6ONLY` 기본값은 OS 마다 다르다. 포트 충돌일 때 IPv4 로 물러나면 "IPv6 는 다른 프로그램이 쓰는데 IPv4 로 떠 버리는" 상황이 가려진다. 3 플래그만 쓰면 "플래그 검사 → poll 진입" 사이에 온 시그널을 놓친다. 4 그대로 두면 대기열의 연결 때문에 poll 이 계속 깨어나 CPU 를 다 쓴다
- **영향**: `src/main.c`, `src/net.c`, `src/server.c`, `tests/run.sh`, Makefile `run`, README, PR 템플릿, CLAUDE.md. 이슈 #7 (keep-alive·모듈 연결은 다음 PR), #8 (`-c`)

## 2026-11-21 · 본문 대기 중인 NEED_MORE 는 필요한 총 길이를 알려 준다 (PR #24)

- **결정**: `http_parse_request` 가 헤더는 다 받고 본문만 덜 받아 `NEED_MORE` 를 돌려줄 때 `*consumed` 에 요청 전체 길이(`head_len + body_len`)를 넣는다. 헤더가 아직이면 지금처럼 0. 서버(#7)는 이 값을 기억해 `in` 이 그만큼 찰 때까지 파서를 다시 부르지 않는다. 시그니처는 그대로
- **이유**: 파서가 상태를 저장하지 않아서, 본문이 1 바이트씩 오면 바이트마다 헤더 전체를 다시 파싱한다(합계 ≈ 헤더 길이 × 호출 횟수). 헤더 쪽은 8KB 상한으로 묶이지만 본문은 최대 1MB 다. 바이트마다 유휴 타이머도 리셋돼 타임아웃으로도 끊기지 않는다 (상철 계산 + 건호 리뷰)
- **영향**: `include/http_request.h`, `src/http_request.c`, `tests/unit/test_http_request.c`, `docs/interfaces.md`, `CLAUDE.md` 계약, #7 `server.c` 의 파서 호출부 (비교 기준은 `buffer_pending(&in)`, DONE·ERROR 에서 기억한 값을 지움)
- **메모**: `Expect: 100-continue` 는 #9 뿐 아니라 프록시를 안 거치는 `POST /api/echo`(#10) 에도 해당한다. curl 은 1초 기다린 뒤 본문을 보내서 멈추지는 않고 1초 지연이 생긴다 (건호 확인). 지금은 처리하지 않는다

## 2026-11-21 · 정적 파일 경로 판정 (PR #22, 이슈 #6)

- **결정**:
  1. `static_open` 은 `.`·`..`·`//` 를 글자 단위로 먼저 정리하고, 루트 위로 가면 파일 존재와 상관없이 403
  2. 그다음 `realpath` 결과가 docroot 밖이면(심볼릭 링크) 403. 디렉터리는 끝 `/` 가 없어도(`/docs`) 리다이렉트 없이 `index.html`(없으면 404), `/파일/` 은 404, 일반 파일이 아니면 403, docroot 를 못 찾으면 500
  3. `realpath`(XSI 확장)를 쓰는 파일은 맨 위에 `#define _XOPEN_SOURCE 700` 을 둔다. 공통 플래그 `-D_POSIX_C_SOURCE=200809L` 은 그대로
- **이유**: 1 Tiny `parse_uri` 가 `/../secret.txt` 를 200 으로 열었음 (#3). 존재 여부로 403/404 가 갈리면 docroot 밖 파일이 있는지 알려 주게 됨. 2 글자 정리만으로는 docroot 안의 링크가 밖을 가리키는 경우를 못 막음. `/docs` 에 301 → `/docs/` 를 하지 않아 그 페이지의 상대 링크는 `/` 기준으로 풀린다. 지금 `www/` 는 절대 경로만 써서 괜찮고, 필요해지면 후속 (건호 리뷰). 3 musl·clang 은 `_POSIX_C_SOURCE` 만으로 `realpath` 선언을 보여 주지 않아 CI 가 실패함 (glibc gcc 는 통과해서 로컬에서 못 잡음)
- **영향**: `src/static_file.c`, `include/static_file.h`, `docs/interfaces.md` "정한 세부 규칙", `tests/unit/test_static_file.c`, `www/`. 시그니처는 그대로. 파서(#5)는 디코딩한 path 를 넘기고 NUL·제어 문자는 400 으로 막는다 (상철 확인)

## 2026-11-21 · 응답 헤더 규칙 4가지 (PR #21, 이슈 #6)

- **결정**:
  1. `http_append_header` 의 줄 순서를 상태 줄, `Date`, `Server`, `Content-Type`, `Content-Length`, `Connection`, `extra_headers`, 빈 줄로 고정. 상태 코드가 100~599 밖이면 -1
  2. `content_length < 0` (길이 모름) 이면 `Content-Length` 를 생략하고 `keep_alive` 와 상관없이 `Connection: close`
  3. `extra_headers` 는 `NULL`, `""`, 또는 CRLF 로 끝나는 줄들만 (빈 줄·짝 없는 CR·LF 금지). `content_type` 에 CR·LF 가 있어도 -1
  4. 오류 본문은 상태 코드와 문구만 (`<html><body><h1>404 Not Found</h1></body></html>`). 요청 내용은 넣지 않는다
- **이유**: 1 응답 모양이 정해져야 테스트가 필요한 줄을 찾을 수 있음. `Date` 때문에 전체 바이트 비교는 못 함. 2 길이가 없으면 클라이언트가 응답 끝을 연결 종료로만 알 수 있음. 3 빈 줄·짝 없는 LF 가 들어가면 헤더가 거기서 끝나 응답 분할(keep-alive 다음 응답까지 밀림). 4 Tiny `clienterror` 가 요청 경로를 본문에 넣어 반사형 XSS (#3)
- **영향**: `src/http_response.c`, `include/http_response.h`, `docs/interfaces.md` "정한 세부 규칙", `tests/unit/test_http_response.c`. 시그니처는 그대로. 서버(#7)·프록시(#9)는 고정 문자열·`mime_type()` 상수만 넘기므로 영향 없음 (건호 확인)

## 2026-11-20 · 프로젝트 노션 주소 변경

- **결정**: 프로젝트 노션 주소를 https://app.notion.com/p/jungle-webserver-41efacdfe6e480e59bc8e888ca466fb6 ("jungle-webserver" 페이지) 로 바꾼다. 아래 "프로젝트 노션 연결" 항목의 이전 주소는 더 이상 쓰지 않는다. 사용 범위(이 페이지와 하위 페이지만, 쓰기 전 확인)는 그대로
- **이유**: 노션 페이지 주소가 바뀜. 새 주소로 페이지가 열리는 것을 확인함
- **영향**: CLAUDE.md 노션 절·"아직 하지 않은 것", README

## 2026-11-20 · 인터페이스 리뷰 반영 (PR #18)

- **결정**:
  1. 431 을 가장 먼저 판정한다 (앞 8KB 안에 `\r\n\r\n` 이 없으면 431)
  2. `app_match` 는 `proxy_match` 와 같이 디코딩 전 `target` 을 본다. 정적 파일만 디코딩한 `path`
  3. 프록시는 클라이언트 `Connection` 값에 적힌 헤더 이름도 뺀다 (RFC 7230 6.1)
  4. 클라이언트가 보낸 `X-Forwarded-For` 는 버리고 새로 쓴다
  5. `http_append_error` 에 `extra_headers` 인자를 추가하고, 405 `Allow` 는 호출자가 넣는다
  6. 디코딩한 경로의 NUL·제어 문자(0x01-0x1F, 0x7F)·잘못된 `%XX` 는 400
  7. `buffer_mark`/`buffer_rollback` 추가. 실패 시 `len` 을 직접 복원하지 않는다
- **이유**: 1 TCP 분할에 따라 414/431 이 갈림 (상철). 2 `/%61pi/add` 가 프록시를 건너뛰고 앱으로 감 (상철 지적, 혜성 제안). 3·4 표준 준수, 클라이언트가 지어낸 값 차단 (상철). 5 앱 오류에 `X-Instance` 가 없고 echo 의 405 `Allow` 가 틀림 (혜성). 6 `static_open` 이 믿을 보장이 헤더에 없었음 (혜성). 7 덧붙이는 도중 compact 되면 저장한 `len` 이 어긋남 (혜성)
- **영향**: `http_request.h/.c`, `app.h/.c`, `proxy.h/.c`, `http_response.h/.c`, `static_file.h`, `buffer.h/.c`, `docs/interfaces.md`, `test_buffer.c`, `test_contract.c`. 이슈 #5 #6 #9 #10

## 2026-11-20 · 모듈 인터페이스 확정안 (이슈 #4)

- **결정**: 응답·프록시 요청은 고정 배열 대신 공통 `buffer`(연결이 소유, 늘어남, `off` 가 송신 진행 상태)에 덧붙인다. `http_request` 에 원본 헤더 블록(`headers`)과 `method_str` 추가, `headers`/`body` 는 입력 버퍼를 가리키므로 처리 후 consume. 반환값은 0/-1 로 통일(결과는 buffer 에). `http_append_header` 는 `http_response_head` 구조체 + `extra_headers`. `handler_route` 가 매칭된 라우트를 돌려주고 `app_match` 분리. 프록시 매칭은 디코딩 전 경로 + 경로 경계. 타임아웃 상수(클라이언트 10초, 업스트림 5초)는 `server.h`
- **이유**: 초안대로면 프록시가 원본 헤더를 전달할 수 없고, 1MB 본문을 담을 출력 버퍼의 크기·소유자가 없고, 입력 버퍼를 당기면 body 포인터가 무효가 되고, 모듈마다 반환값 형태가 달라 연결 코드가 복잡해짐
- **영향**: `include/*.h` 전부, `src/buffer.c`(신규), 스텁 시그니처, `docs/interfaces.md`(신규), `tests/unit/test_buffer.c`, `test_contract.c`, CODEOWNERS

## 2026-11-20 · 단위 테스트·sanitizer 기반 (이슈 #4)

- **결정**: `tests/unit/test_*.c` 파일 하나가 실행 파일 하나 (main.c 를 뺀 모든 모듈과 링크). 외부 프레임워크 없이 `tests/unit/test.h` 매크로만 쓴다. `make test` 가 단위 테스트까지 돌리므로 CI 5개·docker-test 에 자동 포함. `make asan` 은 `-fsanitize=address,undefined` 로 `build/asan/` 에 따로 빌드하고, CI 에는 필수가 아닌 `ubuntu / asan+ubsan` 잡으로 추가
- **이유**: 세 트랙이 소켓 없이 각자 모듈을 검증하기 위해. sanitizer 는 마지막 날이 아니라 PR 마다 돌린다. Alpine(musl)은 ASan 을 지원하지 않아 Ubuntu 에서만. 필수 검사로 넣으면 보호 규칙 변경이 필요해서 일단 권고로 둠
- **영향**: Makefile, `tests/unit/`, `.github/workflows/ci.yml`, CLAUDE.md 검증 흐름, README

## 2026-11-20 · 프로젝트 노션 연결 (이 프로젝트 전용)

- **결정**: 프로젝트 노션은 https://app.notion.com/p/41efacdfe6e480ee9f8ccec38a3af93c (팀원 워크스페이스). 저장소 `.mcp.json` 으로 이 프로젝트에서만 Notion MCP 를 켠다. Claude는 이 페이지와 하위 페이지만 읽고 쓰며, 쓰기 전에는 사용자에게 확인
- **이유**: 노션은 이 프로젝트에서만 쓰기로 함. 같은 계정의 개인 노션을 건드리지 않기 위해
- **영향**: `.mcp.json`, CLAUDE.md 노션 절, README

## 2026-11-20 · 교차 리뷰 + 노션 역할

- **결정**: A→B→C→A 순서로 교차 리뷰 (`.github/CODEOWNERS` 로 자동 요청), develop 머지에 승인 1개 필수. 노션은 각자 파트 학습 정리, 결정·규칙·인터페이스는 저장소가 기준. #3 상세는 노션, `docs/classic.md` 는 요약 표만
- **이유**: 트랙을 나누면 각자 자기 파트만 알게 됨. 모두가 최소 한 파트는 남의 코드를 줄 단위로 읽도록. Claude 세션은 노션을 읽지 못함
- **영향**: CLAUDE.md 작업 관리, `.github/CODEOWNERS`, develop 보호 규칙, 이슈 #3

## 2026-11-20 · 트랙 담당 확정

- **결정**: A 건호(`stereokkh`), B 상철(`Shirokuma0704`), C 혜성(`sikhesung`). 이슈 Assignee 지정 완료 (#3 전원, #11 A·C)
- **이유**: A가 모든 기능의 바닥(poll 루프)이라 소켓에 익숙한 사람, 일요일 저녁 첫 통합은 A·B가 함께
- **영향**: CLAUDE.md 트랙 표, 이슈 #3–#11 Assignee

## 2026-11-20 · 세션 간 동기화 방식

- **결정**: 계획·관리 세션(Windows)과 코딩 세션(WSL), 팀원 세션이 같은 결정을 따르도록 저장소를 단일 기준으로 쓴다. `.claude/settings.json` 의 SessionStart / UserPromptSubmit 훅이 `origin/develop` 을 확인해서 새 커밋과 바뀐 공유 문서를 Claude에게 알린다 (프롬프트 훅은 15분에 한 번)
- **이유**: 세션끼리 대화가 이어지지 않는다. 각 세션이 시작할 때 읽는 CLAUDE.md 는 로컬 사본이라 pull 전에는 낡아 있다
- **영향**: `.claude/settings.json`, `scripts/claude-sync.sh`, 이 파일, CLAUDE.md "세션 간 동기화"

## 2026-11-20 · 모듈 뼈대와 파일 배치 (PR #13)

- **결정**: `src/*.c`, `include/*.h` 10개 모듈로 나누고 각 파일에 대응하는 CS:APP Tiny 함수·트랙·이슈를 적는다. 시그니처는 초안
- **이유**: 책의 어느 부분을 어디에 구현하는지 팀이 같은 그림을 갖기 위해
- **영향**: `src/`, `include/`, CLAUDE.md 파일 구조 표. 시그니처 확정은 #4

## 2026-11-20 · CS:APP Tiny는 분석만, 원본 비커밋 (PR #12)

- **결정**: Tiny는 직접 구현하지 않고 `make classic` 으로 공식 원본을 받아 읽기·재현만 한다 (1인 60~90분). 원본은 `classic/` (gitignore)
- **이유**: 정답 코드를 다시 쓰는 비용 대비 이득이 작음. 저작권 (Codex 3라운드 검토)
- **영향**: #3, `scripts/fetch-tiny.sh`, `.gitignore`

## 2026-11-20 · 이슈·보드·기본 브랜치

- **결정**: 마일스톤 "11/25 마감" + 이슈 #3–#11, Projects 보드(Status / Track / Due), 기본 브랜치를 `develop` 으로 변경
- **이유**: 3명 병렬 작업의 진행 상황 공유, `close #N` 이 develop 머지 시 동작하도록
- **영향**: README, CLAUDE.md 작업 관리

## 2026-11-20 · 범위와 설계 (Claude ↔ Codex 2라운드 합의)

- **결정**: 처음부터 `poll()` 이벤트 루프 / CGI 대신 내장 앱 라우트 / Reverse Proxy만 / CGI·Forward Proxy·캐시·자동 재시도 제외 / 일 저녁 프록시 첫 통합, 화 저녁 릴리스
- **이유**: 4.5일·3명 일정, poll 루프 안 CGI 파이프의 위험, 결정적인 테스트
- **영향**: CLAUDE.md 설계 결정, 이슈 #4–#11
