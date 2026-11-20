# CLAUDE.md

이 파일은 Claude가 이 저장소에서 작업을 시작할 때 읽는 프로젝트 안내입니다. 사람이 읽을 사용법은 README.md에 있습니다.

## 프로젝트

- C로 작성하는 웹서버 + Proxy / Reverse Proxy (크래프톤 정글 과제)
- **외부 HTTP 라이브러리·프레임워크 금지.** POSIX 소켓 API와 C 표준 라이브러리만 사용
- macOS / WSL / Linux 에서 같은 소스가 빌드·실행되어야 함 (과제 요구사항)
- 마감: **2026-11-25(수) 12:00 KST**. 3명 (트랙 A: 서버 코어·릴리스, B: HTTP 파서·프록시, C: 응답·정적·앱·검증 조율)

## 설계 결정 (2026-11-20 합의)

과제 원본·이슈 초안을 Codex와 교차 검토해서 정한 범위다. 바꾸려면 팀 합의 후 이 절을 먼저 고친다.

| 항목 | 결정 | 이유 |
|---|---|---|
| 동시성 | **처음부터 `poll()` 이벤트 루프** (논블로킹). 블로킹 서버를 먼저 만들고 갈아엎지 않는다 | 팀 이식성 규칙, 느린 클라이언트 격리, 재작성 비용 회피 |
| 동적 처리 | **서버 내장 앱 라우트** `GET /api/add`, `POST /api/echo`. CGI는 하지 않는다 | 과제가 "CGI 또는 백엔드 전달"을 허용. poll 루프 안의 CGI 파이프·`waitpid`는 서버 전체를 막을 위험 |
| 프록시 | **Reverse Proxy만** (경로 최장 접두사 라우팅, 502/504, 마지막에 Round-Robin). 백엔드는 같은 바이너리를 프록시 설정 없이 다른 포트로 실행 | 외부 인터넷 없이 결정적 테스트 가능 |
| 처리 순서 | 프록시 경로 → 내장 앱 경로 → 정적 파일 | 백엔드 인스턴스에서 프록시 순환 방지 |
| 범위 밖 | CGI, Forward Proxy, 캐시, 백엔드 장애 시 자동 재시도/페일오버, chunked, TLS | 4.5일 일정. 필요하면 마감 후 후속 이슈로 |

구현 시 지킬 계약 (자세한 건 이슈 #4):

- 모듈(파서·응답·정적·앱·프록시 정책)은 소켓과 무관한 순수 함수로 두고 단위 테스트한다
- 파서는 증분 방식: 완료 / 추가 입력 필요 / 오류 + **소비 바이트 수**를 반환하고, 다음 요청의 잔여 바이트를 보존한다 (keep-alive)
- 원본 요청 대상(프록시 전달용)과 디코딩된 경로(파일 접근용)를 따로 저장한다
- 송신은 진행 상태(offset)를 저장하고, 막히면(`EAGAIN`) 이벤트 루프에 제어권을 돌려준다. 루프 안에서 끝날 때까지 `write` 를 반복하지 않는다
- 백엔드 주소는 설정 로드 시 `getaddrinfo` 로 한 번 해석한다. 요청 처리 중에는 이름 해석을 하지 않는다
- 응답 송신 시작 전 실패는 502/504, 시작 후 실패는 연결 종료 + 자원 회수
- 종료(`SIGINT`/`SIGTERM`) 시 listen 소켓뿐 아니라 모든 연결·파일·업스트림을 정리한다

CS:APP Tiny (이슈 #3):

- 분석·비교 대상일 뿐이다. 원본(`tiny.c`, `csapp.c`)은 **커밋하지 않는다** (URL·해시·분석 문서만). `src/` 에서 `csapp.c` 를 링크하지 않는다
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
tests/      테스트 (tests/run.sh, make test 로 실행)
docker/     검증용 Dockerfile (ubuntu, alpine) — 배포용 아님
.github/    CI (ci.yml), main 보호 검사 (main-guard.yml), PR/이슈 템플릿
docs/       (예정) api.md, benchmark.md, classic.md
www/        (예정) 정적 파일 예제
```

## 명령

```bash
make                                    # ./webserver 빌드
make test                               # 빌드 + 테스트
make WERROR=1                           # 경고를 에러로 (CI와 동일)
make CC=clang                           # 컴파일러 지정
make docker-test [DISTRO=alpine] [CC=clang]   # 컨테이너에서 빌드+테스트
make docker-test-all                    # Ubuntu + Alpine
make docker-shell DISTRO=alpine         # 컨테이너 안 셸
```

- Docker 컨테이너는 **검증용**이다. 소스를 읽기 전용으로 마운트하고 컨테이너 내부 복사본에서 빌드하므로 호스트 `build/` 는 건드리지 않는다
- macOS는 Docker로 띄울 수 없으므로 CI(macos-latest)가 확인한다

## 검증 흐름

| 단계 | 어디서 |
|---|---|
| 개발 | 각자 환경 (Windows는 WSL Ubuntu, 맥은 macOS) |
| 빠른 확인 | 로컬 `make test`, `make docker-test DISTRO=alpine` |
| 최종 판정 | CI: ubuntu(gcc, clang), macos(clang), alpine(gcc, clang), 모두 `-Werror` |

- 기능을 추가하면 `tests/run.sh` 에 테스트를 추가한다 (서버를 띄우고 curl로 요청/응답 확인)
- 작업을 마치면 최소 `make test` 를 돌리고, 소켓·헤더 관련 변경이면 `make docker-test DISTRO=alpine` 과 `make CC=clang WERROR=1` 도 확인한다

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

## 개발 환경 메모

- Windows 사용자는 WSL Ubuntu 안(`~/jungle-webserver`)에 클론해서 작업한다. `/mnt/c/...` 경로는 느리고 권한 문제가 있음
- Claude CLI도 WSL 안에 설치한 것을 쓴다 (`which claude` → `~/.local/bin/claude`). WSL에서 `claude.exe` 가 잡히면 Windows 쪽이 실행된 것이고, 이때는 make/gcc가 없다
- Docker Desktop → Settings → Resources → WSL Integration 에서 Ubuntu를 켜야 WSL에서 `make docker-test` 가 동작한다

## 아직 하지 않은 것 (나중에 할 일)

- 서버 구현 (현재 `src/main.c` 는 포트 인자만 받는 뼈대) → 이슈 #4–#10
- `tests/run.sh` 는 아직 `./webserver 8080` 이 바로 exit 0 하는지 검사한다. 서버가 상주하게 되면 깨지므로 첫 서버 PR(#7)에서 백그라운드 실행 방식으로 바꾼다
- `docs/api.md`, `docs/benchmark.md`, `docs/classic.md` → 이슈 #11, #3
- 배포용 Docker 이미지 (빌드된 바이너리만 담는 작은 이미지). 마감 범위 밖
- 팀원 Collaborator 초대는 완료. `main`/`develop` 보호 규칙의 필수 리뷰 수 0 → 1 은 아직
- `main ← develop only` 검사는 첫 develop→main PR 때 처음 실행됨 (아직 실제 동작 미확인)
