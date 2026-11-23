#!/bin/sh
# #10: 같은 바이너리를 백엔드 2개(-c 없음)와 프런트 1개(-c, /api/ 를 두 백엔드로)로 띄우고
# 직접 호출과 프록시 경유 호출의 상태·Content-Type·본문 바이트가 같은지 본다.
#
#   sh tests/backends.sh          검사하고 끝낸다 (실패가 있으면 exit 1)
#   KEEP=1 sh tests/backends.sh   검사 뒤 세 서버를 띄운 채로 둔다 (Ctrl-C 로 종료)
#
# - make test 가 run.sh 다음에 부른다 (make asan 이면 sanitizer 빌드로)
# - POSIX sh 만 쓴다 (run.sh 와 같은 이유)
set -u
echo "== backends (#10): direct = proxy"

BIN=${BIN:-./webserver}
DOCROOT=${WWW:-www}
HOST=127.0.0.1
BASE=${PORT:-$((20000 + $$ % 12000))}   # run.sh 와 같은 범위: 리눅스 ephemeral(32768~) 아래
P1=$BASE
P2=$((BASE + 1))
PF=$((BASE + 2))
TMP=$(mktemp -d "${TMPDIR:-/tmp}/webserver-backends.XXXXXX") || exit 1
PIDS=
fail=0

cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

ok()  { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=1; }

# start 이름 포트 [서버 인자...]: 띄우고 listening 로그 + curl 응답을 최대 약 5초 기다린다
start() {
    _log=$TMP/$1.log _port=$2
    shift 2
    "$BIN" -p "$_port" -r "$DOCROOT" "$@" >"$_log" 2>&1 &
    PIDS="$PIDS $!"
    i=0
    while [ $i -lt 50 ]; do
        grep -q '^webserver: cannot' "$_log" && break
        if grep -q '^webserver: listening' "$_log" &&
           curl -s -o /dev/null -m 1 "http://$HOST:$_port/"; then
            return 0
        fi
        sleep 0.1
        i=$((i + 1))
    done
    echo "server on port $_port did not start:"
    cat "$_log"
    exit 1
}

# get 이름 포트 경로 [curl 인자...]: 헤더(CR 제거)는 $TMP/이름.h, 본문은 $TMP/이름.b, 상태 코드 출력
get() {
    _n=$1 _p=$2 _u=$3
    shift 3
    _c=$(curl -s -D "$TMP/$_n.raw" -o "$TMP/$_n.b" -w '%{http_code}' -m 3 "$@" "http://$HOST:$_p$_u")
    tr -d '\r' <"$TMP/$_n.raw" >"$TMP/$_n.h"
    echo "$_c"
}
header() { sed -n "s/^$2: //p" "$TMP/$1.h"; }

# 백엔드 2개: 프록시 설정 없이 같은 바이너리, 각자 자기 포트를 X-Instance 로
start b1 "$P1"
start b2 "$P2"
for p in "$P1" "$P2"; do
    code=$(get d "$p" '/api/add?a=1&b=2')
    if [ "$code" = 200 ] && [ "$(cat "$TMP/d.b")" = 3 ] && [ "$(header d X-Instance)" = "$p" ]; then
        ok "backend :$p /api/add -> 3, X-Instance: $p"
    else
        bad "backend :$p /api/add -> 3, X-Instance: $p (got $code, X-Instance '$(header d X-Instance)')"
    fi
done

# 프런트: /api/ 를 두 백엔드로
printf '/api/ %s:%s %s:%s\n' "$HOST" "$P1" "$HOST" "$P2" >"$TMP/proxy.conf"
start front "$PF" -c "$TMP/proxy.conf"

# same 설명 경로 [curl 인자...]: 백엔드 직접(:P1)과 프런트 경유의 상태·Content-Type·본문이 같고,
# 경유 응답의 X-Instance 가 백엔드 포트 중 하나(프런트가 직접 답한 게 아님)
same() {
    desc=$1 path=$2
    shift 2
    cd_=$(get d "$P1" "$path" "$@")
    cp_=$(get p "$PF" "$path" "$@")
    inst=$(header p X-Instance)
    if [ "$cd_" != 000 ] && [ "$cd_" = "$cp_" ] && cmp -s "$TMP/d.b" "$TMP/p.b" &&
       [ "$(header d Content-Type)" = "$(header p Content-Type)" ] &&
       { [ "$inst" = "$P1" ] || [ "$inst" = "$P2" ]; }; then
        ok "$desc: direct = proxy ($cd_, X-Instance $inst)"
    else
        bad "$desc: direct = proxy (direct $cd_, proxy $cp_, X-Instance '$inst')"
    fi
}

# Expect: 를 비워 curl 의 100-continue 1초 대기를 피한다 (decisions.md 2026-11-21 메모)
printf 'a\000b\r\nc' >"$TMP/body"
head -c 65536 /dev/urandom >>"$TMP/body"

same "GET /api/add?a=1&b=2"       '/api/add?a=1&b=2'
same "GET /api/add?a=-5&b=2"      '/api/add?a=-5&b=2'
same "GET /api/add?a=x (400)"     '/api/add?a=x&b=1'
same "GET /api/add overflow (400)" '/api/add?a=9223372036854775807&b=1'
same "POST /api/echo NUL/CRLF + 64KB" '/api/echo' -H 'Expect:' --data-binary "@$TMP/body"
cmp -s "$TMP/p.b" "$TMP/body" && ok "POST /api/echo via proxy -> sent bytes back" \
                              || bad "POST /api/echo via proxy -> sent bytes back"
same "POST /api/echo empty"       '/api/echo' -H 'Expect:' -X POST
same "GET /api/echo (405)"        '/api/echo'
same "GET /api/nope (404)"        '/api/nope'

# Round-Robin: 경유 요청 4개가 두 백엔드에 모두 간다
seen=
for i in 1 2 3 4; do
    get r "$PF" '/api/add?a=1&b=1' >/dev/null
    seen="$seen $(header r X-Instance)"
done
case "$seen" in
    *" $P1"*" $P2"* | *" $P2"*" $P1"*) ok "round-robin: both backends answer ($seen )" ;;
    *) bad "round-robin: both backends answer (X-Instance:$seen )" ;;
esac

if [ "${KEEP:-0}" = 1 ]; then
    echo "backends :$P1 :$P2, front :$PF (proxy.conf: $(cat "$TMP/proxy.conf")). Ctrl-C to stop"
    wait
fi
exit $fail
