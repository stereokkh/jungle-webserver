#!/bin/sh
# Claude Code 세션 동기화 훅 (.claude/settings.json 에서 호출)
#
#   claude-sync.sh session   세션 시작 시: origin/develop 에 새 커밋이 있으면 요약을 출력
#   claude-sync.sh prompt    프롬프트마다 호출되지만 CLAUDE_SYNC_INTERVAL(기본 900초)에 한 번만
#                            fetch 하고, 지난번에 알린 뒤로 develop 이 움직였을 때만 출력
#
# 출력(stdout)은 Claude의 컨텍스트에 들어간다. 아무 일도 없으면 아무것도 출력하지 않는다.
# 네트워크가 없거나 git 저장소가 아니면 조용히 끝난다 (작업을 막지 않음).
#
# 하는 일은 fetch 와 알림뿐이다. 예외: 현재 브랜치가 develop 이고 작업 트리가 깨끗하면
# fast-forward 만 한다 (merge 커밋·충돌 없음). 작업 브랜치는 절대 건드리지 않는다.
set -u

mode=${1:-session}
cd "${CLAUDE_PROJECT_DIR:-.}" 2>/dev/null || exit 0
gitdir=$(git rev-parse --git-dir 2>/dev/null) || exit 0

stamp="$gitdir/claude-sync-checked"
seen="$gitdir/claude-sync-seen"
now=$(date +%s)

if [ "$mode" = prompt ]; then
    last=$(cat "$stamp" 2>/dev/null || echo 0)
    [ $((now - last)) -lt "${CLAUDE_SYNC_INTERVAL:-900}" ] && exit 0
fi
echo "$now" > "$stamp"

GIT_TERMINAL_PROMPT=0 git fetch --quiet origin develop 2>/dev/null || exit 0
remote=$(git rev-parse -q --verify refs/remotes/origin/develop) || exit 0

if [ "$mode" = prompt ] && [ "$(cat "$seen" 2>/dev/null)" = "$remote" ]; then
    exit 0   # 이미 알린 상태 그대로
fi
echo "$remote" > "$seen"

behind=$(git rev-list --count "HEAD..$remote" 2>/dev/null || echo 0)
[ "$behind" -eq 0 ] && exit 0

branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null)
watched="CLAUDE.md docs include Makefile tests scripts .claude .github"

echo "[팀 동기화] origin/develop 에 현재 브랜치($branch)에 없는 커밋이 ${behind}개 있습니다."
echo "최근 커밋:"
git log --oneline --no-decorate -n 8 "HEAD..$remote" | sed 's/^/  /'

# shellcheck disable=SC2086
changed=$(git diff --name-only "HEAD...$remote" -- $watched 2>/dev/null)
if [ -n "$changed" ]; then
    echo "바뀐 공유 문서·인터페이스:"
    echo "$changed" | sed 's/^/  /'
fi

if [ "$branch" = develop ] && [ -z "$(git status --porcelain --untracked-files=no)" ]; then
    if git merge --ff-only --quiet "$remote" 2>/dev/null; then
        echo "→ develop 이 깨끗해서 fast-forward 했습니다. 위 파일은 이미 작업 트리에 반영됨."
    fi
fi

cat <<'EOF'
지시:
- 처음 읽은 CLAUDE.md 는 낡았을 수 있다. 위에 CLAUDE.md / docs/decisions.md 가 있으면
  `git show origin/develop:CLAUDE.md`, `git diff HEAD...origin/develop -- docs/decisions.md` 로 최신 결정을 읽고 따른다.
- include/ 가 바뀌었으면 지금 작업이 바뀐 시그니처와 맞는지 확인한다.
- 지금 작업이 새 결정과 충돌하면 진행하기 전에 사용자에게 알린다.
- pull / rebase / merge 는 사용자에게 묻고 한다 (develop 의 fast-forward 는 위에서 이미 처리).
EOF
