#!/bin/sh
# 시연 페이지를 "실서버 모드"로 띄운다 (로컬 전용).
#
#   sh docs/demo/run-local.sh            ./webserver 로 (없으면 make)
#   BIN=/path/to/webserver sh docs/demo/run-local.sh
#
# 띄우는 것: 백엔드 18801 · 18802 · 18803(백업), 프런트 18800 (-c, /api/echo 만 백엔드로).
# 프런트의 docroot 는 임시 폴더: www/ 복사본 + 이 페이지(demo.html) + live.json.
# 페이지는 live.json 이 보이면 실서버 모드 스위치를 켤 수 있게 한다 (GitHub Pages 에는 이 파일이 없다).
# 페일오버 옵션(backup=, failover=)은 PR #51 빌드가 필요하다. 옛 빌드면 설정을 못 읽고 멈춘다.
# Ctrl-C 로 네 프로세스를 다 끄고 임시 폴더를 지운다.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1
BIN=${BIN:-./webserver}
case "$BIN" in /*) ;; *) BIN="$ROOT/${BIN#./}" ;; esac
if [ ! -x "$BIN" ]; then make -s || exit 1; fi
WWW="$ROOT/www"
FRONT=${FRONT:-18800}
T=$(mktemp -d "${TMPDIR:-/tmp}/webserver-demo.XXXXXX") || exit 1
CONF="/api/echo 127.0.0.1:18801 127.0.0.1:18802 backup=127.0.0.1:18803 failover=connect"

cleanup() {
    for f in "$T"/*.pid; do [ -f "$f" ] && kill "$(cat "$f")" 2>/dev/null; done
    rm -rf "$T"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

mkdir -p "$T/docroot" && cp -R "$WWW"/. "$T/docroot/" || exit 1
cp "$ROOT/docs/demo/index.html" "$T/docroot/demo.html"
printf '%s\n' "$CONF" >"$T/proxy.conf"

start_backend() {   # 포트
    "$BIN" -p "$1" -r "$WWW" >"$T/$1.log" 2>&1 &
    echo $! >"$T/$1.pid"
}
for p in 18801 18802 18803; do start_backend "$p"; done

# 페이지가 읽는 정보: 설정 한 줄, 백엔드를 끄고 켜는 명령
{
    printf '{"conf":"%s",' "$CONF"
    printf '"stop":{'
    sep=""
    for p in 18801 18802 18803; do printf '%s"%s":"kill $(cat %s/%s.pid)"' "$sep" "$p" "$T" "$p"; sep=","; done
    printf '},"start":{'
    sep=""
    for p in 18801 18802 18803; do
        printf '%s"%s":"%s -p %s -r %s >%s/%s.log 2>&1 & echo $! >%s/%s.pid"' "$sep" "$p" "$BIN" "$p" "$WWW" "$T" "$p" "$T" "$p"
        sep=","
    done
    printf '}}\n'
} >"$T/docroot/live.json"

"$BIN" -p "$FRONT" -r "$T/docroot" -c "$T/proxy.conf" --access-log >"$T/front.log" 2>&1 &
echo $! >"$T/front.pid"
sleep 0.5
if ! grep -q '^webserver: listening' "$T/front.log"; then
    echo "프런트가 안 떴어요:"; cat "$T/front.log"
    echo "(backup=/failover= 를 못 읽으면 PR #51 빌드로: BIN=... sh $0)"
    exit 1
fi

echo "시연 페이지 (실서버 모드):  http://localhost:$FRONT/demo.html"
echo "설정: $CONF"
echo "백엔드 끄기 예:  kill \$(cat $T/18802.pid)"
echo "프런트 access log:  tail -f $T/front.log"
echo "Ctrl-C 로 전부 끔"
wait
