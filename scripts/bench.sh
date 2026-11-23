#!/usr/bin/env bash
# 이슈 #11: 정적 파일 처리량(ab)과 Tiny 제한 비교. 결과는 docs/benchmark.md 에 옮긴다
#   sh 가 아니라 bash (/dev/tcp 로 연결만 열어 두는 데 씀)
#   bash scripts/bench.sh            # 우리 서버 + (classic/tiny 가 있으면) Tiny
#   REPS=5 N=20000 bash scripts/bench.sh
# 측정용이라 CI·make test 에는 넣지 않는다. sanitizer 빌드는 측정하지 않는다 (일반 -O2 빌드만)
set -euo pipefail
cd "$(dirname "$0")/.."

REPS=${REPS:-3}        # 같은 조건 반복 횟수, 표에는 중앙값
N=${N:-10000}          # ab 요청 수
CONCS=${CONCS:-"10 100 1000"}
P_OURS=18501
P_TINY=18502

command -v ab >/dev/null || { echo "ab 가 필요함: sudo apt install apache2-utils" >&2; exit 1; }
make -s webserver
HAVE_TINY=0; [ -x classic/tiny ] && HAVE_TINY=1

W=$(mktemp -d /tmp/bench.XXXXXX)
PID=""
stop() {
    if [ -n "$PID" ]; then kill "$PID" 2>/dev/null || true; wait "$PID" 2>/dev/null || true; fi
    PID=""
}
trap 'stop; rm -rf "$W"' EXIT

# 같은 파일을 두 서버가 같은 폴더에서 준다. Tiny 는 현재 폴더 기준으로 파일을 찾음
yes 'jungle webserver benchmark' | head -c 4096 > "$W/bench.html" || true   # yes 는 SIGPIPE 로 끝남
head -c 20000000 /dev/zero > "$W/big.bin"
[ "$HAVE_TINY" = 1 ] && cp classic/tiny "$W/"
BIN=$(pwd)/webserver

start() {   # start ours|tiny
    stop
    if [ "$1" = ours ]; then
        "$BIN" -p "$P_OURS" -r "$W" 2>>"$W/ours.log" & PID=$!; PORT=$P_OURS
    else
        # Tiny 는 요청마다 stdout 에 헤더를 찍음 → 같은 조건이 되도록 /dev/null
        (cd "$W" && exec ./tiny "$P_TINY" >/dev/null 2>>"$W/tiny.log") & PID=$!; PORT=$P_TINY
    fi
    for _ in $(seq 50); do
        curl -s -o /dev/null "http://127.0.0.1:$PORT/bench.html" && return 0
        sleep 0.1
    done
    echo "$1 가 뜨지 않음" >&2; exit 1
}

# 서버가 살아 있고 정상 요청에 200 을 주는지: "200 / 살아 있음" 또는 "응답 없음 / 종료(상태 N)"
check_alive() {
    local code st
    code=$(curl -s -m 2 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/bench.html" || true)
    if kill -0 "$PID" 2>/dev/null; then echo "$code / 살아 있음"; return; fi
    st=0; wait "$PID" 2>/dev/null || st=$?; PID=""
    echo "응답 없음 / 종료(상태 $st)"
}

median() { sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'; }

# ab 한 조건을 REPS 번 돌려 중앙값 한 줄: rps, p50(ms), p99(ms), 실패, non-2xx
run_ab() {   # run_ab 이름 동시성 [ab 추가 옵션]
    local name=$1 c=$2; shift 2
    local rps=() p50=() p99=() fail=0 non2xx=0 out
    for _ in $(seq "$REPS"); do
        out=$(ab -r -q -s 30 -n "$N" -c "$c" "$@" "http://127.0.0.1:$PORT/bench.html" 2>&1) || true
        rps+=("$(awk '/^Requests per second/ {print $4}' <<<"$out")")
        p50+=("$(awk '$1=="50%" {print $2}' <<<"$out")")
        p99+=("$(awk '$1=="99%" {print $2}' <<<"$out")")
        fail=$((fail + $(awk '/^Failed requests/ {print $3}' <<<"$out" | grep . || echo "$N")))
        non2xx=$((non2xx + $(awk '/^Non-2xx responses/ {print $3}' <<<"$out" | grep . || echo 0)))
    done
    printf '| %s | %s | %s | %s | %s | %s / %s / %s |\n' "$name" "$c" \
        "$(printf '%s\n' "${rps[@]}" | median)" "$(printf '%s\n' "${p50[@]}" | median)" \
        "$(printf '%s\n' "${p99[@]}" | median)" "$fail" "$non2xx" "$((N * REPS))"
}

echo "## 환경"
echo "- $(uname -sr), $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //'), 논리 코어 $(nproc)"
echo "- $(cc --version | head -1), $(ab -V | head -1)"
echo "- REPS=$REPS N=$N, 파일 bench.html 4096 바이트"
echo

echo "## 응답 본문 일치"
for s in ours tiny; do
    [ "$s" = tiny ] && [ "$HAVE_TINY" = 0 ] && continue
    start "$s"
    if curl -s "http://127.0.0.1:$PORT/bench.html" | cmp -s - "$W/bench.html"; then echo "- $s: 일치"; else echo "- $s: 불일치"; exit 1; fi
done
stop
echo

echo "## 처리량 (ab, 중앙값)"
echo "| 서버 | 동시성 | 요청/초 | p50 ms | p99 ms | 실패 / non-2xx / 전체 |"
echo "|---|---|---|---|---|---|"
for c in $CONCS; do
    start ours; run_ab "우리 (연결 재사용 없음)" "$c"
    start ours; run_ab "우리 (keep-alive, -k)" "$c" -k
    if [ "$HAVE_TINY" = 1 ]; then start tiny; run_ab "Tiny (연결 재사용 없음)" "$c"; fi
done
stop
echo

echo "## 느린 연결이 있을 때 다른 요청 (아무것도 안 보내는 연결 1개 + curl -m 5)"
echo "| 서버 | curl 결과 (상태, 걸린 초) | 그 뒤 서버 |"
echo "|---|---|---|"
for s in ours tiny; do
    [ "$s" = tiny ] && [ "$HAVE_TINY" = 0 ] && continue
    start "$s"
    exec 3<>"/dev/tcp/127.0.0.1/$PORT"
    sleep 0.2
    r=$(curl -s -o /dev/null -m 5 -w '%{http_code}, %{time_total}' "http://127.0.0.1:$PORT/bench.html" || true)
    exec 3>&-
    sleep 0.5
    echo "| $s | $r | $(check_alive) |"
done
stop
echo

echo "## 잘못된 입력·연결 중단 뒤 생존 (경우마다 새로 띄움)"
echo "| 경우 | 우리 | Tiny |"
echo "|---|---|---|"
case_garbage() { exec 3<>"/dev/tcp/127.0.0.1/$PORT"; printf 'HELLO\r\n\r\n' >&3; timeout 2 cat <&3 >/dev/null || true; exec 3>&-; }
case_long()    { curl -s -o /dev/null -m 5 "http://127.0.0.1:$PORT/$(head -c 9000 /dev/zero | tr '\0' a)" || true; }
case_hangup()  { curl -s -o /dev/null --max-time 0.3 --limit-rate 200k "http://127.0.0.1:$PORT/big.bin" || true; }
for k in garbage long hangup; do
    case $k in
        garbage) label="요청 줄이 HTTP 가 아님 (HELLO)";;
        long)    label="대상 9000 바이트 GET";;
        hangup)  label="20MB 받는 중 0.3초에 끊음";;
    esac
    row="| $label"
    for s in ours tiny; do
        if [ "$s" = tiny ] && [ "$HAVE_TINY" = 0 ]; then row="$row | (Tiny 없음)"; continue; fi
        start "$s"; "case_$k"; sleep 0.5
        row="$row | $(check_alive)"
    done
    echo "$row |"
done
