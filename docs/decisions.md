# 결정 기록

계획·범위·규칙이 바뀔 때마다 **맨 위에** 항목을 추가한다. CLAUDE.md 는 "지금 상태"만, 이 파일은 "언제, 왜 바뀌었나"를 남긴다.

- 결정을 바꾸는 PR은 이 파일 항목 + CLAUDE.md 수정을 **같은 PR**에 넣는다
- 형식: 날짜 · 제목 / 결정 / 이유 / 영향 (파일·이슈)
- 각 세션의 Claude는 동기화 훅(`scripts/claude-sync.sh`)이 이 파일 변경을 알려 주면 새 항목을 읽고 따른다

---

## 2026-11-21 · 정적 파일 경로 판정 (PR #22, 이슈 #6)

- **결정**:
  1. `static_open` 은 `.`·`..`·`//` 를 글자 단위로 먼저 정리하고, 루트 위로 가면 파일 존재와 상관없이 403
  2. 그다음 `realpath` 결과가 docroot 밖이면(심볼릭 링크) 403. 디렉터리는 `index.html`(없으면 404), `/파일/` 은 404, 일반 파일이 아니면 403, docroot 를 못 찾으면 500
  3. `realpath`(XSI 확장)를 쓰는 파일은 맨 위에 `#define _XOPEN_SOURCE 700` 을 둔다. 공통 플래그 `-D_POSIX_C_SOURCE=200809L` 은 그대로
- **이유**: 1 Tiny `parse_uri` 가 `/../secret.txt` 를 200 으로 열었음 (#3). 존재 여부로 403/404 가 갈리면 docroot 밖 파일이 있는지 알려 주게 됨. 2 글자 정리만으로는 docroot 안의 링크가 밖을 가리키는 경우를 못 막음. 3 musl·clang 은 `_POSIX_C_SOURCE` 만으로 `realpath` 선언을 보여 주지 않아 CI 가 실패함 (glibc gcc 는 통과해서 로컬에서 못 잡음)
- **영향**: `src/static_file.c`, `include/static_file.h`, `docs/interfaces.md` "정한 세부 규칙", `tests/unit/test_static_file.c`, `www/`. 시그니처는 그대로. 파서(#5)는 디코딩한 path 를 넘기고 NUL·제어 문자는 400 으로 막는다 (상철 확인)

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
