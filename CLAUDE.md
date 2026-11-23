# CLAUDE.md

이 파일은 Claude가 이 저장소에서 작업을 시작할 때 읽는 프로젝트 안내입니다. 사람이 읽을 사용법은 README.md에 있습니다.

## 프로젝트

- C로 작성하는 웹서버 + Proxy / Reverse Proxy (크래프톤 정글 과제)
- **외부 HTTP 라이브러리·프레임워크 금지.** POSIX 소켓 API와 C 표준 라이브러리만 사용
- macOS / WSL / Linux 에서 같은 소스가 빌드·실행되어야 함 (과제 요구사항)
- 마감: **2026-11-25(수) 12:00 KST**. 3명

| 트랙 | 담당 (GitHub) | 맡은 일 | 이슈 |
|---|---|---|---|
| A | 건호 (`stereokkh`, 팀장) | 서버 코어(net, server, main), 공통 인터페이스, 릴리스 | #4 #7 #11(릴리스) |
| B | 상철 (`Shirokuma0704`) | HTTP 파서, Reverse Proxy 설정·중계 | #5 #8 #9 |
| C | 혜성 (`sikhesung`) | 응답·MIME·정적 파일, 내장 앱 라우트, 검증 조율, Tiny 분석 취합 | #6 #10 #11(검증) #3(취합) |

## 설계 결정 (2026-11-20 합의)

과제 원본·이슈 초안을 Codex와 교차 검토해서 정한 범위다. 바꾸려면 팀 합의 후 이 절을 먼저 고친다.

| 항목 | 결정 | 이유 |
|---|---|---|
| 동시성 | **처음부터 `poll()` 이벤트 루프** (논블로킹). 블로킹 서버를 먼저 만들고 갈아엎지 않는다 | 팀 이식성 규칙, 느린 클라이언트 격리, 재작성 비용 회피 |
| 동적 처리 | **서버 내장 앱 라우트** `GET /api/add`, `POST /api/echo`. CGI는 하지 않는다 | 과제가 "CGI 또는 백엔드 전달"을 허용. poll 루프 안의 CGI 파이프·`waitpid`는 서버 전체를 막을 위험 |
| 프록시 | **Reverse Proxy만** (경로 최장 접두사 라우팅, 502/504, 마지막에 Round-Robin). 백엔드는 같은 바이너리를 프록시 설정 없이 다른 포트로 실행 | 외부 인터넷 없이 결정적 테스트 가능 |
| 처리 순서 | 프록시 경로 → 내장 앱 경로 → 정적 파일 | 백엔드 인스턴스에서 프록시 순환 방지 |
| 범위 밖 | CGI, Forward Proxy, 캐시, 백엔드 장애 시 자동 재시도/페일오버, chunked, TLS | 4.5일 일정. 필요하면 마감 후 후속 이슈로 |

구현 시 지킬 계약 (모듈 연결·버퍼 소유권·반환값 규칙은 `docs/interfaces.md`, 함수별 계약은 각 헤더 주석):

- 모듈(파서·응답·정적·앱·프록시 정책)은 소켓과 무관한 순수 함수로 두고 단위 테스트한다
- 파서는 증분 방식: 완료 / 추가 입력 필요 / 오류 + **소비 바이트 수**를 반환하고, 다음 요청의 잔여 바이트를 보존한다 (keep-alive). 본문만 덜 온 추가 입력 필요는 소비 바이트 대신 **필요한 총 길이**를 돌려주고, 서버는 그만큼 모일 때까지 파서를 다시 부르지 않는다 (consume 하지 않음, `docs/interfaces.md`)
- 원본 요청 대상(프록시 전달용)과 디코딩된 경로(파일 접근용)를 따로 저장한다
  - **#31 에서 바뀜 (결정됨, 구현 전)**: 라우팅·백엔드 전달은 파서가 만든 경로 정규형 하나를 쓰고, `%2F`, 그냥 `\`·`%5C` 와 빈 조각 `//` 는 400 (`--merge-slashes` 면 `//` 를 합침, nginx 와 이름은 같고 기본값은 반대). 순서·이유는 `docs/decisions.md` 2026-11-23
- 송신은 진행 상태(offset)를 저장하고, 막히면(`EAGAIN`) 이벤트 루프에 제어권을 돌려준다. 루프 안에서 끝날 때까지 `write` 를 반복하지 않는다
- 백엔드 주소는 설정 로드 시 `getaddrinfo` 로 한 번 해석한다. 요청 처리 중에는 이름 해석을 하지 않는다
- 응답 송신 시작 전 실패는 502/504, 시작 후 실패는 연결 종료 + 자원 회수
- 종료(`SIGINT`/`SIGTERM`) 시 listen 소켓뿐 아니라 모든 연결·파일·업스트림을 정리한다

파일 구조와 CS:APP Tiny 대응 (각 파일 맨 위 주석에 자세히):

| 파일 (`src/*.c`, `include/*.h`) | Tiny 대응 | 트랙 · 이슈 |
|---|---|---|
| `net` | `open_listenfd` | A · #7 |
| `server` | `main` 의 반복 accept 루프 → poll 이벤트 루프 | A · #7 |
| `handler` | `doit` 의 정적/동적 분기 → 프록시 → 앱 → 정적 | A(틀) · #4, #8, #10 |
| `http_request` | `doit` 요청 라인, `read_requesthdrs`, `parse_uri` | B · #5 |
| `http_response` | `clienterror`, `serve_static` 헤더 | C · #6 |
| `mime` | `get_filetype` | C · #6 |
| `static_file` | `serve_static`, `parse_uri` 정적 경로 | C · #6 |
| `app` | `serve_dynamic` + `adder.c` (CGI 대신 함수) | C · #10 |
| `proxy_config` | 없음 | B · #8 |
| `proxy` | 없음 | B · #8, #9 |
| `buffer` | `rio_t` 내부 버퍼 (고정 배열 대신 늘어나는 버퍼) | A · #4 |

- 지금은 선언과 `TODO(#이슈)` 스텁만 있다 (`buffer`, `http_request`, `mime`, `http_response`, `static_file`, `app`, `proxy_config` 는 구현·테스트 완료, `proxy` 는 `proxy_build_request` 만). 시그니처는 #4 인터페이스 PR 에서 확정했고, 바꾸려면 관련 담당자 합의 후 헤더와 `docs/interfaces.md` 를 같은 PR 에서 고친다
- 응답·프록시 요청은 고정 배열이 아니라 `buffer` 에 덧붙인다. 실패하면 `buffer_mark`/`buffer_rollback` 으로 버퍼를 호출 전 상태로 되돌린다
- `main.c`(CLI `-p -r -c`)·`net.c`·`server.c` 는 #7 에서 구현했다. 서버는 파서 → `handler_route` → 정적 파일·응답으로 실제 요청을 처리하고, keep-alive·파이프라이닝, 본문 대기 중 파서 재호출 생략(`NEED_MORE` 의 `consumed`, PR #24), 요청 타임아웃(30초), lingering close 를 한다. 앱(#10)·프록시(#9)는 모듈이 붙으면 `server.c` 의 `respond()` 에서 스텁 처리(500, 502)만 걷어 내면 된다

CS:APP Tiny (이슈 #3):

- 분석·비교 대상일 뿐이다. 원본(`tiny.c`, `csapp.c`)은 **커밋하지 않는다** (URL·해시·분석 문서만). `src/` 에서 `csapp.c` 를 링크하지 않는다
- 로컬에서 받기·빌드: `make classic` (= `sh scripts/fetch-tiny.sh`) → `classic/` 에 원본·`SOURCES.txt`(URL·sha256)·바이너리. `classic/` 은 `.gitignore` 대상이고, 팀 빌드 플래그·CI와 무관하다 (Makefile은 `src/*.c` 만 빌드)
- Tiny와의 벤치마크는 "관찰"로만 기록한다. "poll로 바꿔 몇 배 빨라졌다" 같은 인과 주장은 쓰지 않는다

## 지원 환경

| 우선순위 | 환경 | 이유 |
|---|---|---|
| 1 | Linux glibc (Ubuntu) | 실제 서버 운영 환경, WSL 기본 배포판 |
| 2 | macOS (clang) | 팀원·채점자의 개발 환경. 빌드되고 동작하면 충분, OS별 최적화 대상 아님 |
| 2 | Alpine (musl) | Docker 배포 이미지용 |
| 미지원 | Windows 네이티브 | POSIX가 아님 (Winsock). WSL 또는 Docker 사용 |

## 이식성 규칙 (코드 작성 시 반드시 지킬 것)

POSIX로 해결되는 방법을 먼저 쓴다.

| 쓰지 말 것 (Linux 전용 등) | 대신 쓸 것 (POSIX) |
|---|---|
| `MSG_NOSIGNAL` | 시작 시 `signal(SIGPIPE, SIG_IGN)` |
| `socket(..., SOCK_NONBLOCK)`, `accept4()` | `fcntl(fd, F_SETFL, O_NONBLOCK)` |
| `gethostbyname()` | `getaddrinfo()` (IPv4/IPv6) |
| `epoll` / `kqueue` 를 기본 경로로 | `poll()` 기본, epoll/kqueue는 나중에 최적화로만 |
| `sendfile()` | `read`/`write` 루프 (시그니처가 OS마다 다름) |

- `struct sockaddr_*` 는 사용 전 반드시 `memset` 0 초기화 (macOS에는 `sin_len` 필드가 있음)
- 헤더는 필요한 것을 모두 직접 include (`<sys/types.h>`, `<sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`, `<netdb.h>`, `<unistd.h>`). Ubuntu에서는 다른 헤더를 통해 따라 들어와서 빌드되지만 macOS/musl에서는 실패함
- 스레드를 쓸 경우 큰 버퍼를 스택에 잡지 말 것 (musl 기본 스레드 스택 128KB → Alpine에서만 segfault)
- IPv6 소켓의 `IPV6_V6ONLY` 기본값이 OS마다 다르므로 필요하면 명시적으로 설정
- OS별 분기(`#ifdef __linux__`, `#ifdef __APPLE__`)는 `src/platform/` 같은 한 곳에만 모은다. 나머지 코드에 흩뿌리지 않는다
- 빌드 플래그: `-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic`. macOS 전용 확장이 필요하면 해당 파일에서만 `_DARWIN_C_SOURCE` 정의

## 디렉터리

```
src/        소스 (.c)
include/    헤더 (.h)
tests/      테스트 (unit/test_*.c: 모듈 단위 테스트, run.sh: 서버 스모크 테스트)
scripts/    보조 스크립트 (fetch-tiny.sh: Tiny 받기·빌드, claude-sync.sh: 세션 동기화 훅)
classic/    (gitignore) make classic 으로 받은 Tiny 원본. 커밋 금지
docker/     검증용 Dockerfile (ubuntu, alpine) — 배포용 아님
.github/    CI (ci.yml), main 보호 검사 (main-guard.yml), PR/이슈 템플릿
docs/       decisions.md (결정 기록), interfaces.md (모듈 연결 약속), classic.md (Tiny 관찰 → 개선 요약 표, #3), api.md (사용법·지원 범위·운영 제약), (예정) benchmark.md
.claude/    settings.json (팀 공용 훅: 세션 동기화)
www/        정적 파일 예제
```

## 명령

```bash
make                                    # ./webserver 빌드
make unit                               # 단위 테스트만 (tests/unit/test_*.c)
make test                               # 빌드 + 단위 테스트 + 스모크 테스트
make asan                               # ASan+UBSan 으로 build/asan/ 에 따로 빌드해 make test (Alpine 불가)
make WERROR=1                           # 경고를 에러로 (CI와 동일)
make CC=clang                           # 컴파일러 지정
make docker-test [DISTRO=alpine] [CC=clang]   # 컨테이너에서 빌드+테스트
make docker-test-all                    # Ubuntu + Alpine
make docker-shell DISTRO=alpine         # 컨테이너 안 셸
make classic                            # CS:APP Tiny를 classic/ 에 받아 빌드 (분석용, 커밋 안 함)
```

- Docker 컨테이너는 **검증용**이다. 소스를 읽기 전용으로 마운트하고 컨테이너 내부 복사본에서 빌드하므로 호스트 `build/` 는 건드리지 않는다
- macOS는 Docker로 띄울 수 없으므로 CI(macos-latest)가 확인한다

## 검증 흐름

| 단계 | 어디서 |
|---|---|
| 개발 | 각자 환경 (Windows는 WSL Ubuntu, 맥은 macOS) |
| 빠른 확인 | 로컬 `make test`, `make asan`, `make docker-test DISTRO=alpine` |
| 최종 판정 | CI: ubuntu(gcc, clang), macos(clang), alpine(gcc, clang), 모두 `-Werror` (필수 5개) + ubuntu asan (필수 아님) |

- 모듈(파서·응답·정적·앱·프록시 정책)을 구현하면 `tests/unit/test_<모듈>.c` 에 단위 테스트를 추가한다. 매크로는 `tests/unit/test.h` (CHECK, CHECK_INT, CHECK_STR, CHECK_MEM, REQUIRE). 파일만 만들면 `make unit` 이 자동으로 빌드·실행한다
- 파서 테스트는 `test_bytes()` 로 NUL 종료 없는 정확한 크기 버퍼를 넘긴다 (len 을 넘어 읽으면 `make asan` 이 잡음)
- `tests/unit/test_contract.c` 는 모든 모듈이 구현 전후로 지킬 공통 계약이다. 깨지면 계약을 바꿀지 팀에서 먼저 정한다
- 서버 동작은 `tests/run.sh` 에 테스트를 추가한다 (서버를 띄우고 curl로 요청/응답 확인)
- 작업을 마치면 최소 `make test` 와 `make asan` 을 돌리고, 소켓·헤더 관련 변경이면 `make docker-test DISTRO=alpine` 과 `make CC=clang WERROR=1` 도 확인한다

## 브랜치 / PR 규칙

```
feat/* , fix/* ──PR(CI)──▶ develop ──PR(CI + main-guard)──▶ main
```

- **기본 브랜치는 `develop`** (2026-11-20 변경). PR 대상 기본값이 develop이고, develop에 머지될 때 `close #N` 이 이슈를 닫는다. 기존 클론은 `git remote set-head origin -a` 로 갱신
- `main`, `develop` 은 보호됨: 직접 push 불가(관리자 포함), PR + CI 5개 통과 필수, force push·삭제 금지
- `main` 은 이 저장소의 `develop` 에서 온 PR만 머지 가능 (`main ← develop only` 검사)
- 작업은 항상 `develop` 에서 새 브랜치를 만들어 시작: `git checkout develop && git pull && git checkout -b feat/<기능>`
- CI push 트리거는 `develop` 만. 작업 브랜치는 PR 이벤트로만 검사됨
- 커밋 메시지: `feat:`, `fix:`, `refactor:`, `docs:`, `test:`, `chore:`, `ci:` 접두사

## 작업 관리 (이슈 / 보드)

- 할 일은 마일스톤 **"11/25 마감"** 의 이슈 #3–#11 에 있다. 작업 전 해당 이슈의 할 일·기한·참고를 읽는다
- 보드: https://github.com/users/stereokkh/projects/12 (Status: Todo → In Progress → In Review → Done, 필드: Track, Due). 이슈 닫힘·PR 머지 시 Done 으로 자동 이동
- 이슈를 잡으면 Assignee 지정 + In Progress. 막히면 이슈 댓글로 남긴다
- PR은 작게 자주. 중간 PR은 `ref #N`, 이슈를 끝내는 PR만 `close #N`
- 일정 체크포인트: 토 10:00 #4 인터페이스 확정 → **일 저녁 프록시 GET/POST 첫 통합** (늦어지면 Round-Robin·Tiny 추가 비교를 미룬다) → 화 저녁 `develop → main` 릴리스, 수 오전은 여유
- sanitizer(`make asan`)·종료 테스트는 각 기능 PR에서 돌린다. 마지막 날 처음 돌리지 않는다

### 교차 리뷰 (전원이 프로젝트 전체를 알기 위해)

- **A의 PR은 B가, B의 PR은 C가, C의 PR은 A가** 리뷰한다. `.github/CODEOWNERS` 가 파일 경로에 따라 리뷰어를 자동으로 요청한다
- develop 머지에는 승인 1개가 필요하다 (보호 규칙). 공통 파일(CLAUDE.md, docs/, Makefile 등)은 다른 두 명 중 아무나
- 리뷰 요청을 받으면 가능한 한 빨리 본다. 리뷰어가 막히면 다른 한 명이 대신 승인해도 된다 (일정 우선)
- 리뷰어는 "동작하나"만이 아니라 **설명할 수 있을 만큼** 읽는다. 모르는 부분은 PR 댓글로 묻는다
- 화요일 릴리스 전 30분: 각자 자기 파트가 아닌 모듈 하나를 나머지에게 설명한다

### 노션과 저장소의 역할

- **프로젝트 노션**: https://app.notion.com/p/jungle-webserver-41efacdfe6e480e59bc8e888ca466fb6 (팀원 워크스페이스)
- **노션**: 각자 자기 파트의 학습 정리 (Tiny 분석, 개념, 주석 단 코드 설명). 사람이 읽는 공부 노트
- **저장소**: 결정·규칙·인터페이스·코드. **기준은 항상 저장소**다. 노션에서 정한 결정은 반드시 `docs/decisions.md` + 이 파일에 옮긴다 (노션에 연결되지 않은 세션·팀원도 있음)

Claude의 노션 사용 범위:

- 이 프로젝트에서는 **위 페이지와 그 하위 페이지만** 읽고 쓴다. 같은 계정의 다른 노션 페이지(개인 기록 등)는 검색·수정하지 않는다
- 팀 공용 공간이므로 노션에 **쓰기·수정·삭제 전에는 사용자에게 확인**한다. 읽기는 자유
- 연결: 저장소의 `.mcp.json` 이 이 프로젝트에서만 Notion MCP 를 켠다. 처음 `claude` 실행 시 프로젝트 MCP 서버 승인 → `/mcp` 에서 Notion 로그인할 때 **페이지가 있는 팀원 워크스페이스**를 선택 (게스트가 아니라 멤버여야 함)
- 코드의 "왜 이렇게 했나"(특히 Tiny와 다른 점)는 노션뿐 아니라 **코드 주석**에도 남긴다. 리뷰어와 Claude가 읽는 곳은 코드다
- #3 Tiny 분석: 상세 정리는 노션, 저장소 `docs/classic.md` 에는 "관찰한 문제 → 어느 이슈에서 개선" 요약 표만

## 세션 간 동기화

계획·관리는 Windows 쪽 세션, 코딩은 각자 WSL/macOS 세션에서 한다. 세션끼리 대화는 이어지지 않으므로 **저장소(develop)가 유일한 기준**이다.

- 계획·범위·규칙이 바뀌면: `docs/decisions.md` 맨 위에 항목 추가 + 이 파일의 "지금 상태" 수정을 같은 PR로 develop에 머지한다. 대화로만 정하고 끝내지 않는다
- `.claude/settings.json` 훅이 `scripts/claude-sync.sh` 를 실행한다
  - 세션 시작 시, 그리고 프롬프트 중 15분에 한 번 `origin/develop` 을 fetch
  - 현재 브랜치에 없는 커밋이 있으면 커밋 목록과 바뀐 공유 파일(CLAUDE.md, docs/, include/ 등)을 컨텍스트로 알려 준다
  - develop 에 있고 작업 트리가 깨끗하면 fast-forward 만 한다. 작업 브랜치는 건드리지 않는다
- 알림을 받으면: `git show origin/develop:CLAUDE.md`, `git diff HEAD...origin/develop -- docs/decisions.md include/` 를 읽고 따른다. 지금 작업과 충돌하면 진행 전에 사용자에게 알린다. 작업 브랜치의 rebase/merge 는 사용자에게 묻고 한다
- 처음 한 번은 훅 승인이 필요할 수 있다 (Claude Code가 새 프로젝트 훅을 확인함). 끄려면 `.claude/settings.local.json` 에 `"disableAllHooks": true`

## 개발 환경 메모

- Windows 사용자는 WSL Ubuntu 안(`~/jungle-webserver`)에 클론해서 작업한다. `/mnt/c/...` 경로는 느리고 권한 문제가 있음
- Claude CLI도 WSL 안에 설치한 것을 쓴다 (`which claude` → `~/.local/bin/claude`). WSL에서 `claude.exe` 가 잡히면 Windows 쪽이 실행된 것이고, 이때는 make/gcc가 없다
- Docker Desktop → Settings → Resources → WSL Integration 에서 Ubuntu를 켜야 WSL에서 `make docker-test` 가 동작한다

## 아직 하지 않은 것 (나중에 할 일)

- 서버 구현: poll 루프·CLI·종료·모듈 연결·keep-alive 는 #7 에서 완료. 남은 것은 앱(#10), 프록시 설정·중계(#8 #9, 업스트림 상태는 A·B 협업)
- `tests/run.sh` 는 서버를 백그라운드로 띄워 검사한다 (준비 대기 최대 5초, 종료 대기는 `wait_exit`). 서버 동작 테스트는 여기에 추가한다. 응답은 상태 코드와 필요한 줄만 검사한다 (Date 가 매초 바뀜). 30초 요청 타임아웃처럼 오래 걸리는 동작은 넣지 않고 `-DCLIENT_REQUEST_TIMEOUT_MS=2000` 으로 따로 빌드해 손으로 확인한다
- `docs/benchmark.md` → 이슈 #11
- 배포용 Docker 이미지 (빌드된 바이너리만 담는 작은 이미지). 마감 범위 밖
- 팀원 Collaborator 초대는 완료. develop 필수 리뷰 수 1 (교차 리뷰), main 은 0 유지 (develop 에서 이미 리뷰됨)
- 노션 접근: 2026-11-20 새 주소로 프로젝트 페이지 읽기 확인됨 (claude.ai Notion 커넥터). 저장소 `.mcp.json` 의 Notion MCP 로그인은 팀원별로 확인 필요
- `main ← develop only` 검사는 첫 develop→main PR 때 처음 실행됨 (아직 실제 동작 미확인)
