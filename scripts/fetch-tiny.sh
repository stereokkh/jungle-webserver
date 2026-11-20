#!/bin/sh
# CS:APP 3판 11장 Tiny Web Server를 공식 사이트에서 받아 로컬에서 빌드한다. (이슈 #3)
#
#   sh scripts/fetch-tiny.sh          # 또는 make classic
#   cd classic && ./tiny 8000
#
# - 받은 원본은 classic/ 에만 둔다. classic/ 은 .gitignore 대상이며 커밋하지 않는다.
#   (저작권은 교재 저자에게 있음. 우리 저장소는 이 스크립트와 분석 문서만 가진다)
# - 팀 빌드 플래그(-std=c11 -Wpedantic -Werror)를 쓰지 않는다. 교재 코드를 고치지 않고
#   그대로 분석하기 위해서이며, 경고가 나와도 무시한다.
# - src/ 의 우리 서버와는 완전히 별개다. src/ 에서 csapp.c 를 링크하지 않는다.
set -eu

BASE=https://csapp.cs.cmu.edu/3e/ics3/code
DIR=${CLASSIC_DIR:-classic}
CC=${CC:-cc}

command -v curl >/dev/null 2>&1 || { echo "curl 이 필요합니다" >&2; exit 1; }
command -v "$CC" >/dev/null 2>&1 || { echo "C 컴파일러($CC)가 필요합니다. WSL/macOS/Docker에서 실행하세요" >&2; exit 1; }

if command -v sha256sum >/dev/null 2>&1; then
    sha256() { sha256sum "$1" | cut -d' ' -f1; }
else
    sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }   # macOS
fi

mkdir -p "$DIR/cgi-bin"
: > "$DIR/SOURCES.txt"
echo "# CS:APP 3e Tiny 원본 (받은 시각: $(date -u +%Y-%m-%dT%H:%M:%SZ))" >> "$DIR/SOURCES.txt"

fetch() {   # fetch <원격 경로> <로컬 경로>
    curl -fsSL "$BASE/$1" -o "$DIR/$2"
    printf '%s  %s  %s\n' "$(sha256 "$DIR/$2")" "$2" "$BASE/$1" >> "$DIR/SOURCES.txt"
    echo "  받음 $2"
}

echo "== CS:APP Tiny 원본 받기 → $DIR/"
fetch netp/tiny/tiny.c        tiny.c
fetch netp/tiny/cgi-bin/adder.c cgi-bin/adder.c
fetch src/csapp.c             csapp.c
fetch include/csapp.h         csapp.h

# 공식 사이트에는 예제 HTML이 없으므로 테스트용 페이지만 직접 만든다
cat > "$DIR/home.html" <<'EOF'
<html><head><title>tiny</title></head>
<body><h1>CS:APP Tiny</h1><p>classic/home.html (fetch-tiny.sh가 만든 테스트 페이지)</p></body></html>
EOF

echo "== 빌드 ($CC)"
( cd "$DIR" \
  && $CC -O2 -I. -o tiny tiny.c csapp.c -lpthread \
  && $CC -O2 -I. -o cgi-bin/adder cgi-bin/adder.c )

echo
echo "완료. 실행:"
echo "  cd $DIR && ./tiny 8000"
echo "  curl -v http://localhost:8000/home.html"
echo "  curl -v 'http://localhost:8000/cgi-bin/adder?1&2'"
echo "원본 URL·해시: $DIR/SOURCES.txt"
