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

## 디렉터리 구조

```
src/        소스 (.c)
include/    헤더 (.h)
tests/      테스트 스크립트
build/      빌드 산출물 (git 무시)
```

## 협업 규칙

### 브랜치
- `main` — 항상 빌드되는 안정 버전
- `develop` — 통합 브랜치
- `feat/<기능>`, `fix/<버그>` — 작업 브랜치 → `develop` 으로 PR

### 커밋 메시지
```
feat: GET 요청 파싱 추가
fix: 연결 종료 시 fd 누수 수정
refactor: / docs: / test: / chore:
```

### PR
- 최소 1명 리뷰 후 머지
- CI(Ubuntu + macOS 빌드)가 통과해야 머지
