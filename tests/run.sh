#!/bin/sh
# 스모크 테스트: 서버를 백그라운드로 띄우고 curl/nc 로 요청·응답·종료를 확인한다.
#
# - POSIX sh 만 쓴다 (Ubuntu dash, Alpine busybox, macOS sh). macOS 에는 timeout 명령이 없어서
#   제한 시간은 wait_exit 의 감시 프로세스로 직접 잰다.
# - 서버 준비는 sleep 어림잡기가 아니라 로그에 listening 이 찍히고 curl 이 성공할 때까지 기다린다.
#   curl 만 보면 포트를 다른 서버가 쓰고 있을 때 그 서버의 응답을 준비 완료로 착각한다.
# - 서버를 기다리는 함수는 $(...) 안에서 부르지 않는다. 서브셸은 서버의 부모가 아니라서
#   wait 로 회수하지 못하고, 끝난 서버가 좀비로 남아 kill -0 이 계속 성공한다 ("안 꺼짐" 오판).
set -u

BIN=${BIN:-./webserver}
HOST=127.0.0.1
PORT=${PORT:-$((20000 + $$ % 20000))}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/webserver-test.XXXXXX") || exit 1
DOCROOT=$TMP/www
LOG=$TMP/server.log
URL=http://$HOST:$PORT/
mkdir -p "$DOCROOT"

fail=0
SERVER_PID=
BG_PIDS=
STALL_N=0
EXIT_CODE=

cleanup() {
    for p in $SERVER_PID $BG_PIDS; do
        kill -9 "$p" 2>/dev/null
    done
    rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

ok()  { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }

# pid 가 limit 초 안에 끝나기를 기다린다. 끝나면 0 과 EXIT_CODE, 시간을 넘기면 강제 종료 후 1.
# 감시 프로세스가 0.1초씩 세다가 시간이 되면 kill -9 한다 (macOS 에 timeout 이 없어서)
wait_exit() {
    _pid=$1 _ticks=$(($2 * 10))
    (
        i=0
        while [ $i -lt "$_ticks" ]; do sleep 0.1; i=$((i + 1)); done
        kill -9 "$_pid"
    ) >/dev/null 2>&1 &
    _wd=$!
    wait "$_pid"
    EXIT_CODE=$?
    kill "$_wd" 2>/dev/null
    wait "$_wd" 2>/dev/null
    [ "$EXIT_CODE" -ne 137 ]   # 137 = 128 + SIGKILL(9): 감시 프로세스가 죽임
}

# 서버를 백그라운드로 띄우고 응답할 때까지 최대 5초 기다린다.
# - 우리 서버가 listen 에 성공했다는 로그가 나온 뒤에만 curl 을 보낸다. 시작에 실패했으면
#   (포트 충돌 등) 바로 실패한다
# - 시간은 반복 횟수가 아니라 감시 프로세스로 잰다. 횟수로 세면 연결은 받는데 응답이 없는
#   서버에서 curl 의 1초가 매번 더해진다. 4초에 마감 파일을 만들고, 마감 직전에 시작한
#   curl 도 -m 1 로 끝나므로 전체는 5초를 넘지 않는다
start_server() {
    "$BIN" -p "$PORT" -r "$DOCROOT" "$@" >"$LOG" 2>&1 &
    SERVER_PID=$!
    _dl=$TMP/ready.deadline
    rm -f "$_dl"
    (
        trap 'kill $_s 2>/dev/null; exit 0' TERM   # 끝낼 때 sleep 도 같이 (남지 않게)
        sleep 4 &
        _s=$!
        wait $_s
        : >"$_dl"
    ) &
    _wd=$!
    _rc=1
    while [ ! -e "$_dl" ]; do
        grep -q '^webserver: cannot' "$LOG" && break
        if grep -q '^webserver: listening' "$LOG" &&
           curl -s -o /dev/null -m 1 "$URL"; then
            _rc=0
            break
        fi
        sleep 0.1
    done
    kill "$_wd" 2>/dev/null
    wait "$_wd" 2>/dev/null
    return $_rc
}

# 시그널을 보내고 5초 안에 exit 0 하는지
stop_server() {
    sig=$1
    kill "-$sig" "$SERVER_PID"
    if wait_exit "$SERVER_PID" 5 && [ "$EXIT_CODE" -eq 0 ]; then
        ok "SIG$sig -> exit 0 within 5s"
    else
        bad "SIG$sig -> exit 0 within 5s (exit $EXIT_CODE)"
        cat "$LOG"
    fi
    SERVER_PID=
}

# 서버가 시작하지 않고 바로 exit 1 해야 하는 경우
expect_exit1() {
    desc=$1
    shift
    "$BIN" "$@" >/dev/null 2>&1 &
    if wait_exit $! 3 && [ "$EXIT_CODE" -eq 1 ]; then
        ok "$desc"
    else
        bad "$desc (exit $EXIT_CODE)"
    fi
}

# 응답 코드만 출력 (연결 실패면 000)
http_code() {
    curl -s -o /dev/null -w '%{http_code}' -m 3 "$@"
}

# 아무것도 보내지 않거나(인자 없음) 요청을 반만 보내고 멈춘 연결을 열어 둔다.
# - 파이프(a | nc &)는 $! 가 마지막 프로세스뿐이라 앞쪽 sleep 이 남는다. FIFO 로 나눠
#   보내는 쪽과 nc 쪽의 PID 를 둘 다 기억한다
# - nc 가 끝나면(연결 실패, nc 없음, 서버가 닫음) $TMP/stall.N.closed 를 만든다.
#   check_stalled_open 이 이것으로 연결이 정말 열려 있는지 본다
open_stalled_conn() {
    STALL_N=$((STALL_N + 1))
    _f=$TMP/stall.$STALL_N
    mkfifo "$_f" || return 1
    ( nc "$HOST" "$PORT" <"$_f" >/dev/null 2>&1; : >"$_f.closed" ) &
    BG_PIDS="$BG_PIDS $!"
    ( printf '%b' "${1:-}"; exec sleep 5 ) >"$_f" &
    BG_PIDS="$BG_PIDS $!"
}

# 지금까지 연 느린 연결이 모두 열려 있는지 (없으면 그 테스트는 아무것도 검사하지 않은 것)
check_stalled_open() {
    for _c in "$TMP"/stall.*.closed; do
        [ -e "$_c" ] || continue
        bad "$1: stalled connection was not held open (${_c##*/})"
        return 1
    done
    return 0
}

close_stalled_conns() {
    for p in $BG_PIDS; do
        kill "$p" 2>/dev/null
    done
    BG_PIDS=
    rm -f "$TMP"/stall.*
}

echo "== smoke test ($(uname -s))"

[ -x "$BIN" ] && ok "binary exists" || { bad "binary exists"; exit 1; }
for tool in curl nc mkfifo; do
    command -v "$tool" >/dev/null 2>&1 || { bad "$tool is required"; exit 1; }
done

expect_exit1 "no args -> exit 1"
expect_exit1 "missing -r -> exit 1"          -p "$PORT"
expect_exit1 "missing -p -> exit 1"          -r "$DOCROOT"
expect_exit1 "bad port -> exit 1"            -p 80x -r "$DOCROOT"
expect_exit1 "port 0 -> exit 1"              -p 0 -r "$DOCROOT"
expect_exit1 "leading zero port -> exit 1"   -p 08080 -r "$DOCROOT"
expect_exit1 "docroot missing -> exit 1"     -p "$PORT" -r "$TMP/nope"
: >"$TMP/afile"
expect_exit1 "docroot is a file -> exit 1"   -p "$PORT" -r "$TMP/afile"
expect_exit1 "extra argument -> exit 1"      -p "$PORT" -r "$DOCROOT" extra

: >"$LOG"
if start_server; then
    ok "server answers within 5s"
else
    bad "server answers within 5s"
    cat "$LOG"
    exit 1
fi

# 고정 응답 (#7 첫 PR. 파서·정적 파일이 붙으면 실제 응답 검사로 바꾼다)
code=$(curl -s -o "$TMP/body" -w '%{http_code}' -m 3 "$URL")
if [ "$code" = 200 ] && grep -q 'webserver: ok' "$TMP/body"; then
    ok "GET / -> 200"
else
    bad "GET / -> 200 (got $code)"
fi

# IPv6 소켓으로 열렸으면 ::1 로도 받아야 한다. 이 머신에 IPv6 루프백이 없으면(컨테이너 등)
# 연결 자체가 안 되므로, "거부(refused)"일 때만 실패로 본다
if grep -q 'IPv6' "$LOG"; then
    if out=$(curl -sS -o /dev/null -w '%{http_code}' -m 3 -g "http://[::1]:$PORT/" 2>&1); then
        [ "$out" = 200 ] && ok "GET via ::1 -> 200" || bad "GET via ::1 -> 200 (got $out)"
    else
        case $out in
            *refused*) bad "GET via ::1 -> 200 ($out)" ;;
            *)         echo "  skip GET via ::1 (no IPv6 loopback here)" ;;
        esac
    fi
else
    echo "  skip GET via ::1 (server is IPv4 only here)"
fi

expect_exit1 "port in use -> exit 1" -p "$PORT" -r "$DOCROOT"

# 느린 클라이언트 격리: 아무것도 안 보내는 연결 + 헤더를 반만 보낸 연결이 있어도 다른 요청은 바로 처리
# (Tiny 는 한 번에 연결 하나라서 여기서 멈춘다)
open_stalled_conn
open_stalled_conn 'GET / HTTP/1.1\r\nHost: x\r\n'
sleep 0.3
if check_stalled_open "stalled connections do not block others"; then
    code=$(curl -s -o /dev/null -w '%{http_code}' -m 2 "$URL")
    [ "$code" = 200 ] && ok "stalled connections do not block others" \
                      || bad "stalled connections do not block others (got $code)"
fi
close_stalled_conns

# 동시 요청 50개
pids=
i=0
while [ $i -lt 50 ]; do
    { http_code "$URL"; echo; } >"$TMP/c.$i" 2>/dev/null &
    pids="$pids $!"
    i=$((i + 1))
done
wait $pids
n=$(cat "$TMP"/c.* | grep -c '^200$')
[ "$n" -eq 50 ] && ok "50 concurrent requests -> 200" || bad "50 concurrent requests -> 200 (only $n)"

# 처음부터 끊긴 클라이언트가 있어도 서버는 계속 돈다
( printf 'GET / HTTP/1.1\r\n\r\n' ) | nc "$HOST" "$PORT" >/dev/null 2>&1
[ "$(http_code "$URL")" = 200 ] && ok "server survives client that hangs up" \
                                || bad "server survives client that hangs up"

stop_server TERM

# 열린 연결이 있는 상태에서 SIGINT: 연결까지 정리하고 끝나야 한다
: >"$LOG"
if start_server; then
    open_stalled_conn 'GET / HTTP/1.1\r\n'
    sleep 0.3
    check_stalled_open "SIGINT with open connection"
    stop_server INT
    close_stalled_conns
else
    bad "server restarts on the same port"
    cat "$LOG"
fi

exit $fail
