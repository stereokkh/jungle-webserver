#!/bin/sh
# 스모크 테스트: 실행 파일이 만들어지고 기본 동작을 하는지 확인
# 서버 구현이 진행되면 curl 요청/응답 검사를 여기에 추가한다.
set -u

BIN=${BIN:-./webserver}
fail=0

check() {
    desc=$1; shift
    if "$@" >/dev/null 2>&1; then
        echo "  ok   $desc"
    else
        echo "  FAIL $desc"
        fail=1
    fi
}

echo "== smoke test ($(uname -s))"
check "binary exists"            test -x "$BIN"
check "no args -> exit non-zero" sh -c "! $BIN"
check "port arg -> exit zero"    "$BIN" 8080

exit $fail
