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
mkdir -p "$DOCROOT/sub"
printf '<h1>smoke</h1>\n' >"$DOCROOT/index.html"
printf 'sub file\n' >"$DOCROOT/sub/a.txt"
head -c 1048576 /dev/urandom >"$DOCROOT/big.bin"   # 여러 POLLOUT 에 나눠 보내지는 크기
printf 'secret\n' >"$TMP/secret.txt"
ln -s ../secret.txt "$DOCROOT/link.txt"            # docroot 밖을 가리키는 링크
cp -R "${WWW:-www}" "$DOCROOT/site" || exit 1      # 저장소 www/ 예제 (html, css, png)

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
for tool in curl nc mkfifo cmp; do
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

# 정적 파일 (docroot/index.html)
code=$(curl -s -o "$TMP/body" -w '%{http_code}' -m 3 "$URL")
if [ "$code" = 200 ] && grep -q '<h1>smoke</h1>' "$TMP/body"; then
    ok "GET / -> 200 index.html"
else
    bad "GET / -> 200 index.html (got $code)"
fi

code=$(curl -s -o "$TMP/big" -w '%{http_code}' -m 5 "${URL}big.bin")
[ "$code" = 200 ] && cmp -s "$TMP/big" "$DOCROOT/big.bin" && ok "GET 1MB file -> identical bytes" \
                                                          || bad "GET 1MB file -> identical bytes (got $code)"

# www/ 예제 파일 (#6): 상태, Content-Type, Content-Length 가 원본 크기, 본문 바이트가 원본과 같음
expect_file() {
    path=$1 type=$2 src=$DOCROOT/site/$1
    size=$(wc -c <"$src" | tr -d ' ')
    code=$(curl -s -D "$TMP/fh" -o "$TMP/fb" -w '%{http_code}' -m 3 "${URL}site/$path")
    tr -d '\r' <"$TMP/fh" >"$TMP/fh.lf"
    if [ "$code" = 200 ] && grep -Fqx "Content-Type: $type" "$TMP/fh.lf" \
       && grep -Fqx "Content-Length: $size" "$TMP/fh.lf" && cmp -s "$TMP/fb" "$src"; then
        ok "GET $path -> 200 $type, identical bytes"
    else
        bad "GET $path -> 200 $type, identical bytes (got $code)"
        cat "$TMP/fh.lf"
    fi
}
expect_file index.html   'text/html; charset=utf-8'
expect_file style.css    'text/css; charset=utf-8'
expect_file img/logo.png 'image/png'

# 상태 코드: 기대값 설명 curl인자...
expect_code() {
    want=$1 desc=$2
    shift 2
    got=$(http_code "$@")
    [ "$got" = "$want" ] && ok "$desc -> $want" || bad "$desc -> $want (got $got)"
}
expect_code 404 "GET missing file"        "${URL}nope.html"
expect_code 403 "GET symlink to outside docroot" "${URL}link.txt"
# .. 조각: 지금은 static_open 이 403, PR #27 머지 후에는 파서가 400. 어느 쪽이든 막혔으면 통과
got=$(http_code --path-as-is "${URL}../etc/passwd")
case $got in
    403|400) ok "GET ../ outside docroot -> blocked ($got)" ;;
    *)       bad "GET ../ outside docroot -> 403 or 400 (got $got)" ;;
esac
expect_code 405 "POST static file"        -d x "$URL"
expect_code 431 "9KB header"              -H "X-Big: $(head -c 9000 /dev/zero | tr '\0' a)" "$URL"
head -c 1100000 /dev/zero >"$TMP/huge"
expect_code 413 "1.1MB body"              -H 'Expect:' --data-binary @"$TMP/huge" "$URL"

# HEAD: 헤더는 GET 과 같고(Content-Length 포함) 본문은 없다.
# curl -X HEAD 는 Content-Length 만큼 본문을 기다려 멈추므로 원시 요청으로 본다
( printf 'HEAD / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'; sleep 1 ) |
    nc "$HOST" "$PORT" 2>/dev/null | tr -d '\r' >"$TMP/head"
if grep -q '^HTTP/1.1 200' "$TMP/head" && grep -qi '^Content-Length: 15$' "$TMP/head" &&
   ! grep -q 'smoke' "$TMP/head"; then
    ok "HEAD -> Content-Length, no body"
else
    bad "HEAD -> Content-Length, no body"
fi

# keep-alive: curl 한 번에 URL 두 개 → 같은 연결을 다시 쓴다.
# num_connects 는 그 요청에서 새로 연 연결 수 (재사용이면 0). -v 문구는 curl 버전마다 달라서 쓰지 않는다
conns=$(curl -s -o /dev/null -o /dev/null -w '%{num_connects} ' -m 3 "$URL" "${URL}sub/a.txt")
[ "$conns" = "1 0 " ] && ok "keep-alive: second request reuses the connection" \
                      || bad "keep-alive: second request reuses the connection (new connects: $conns)"

# 연결 유지 기본값: HTTP/1.1 은 keep-alive, HTTP/1.0 은 close (요청에 Connection 이 없을 때)
h11=$(curl -s -D - -o /dev/null -m 3 "$URL" | tr -d '\r' | grep -i '^Connection:')
h10=$(curl -0 -s -D - -o /dev/null -m 3 "$URL" | tr -d '\r' | grep -i '^Connection:')
[ "$h11" = "Connection: keep-alive" ] && [ "$h10" = "Connection: close" ] \
    && ok "default Connection: 1.1 keep-alive, 1.0 close" \
    || bad "default Connection: 1.1 keep-alive, 1.0 close (got '$h11' / '$h10')"

# 파이프라이닝: 응답을 기다리지 않고 요청 세 개를 한 번에 보내면 순서대로 세 응답
( printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /sub/a.txt HTTP/1.1\r\nHost: x\r\n\r\nGET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'
  sleep 1 ) | nc "$HOST" "$PORT" 2>/dev/null | tr -d '\r' | grep '^HTTP/1.1' >"$TMP/pipe"
if [ "$(tr '\n' ' ' <"$TMP/pipe")" = "HTTP/1.1 200 OK HTTP/1.1 200 OK HTTP/1.1 404 Not Found " ]; then
    ok "pipelined requests -> 200, 200, 404 in order"
else
    bad "pipelined requests -> 200, 200, 404 in order (got: $(tr '\n' ' ' <"$TMP/pipe"))"
fi

# 파이프라이닝 (반쪽): 첫 응답 뒤 in 에 남은 다음 요청의 앞부분이 버려지지 않고,
# 나중에 온 나머지와 합쳐져 처리되는지 (server.c conn_response_done).
# 남은 바이트를 "바로" 처리하는지는 위의 한꺼번에 보내는 테스트가 본다 (이 경우는 나머지가
# 도착하면 poll 이 깨워 주므로 바로 처리하지 않아도 통과한다)
( printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /sub/a.txt HT'
  sleep 0.5
  printf 'TP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'
  sleep 1 ) | nc "$HOST" "$PORT" 2>/dev/null | tr -d '\r' | grep -E '^HTTP/1.1|^sub file' >"$TMP/pipe2"
if [ "$(tr '\n' ' ' <"$TMP/pipe2")" = "HTTP/1.1 200 OK HTTP/1.1 200 OK sub file " ]; then
    ok "pipelined request split across reads -> 200, 200"
else
    bad "pipelined request split across reads -> 200, 200 (got: $(tr '\n' ' ' <"$TMP/pipe2"))"
fi

# 깨진 요청 -> 400
( printf 'GARBAGE\r\n\r\n'; sleep 1 ) | nc "$HOST" "$PORT" 2>/dev/null | head -1 | tr -d '\r' >"$TMP/bad"
grep -q '^HTTP/1.1 400' "$TMP/bad" && ok "malformed request -> 400" || bad "malformed request -> 400 (got $(cat "$TMP/bad"))"

# 파서 오류도 HEAD 면 본문 없이 (Transfer-Encoding 은 501)
( printf 'HEAD / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n'; sleep 1 ) |
    nc "$HOST" "$PORT" 2>/dev/null | tr -d '\r' >"$TMP/head501"
if grep -q '^HTTP/1.1 501' "$TMP/head501" && ! grep -q '<html>' "$TMP/head501"; then
    ok "HEAD with parse error -> 501, no body"
else
    bad "HEAD with parse error -> 501, no body (got: $(head -1 "$TMP/head501"))"
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
