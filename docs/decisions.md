# 결정 기록

계획·범위·규칙이 바뀔 때마다 **맨 위에** 항목을 추가한다. CLAUDE.md 는 "지금 상태"만, 이 파일은 "언제, 왜 바뀌었나"를 남긴다.

- 결정을 바꾸는 PR은 이 파일 항목 + CLAUDE.md 수정을 **같은 PR**에 넣는다
- 형식: 날짜 · 제목 / 결정 / 이유 / 영향 (파일·이슈)
- 각 세션의 Claude는 동기화 훅(`scripts/claude-sync.sh`)이 이 파일 변경을 알려 주면 새 항목을 읽고 따른다

---

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
