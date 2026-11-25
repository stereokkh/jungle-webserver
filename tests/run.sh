#!/bin/sh
# 스모크 테스트: 서버를 백그라운드로 띄우고 curl/nc 로 요청·응답·종료를 확인한다.
#
# - POSIX sh 만 쓴다 (Ubuntu dash, Alpine busybox, macOS sh). macOS 에는 timeout 명령이 없어서
#   제한 시간은 wait_exit 의 감시 프로세스로 직접 잰다.
# - 서버 준비는 sleep 어림잡기가 아니라 로그에 listening 이 찍히고 curl 이 성공할 때까지 기다린다.
#   curl 만 보면 포트를 다른 서버가 쓰고 있을 때 그 서버의 응답을 준비 완료로 착각한다.
# - 서버를 기다리는 함수는 $(...) 안에서 부르지 않는다. 서브셸은 서버의 부모가 아니라서
#   wait 로 회수하지 못하고, 끝난 서버가 좀비로 남아 kill -0 이 계속 성공한다 ("안 꺼짐" 오판).
# - 프록시 검사(맨 아래)는 실제 백엔드 둘(같은 바이너리)과 가짜 백엔드(FAKE, tests/tools/fake_upstream.c)를
#   띄운다. 깨진 응답·일찍 끊김·무응답은 실제 백엔드로 만들 수 없어서 가짜가 필요하다.
set -u

BIN=${BIN:-./webserver}
FAKE=${FAKE:-./build/tools/fake_upstream}
HOST=127.0.0.1
PORT=${PORT:-$((20000 + $$ % 12000))}   # 최대 31999+20 (PORT..PORT+20 를 씀). 리눅스 ephemeral 범위(32768~60999) 아래
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

fail=0
SERVER_PID=
BG_PIDS=
UP_PIDS=       # 프록시 검사용 실제 백엔드·가짜 백엔드. 어떻게 끝나든 cleanup 이 죽인다
STALL_N=0
EXIT_CODE=

cleanup() {
    for p in $SERVER_PID $BG_PIDS $UP_PIDS; do
        kill -9 "$p" 2>/dev/null
    done
    rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
cp -R "${WWW:-www}" "$DOCROOT/site" || exit 1      # 저장소 www/ 예제 (html, css, png)

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

# --- 프록시 검사 도우미 (이슈 #9) ---

# 실제 백엔드(같은 바이너리, -c 없음)를 띄우고 응답할 때까지 최대 5초 기다린다: start_backend <port> <docroot>
start_backend() {
    _bp=$1 _bl=$TMP/backend.$1.log
    "$BIN" -p "$_bp" -r "$2" >"$_bl" 2>&1 &
    UP_PIDS="$UP_PIDS $!"
    _end=$(( $(date +%s) + 5 ))        # 반복 횟수가 아니라 시계로 5초 (curl -m 1 이 반복마다 더해지지 않게)
    _n=0                               # 시계가 뒤로 가도 끝나게 반복 횟수 상한도 둔다
    while [ "$(date +%s)" -lt "$_end" ] && [ $_n -lt 50 ]; do
        _n=$((_n + 1))
        grep -q '^webserver: cannot' "$_bl" && break
        if grep -q '^webserver: listening' "$_bl" &&
           curl -s -o /dev/null -m 1 "http://$HOST:$_bp/"; then
            return 0
        fi
        sleep 0.1
    done
    cat "$_bl"
    return 1
}

# 가짜 백엔드를 띄우고 "ready" 가 찍힐 때까지 최대 5초 기다린다: start_fake <port> <mode> [요청 기록 파일]
# 출력은 $TMP/fake.<port>.out (hold 는 여기에 peer-closed 도 찍는다). 기록 파일을 주면 받은 요청마다 한 줄이 붙는다
start_fake() {
    _fo=$TMP/fake.$1.out
    "$FAKE" "$1" "$2" ${3:+"$3"} >"$_fo" 2>&1 &
    UP_PIDS="$UP_PIDS $!"
    _i=0
    while [ $_i -lt 50 ]; do
        grep -q '^ready' "$_fo" && return 0
        sleep 0.1
        _i=$((_i + 1))
    done
    cat "$_fo"
    return 1
}

# 프런트가 살아 있고 중계가 되는지. 502·504 검사마다 짝으로 붙인다: 실패 응답만 확인하면 "프록시 경로는
# 전부 502" 인 서버(develop 의 스텁)도 통과하기 때문이다. 같은 프런트가 성공 응답도 낸다는 것을 함께 본다
proxy_alive() {
    [ "$(curl -s -m 3 "${URL}api/add?a=1&b=2")" = 3 ]
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
expect_exit1 "unknown long option -> exit 1" -p "$PORT" -r "$DOCROOT" --merge

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
expect_code 501 "unknown method BREW"     -X BREW "$URL"
expect_code 414 "3000-byte target"        "${URL}$(head -c 3000 /dev/zero | tr '\0' a)"

# 내장 앱 (#10): 계산, 400, 405, echo 바이트 그대로, 모든 응답에 X-Instance
body=$(curl -s -m 3 "${URL}api/add?a=1&b=2")
[ "$body" = 3 ] && ok "GET /api/add?a=1&b=2 -> 3" || bad "GET /api/add?a=1&b=2 -> 3 (got '$body')"
expect_code 400 "GET /api/add overflow"   "${URL}api/add?a=9223372036854775807&b=1"
expect_code 405 "GET /api/echo"           "${URL}api/echo"
printf 'a\000b\r\nc' >"$TMP/echo_in"
curl -s -m 3 -H 'Expect:' --data-binary @"$TMP/echo_in" -o "$TMP/echo_out" "${URL}api/echo"
cmp -s "$TMP/echo_in" "$TMP/echo_out" && ok "POST /api/echo -> identical bytes (NUL, CRLF)" \
                                      || bad "POST /api/echo -> identical bytes (NUL, CRLF)"
curl -s -m 3 -D "$TMP/app_head" -o /dev/null "${URL}api/nope"
tr -d '\r' <"$TMP/app_head" | grep -q "^X-Instance: $PORT\$" && ok "404 under /api/ -> X-Instance: $PORT" \
                                                     || bad "404 under /api/ -> X-Instance: $PORT"

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

# keep-alive 정적 파일이 요청마다 멈추지 않는지 (TCP_NODELAY). 끄면 2번째부터 약 44ms 씩이라
# 이어 보낸 9개 합이 0.4초쯤 된다. 느린 CI 를 생각해 0.2초를 기준으로 둔다 (켜면 1ms 안팎)
set --
for _i in 1 2 3 4 5 6 7 8 9 10; do set -- "$@" -o /dev/null "$URL"; done
times=$(curl -s -m 5 -w '%{num_connects}:%{time_total} ' "$@")
if echo "$times" | awk '{ s = 0; n = 0
        for (i = 2; i <= NF; i++) { split($i, a, ":"); if (a[1] != 0) exit 1; s += a[2]; n++ }
        exit !(n == 9 && s < 0.2) }'; then
    ok "keep-alive static: no per-request stall"
else
    bad "keep-alive static: no per-request stall (connects:seconds = $times)"
fi

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

# 모양은 맞는데 모르는 버전 -> 505 (curl 은 HTTP/9.9 를 보낼 수 없어 원시 요청)
( printf 'GET / HTTP/9.9\r\nHost: x\r\n\r\n'; sleep 1 ) | nc "$HOST" "$PORT" 2>/dev/null | head -1 | tr -d '\r' >"$TMP/ver"
grep -q '^HTTP/1.1 505' "$TMP/ver" && ok "GET / HTTP/9.9 -> 505" || bad "GET / HTTP/9.9 -> 505 (got $(cat "$TMP/ver"))"

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

# ---------------------------------------------------------------
# 프록시 (이슈 #9): -c 로 라우트를 읽은 프런트 하나, 실제 백엔드 둘, 가짜 백엔드 여섯.
#   포트: 프런트 PORT, 백엔드 PORT+1·+2, 가짜 PORT+3..+8, 아무도 안 듣는 포트 PORT+9
#   라우트 (접두사는 떼지 않고 백엔드로 간다. 백엔드는 같은 바이너리라 /api/... 를 스스로 처리한다):
#     /api    → 백엔드 1             (앱: add, echo)
#     /files  → 백엔드 1             (정적 파일. 6 MB 파일)
#     /rr     → 백엔드 1, 2          (Round-Robin. 두 백엔드의 rr/id 파일 내용이 "1" 과 "2")
#     /down   → 듣는 이 없는 포트    (연결 거절 → 502)
#     /f/<모드> → 가짜 백엔드
#   502·504 검사에는 proxy_alive (같은 프런트의 성공 응답)를 짝으로 붙인다.
#   시간이 오래 걸리는 검사는 504 하나(약 5초)만 넣는다. 느린 응답(drip)은 아래 주석의 방법으로 손으로 본다.
# ---------------------------------------------------------------
proxy_tests() {
    B1=$((PORT + 1)) B2=$((PORT + 2))
    DOWN=$((PORT + 9))
    mkdir -p "$TMP/b1/files" "$TMP/b1/rr" "$TMP/b2/rr"
    printf 'backend 1\n' >"$TMP/b1/index.html"
    printf 'backend 2\n' >"$TMP/b2/index.html"
    printf '1\n' >"$TMP/b1/rr/id"
    printf '2\n' >"$TMP/b2/rr/id"
    head -c 6291456 /dev/urandom >"$TMP/b1/files/big6.bin"   # BUFFER_MAX(4 MiB)보다 크다: 모아서 보내면 실패한다

    if [ ! -x "$FAKE" ]; then
        bad "fake_upstream exists ($FAKE)"
        return
    fi
    start_backend "$B1" "$TMP/b1" && start_backend "$B2" "$TMP/b2" || { bad "backends start"; return; }
    n=3
    for m in split early-eof partial-head garbage hold silent; do
        start_fake $((PORT + n)) "$m" || { bad "fake $m starts"; return; }
        n=$((n + 1))
    done
    cat >"$TMP/proxy.conf" <<EOF
/api       $HOST:$B1
/files     $HOST:$B1
/rr        $HOST:$B1 $HOST:$B2
/api/rr    $HOST:$B1 $HOST:$B2
/down      $HOST:$DOWN
/f/split   $HOST:$((PORT + 3))
/f/early   $HOST:$((PORT + 4))
/f/partial $HOST:$((PORT + 5))
/f/garbage $HOST:$((PORT + 6))
/f/hold    $HOST:$((PORT + 7))
/f/silent  $HOST:$((PORT + 8))
EOF

    : >"$LOG"
    if ! start_server -c "$TMP/proxy.conf"; then
        bad "proxy front starts"
        cat "$LOG"
        return
    fi

    # 1. GET: 상태, 본문, 어느 백엔드가 답했는지, 클라이언트 연결은 닫는다 (백엔드에 Connection: close 를 보냈으므로)
    code=$(curl -s -D "$TMP/p.head" -o "$TMP/p.body" -w '%{http_code}' -m 3 "${URL}api/add?a=1&b=2")
    tr -d '\r' <"$TMP/p.head" >"$TMP/p.head.lf"
    if [ "$code" = 200 ] && [ "$(cat "$TMP/p.body")" = 3 ] &&
       grep -qi "^X-Instance: $B1\$" "$TMP/p.head.lf" && grep -qi '^Connection: close$' "$TMP/p.head.lf"; then
        ok "proxy GET -> 200, body 3, X-Instance $B1, Connection: close"
    else
        bad "proxy GET -> 200, body 3, X-Instance $B1, Connection: close (got $code, $(tr '\n' '|' <"$TMP/p.head.lf"))"
    fi

    # 2. POST 100 KB: 요청 본문이 백엔드까지 가고 응답이 그대로 돌아온다
    head -c 102400 /dev/urandom >"$TMP/echo.in"
    code=$(curl -s -H 'Expect:' -H 'Content-Type: application/octet-stream' --data-binary @"$TMP/echo.in" \
           -o "$TMP/echo.out" -w '%{http_code}' -m 5 "${URL}api/echo")
    if [ "$code" = 200 ] && cmp -s "$TMP/echo.in" "$TMP/echo.out"; then
        ok "proxy POST 100 KB echo -> identical"
    else
        bad "proxy POST 100 KB echo -> identical (got $code)"
    fi

    # 3. HEAD: 응답 머리는 Content-Length 가 있어도 본문이 없다. 본문을 기다리지 않고 끝난다 (-m 3)
    curl -s -I -o /dev/null -D "$TMP/h.head" -m 3 "${URL}api/add?a=1&b=2"
    rc=$?
    ( printf 'HEAD /api/add?a=1&b=2 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'; sleep 1 ) |
        nc "$HOST" "$PORT" 2>/dev/null | tr -d '\r' >"$TMP/h.raw"
    if [ "$rc" -eq 0 ] && grep -q '^HTTP/1.1 200' "$TMP/h.head" &&
       grep -q '^HTTP/1.1 200' "$TMP/h.raw" && ! grep -qx 3 "$TMP/h.raw"; then
        ok "proxy HEAD -> 200, no body"
    else
        bad "proxy HEAD -> 200, no body (curl rc $rc, raw: $(tr '\n' '|' <"$TMP/h.raw"))"
    fi

    # 4. 6 MB 파일: 버퍼 상한(4 MiB)보다 크므로 중계가 응답을 한꺼번에 모으지 않고 흘려보내야 통과한다
    code=$(curl -s -o "$TMP/big6.out" -w '%{http_code}' -m 10 "${URL}files/big6.bin")
    if [ "$code" = 200 ] && cmp -s "$TMP/b1/files/big6.bin" "$TMP/big6.out"; then
        ok "proxy 6 MB file -> identical (streamed)"
    else
        bad "proxy 6 MB file -> identical (got $code)"
    fi

    # 5. Round-Robin: 두 백엔드에 번갈아 간다 (rr/id 내용이 1, 2). 시작이 어느 쪽인지는 따지지 않는다
    rrseq=
    for _n in 1 2 3 4; do
        rrseq=$rrseq$(curl -s -m 3 "${URL}rr/id" | tr -d '\n')
    done
    case $rrseq in
        1212|2121) ok "proxy Round-Robin alternates ($rrseq)" ;;
        *)         bad "proxy Round-Robin alternates (got '$rrseq')" ;;
    esac
    # 같은 확인을 X-Instance 로: 두 백엔드 모두 /api 아래 404 에 자기 포트를 단다 (정적 파일은 안 단다)
    xseq=
    for _n in 1 2 3 4; do
        xseq=$xseq$(curl -s -D - -o /dev/null -m 3 "${URL}api/rr/x" | tr -d '\r' |
                    sed -n 's/^[Xx]-[Ii]nstance: //p')-
    done
    case $xseq in
        "$B1-$B2-$B1-$B2-"|"$B2-$B1-$B2-$B1-") ok "proxy Round-Robin X-Instance alternates ($xseq)" ;;
        *) bad "proxy Round-Robin X-Instance alternates (got '$xseq')" ;;
    esac

    # 6. 백엔드가 없으면 502
    code=$(http_code "${URL}down/x")
    if [ "$code" = 502 ] && proxy_alive; then
        ok "backend down -> 502"
    else
        bad "backend down -> 502 (got $code)"
    fi

    # 7. 431: 프런트 한도(HTTP_MAX_HEADER 8192, 빈 줄까지)에는 들어오지만 다시 쓴 머리(Via, X-Forwarded-For,
    #    Connection: close 가 붙어 약 74바이트 길어진다)가 한도를 넘는 요청. 요청 크기는 curl 이 붙이는
    #    User-Agent 등에 좌우되지 않게 nc 로 정확히 보낸다 (8185바이트). 정적 경로로 보낸 같은 요청이 200 이어야
    #    프런트 자체의 한도에 걸린 게 아니라는 뜻이다
    pad=$(head -c 8120 /dev/zero | tr '\0' a)
    ( printf 'GET / HTTP/1.1\r\nHost: %s:%s\r\nX-Pad: %s\r\n\r\n' "$HOST" "$PORT" "$pad"; sleep 1 ) |
        nc "$HOST" "$PORT" 2>/dev/null | head -1 | tr -d '\r' >"$TMP/p431.ctl"
    ( printf 'GET /api/add?a=1&b=2 HTTP/1.1\r\nHost: %s:%s\r\nX-Pad: %s\r\n\r\n' "$HOST" "$PORT" "$pad"; sleep 1 ) |
        nc "$HOST" "$PORT" 2>/dev/null | head -1 | tr -d '\r' >"$TMP/p431"
    if grep -q '^HTTP/1.1 200' "$TMP/p431.ctl" && grep -q '^HTTP/1.1 431' "$TMP/p431" && proxy_alive; then
        ok "rewritten head over the limit -> 431"
    else
        bad "rewritten head over the limit -> 431 (static control: $(cat "$TMP/p431.ctl"), proxy: $(cat "$TMP/p431"))"
    fi

    # 8. 응답이 1바이트씩 나뉘어 와도 (머리가 여러 read 에 걸쳐 도착) 그대로
    code=$(curl -s -o "$TMP/split.out" -w '%{http_code}' -m 5 "${URL}f/split")
    rc=$?
    printf 'fake ok\n' >"$TMP/split.want"     # 끝 개행까지 비교한다 ($(...) 는 개행을 지운다)
    if [ "$rc" -eq 0 ] && [ "$code" = 200 ] && cmp -s "$TMP/split.want" "$TMP/split.out"; then
        ok "upstream response in 1-byte pieces -> 200, body intact"
    else
        bad "upstream response in 1-byte pieces -> 200, body intact (curl rc $rc, got $code, '$(cat "$TMP/split.out")')"
    fi

    # 9. 응답 시작(머리 전달) 뒤 업스트림이 일찍 끊기면 이미 받은 바이트까지만 주고 연결을 닫는다 (502 로 바꿀 수 없다)
    curl -s -D "$TMP/eof.head" -o "$TMP/eof.body" -m 3 "${URL}f/early"
    rc=$?
    size=$(wc -c <"$TMP/eof.body" | tr -d ' ')
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 28 ] && grep -q '^HTTP/1.1 200' "$TMP/eof.head" && [ "$size" = 40 ]; then
        ok "upstream cut mid-body -> head + 40 bytes, then connection closed (curl rc $rc)"
    else
        bad "upstream cut mid-body -> head + 40 bytes, then connection closed (curl rc $rc, $size bytes)"
    fi

    # 10, 11. 응답 시작 전 실패: 머리가 반만 오고 EOF, 머리가 HTTP 가 아님 → 502
    code=$(http_code "${URL}f/partial")
    if [ "$code" = 502 ] && proxy_alive; then
        ok "upstream EOF inside the head -> 502"
    else
        bad "upstream EOF inside the head -> 502 (got $code)"
    fi
    code=$(http_code "${URL}f/garbage")
    if [ "$code" = 502 ] && proxy_alive; then
        ok "upstream garbage head -> 502"
    else
        bad "upstream garbage head -> 502 (got $code)"
    fi

    # 12. 업스트림이 본문 10바이트를 보내고 연결을 열어 두어도, Content-Length 로 끝을 알고 5초 제한 한참 전에
    #     끝낸다. 프록시가 업스트림 연결을 닫았는지는 가짜가 peer-closed 로 알려 준다
    #     (유휴 만료 5초 뒤에 닫힌 것과 구분하려고, 시작 시각부터 잰 시간이 3초 이하여야 한다. 초 단위 시계라
    #     5초 만료는 4 이상으로 나온다)
    t0=$(date +%s)
    code=$(curl -s -o "$TMP/hold.out" -w '%{http_code}' -m 3 "${URL}f/hold")
    _i=0
    while [ $_i -lt 30 ] && ! grep -q '^peer-closed' "$TMP/fake.$((PORT + 7)).out"; do
        sleep 0.1
        _i=$((_i + 1))
    done
    elapsed=$(($(date +%s) - t0))
    if [ "$code" = 200 ] && [ "$(cat "$TMP/hold.out")" = 0123456789 ] && [ "$elapsed" -le 3 ] &&
       grep -q '^peer-closed' "$TMP/fake.$((PORT + 7)).out"; then
        ok "upstream holds the connection after Content-Length bytes -> done, upstream closed"
    else
        bad "upstream holds the connection after Content-Length bytes -> done, upstream closed (got $code)"
    fi

    # 13. 업스트림이 아무 말도 안 하면 UPSTREAM_TIMEOUT_MS(5초) 뒤 504. 시계는 초 단위라 4~7초를 허용한다
    t0=$(date +%s)
    code=$(curl -s -o /dev/null -w '%{http_code}' -m 8 "${URL}f/silent")
    elapsed=$(($(date +%s) - t0))
    if [ "$code" = 504 ] && [ "$elapsed" -ge 4 ] && [ "$elapsed" -le 7 ] && proxy_alive; then
        ok "silent upstream -> 504 after about 5s (${elapsed}s)"
    else
        bad "silent upstream -> 504 after about 5s (got $code after ${elapsed}s)"
    fi

    # 14. 클라이언트가 큰 응답을 받다가 포기해도 (느리게 받다가 1초 만에 끊음) 프런트는 멀쩡하다.
    #     업스트림 연결·버퍼가 남지 않았는지는 아래 SIGTERM 종료(asan 이면 누수 검사)가 같이 본다
    curl -s -o /dev/null --limit-rate 200k -m 1 "${URL}files/big6.bin"
    rc=$?
    sleep 0.3
    if [ "$rc" -eq 28 ] && proxy_alive; then
        ok "client gives up mid-transfer -> front still serves"
    else
        bad "client gives up mid-transfer -> front still serves (curl rc $rc)"
    fi

    stop_server TERM
}

# 손으로 하는 확인 (run.sh 에 넣지 않는다: 제한을 줄여 따로 빌드해야 하고 4초 가까이 걸린다).
# 느린 응답 두 시계: 전체 시간이 UPSTREAM_TIMEOUT_MS 보다 길어도 바이트 사이 간격이 짧으면 끝까지 받아야 한다.
# fake drip 은 머리를 바로 보내고 본문 6바이트를 700 ms 마다 1바이트씩 (전체 약 3.5초). 제한 1000 ms 로 빌드한다
# (connect 시간 초과는 server.h 가 그 절반으로 알아서 정한다. -DPROXY_CONNECT_TIMEOUT_MS 를 직접 줘도 되지만 전체 마감보다 작아야 한다):
#   make BUILD_DIR=/tmp/jw-drip TARGET=/tmp/jw-drip/webserver CFLAGS='-O2 -g -DUPSTREAM_TIMEOUT_MS=1000' \
#        /tmp/jw-drip/webserver /tmp/jw-drip/tools/fake_upstream
#   /tmp/jw-drip/tools/fake_upstream 9301 drip &
#   echo '/ 127.0.0.1:9301' >/tmp/jw-drip/p.conf
#   /tmp/jw-drip/webserver -p 9300 -r www -c /tmp/jw-drip/p.conf &
#   curl -sS -m 10 -w '\n%{http_code} %{time_total}s\n' http://127.0.0.1:9300/x
# 기대: abcdef, 200, 3.5초 안팎 (제한 1초보다 길다). 실패하면 504 가 아니라 본문이 중간에 끊긴다
# (응답 시작 뒤라 502 가 아니다). 끝나면 kill %1 %2.
#
# connect 시간 초과(PROXY_CONNECT_TIMEOUT_MS)는 failover_tests 가 가짜 blackhole(연결 대기열을 가득 채워 SYN 을 버리게 한
# 소켓)로 자동으로 본다. 그 방법이 안 되는 환경에서는 건너뛰므로, 라우팅되지 않는 주소로 손으로도 볼 수 있다:
#   echo '/x/ 10.255.255.1:80 127.0.0.1:9301 failover=timeout' >/tmp/p.conf   (9301 에 fake_upstream ok 를 띄운다)
#   ./webserver -p 9300 -r www -c /tmp/p.conf &   curl -s -m 8 -w '\n%{http_code} %{time_total}s\n' http://127.0.0.1:9300/x/y
# 기대: fake ok, 200, 1초 조금 넘게 (첫 요청이 10.255.255.1 에서 시작해야 한다. 환경에 따라 그 주소가 즉시 거절될 수 있다).
# 페일오버 (failover=, backup=, tries=, 결정: docs/decisions.md 2026-11-25).
#   포트: 살아 있는 백엔드 L1=PORT+10, L2=PORT+11 (실제 백엔드, /foN/id 가 자기 이름 L1·L2 를 돌려준다),
#         아무도 안 듣는 D1=PORT+9, D2=PORT+12, D3=PORT+13, Host 를 돌려주는 가짜 FH=PORT+15,
#         대기열이 가득 차 connect 가 멈추는 가짜 HOLE=PORT+14 (connect 시간 초과 검사. 이 환경이 못 만들면 그 검사만 건너뛴다),
#         받은 요청을 파일에 남기는 가짜 R1=PORT+16 (ok), PH=PORT+17 (응답 머리를 반만 보내고 닫음), R2=PORT+18 (ok),
#         요청을 받고 답이 없는 가짜 SIL=PORT+19 (받은 요청을 파일에 남긴다), 처음에는 죽어 있다가 테스트 도중 실제 백엔드(L3)가 뜨는 D4=PORT+20
#   라우트마다 접두사를 따로 둔다: 죽은 표시(10초)와 Round-Robin 순번이 라우트별이라 서로 영향을 주지 않고,
#   새로 띄운 프런트에서 각 라우트를 처음부터 쓰므로 순서가 정해져 있다 (첫 요청은 설정 줄의 첫 주 백엔드에서 시작).
#   502·504 검사에는 proxy_alive (/api/add → L1, 페일오버 없는 라우트)를 짝으로 붙인다.

# "코드:본문" 한 줄 (200 이 아니면 코드만). 본문의 개행은 뗀다.
# curl 이 0 이 아닌 값으로 끝나면(전송 오류: 본문이 Content-Length 보다 짧게 끊김 18, 시간 초과 28 등) 상태 줄이 200 이어도
# 정상 응답이 아니다. 그래서 "curl-rc<번호>" 를 돌려줘 어느 기대값("200:L1", "502" ...)과도 맞지 않게 한다
fo_get() {
    _c=$(curl -s -m 8 -o "$TMP/fo.body" -w '%{http_code}' "${URL}$1")
    _rc=$?
    if [ "$_rc" -ne 0 ]; then
        printf 'curl-rc%s' "$_rc"
    elif [ "$_c" = 200 ]; then
        printf '%s:%s' "$_c" "$(tr -d '\n' <"$TMP/fo.body")"
    else
        printf '%s' "$_c"
    fi
}

# fo_timed <경로>: 한 번 요청해서 FO_CODE, FO_BODY(개행 뗌), FO_T(curl 이 잰 time_total, 소수 초)를 채운다.
# curl 이 0 이 아닌 값으로 끝나면 FO_CODE 는 "curl-rc<번호>" (fo_get 과 같은 이유)
fo_timed() {
    _r=$(LC_ALL=C curl -s -m 10 -o "$TMP/fo.body" -w '%{http_code} %{time_total}' "${URL}$1")
    _rc=$?
    FO_CODE=${_r%% *}
    [ "$_rc" -ne 0 ] && FO_CODE="curl-rc$_rc"
    FO_T=${_r#* }
    FO_BODY=$(tr -d '\n' <"$TMP/fo.body")
}

# fo_between <값> <하한> <상한>: 하한 <= 값 < 상한 (소수 비교는 awk)
fo_between() {
    awk -v t="$1" -v lo="$2" -v hi="$3" 'BEGIN { exit !(t >= lo && t < hi) }'
}

# 요청 기록 파일의 줄 수. 가짜는 ready 를 찍기 전에 파일을 만들어 두므로 요청 0건이어도 파일은 있다.
# 파일이 없으면 "0" 이 아니라 "nolog" 를 돌려줘서 기록을 못 한 가짜가 "요청을 안 받았다"로 읽히지 않게 한다 (비교는 전부 숫자와 하므로 실패로 센다)
fo_lines() {
    if [ -f "$1" ]; then wc -l <"$1" | tr -d ' '; else echo nolog; fi
}

# fo_seq <경로> <횟수>: 같은 경로를 이어서 요청해 fo_get 결과를 공백으로 이어 붙인다
fo_seq() {
    _s=
    _n=0
    while [ $_n -lt "$2" ]; do
        _s="$_s $(fo_get "$1")"
        _n=$((_n + 1))
    done
    printf '%s' "$_s"
}

# 대기열이 가득 찬 가짜(blackhole)를 띄운다. 0 준비됨, 2 이 환경은 못 만듦 (건너뜀), 1 시작 실패
start_blackhole() {
    _fo=$TMP/fake.$1.out
    "$FAKE" "$1" blackhole >"$_fo" 2>&1 &
    UP_PIDS="$UP_PIDS $!"
    _i=0
    while [ $_i -lt 50 ]; do
        grep -q '^ready' "$_fo" && return 0
        grep -q '^unsupported' "$_fo" && return 2
        sleep 0.1
        _i=$((_i + 1))
    done
    cat "$_fo"
    return 1
}

failover_tests() {
    L1=$((PORT + 10)) L2=$((PORT + 11)) D1=$((PORT + 9)) D2=$((PORT + 12)) D3=$((PORT + 13))
    HOLE=$((PORT + 14)) FH=$((PORT + 15)) R1=$((PORT + 16)) PH=$((PORT + 17)) R2=$((PORT + 18))
    SIL=$((PORT + 19)) D4=$((PORT + 20))
    for d in fo1 fo2 fo3 fo3b fo3c fo4 fo4b fo5 fo6 fo7 fo8 fo13 fo14 fo15; do
        mkdir -p "$TMP/l1/$d" "$TMP/l2/$d" "$TMP/l3/$d"
        printf 'L1\n' >"$TMP/l1/$d/id"
        printf 'L2\n' >"$TMP/l2/$d/id"
        printf 'L3\n' >"$TMP/l3/$d/id"
    done
    printf 'backend L1\n' >"$TMP/l1/index.html"
    printf 'backend L2\n' >"$TMP/l2/index.html"
    printf 'backend L3\n' >"$TMP/l3/index.html"
    rm -f "$TMP/fo.r1.log" "$TMP/fo.ph.log" "$TMP/fo.r2.log" "$TMP/fo.sil.log"
    start_backend "$L1" "$TMP/l1" && start_backend "$L2" "$TMP/l2" || { bad "failover backends start"; return; }
    start_fake "$FH" host || { bad "fake host starts"; return; }
    start_fake "$R1" ok "$TMP/fo.r1.log" && start_fake "$PH" partial-head "$TMP/fo.ph.log" &&
        start_fake "$R2" ok "$TMP/fo.r2.log" && start_fake "$SIL" silent "$TMP/fo.sil.log" || { bad "failover fakes start"; return; }
    start_blackhole "$HOLE"
    hole=$?
    [ $hole -eq 1 ] && { bad "fake blackhole starts"; return; }

    cat >"$TMP/fo.conf" <<EOF
/api/add   $HOST:$L1
/api/echo  $HOST:$D1 $HOST:$L1 failover=connect
/fo1/      $HOST:$D1 $HOST:$L1 failover=connect
/fo2/      $HOST:$D1 $HOST:$L1
/fo3/      $HOST:$D1 $HOST:$D2 backup=$HOST:$L1 failover=connect
/fo3b/     $HOST:$L1 backup=$HOST:$L2 failover=connect
/fo3c/     $HOST:$D1 backup=$HOST:$L1
/fo4/      $HOST:$D1 $HOST:$D2 $HOST:$D3 failover=connect tries=2
/fo4b/     $HOST:$D1 $HOST:$D2 $HOST:$L1 failover=connect tries=2
/fo5/      $HOST:$D1 $HOST:$D2 $HOST:$L1 $HOST:$L2 failover=connect
/fo9/      $HOST:$D1 $HOST:$FH failover=connect
/fo10/     $HOST:$D1 $HOST:$R1 failover=connect
/fo11/     $HOST:$PH $HOST:$R2 failover=connect,timeout
/fo13/     $HOST:$D4 $HOST:$L1 failover=connect
/fo14/     $HOST:$D4 $HOST:$L1 failover=connect
EOF
    # connect 시간 초과 라우트는 설정이 따로다 (한 설정의 라우트는 최대 PROXY_MAX_ROUTES 개). 아래 6~8b 에서 서버를 다시 띄운다
    cat >"$TMP/fo.hole.conf" <<EOF
/api/add   $HOST:$L1
/fo6/      $HOST:$HOLE $HOST:$L1 failover=timeout
/fo7/      $HOST:$HOLE failover=timeout
/fo8/      $HOST:$HOLE $HOST:$L1 failover=connect
/fo12/     $HOST:$HOLE $HOST:$SIL failover=timeout
/fo15/     $HOST:$HOLE backup=$HOST:$L1 failover=connect
EOF

    : >"$LOG"
    if ! start_server -c "$TMP/fo.conf"; then
        bad "failover front starts"
        cat "$LOG"
        return
    fi

    # 1. 주 백엔드 둘 중 하나가 죽어 있어도 failover=connect 면 모든 요청이 200 (502 없음). 죽은 쪽 차례에서 넘어간다
    got=$(fo_seq fo1/id 6)
    if [ "$got" = " 200:L1 200:L1 200:L1 200:L1 200:L1 200:L1" ]; then
        ok "failover=connect: dead + live primary -> every request 200 from the live one"
    else
        bad "failover=connect: dead + live primary -> every request 200 from the live one (got$got)"
    fi

    # 2. 같은 구성에서 failover 를 안 쓰면 예전 동작: 번갈아 502, 200
    got=$(fo_seq fo2/id 4)
    case $got in
        " 502 200:L1 502 200:L1"|" 200:L1 502 200:L1 502") ok "failover off: dead + live primary -> 502 and 200 alternate (as before)" ;;
        *) bad "failover off: dead + live primary -> 502 and 200 alternate (got$got)" ;;
    esac

    # 3. 주 백엔드가 모두 죽었으면 백업이 답한다
    got=$(fo_seq fo3/id 3)
    if [ "$got" = " 200:L1 200:L1 200:L1" ]; then
        ok "primaries all dead + backup alive -> 200 from the backup"
    else
        bad "primaries all dead + backup alive -> 200 from the backup (got$got)"
    fi
    # 3b. 주 백엔드가 멀쩡하면 백업은 쓰지 않는다 (L2 의 id 는 나오지 않는다)
    got=$(fo_seq fo3b/id 4)
    if [ "$got" = " 200:L1 200:L1 200:L1 200:L1" ]; then
        ok "primary alive -> backup is not used"
    else
        bad "primary alive -> backup is not used (got$got)"
    fi
    # 3c. failover 가 꺼져 있으면 백업이 있어도 쓰지 않는다 (예전 동작 그대로 502)
    got=$(fo_seq fo3c/id 2)
    if [ "$got" = " 502 502" ] && proxy_alive; then
        ok "backup= without failover -> backup is never used (502)"
    else
        bad "backup= without failover -> backup is never used (got$got)"
    fi

    # 4. 전부 죽었으면 502, 그리고 빠르게 (tries=2 로 시도를 줄였다. 연결 거절은 즉시라 시간은 참고용)
    t0=$(date +%s)
    got=$(fo_seq fo4/id 3)
    elapsed=$(($(date +%s) - t0))
    if [ "$got" = " 502 502 502" ] && [ "$elapsed" -le 2 ] && proxy_alive; then
        ok "all backends dead, tries=2 -> 502, fast (${elapsed}s)"
    else
        bad "all backends dead, tries=2 -> 502, fast (got$got after ${elapsed}s)"
    fi
    # 4b. tries=2 는 살아 있는 셋째 백엔드까지 가지 않는다: 첫 요청은 둘(D1, D2)만 해 보고 502, 그다음 요청은 Round-Robin
    #     시작이 한 칸 밀려 L1 에 닿는다. (tries 가 없었다면 첫 요청도 200.) 이 검사는 죽은 표시를 보지 않는다 (아래 4c)
    got=$(fo_seq fo4b/id 3)
    if [ "$got" = " 502 200:L1 200:L1" ]; then
        ok "tries=2 stops before the third (live) backend: 502, then 200 from the live one"
    else
        bad "tries=2 stops before the third (live) backend (got$got)"
    fi

    # 4c. 죽은 표시가 다음 요청에 쓰인다: D4 는 처음에 죽어 있다. 첫 요청이 D4 → 실패 → L1 (D4 에 죽은 표시).
    #     그 뒤 D4 자리에 실제 백엔드(L3)를 띄운다. 셋째 요청은 Round-Robin 이 다시 D4 차례지만 표시가 남아 있어 L1 이 답한다.
    #     표시를 안 쓰면 L3 가 답한다. 같은 D4 를 쓰는 다른 라우트(fo14)의 첫 요청이 L3 인 것으로 L3 가 정말 살아났음을 확인한다
    a1=$(fo_get fo13/id)
    if start_backend "$D4" "$TMP/l3"; then
        a2=$(fo_get fo13/id)
        a3=$(fo_get fo13/id)
        a4=$(fo_get fo14/id)
        if [ "$a1 $a2 $a3 $a4" = "200:L1 200:L1 200:L1 200:L3" ]; then
            ok "dead mark is remembered: the revived backend is skipped while marked (L1 answers, L3 alive via another route)"
        else
            bad "dead mark is remembered (got $a1 $a2 $a3 $a4, want 200:L1 200:L1 200:L1 200:L3)"
        fi
    else
        bad "dead mark is remembered (L3 backend did not start)"
    fi

    # 5. POST 도 페일오버를 거쳐 한 번만 실행된다: 첫 요청은 D1 connect 실패 뒤 L1 로 가고 본문이 그대로 돌아온다.
    #    요청 바이트는 connect 가 끝난 뒤에야 나가므로 실패한 백엔드에는 아무것도 가지 않는다
    head -c 102400 /dev/urandom >"$TMP/fo.echo.in"
    ok5=1
    for _n in 1 2; do
        code=$(curl -s -H 'Expect:' -H 'Content-Type: application/octet-stream' --data-binary @"$TMP/fo.echo.in" \
               -o "$TMP/fo.echo.out" -w '%{http_code}' -m 5 "${URL}api/echo")
        rc=$?                      # 완전한 응답을 기대하는 자리: curl 이 0 으로 끝나야 한다 (본문이 끊겨도 상태 줄은 200 일 수 있다)
        [ "$rc" -eq 0 ] && [ "$code" = 200 ] && cmp -s "$TMP/fo.echo.in" "$TMP/fo.echo.out" || ok5=0
    done
    if [ $ok5 -eq 1 ]; then
        ok "POST 100 KB through failover (dead primary first) -> echoed back identical, twice"
    else
        bad "POST 100 KB through failover -> echoed back identical (last code $code, curl rc $rc)"
    fi

    # 5a. 백엔드가 받은 횟수로 센다 (위 5 는 응답이 같은지만 본다: 요청이 두 번 나가 한쪽 응답만 중계돼도 통과한다).
    #     가짜 R1 은 받은 요청마다 "<요청 줄> body=<본문 바이트>" 한 줄을 남긴다. POST 100 KB 두 번 → 정확히 두 줄, 본문은 온전히
    head -c 102400 /dev/urandom >"$TMP/fo.post.in"
    ok5a=1
    for _n in 1 2; do
        code=$(curl -s -H 'Expect:' -H 'Content-Type: application/octet-stream' --data-binary @"$TMP/fo.post.in" \
               -o /dev/null -w '%{http_code}' -m 5 "${URL}fo10/x")
        rc=$?
        [ "$rc" -eq 0 ] && [ "$code" = 200 ] || ok5a=0
    done
    n=$(fo_lines "$TMP/fo.r1.log")
    if [ $ok5a -eq 1 ] && [ "$n" = 2 ] && [ "$(sort -u "$TMP/fo.r1.log")" = "POST /fo10/x HTTP/1.1 body=102400" ]; then
        ok "POST through failover (dead primary first) reaches the live backend exactly once per request, whole body"
    else
        bad "POST through failover reaches the live backend exactly once per request (codes ok=$ok5a, last curl rc $rc, $n lines: $(tr '\n' '|' <"$TMP/fo.r1.log"))"
    fi
    # 5b0. 요청을 보낸 뒤의 실패는 넘기지 않는다: PH 는 본문까지 받고 응답 머리를 반만 보낸 채 닫는다. 프록시는 이미 요청을 썼으므로
    #      두 번째 백엔드 R2 로 다시 보내면 안 된다 → 502, PH 는 한 번, R2 는 한 번도 못 받는다 (failover=connect,timeout 이어도)
    head -c 1000 /dev/urandom >"$TMP/fo.post.small"
    code=$(curl -s -H 'Expect:' -H 'Content-Type: application/octet-stream' --data-binary @"$TMP/fo.post.small" \
           -o /dev/null -w '%{http_code}' -m 5 "${URL}fo11/x")
    rc=$?                          # 502 도 완전한 응답이다 (curl 0). 본문이 끊기거나 시간 초과면 502 로 보여도 실패
    nph=$(fo_lines "$TMP/fo.ph.log")
    nr2=$(fo_lines "$TMP/fo.r2.log")
    # 로그가 비었다는 것만으로는 R2 가 "받지 못했다"인지 "받고 기록하다 죽었다"인지 모른다. 그래서 R2 에 직접 확인 요청을 하나 보내
    # 답을 받고(200) 그 요청이 로그에 정확히 한 줄로 남는지 본다: R2 가 살아 있고 기록도 한다는 증거다
    code2=$(curl -s -m 5 -o /dev/null -w '%{http_code}' "http://$HOST:$R2/confirm")
    rc2=$?
    nr2c=$(fo_lines "$TMP/fo.r2.log")
    if [ "$rc" -eq 0 ] && [ "$rc2" -eq 0 ] && [ "$code" = 502 ] && [ "$nph" = 1 ] && [ "$nr2" = 0 ] && [ "$(cat "$TMP/fo.ph.log")" = "POST /fo11/x HTTP/1.1 body=1000" ] &&
       [ "$code2" = 200 ] && [ "$nr2c" = 1 ] && [ "$(cat "$TMP/fo.r2.log")" = "GET /confirm HTTP/1.1 body=0" ]; then
        ok "failure after the request was sent is not failed over: 502, first backend got it once, second never (and the second is alive and logging)"
    else
        bad "failure after the request was sent is not failed over (code $code curl rc $rc, first backend $nph, second $nr2; confirm request: code $code2 curl rc $rc2, log lines $nr2c)"
    fi

    # 5b. 동시 요청: 죽은 백엔드 둘, 산 백엔드 둘. 한 poll 바퀴 안에서 여러 연결이 동시에 실패하고 넘어가도 전부 200.
    #     요청마다 본문 파일(fo5.<i>.body)과 결과 파일(fo5.<i>.res: "curl 종료코드 상태코드")을 따로 둔다.
    #     curl 종료코드가 0 이고 상태 200 이고 본문이 L1 또는 L2 여야 센다 (본문이 끊긴 200 은 정상 응답이 아니다)
    pids=
    i=0
    rm -f "$TMP"/fo5.*
    while [ $i -lt 40 ]; do
        {
            c=$(curl -s -m 5 -o "$TMP/fo5.$i.body" -w '%{http_code}' "${URL}fo5/id" 2>/dev/null)
            echo "$? $c" >"$TMP/fo5.$i.res"
        } &
        pids="$pids $!"
        i=$((i + 1))
    done
    wait $pids
    n=0
    i=0
    while [ $i -lt 40 ]; do
        if [ "$(cat "$TMP/fo5.$i.res" 2>/dev/null)" = "0 200" ]; then
            case "$(tr -d '\n' <"$TMP/fo5.$i.body" 2>/dev/null)" in L1|L2) n=$((n + 1)) ;; esac
        fi
        i=$((i + 1))
    done
    [ "$n" -eq 40 ] && ok "failover: 40 concurrent requests, 2 dead + 2 live backends -> all 200" \
                    || bad "failover: 40 concurrent requests, 2 dead + 2 live backends -> all 200 (only $n complete; first bad: $(cat "$TMP"/fo5.*.res | grep -v '^0 200$' | head -3 | tr '\n' '|'))"

    # 9. 넘어간 요청의 Host 는 처음 고른(죽은) 백엔드가 아니라 실제로 보낸 백엔드의 이름이다
    got=$(fo_get fo9/x)
    if [ "$got" = "200:$HOST:$FH" ]; then
        ok "failover rewrites Host for the backend actually used ($got)"
    else
        bad "failover rewrites Host for the backend actually used (got $got, want 200:$HOST:$FH)"
    fi

    stop_server TERM

    # 6~8. connect 시간 초과 (PROXY_CONNECT_TIMEOUT_MS 1초, 전체 마감 UPSTREAM_TIMEOUT_MS 5초). 시간은 curl 의 time_total 로 재고 폭을 둔다
    if [ $hole -eq 0 ]; then
        : >"$LOG"
        if ! start_server -c "$TMP/fo.hole.conf"; then
            bad "failover front (connect timeout routes) starts"
            cat "$LOG"
            return
        fi
        # 6. failover=timeout: 멈춘 connect 를 1초 뒤 닫고 다음 백엔드로 → 200
        #    시간은 curl 의 time_total (소수 초). 정수 초 시계는 두 date 가 초 경계에 걸치면 너무 이른 재시도도 통과시킨다.
        #    아래 한계 0.95 는 서버의 1000 ms 시계(ms 로 버림)가 curl 보다 늦게 시작해도 정상 구현이 걸리지 않을 만큼의 여유다
        fo_timed fo6/id
        if [ "$FO_CODE" = 200 ] && [ "$FO_BODY" = L1 ] && fo_between "$FO_T" 0.95 3; then
            ok "failover=timeout: stalled connect -> next backend after about 1s, 200 (${FO_T}s)"
        else
            bad "failover=timeout: stalled connect -> next backend after about 1s, 200 (got $FO_CODE:$FO_BODY after ${FO_T}s, want 200:L1 in 0.95-3s)"
        fi
        # 7. 넘어갈 곳이 없으면 마지막 실패가 시간 초과라서 504 (1초 안팎. 5초 전체 마감이 아니다)
        fo_timed fo7/x
        if [ "$FO_CODE" = 504 ] && fo_between "$FO_T" 0.95 3 && proxy_alive; then
            ok "failover=timeout, no backend left -> 504 after about 1s (${FO_T}s)"
        else
            bad "failover=timeout, no backend left -> 504 after about 1s (got $FO_CODE after ${FO_T}s, want 504 in 0.95-3s)"
        fi
        # 8. failover=connect 만이면 connect 시계는 없다: 멈춘 connect 는 전체 마감(5초)에 504. 뒤에 L1 이 있어도 시계를 새로 잡지 않는다
        fo_timed fo8/id
        if [ "$FO_CODE" = 504 ] && fo_between "$FO_T" 4.5 7 && proxy_alive; then
            ok "failover=connect only: stalled connect -> 504 at the overall deadline (${FO_T}s)"
        else
            bad "failover=connect only: stalled connect -> 504 at the overall deadline (got $FO_CODE after ${FO_T}s, want 504 in 4.5-7s)"
        fi
        # 8b. 전체 마감은 첫 시도부터 한 번만 잰다: HOLE 에서 1초 기다린 뒤 SIL 로 넘어가 요청을 보내도 답이 없다.
        #     첫 시도 기준이면 5초 안팎에 504, 넘어갈 때 시계를 새로 잡으면 6초 이상이다 (curl 이 잰 시간, 초 단위 아님)
        #     SIL 이 요청을 실제로 한 번 받았는지도 본다 (fake 의 기록 줄 수): 넘어가지 못한 채 HOLE 에서 5초를 채우고 504 가 나도
        #     코드와 시간은 같아서, 줄 수를 안 보면 페일오버가 깨져도 통과한다
        res=$(LC_ALL=C curl -s -m 10 -o /dev/null -w '%{http_code} %{time_total}' "${URL}fo12/x")
        rc=$?                      # 504 도 완전한 응답이다: curl 이 0 으로 끝나야 한다 (-m 10 시간 초과는 28)
        nsil=$(fo_lines "$TMP/fo.sil.log")
        if [ "$rc" -eq 0 ] && [ "${res%% *}" = 504 ] && awk -v t="${res#* }" 'BEGIN { exit !(t >= 4.5 && t < 5.9) }' && [ "$nsil" = 1 ] && proxy_alive; then
            ok "failover=timeout: overall deadline counts from the first attempt, not from the failover (504 after ${res#* }s, second backend got the request)"
        else
            bad "failover=timeout: overall deadline counts from the first attempt (got '$res' curl rc $rc, second backend got $nsil request(s), want 504 in 4.5-5.9s and 1)"
        fi
        # 8c. 마감에 connect 가 아직 멈춰 있던 백엔드는 죽은 표시를 받는다. failover=connect 만이면 connect 시계가 없어 이 길뿐이다.
        #     첫 요청: 주 백엔드 HOLE 에서 멈춰 5초 마감에 504. 둘째 요청: 표시가 없다면 또 HOLE 에서 5초를 기다려 504 이고,
        #     표시가 있으면 HOLE 을 건너뛰고 백업 L1 이 바로 답한다 (1초 안)
        fo_timed fo15/id
        first_code=$FO_CODE first_t=$FO_T
        fo_timed fo15/id
        if [ "$first_code" = 504 ] && fo_between "$first_t" 4.5 7 && [ "$FO_CODE" = 200 ] && [ "$FO_BODY" = L1 ] && fo_between "$FO_T" 0 1; then
            ok "failover=connect only: backend stuck at the overall deadline is marked down (504 at ${first_t}s, next request 200 from the backup in ${FO_T}s)"
        else
            bad "failover=connect only: stuck backend is marked down at the deadline (first $first_code after ${first_t}s, want 504 in 4.5-7s; next $FO_CODE:$FO_BODY after ${FO_T}s, want 200:L1 in under 1s)"
        fi
        stop_server TERM
    else
        echo "  skip connect-timeout checks (this kernel does not drop SYNs for a full backlog; see the manual check in the notes)"
    fi
}

: >"$LOG"
proxy_tests
: >"$LOG"
failover_tests

# 경로 정규형 (#31): 라우팅과 정적 파일이 같은 정규형을 본다.
# 프런트 docroot 에 프록시 접두사와 같은 경로의 파일(admin/x)을 두고, 라우트를 건너뛰어 그 파일이
# 열리는지 본다. 프록시로 가면 502 (백엔드 127.0.0.1:9 는 아무도 안 듣는 포트라 연결 거절)
mkdir -p "$DOCROOT/admin"
printf 'front file\n' >"$DOCROOT/admin/x"
printf '/admin 127.0.0.1:9\n' >"$TMP/norm.conf"
: >"$LOG"
if start_server -c "$TMP/norm.conf"; then
    expect_code 502 "GET /admin/x (proxy route)"          --path-as-is "${URL}admin/x"
    expect_code 400 "GET //admin/x (empty segment)"       --path-as-is "${URL}/admin/x"
    expect_code 502 "GET /%61dmin/x (normalized to proxy)" --path-as-is "${URL}%61dmin/x"
    expect_code 400 "GET /admin%2Fx"                      --path-as-is "${URL}admin%2Fx"
    expect_code 400 "GET /admin%5Cx"                      --path-as-is "${URL}admin%5Cx"
    expect_code 400 'GET /admin\x'                        --path-as-is "${URL}admin\\x"
    body=$(curl -s -m 3 --path-as-is "${URL}%61pi/add?a=1&b=2")
    [ "$body" = 3 ] && ok "GET /%61pi/add?a=1&b=2 -> app 3" || bad "GET /%61pi/add?a=1&b=2 -> app 3 (got '$body')"
    stop_server TERM
else
    bad "server starts with proxy config"
    cat "$LOG"
fi

# --merge-slashes: "//" 를 합쳐서 프록시로. getopt 에 넘어가면 glibc 가 -r 값을 "ge-slashes" 로
# 덮어쓰므로(인자 위치와 무관), 로그의 docroot 가 그대로인지 본다
: >"$LOG"
if start_server --merge-slashes -c "$TMP/norm.conf"; then
    grep -q "docroot $DOCROOT, .*merge slashes" "$LOG" && ok "--merge-slashes keeps -r and is logged" \
                                                     || { bad "--merge-slashes keeps -r and is logged"; cat "$LOG"; }
    expect_code 502 "--merge-slashes: GET //admin/x -> proxy" --path-as-is "${URL}/admin/x"
    expect_code 200 "--merge-slashes: GET /sub//a.txt"        --path-as-is "${URL}sub//a.txt"
    stop_server TERM
else
    bad "server starts with --merge-slashes"
    cat "$LOG"
fi

exit $fail
