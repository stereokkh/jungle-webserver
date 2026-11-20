# jungle-webserver

외부 HTTP 라이브러리 없이 **POSIX 소켓 API만으로** 작성하는 C 웹서버 + Proxy / Reverse Proxy.

## 개발 환경

| OS | 환경 |
|---|---|
| macOS | 기본 터미널 (Xcode Command Line Tools) |
| Linux | gcc 또는 clang, make |
| Windows | **WSL2 + Ubuntu** 필수 (또는 Docker / 클라우드) |

WSL2 Ubuntu 준비:

```bash
sudo apt update && sudo apt install -y build-essential git curl
```

> Windows 쪽 폴더(`/mnt/c/...`)보다 WSL 홈(`~/`)에 클론하는 것을 권장합니다 (빌드 속도, 파일 권한 문제).

## 빌드 & 실행

```bash
make            # ./webserver 생성
./webserver 8080
make clean
```

컴파일 플래그: `-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic`
POSIX 범위를 벗어나는 함수가 필요하면 `#ifdef __APPLE__` / `#ifdef __linux__` 로 분기합니다.

## 지원 환경과 검증

| 대상 | 확인 방법 |
|---|---|
| Ubuntu (glibc) | WSL / Linux 직접, `make docker-test`, CI |
| Alpine (musl) | `make docker-test DISTRO=alpine`, CI |
| macOS | 맥 사용 팀원, CI (macOS 러너) |

```bash
make test                               # 내 환경에서 빌드 + 테스트
make docker-test                        # Ubuntu 컨테이너에서
make docker-test DISTRO=alpine          # Alpine 컨테이너에서
make docker-test DISTRO=alpine CC=clang # 컴파일러 지정
make docker-test-all                    # Ubuntu + Alpine 모두
make docker-shell DISTRO=alpine         # 컨테이너 안에서 직접 확인
```

- Docker 컨테이너는 **개발용이 아니라 검증용**입니다. 개발은 각자 환경에서 하고, 다른 환경에서 되는지만 명령 한 줄로 확인합니다.
- 소스는 읽기 전용으로 마운트되고 컨테이너 안 복사본에서 빌드하므로 호스트의 `build/`는 건드리지 않습니다.
- Windows는 WSL 안에서 실행하세요 (Docker Desktop의 WSL 연동 필요).
- CI는 push/PR마다 Ubuntu(gcc, clang), macOS(clang), Alpine(gcc, clang) 5개 조합으로 `-Werror` 빌드와 테스트를 실행합니다.

## 디렉터리 구조

```
src/        소스 (.c)
include/    헤더 (.h)
tests/      테스트 스크립트 (make test)
docker/     검증용 Dockerfile (ubuntu, alpine)
build/      빌드 산출물 (git 무시)
```

## 협업 규칙

### 브랜치
- `main` — 안정 버전. **직접 push 불가, `develop` 에서 온 PR만 머지 가능**
- `develop` — 통합 브랜치이자 **기본 브랜치**. **직접 push 불가, PR + CI 통과 후 머지**
  - 기본 브랜치 변경 전에 클론했다면 `git remote set-head origin -a` 로 갱신
- `feat/<기능>`, `fix/<버그>` — 작업 브랜치 → `develop` 으로 PR

```
feat/* ──PR(CI)──▶ develop ──PR(CI + main-guard)──▶ main
```

브랜치 보호 규칙 (`main`, `develop` 공통):
- PR 필수 (직접 push 금지, 관리자 포함)
- CI 5개 조합 모두 통과 필수
- force push, 브랜치 삭제 금지

`main` 추가 규칙:
- `main ← develop only` 검사 통과 필수 (develop 이외 브랜치의 PR은 머지 불가)

### 작업 관리
- 할 일: 마일스톤 [11/25 마감](https://github.com/stereokkh/jungle-webserver/milestone/1) 의 이슈
- 보드: [Projects](https://github.com/users/stereokkh/projects/12) — 이슈를 잡으면 Assignee 지정 + In Progress
- PR 설명에 `close #이슈번호` 를 쓰면 develop 머지 시 이슈가 닫히고 보드에서 Done 으로 이동

### 커밋 메시지
```
feat: GET 요청 파싱 추가
fix: 연결 종료 시 fd 누수 수정
refactor: / docs: / test: / chore:
```

### PR
- 최소 1명 리뷰 후 머지
- CI(Ubuntu / macOS / Alpine)가 통과해야 머지
- macOS 사용자가 있다면 리뷰어 중 한 명은 macOS 사용자로 지정
