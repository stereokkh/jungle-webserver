# 결정 기록

계획·범위·규칙이 바뀔 때마다 **맨 위에** 항목을 추가한다. CLAUDE.md 는 "지금 상태"만, 이 파일은 "언제, 왜 바뀌었나"를 남긴다.

- 결정을 바꾸는 PR은 이 파일 항목 + CLAUDE.md 수정을 **같은 PR**에 넣는다
- 형식: 날짜 · 제목 / 결정 / 이유 / 영향 (파일·이슈)
- 각 세션의 Claude는 동기화 훅(`scripts/claude-sync.sh`)이 이 파일 변경을 알려 주면 새 항목을 읽고 따른다

---

## 2026-11-25 · [제안] 이벤트 대기 계층 `ev.h` (poll 기본, epoll·kqueue 선택 빌드)

- **상태**: 제안 (상철, 11-25). 트랙 A(건호) 합의 전이다. 같은 문제의 다른 제안이 `feat/kqueue`(혜성, `poller.h`)에 있어 둘 중 하나로 정한다. 정해지면 이 줄을 지우고 제목의 [제안] 을 떼거나, 이 항목을 지운다
- **제안**: `server.c` 는 `include/ev.h` 함수만 부르고 백엔드는 `make EVENT=poll|epoll|kqueue` 로 고른다 (기본 poll). 원하는 이벤트는 `build_interest` 한 곳에서 계산하고, epoll·kqueue 는 바뀐 fd 만 커널에 고친다. 명세는 `docs/event-backend.md`
  1. 프록시 중(`CONN_UP_*`) 클라이언트 fd 는 세 백엔드 모두 보지 않는다 (`ev_pause`). 결정 C6(11-23)의 "events 0 으로 끊김만 감지"를 바꾼다
  2. `ev_want` 는 `WS_EV_IN` 이나 `WS_EV_OUT` 하나만
  3. 등록 실패는 그 자리에서 -1 (kqueue 도 변경을 모으지 않는다)
  4. tag 는 64비트 (kqueue `udata` 에 `uintptr_t` 로)
  5. 이벤트 비트 접두사 `WS_EV_` (`<sys/event.h>` 의 `EV_*` 와 섞이지 않게)
- **이유**: 원래 목적은 poll 과 epoll/kqueue 를 연결 수별로 비교하는 것이다. 바퀴마다 다시 등록하면 비용 모양이 poll 과 같아 비교할 차이가 작아지므로 등록을 커널에 유지한다. 비교 결과를 백엔드 차이로 읽으려면 세 백엔드의 동작이 같아야 해서 1~3 을 그 기준으로 골랐다. 1 에서 잃는 것: 지금 poll 이 이 경로로 감지하는 것은 RST 뿐이고(평범한 close 의 FIN 으로는 `POLLHUP` 이 오지 않는다), 응답 시작 전에는 RST 가 드물다. 그런 클라이언트는 응답을 쓸 때나 `UPSTREAM_TIMEOUT_MS` 마감 때 닫힌다 (지식으로 판단, 실행 확인 안 함)
- **영향 (가기로 하면)**: `include/ev.h`, `src/ev_poll.c`, `src/ev_epoll.c`, `src/ev_kqueue.c`, `src/server.c` (트랙 A 파일: `build_interest`, `dispatch`, `close_watched`, tag·칸 세대), Makefile `EVENT`, CI 필수 아닌 epoll·kqueue 항목 (기존 필수 검사 이름은 그대로), `tests/unit/test_ev.c`, `docs/interfaces.md`, README, CLAUDE.md. 이 PR 자체는 명세와 헤더만 넣는다 (빌드 영향 없음)

## 2026-11-25 · 내장 덧셈(/api/add)을 헬스체크(/healthz)로 교체, 헬스체크는 프록시보다 먼저

- **결정** (건호 판단, 11-25. 마감 뒤 정리. 앱은 C 트랙 혜성님 파일이라 리뷰 필요):
  1. `GET /api/add` 를 없앤다. 대신 `GET /healthz` 를 둔다: GET·HEAD 는 200 `ok\n` (`text/plain; charset=utf-8`), 그 외 메서드는 405 `Allow: GET, HEAD`. 모든 응답에 `X-Instance: <포트>`. `POST /api/echo` 는 그대로 둔다 (동적 응답 예시, 프록시 POST 중계 검사)
  2. 처리 순서를 **헬스체크 → 프록시 → 앱 → 정적** 으로 바꾼다 (`handler_route` 가 `app_health_match` 를 맨 먼저, 새 `ROUTE_HEALTH`). 정규형 `target` 의 `?` 앞이 **정확히** `/healthz` 일 때만이다. 질의는 보지 않는다 (`/healthz?probe=1` 도 헬스체크). `/healthz/x`, `/healthzz`, `/api/healthz` 는 해당하지 않는다
  3. 헬스체크는 "이 응답을 쓴 인스턴스가 살아 있다"만 뜻한다. 백엔드 상태·페일오버 죽은 표시는 보지 않는다
  4. 백엔드를 거치는지 보는 시험(`tests/run.sh` 의 `proxy_alive`, 프록시 GET·HEAD·431, `scripts/bench.sh` 의 프록시 측정)은 `/api/add` 대신 백엔드 docroot 의 작은 정적 파일(`/alive/id`, `/files/small`, `/px/ok`)로 본다. `/healthz` 는 프런트가 직접 답해서 중계 확인에 쓸 수 없다
- **이유**: 덧셈은 Tiny `adder` 를 옮긴 시연용이었고 범용 웹서버가 할 일이 아니다. 운영에서 서버 자체에 두는 동적 주소로는 "살아 있나"를 묻는 헬스체크가 흔하다 (Docker `HEALTHCHECK`, 쿠버네티스 probe, 가동 감시, nginx `location = /health { return 200; }`). 프록시보다 먼저 둔 것은 프런트에 보낸 헬스체크를 백엔드가 대신 답하면 프런트가 살아 있는지 알 수 없기 때문이다. 정확 일치라서 가로채는 경로는 이 하나뿐이고, `/` 를 통째로 넘기는 설정에서도 다른 경로는 그대로 프록시로 간다. 이름 `/healthz` 는 쿠버네티스 관례로, 끝의 `z` 가 일반 페이지 이름과 겹치지 않게 한다
- **영향**: `include/app.h`, `src/app.c` (`app_health_match`, `app_health`, 덧셈·질의 파싱 삭제), `include/handler.h`, `src/handler.c` (`ROUTE_HEALTH`), `src/server.c` (`respond`), `tests/unit/test_app.c`, `tests/unit/test_contract.c`, `tests/run.sh`, `tests/backends.sh`, `scripts/bench.sh`, `docs/api.md`, `docs/interfaces.md`, `docs/demo/index.html`, `docs/classic.md`·`docs/benchmark.md` (기록 당시 `/api/add` 였다는 메모만), `README.md`, `CLAUDE.md`. 예전 측정값(`docs/benchmark.md` 프록시 표)은 `/api/add` 로 잰 그대로 둔다

## 2026-11-25 · 프록시 페일오버: backup=, failover=, tries= (범위 안으로)

- **결정** (상철 판단, 11-25. 설계는 이대로 두고 구현한다):
  1. CLAUDE.md "범위 밖"의 "백엔드 장애 시 자동 재시도/페일오버" 중 **페일오버를 범위 안으로** 옮긴다. 요청이 업스트림에 나간 뒤의 자동 재시도는 계속 범위 밖이다
  2. 설정은 라우트 줄의 옵션이다 (옵션은 접두사 뒤 어디에나, `=` 없는 토큰은 주 백엔드): `/api/ 127.0.0.1:18801 127.0.0.1:18802 backup=127.0.0.1:18803 failover=connect,timeout tries=3`
     - `backup=<host:port>`: 백업 백엔드. 여러 번 가능. 주 + 백업이 `PROXY_MAX_BACKENDS`(8)를 넘으면 오류, 주 백엔드가 하나도 없는 줄(백업만)은 오류
     - `failover=<목록>`: 쉼표로 나눈 `off`, `connect`, `timeout`. `off` 는 혼자서만. 빈 항목·중복·모르는 값은 오류. 없으면 `off` = 예전 동작 그대로
     - `tries=N`: 요청 하나가 시도할 백엔드 수 (첫 시도 포함). 앞자리 0 없는 10진수 1..8. 없으면 주 + 백업 전부
     - `failover=`, `tries=` 는 한 줄에 한 번, 모르는 이름·빈 값·`=` 로 시작하는 토큰은 오류 (기존 `경로:줄: 이유` 형식)
     - `failover=off`(또는 없음)이면 `backup=` 은 오류가 아니지만 절대 쓰이지 않고, `tries=` 는 효과가 없다
  3. `connect`: connect 실패 (`proxy_connect_start` 가 -1 이거나 `proxy_connect_result` 가 0 이 아님) → 다음 백엔드. `timeout`: connect 가 `PROXY_CONNECT_TIMEOUT_MS`(기본 1초, `UPSTREAM_TIMEOUT_MS` 가 1초 이하로 줄어 있으면 그 절반, `#ifndef`, `server.h`) 안에 안 끝남 → 그 fd 를 닫고 다음 백엔드. connect 시계는 라우트에 `timeout` 이 있을 때만 잰다 (없으면 예전처럼 전체 마감 하나). `0 < PROXY_CONNECT_TIMEOUT_MS < UPSTREAM_TIMEOUT_MS` 를 빌드에서 검사한다 (0 이하면 모든 connect 가 시작하자마자 시간 초과. 기본값 파생 덕에 `-DUPSTREAM_TIMEOUT_MS=1000` 하나만 줘도 빌드되고, `UPSTREAM_TIMEOUT_MS` 가 2 미만이면 #error)
  4. **요청 바이트를 업스트림에 쓰기 전(`CONN_UP_CONNECTING`)에만** 넘긴다. `CONN_UP_SENDING` 이후 실패는 예전 그대로 502/504/연결 종료. 그래서 POST 를 포함한 같은 요청이 두 백엔드에서 실행되지 않는다
  5. 전체 마감은 그대로다: `up_start_ms` 는 첫 시도에서 한 번만 잡고 넘어가도 다시 잡지 않는다. 넘어가는 중에도 `UPSTREAM_TIMEOUT_MS` 가 지나면 504. 새 시도를 열기 전, 요청을 다시 쓴 직후(connect 직전), 연결이 끝난 뒤 첫 write 직전(`up_on_event`: 넘었으면 요청을 쓰지 않고 504, 연결된 백엔드라 죽은 표시 없음), 상태 코드(431·502)를 정하기 전에도 이 마감을 본다 (이미 넘었으면 다음 connect 를 열지 않고 502·431 대신 504. 페일오버로 넘길 수 없는 connect 실패의 최종 상태 코드를 정하는 자리(`up_connect_failed`)에서도 다시 보므로 마감이 지난 뒤의 502 는 없다. 재작성 중에 마감이 지나도 connect 를 열지 않아 멀쩡한 백엔드를 죽었다고 표시하지 않는다). 시도별 connect 시계(`timeout`)는 그 직전 검사에 쓴 같은 시각에서 시작하고(재작성 시간은 전체 마감에만 든다), poll 이 연결 이벤트를 늦게 돌려줘도 그 마감을 넘긴 시도는 연결됐다고 받지 않고 시간 초과로 다룬다. 다음 poll 대기 시간은 가장 이른 마감을 절대 시각으로 들고 있다가 poll 직전에 now 를 다시 읽어 빼므로(`expire_idle` → `poll_timeout_ms`), 만료 패스 안에서 한 연결의 페일오버가 쓴 시간이 다른 연결의 마감 대기에 더해지지 않는다
  6. 더 시도할 수 없을 때 (`tries` 소진, 후보 없음): 마지막 실패가 connect 오류면 502, connect 시간 초과면 504
  7. 선택 순서 (한 요청 안): 주 백엔드를 Round-Robin 으로 (죽은 표시가 없고 이 요청에서 안 해 본 것), 주가 남지 않으면 백업을 적은 순서로 (같은 조건). 라우트의 백엔드가 **전부**(이미 시도한 것도 센다) 죽은 표시 중이면 표시를 무시하고 같은 순서로 안 해 본 것을 고른다 (살아난 백엔드를 기다리지 않고 찾는다). 하나라도 죽지 않았는데 남은 후보가 죽은 표시뿐이면 후보 없음이다. `route->next` 는 주 백엔드끼리만, 요청 시작에 한 칸 (페일오버가 꺼져 있으면 예전 순서 0, 1, ..., n-1, 0 과 같다)
  8. 수동 건강 검사("잠깐 기억"): 페일오버가 켜진 라우트에서 connect 오류·시간 초과가 난 백엔드를 `now + PROXY_BACKEND_DOWN_MS`(10초, `#ifndef`)까지 죽은 것으로 표시한다. 시간으로만 풀린다. `failover=off` 면 아무것도 표시하지 않는다
  9. 선택·표시는 `proxy_config.c` 의 순수 함수다 (시계 없이 `now_ms` 를 받는다): `proxy_rr_begin`, `proxy_pick_backend`, `proxy_mark_down`. `proxy_next_backend` 는 없앴다 (고르는 쪽이 이제 요청마다 tried 와 시각을 알아야 한다)
- **구현 중 정한 것**:
  - I1. 죽은 표시는 켜진 `failover` 종류와 상관없이 붙인다 (`failover=timeout` 만이어도 connect 거절이 난 백엔드는 표시된다. 그 요청은 502 이지만 다음 요청부터는 건너뛴다). "페일오버가 켜져 있고 connect·시간 초과 실패가 났다" 를 그대로 읽은 것
  - I2. `tries` 한도는 호출자(`server.c`)가 센다. `proxy_pick_backend` 는 `tries` 를 모르고 `tried_mask` 로 중복만 피한다. 로더는 `tries` 를 백엔드 수로 자르지 않는다 (`tries=5` 에 백엔드 3 이면 후보가 없어 3번에서 끝난다)
  - I3. 넘어갈 때 요청을 새 백엔드용으로 다시 쓴다 (`Host` 가 백엔드 이름이다). `proxy_build_request` 가 `c->req`(입력 버퍼 안을 가리킴)를 읽으므로 프록시로 간 요청의 `in` 은 연결이 닫힐 때까지 consume 하지 않는다. 프록시 중에는 클라이언트 입력을 읽지 않고 응답 뒤 항상 lingering close 가 `in` 을 비운다. 새 이름 때문에 다시 쓴 머리가 `HTTP_MAX_HEADER` 를 넘으면 결정 6(11-23)대로 431
  - I4. `proxy_connect_start` 가 동기적으로 실패한 시도도 실패 한 번으로 세고 이벤트 루프를 거치지 않고 바로 다음 후보로 간다 (반복은 `tries` 와 후보 수로 끝난다)
  - I5. 동기 실패 중 이쪽 문제(`socket()` 의 EMFILE·ENFILE·ENOBUFS·ENOMEM, 논블로킹 설정 실패, connect 의 `EADDRNOTAVAIL` = 리눅스에서 임시 포트가 바닥남)는 `PROXY_CONNECT_LOCAL`(-2)로 구별해 돌려준다. 백엔드 탓이 아니므로 그 백엔드에 죽은 표시를 붙이지 않는다 (시도 횟수에는 센다). 그 밖의 동기 connect 실패(`ENETUNREACH`, `ECONNREFUSED` 등, -1)는 표시한다
  - I6. 응답 시작 전 전체 마감에 걸렸을 때 아직 `CONN_UP_CONNECTING` 이었으면(connect 가 마감까지 안 끝남) 그 백엔드에도 죽은 표시를 붙인다. `failover=connect` 만 켠 라우트는 connect 시계가 없어 이 길로만 표시가 붙고, 안 붙이면 그 백엔드로 가는 요청마다 5초를 기다려 504 를 받는다. 마감을 발견하는 두 자리(`expire_idle`, `up_on_event`)는 같은 판단(`up_connect_state`)을 쓴다: `SO_ERROR` 가 0 이 아니거나(실패) `getpeername` 이 `ENOTCONN`(아직 진행 중)이면 표시하고, `getpeername` 이 성공하면(연결은 끝났고 POLLOUT 을 늦게 처리했을 뿐) 표시하지 않는다 (멀쩡한 백엔드는 죽은 것이 아니다)
  - I7. `failover=off` 에서 `proxy_pick_backend` 의 후보는 `rr_start` 번째 주 백엔드 하나뿐이다 (이미 시도했거나 주 백엔드가 없으면 -1). 백업을 고르지 않는다는 계약과 코드가 같다
- **이유**: `tries` 는 모든 백엔드가 죽었을 때 응답이 끝없이 늦어지는 것을 막는다 (기본은 "다 해 본다"이고 줄이고 싶을 때만 쓴다). 요청을 보내기 시작한 뒤에는 백엔드가 요청을 이미 실행했을지 모르므로(특히 POST) 넘기지 않는다. connect 시간 초과만 따로 둔 것은 연결이 멈춘 백엔드(SYN 이 버려짐)가 전체 5초를 다 쓰게 하지 않기 위해서다. 죽은 표시를 10초 기억하는 것은 nginx 의 `max_fails`/`fail_timeout` 과 같은 발상이다: 요청마다 죽은 백엔드에 connect 를 시도하고 기다리지 않는다. 옵션을 `backup=` 처럼 `key=value` 로 둔 것은 기존 토큰(`host:port`)과 겹치지 않고 앞으로 옵션을 늘릴 때 설정 형식이 안 깨지기 때문이다
- **영향**: `proxy_config.h/.c` (옵션 읽기, 선택 함수), `proxy.h/.c` (주석), `server.h` (`PROXY_CONNECT_TIMEOUT_MS`), `server.c` (A 트랙 건호님 파일: UP_CONNECTING 의 재시도, connect 시계, `in` consume 시점. 리뷰 필요), `tests/unit/test_proxy_config.c`, `tests/unit/test_contract.c` (`proxy_next_backend` 시험을 새 함수로 옮김), `include/http_request.h` (포인터 수명 계약의 프록시 예외), `tests/run.sh`, `tests/tools/fake_upstream.c` (`host`, `blackhole` 모드, 요청 기록 파일 인자, 기록 파일은 ready 전에 열고 쓰기 실패 시 종료, blackhole 의 시스템 호출 오류는 unsupported 가 아니라 시작 실패), `docs/api.md`, `docs/interfaces.md`, `CLAUDE.md`, `README.md`

## 2026-11-23 · 업스트림 중계 방식 (이슈 #9)

- **결정** (상철 판단, 11-23 설계 문답):
  1. 업스트림 응답은 머리(상태 줄 + `Content-Length`)만 해석하고 받은 바이트를 그대로 중계한다 (머리를 고치지 않음). 백엔드에는 늘 `Connection: close` 를 보내므로(#8) 프록시 응답 뒤 클라이언트 연결은 항상 닫는다
  2. "응답 시작" = 머리 해석이 끝나 상태 줄 검사를 통과하고 첫 업스트림 바이트가 클라이언트 `out` 에 들어간 때. 그 전 실패는 502 (깨진·끊긴 머리, connect 실패, 송신 실패, EOF) 또는 504 (시간 초과), 그 뒤 실패는 `out` 에 있는 것까지 보내고 닫는다. 재시도 없음
  3. 요청이 `HEAD` 이거나 상태가 1xx·204·304 이면 `Content-Length` 가 있어도 본문이 없다 (우리 `http_response.c` 가 HEAD 응답에 GET 의 길이를 쓴다). 그래서 요청 메서드를 기억한다
  4. `UPSTREAM_TIMEOUT_MS`(5초) 는 시계가 둘이다. (a) 시작 전: 요청을 프록시에 넘긴 때부터 머리가 다 올 때까지, 넘으면 504. (b) 시작 후: 업스트림을 읽으려고 기다리는 동안 바이트가 5초 안 오면 닫기. 역압력으로 쉰 시간은 세지 않고, 느린 클라이언트는 `CLIENT_IDLE_TIMEOUT_MS`(10초, 클라이언트 쓰기가 진행되면 리셋)가 맡는다
  5. 업스트림 fd 의 POLLOUT·POLLERR·POLLHUP 는 connect 중에는 `getsockopt(SO_ERROR)` 로만 판단한다 (0 이 아니면 502). 클라이언트 fd 는 기존대로 POLLERR 면 닫는다
  6. `proxy_build_request` 가 -1 이면 431. `PARSE_DONE` 뒤에는 다른 원인을 파서·설정 로더가 막으므로 현실적인 원인은 "다시 쓴 머리가 `HTTP_MAX_HEADER` 초과"와 메모리 부족이고, 앞의 것에 맞춰 431 로 답한다 (코드 주석에도 적음)
- **구현 중 정한 것 (11-23 상철 확인)**:
  - C1. 1xx 는 깨진 머리로 본다 → 502 (요청에서 `Expect`·`Upgrade` 를 빼므로 올 일이 없다). 그래서 결정 3 의 "1xx 는 본문 없음"은 코드에서 이 502 가 먼저 걸린다
  - C2. `Transfer-Encoding` 이 있으면(값 무관) 업스트림 EOF 까지가 본문이다. `Content-Length` 는 무시하고 바이트는 그대로 넘긴다 (chunked 틀도 그대로 지나감)
  - C3. 중계 상한 `PROXY_RELAY_MAX` 64KB: 클라이언트 `out` 대기가 이보다 적을 때만 업스트림을 읽는다
  - C4. 머리 문법은 엄격하게: CRLF 줄 끝(맨 LF·맨 CR 은 502), obs-fold 502, 빈 줄까지 8192바이트 초과 502, 상태 줄 `HTTP/1.` 숫자 + 세 자리 100..599
  - C5. `Content-Length` 는 앞뒤 SP/TAB 을 뗀 숫자만 (부호·쉼표 목록 불가), 2^53 이나 `size_t` 초과는 502, 줄이 여러 개면 값이 같아야 한다 (아니면 502)
  - C6. 응답 시작 전에는 클라이언트 fd 를 `events = 0` 으로 두고 (끊김만 감지), 시작 후 `out` 에 바이트가 있을 때만 POLLOUT. 프록시 중에는 클라이언트 입력을 읽지 않는다 (파이프라이닝된 바이트는 lingering close 가 버린다)
  - C7. `X-Forwarded-For` 값은 accept 때 `getpeername` + `getnameinfo(NI_NUMERICHOST)` 로 `conn.client_ip` 에 저장한다. `::ffff:a.b.c.d` 는 `a.b.c.d` 로, 링크 로컬의 `%인터페이스` 꼬리는 떼고, 주소를 못 얻으면 `unknown`
  - C8. `UPSTREAM_TIMEOUT_MS` 를 `#ifndef` 로 바꿔 `-D` 로 줄여 빌드할 수 있게 했다 (느린 응답을 손으로 보는 용도, `tests/run.sh` 맨 아래 주석)
  - C9. Round-Robin: `proxy_next_backend` 가 라우트마다 `next` 를 돌려 0, 1, ..., n-1, 0. 이슈 #9 의 "RR 은 마지막, 늦으면 뺀다" 에 맞춰 이 함수 하나만 되돌리면(항상 `backends[0]`) 빠지게 작은 변경으로 뒀다
  - 코드가 정한 세부: 시작 후 업스트림 유휴 마감과 업스트림 EOF·오류(`TRUNCATED`)에서도 `out` 에 대기한 바이트는 보낸 뒤 닫는다. 시작 전 마감이 이미 지났으면 늦게 완성된 머리도 504 로 답한다
- **이유**: 머리를 고쳐 쓰지 않으면 해석할 것이 상태 줄과 길이뿐이라 코드와 검증 범위가 작다. 연결을 항상 닫으면 응답 끝을 길이나 EOF 로만 알면 되고, 다음 요청과 섞이지 않는다. 시작 전에는 클라이언트에게 아직 아무것도 안 나갔으므로 정확한 오류 코드를 줄 수 있고, 시작 뒤에는 이미 나간 응답을 바꿀 수 없어 닫는 것밖에 없다. 시계를 둘로 나눈 것은 전체 전송이 오래 걸려도 바이트 사이가 짧으면 정상이고, 클라이언트가 느린 것은 업스트림 탓이 아니기 때문이다
- **영향**: `proxy.h/.c`, `proxy_config.h/.c`, `server.h`, `server.c` (A 트랙 건호님 파일: 업스트림 상태 3개, pollfd 둘, 시간 초과 처리. 지역 제안이라 리뷰 필요), `Makefile`, `tests/run.sh`, `tests/tools/fake_upstream.c`, `tests/unit/test_proxy_relay.c`, `tests/unit/test_contract.c`, `docs/interfaces.md`, `docs/api.md`, `CLAUDE.md`, `README.md`. 이슈 #9

## 2026-11-23 · 경로 정규형 하나로 라우팅·전달, `--merge-slashes` (이슈 #31)

- **결정**:
  1. #31 은 2안을 고친 형태로 간다. 파서가 경로의 **정규형**을 만들고, 프록시·앱 라우팅과 백엔드 전달이 모두 이 정규형을 쓴다. 백엔드에는 원본 `target` 대신 `정규형 경로` + (있으면) `?` + 원본 query 를 보낸다. 정적 파일은 정규형을 끝까지 디코딩한 `path` 를 쓴다
  2. 정규형 만드는 순서 (하나라도 걸리면 400)
     1. `%XX` 문법 오류 (`%G1`, 끝이 잘린 `%2`) — 지금과 같음
     2. 인코딩 안 된 `\`, 그리고 디코딩하면 `/`, `\`, NUL·제어 문자가 되는 `%XX` (`%2F`, `%5C`, `%00` …)
     3. 빈 조각 `//`: **기본 400**, `--merge-slashes` 를 주면 `/` 하나로 합친다. 끝 `/`(`/a/`)는 빈 조각으로 치지 않는다
     4. 비예약 문자(`A-Z a-z 0-9 - . _ ~`)의 `%XX` 만 디코딩한다 (`%61` → `a`, `%7E` → `~`). 나머지 `%XX` 는 그대로 두고 16진수만 대문자로 (`%eb` → `%EB`)
     5. `.`·`..` 조각은 400 (PR #27 규칙 그대로. 정리하지 않으므로 3 과 순서가 얽히지 않는다)
  3. 디코딩은 한 번만 한다. `%252F` 는 정규형에서도 `%252F` 이고 `path` 에서는 글자 `%2F` 다
  4. 플래그 이름은 nginx 지시어 `merge_slashes` 를 따른다. **기본값은 nginx(켜짐)와 반대로 꺼짐(400)**. `getopt_long` 은 POSIX 가 아니라서 `main.c` 가 `getopt` 루프 **전에** argv 에서 이 긴 옵션을 빼낸다 (그대로 두면 glibc `getopt` 가 `-`, `m`, `e` 를 거쳐 `-r ge-slashes` 로 읽어 문서 루트를 조용히 덮어씀, 2026-11-23 확인). 파서에는 인자를 하나씩 늘리지 않고 **파서 옵션 구조체**로 넘긴다. 지금 필드는 `merge_slashes` 하나이고, 이후 옵션(#35 `--backslash`)은 필드만 늘리면 시그니처가 바뀌지 않는다
  5. `\` 를 `/` 로 바꿔 처리하는 방식(Envoy/Istio 정규화, IIS)을 고르는 `--backslash=reject|slash` 는 마감 후 #35 로 미룬다. "그대로 통과" 선택지는 두지 않는다
- **이유**:
  - 판정한 경로와 백엔드가 받는 경로가 같아야 백엔드가 같은 바이너리가 아니어도 해석이 갈리지 않는다. nginx 처럼 "정리한 경로로 판정 + 원본 전달"이면 백엔드가 원본을 자기 방식으로 다시 파싱한다 (nginx + PHP-FPM `cgi.fix_pathinfo` 로 `/uploads/a.jpg/x.php` 가 PHP 로 실행된 사례). Envoy 가 정규형을 전달하는 것과 같은 방식 (`\` 처리만 다름, 아래)
  - 원본을 보존할 이유는 `%2F` 뿐이었는데 2-2 에서 거절하므로 정규형 전달로 잃는 것이 없다. 남는 차이(`%61`↔`a`, `%7E`↔`~`)는 RFC 3986 2.3 에서 같은 URI 다
  - 1안(인코딩된 비예약 문자도 400)은 같은 URI 를 거절하고, `~` 를 `%7E` 로 보내는 클라이언트(Java `URLEncoder` 등)를 막아서 뺐다
  - `%2F` 는 구분자인지 데이터인지 서버마다 해석이 갈린다. Tomcat(400)·Apache(404) 기본값처럼 거절한다
  - `\` 는 Windows 기반 서버(IIS, nginx Windows 빌드)와 Envoy 정규화가 `/` 로 읽는다. `%5C` 만 막고 그냥 `\` 를 통과시키면 같은 우회가 인코딩만 빼고 들어온다. RFC 3986 `pchar` 에 없는 글자라 정상 클라이언트는 보내지 않으므로, Tomcat 기본값(`ALLOW_BACKSLASH=false`)처럼 둘 다 거절한다. 이 부분은 Envoy(`/` 로 바꿈)와 다르다 (PR #34 리뷰)
  - `//` 는 RFC 상 `/` 와 다른 경로라서 기본은 거절하고, 합칠지는 nginx·Apache 처럼 운영자가 고른다
- **영향**: PR #18 결정 2(`app_match` 는 원본 `target`), 이슈 #8 결정 3(원본 `target` 그대로 전달), `docs/interfaces.md` "프록시 경로 매칭"·"앱 경로" 를 바꾼다. 구현은 #31: `http_request.h/.c`, `app.h/.c`, `proxy_config.h/.c`, `proxy.c`, `main.c`(`--merge-slashes`), `docs/interfaces.md`, `docs/api.md`, 테스트 (`//a` 기본 400·플래그로 통과, `/api/a%2Fb` 400, `/a\b`·`/a%5Cb` 400, `/%61pi/add` → 앱, `/docs/%2E%2Ex` 통과). 구현 전까지 코드는 원본 `target` 으로 라우팅·전달한다

## 2026-11-21 · 경로의 `.`·`..` 조각은 파서가 400 (PR #26 리뷰)

- **결정**: 파서가 디코딩한 `path` 에 `.` 이나 `..` 조각이 있으면 400. 평문과 `%2E%2E`, `%2F` 로 만든 조각 모두 같은 곳에서 거절한다. `static_open`(#22) 의 글자 정리와 `realpath` 검사는 그대로 둔다
- **이유**: 라우팅은 원본 `target` 으로 하고(PR #18) 정적 파일은 정리한 경로로 연다. 그래서 `/public/../admin/x` 는 `/admin` 프록시 라우트에 안 걸리고 프런트 docroot 의 `/admin/x` 를 열었다 (혜성 재현). 라우팅 전에 한 곳(파서)에서 막아야 점 조각 때문에 프록시·앱·정적 판정이 갈리지 않는다 (빈 조각·퍼센트 인코딩으로 생기는 차이는 남음, `docs/interfaces.md` "경로의 점 조각", #31). 파서가 path 를 직접 디코딩하므로 `%2e%2e` 도 여기서 같이 잡힌다. curl·브라우저는 `..` 를 보내기 전에 지우므로 (#3 재현 ②) 정상 요청에는 영향이 없다. 검토한 다른 안: 문서로만 금지(설정 실수를 막지 못함), `static_open` 에서 403(프록시로 가는 `/api/../secret` 은 남음)
- **영향**: `src/http_request.c`, `include/http_request.h`, `tests/unit/test_http_request.c`, `tests/unit/test_proxy.c`(R2 의 대상에서 `..` 조각을 뺌), `docs/interfaces.md`

## 2026-11-21 · 프록시 설정 형식과 요청 재작성 규칙 (이슈 #8)

- **결정**:
  1. 설정 파일: 한 줄에 `접두사 host:port...`, `#` 주석, 줄 1024바이트·접두사 127·이름 63·라우트 16·라우트당 백엔드 8, 포트 1-65535, 같은 접두사 중복과 빈 파일은 오류. 오류는 -1 + stderr `파일:줄: 이유`, cfg 는 전부 0
  2. 이름은 로드 시 한 번 해석하고 IPv4 를 먼저 고른다. IPv6 는 `[::1]:포트` 로 명시 (`AI_ADDRCONFIG` 는 쓰지 않음)
  3. 접두사를 떼지 않고 원본 `target` 을 그대로 전달한다
  4. `Content-Length` 는 클라이언트 줄을 버리고 본문 길이로 다시 쓴다 (원래 있었을 때만). `Connection` 에 적힌 이름은 클라이언트 줄에만 작용하고 프록시가 쓰는 줄(Host, X-Forwarded-For, Via, Content-Length, Connection)은 지우지 못한다
  5. `Expect` 는 항상 뺀다
  6. `proxy_build_request` 는 #9 가 아니라 #8 에서 구현한다
  7. (리뷰 반영) 포트와 숫자 IPv4 는 앞자리 0 을 금지한다 (`08001`, `127.0.0.010` 은 오류). 줄 없는 파일 전체 오류는 `파일: 이유` 로 예외. 줄 끝 CR 은 LF 앞에서만 인정. 다시 쓴 머리가 `HTTP_MAX_HEADER` 를 넘으면 `proxy_build_request` 가 -1, 깨진 헤더 줄도 파서와 같은 기준으로 -1
- **이유**: 2 주소를 하나만 저장하고 재시도가 없어서 family 가 틀리면 복구 없이 502 가 된다. #7 `net_listen`(PR #25)은 dual-stack(`IPV6_V6ONLY=0`)이고 IPv6 를 못 쓰면 IPv4 만 연다. 어느 쪽이든 IPv4 loopback 은 받으므로 IPv4 가 안전하고, ::1 을 고르면 IPv4 만 연 백엔드에서 거절된다 (노션 #3 에서 Tiny 의 `open_listenfd` 가 0.0.0.0 만 묶은 것도 같은 경우). 4 RFC 9110 7.6.1 은 중개자가 Connection 에 적힌 필드를 지우라고 하고, 메시지 길이는 홉마다 보내는 쪽이 정한다 (RFC 9112 6.3). 본문이 있는데 길이가 없는 메시지는 만들지 않는다. 5 본문이 이미 전부 버퍼에 있어 기다릴 것이 없고, 전달하면 백엔드의 `100 Continue` 를 #9 가 건너뛰어야 한다. 4·5 는 PR #24 에서 #9 로 미룬 두 항목이다. 7 glibc `getaddrinfo` 는 앞자리 0 을 8진수로 읽어 `127.0.0.010` 을 127.0.0.8 로 풀기 때문에 검사한 값과 저장한 주소가 달라지고, 포트 토큰은 그대로 Host 헤더 값이 되므로 표기를 하나로 둔다. 프런트 한도에 꽉 찬 요청은 Host·XFF·Via·Connection 줄이 붙으면 백엔드의 8192바이트 한도를 넘는다
- **영향**: `proxy_config.h/.c`, `proxy.h/.c`, `docs/interfaces.md`, `CLAUDE.md`, `tests/unit/test_proxy_config.c`, `tests/unit/test_proxy.c`. #7 (백엔드 인스턴스는 IPv4 loopback 을 받아야 함), #9 (요청 생성은 끝났고 연결·중계가 남음)

## 2026-11-21 · 서버 모듈 연결·keep-alive·요청 타임아웃·lingering close (이슈 #7 두 번째 PR)

결정
1. `handle_request` 는 `http_parse_request` → `handler_route` → (정적) `static_open` + `http_append_header` 순서로 응답을 만들고, 응답을 다 만든 뒤에 `buffer_consume` 한다. 파일 본문은 `CONN_SENDING_FILE` 에서 `static_send_some` 으로 나눠 보내고, 다 보내면 바로 `static_close` 한다 (keep-alive 에서 다음 `static_open` 이 fd 를 덮어쓰지 않게, PR #25 혜성 리뷰)
2. 파서 `NEED_MORE` 의 `consumed` 가 0 이 아니면 `need_len` 에 기억하고 `buffer_pending(&in)` 이 그만큼 찰 때까지 파서를 부르지 않는다. DONE·ERROR 에서 지운다 (PR #24 계약)
3. keep-alive·파이프라이닝: 응답을 끝까지 보낸 뒤 `in` 에 남은 바이트가 있으면 새 바이트를 기다리지 않고 바로 다음 요청을 처리한다. 응답을 보내는 동안은 읽지 않는다
4. 요청 타임아웃 `CLIENT_REQUEST_TIMEOUT_MS` = 30초 (요청의 첫 바이트부터 완성까지). 넘으면 응답 없이 닫는다
5. 연결을 닫는 응답은 `shutdown(SHUT_WR)` 후 `CONN_LINGERING` 에서 남은 입력을 읽어 버리다가 EOF 나 2초(`CLIENT_LINGER_TIMEOUT_MS`)에 닫는다
6. 파서 오류(400, 413, 414, 431, 501, 505)는 `keep_alive = 0` 으로 오류 응답 후 닫는다. HEAD 면 본문을 쓰지 않는다 (`req.method == HTTP_HEAD`. 요청 라인 전에 난 431 등은 method 가 기본값 GET 이라 본문까지 씀). 앱 스텁(-1)은 500, 프록시 경로는 502 로 임시 처리
7. 파일 크기가 `LONG_MAX` 를 넘으면 500 (`http_response_head.content_length` 가 `long`)

이유
- 유휴 타이머는 바이트가 올 때마다 리셋돼서, 헤더나 본문을 1바이트씩 보내는 클라이언트는 연결 하나를 무기한 붙잡을 수 있다. 30초는 1MB 본문 상한을 약 35KB/s 로도 받을 수 있는 값
- 읽지 않은 입력을 남긴 채 `close` 하면 커널이 RST 를 보내고, 클라이언트는 아직 읽지 않은 응답(413, 431 등)을 버릴 수 있다 (RFC 9112 9.6). Linux 루프백에서는 lingering 없이도 재현되지 않았지만(20/20) 네트워크·OS 에 따라 달라서 표준 방식을 따른다
- 파서 오류는 요청 경계를 믿을 수 없어 다음 요청을 이어 처리할 수 없다. HEAD 를 파서가 읽기 전에 난 오류는 본문이 붙지만, 응답 뒤에 닫으므로 다음 응답과 섞이지 않는다. ERROR 때 `method` 동작은 `http_request.h` 계약에 아직 없어 PR #29 에서 상철님께 확인 중
- `long` 이 32비트인 환경에서 2GB 넘는 파일은 Content-Length 가 잘려 응답 경계가 어긋난다

영향
- `src/server.c`, `include/server.h` (`CLIENT_REQUEST_TIMEOUT_MS`, `CLIENT_LINGER_TIMEOUT_MS`), `docs/interfaces.md` 상태·타임아웃 표, `tests/run.sh` (정적 200·1MB·404·403(링크)·`..` 차단(403 또는 #27 뒤 400)·405·431·413·HEAD·keep-alive·1.0/1.1 기본 Connection·파이프라이닝(한꺼번에, 반쪽)·400·파서 오류 HEAD)
- #9 는 `respond()` 의 `ROUTE_PROXY` 분기, #10 은 `ROUTE_APP` 의 500 대체 처리를 걷어 낸다

## 2026-11-21 · 서버 CLI·listen 소켓·종료 방식 (이슈 #7 첫 PR)

- **결정**:
  1. CLI 는 `./webserver -p <port> -r <docroot> [-c <proxy.conf>]`. 포트는 1–65535 10진수, docroot 는 존재하는 디렉터리여야 한다. 아니면 서버를 띄우지 않고 exit 1. `-c` 가 있으면 시작할 때 `proxy_config_load` 하고 실패하면 exit 1 (#8 구현 전에는 항상 실패)
  2. listen 소켓은 **IPv6 소켓 하나에 `IPV6_V6ONLY=0` 을 명시**해서 IPv4 도 받는다. IPv6 를 쓸 수 없으면(커널·컨테이너에서 꺼짐) IPv4 로 물러난다. 단 `EADDRINUSE`·`EACCES` 는 물러나지 않고 실패한다
  3. `SIGINT`/`SIGTERM` 은 self-pipe 로 poll 을 깨운다. 받으면 listen 소켓 닫기 → 모든 연결 정리 → `server_run` 0 반환 → exit 0. 보내던 응답을 마저 보내지는 않는다
  4. `accept` 가 fd 고갈(`EMFILE` 등)로 실패하면 종료하지 않고 100ms 동안 listen 소켓 감시를 멈춘다. 연결이 하나 닫히면 바로 재개
  5. `tests/run.sh` 는 서버를 백그라운드로 띄우고 curl 이 성공할 때까지 최대 5초 기다린다. 종료는 감시 프로세스로 제한 시간을 잰다 (macOS 에 `timeout` 없음)
- **이유**: 2 `net_listen` 이 fd 하나를 돌려주는 계약을 유지하면서 `localhost` 가 `::1` 로 풀리는 환경도 받기 위해 (Tiny 는 0.0.0.0 하나만 열어 `::1` 접속이 거부됨, #3). `IPV6_V6ONLY` 기본값은 OS 마다 다르다. 포트 충돌일 때 IPv4 로 물러나면 "IPv6 는 다른 프로그램이 쓰는데 IPv4 로 떠 버리는" 상황이 가려진다. 3 플래그만 쓰면 "플래그 검사 → poll 진입" 사이에 온 시그널을 놓친다. 4 그대로 두면 대기열의 연결 때문에 poll 이 계속 깨어나 CPU 를 다 쓴다
- **영향**: `src/main.c`, `src/net.c`, `src/server.c`, `tests/run.sh`, Makefile `run`, README, PR 템플릿, CLAUDE.md. 이슈 #7 (keep-alive·모듈 연결은 다음 PR), #8 (`-c`)

## 2026-11-21 · 본문 대기 중인 NEED_MORE 는 필요한 총 길이를 알려 준다 (PR #24)

- **결정**: `http_parse_request` 가 헤더는 다 받고 본문만 덜 받아 `NEED_MORE` 를 돌려줄 때 `*consumed` 에 요청 전체 길이(`head_len + body_len`)를 넣는다. 헤더가 아직이면 지금처럼 0. 서버(#7)는 이 값을 기억해 `in` 이 그만큼 찰 때까지 파서를 다시 부르지 않는다. 시그니처는 그대로
- **이유**: 파서가 상태를 저장하지 않아서, 본문이 1 바이트씩 오면 바이트마다 헤더 전체를 다시 파싱한다(합계 ≈ 헤더 길이 × 호출 횟수). 헤더 쪽은 8KB 상한으로 묶이지만 본문은 최대 1MB 다. 바이트마다 유휴 타이머도 리셋돼 타임아웃으로도 끊기지 않는다 (상철 계산 + 건호 리뷰)
- **영향**: `include/http_request.h`, `src/http_request.c`, `tests/unit/test_http_request.c`, `docs/interfaces.md`, `CLAUDE.md` 계약, #7 `server.c` 의 파서 호출부 (비교 기준은 `buffer_pending(&in)`, DONE·ERROR 에서 기억한 값을 지움)
- **메모**: `Expect: 100-continue` 는 #9 뿐 아니라 프록시를 안 거치는 `POST /api/echo`(#10) 에도 해당한다. curl 은 1초 기다린 뒤 본문을 보내서 멈추지는 않고 1초 지연이 생긴다 (건호 확인). 지금은 처리하지 않는다

## 2026-11-21 · 정적 파일 경로 판정 (PR #22, 이슈 #6)

- **결정**:
  1. `static_open` 은 `.`·`..`·`//` 를 글자 단위로 먼저 정리하고, 루트 위로 가면 파일 존재와 상관없이 403
  2. 그다음 `realpath` 결과가 docroot 밖이면(심볼릭 링크) 403. 디렉터리는 끝 `/` 가 없어도(`/docs`) 리다이렉트 없이 `index.html`(없으면 404), `/파일/` 은 404, 일반 파일이 아니면 403, docroot 를 못 찾으면 500
  3. `realpath`(XSI 확장)를 쓰는 파일은 맨 위에 `#define _XOPEN_SOURCE 700` 을 둔다. 공통 플래그 `-D_POSIX_C_SOURCE=200809L` 은 그대로
- **이유**: 1 Tiny `parse_uri` 가 `/../secret.txt` 를 200 으로 열었음 (#3). 존재 여부로 403/404 가 갈리면 docroot 밖 파일이 있는지 알려 주게 됨. 2 글자 정리만으로는 docroot 안의 링크가 밖을 가리키는 경우를 못 막음. `/docs` 에 301 → `/docs/` 를 하지 않아 그 페이지의 상대 링크는 `/` 기준으로 풀린다. 지금 `www/` 는 절대 경로만 써서 괜찮고, 필요해지면 후속 (건호 리뷰). 3 musl·clang 은 `_POSIX_C_SOURCE` 만으로 `realpath` 선언을 보여 주지 않아 CI 가 실패함 (glibc gcc 는 통과해서 로컬에서 못 잡음)
- **영향**: `src/static_file.c`, `include/static_file.h`, `docs/interfaces.md` "정한 세부 규칙", `tests/unit/test_static_file.c`, `www/`. 시그니처는 그대로. 파서(#5)는 디코딩한 path 를 넘기고 NUL·제어 문자는 400 으로 막는다 (상철 확인)

## 2026-11-21 · 응답 헤더 규칙 4가지 (PR #21, 이슈 #6)

- **결정**:
  1. `http_append_header` 의 줄 순서를 상태 줄, `Date`, `Server`, `Content-Type`, `Content-Length`, `Connection`, `extra_headers`, 빈 줄로 고정. 상태 코드가 100~599 밖이면 -1
  2. `content_length < 0` (길이 모름) 이면 `Content-Length` 를 생략하고 `keep_alive` 와 상관없이 `Connection: close`
  3. `extra_headers` 는 `NULL`, `""`, 또는 CRLF 로 끝나는 줄들만 (빈 줄·짝 없는 CR·LF 금지). `content_type` 에 CR·LF 가 있어도 -1
  4. 오류 본문은 상태 코드와 문구만 (`<html><body><h1>404 Not Found</h1></body></html>`). 요청 내용은 넣지 않는다
- **이유**: 1 응답 모양이 정해져야 테스트가 필요한 줄을 찾을 수 있음. `Date` 때문에 전체 바이트 비교는 못 함. 2 길이가 없으면 클라이언트가 응답 끝을 연결 종료로만 알 수 있음. 3 빈 줄·짝 없는 LF 가 들어가면 헤더가 거기서 끝나 응답 분할(keep-alive 다음 응답까지 밀림). 4 Tiny `clienterror` 가 요청 경로를 본문에 넣어 반사형 XSS (#3)
- **영향**: `src/http_response.c`, `include/http_response.h`, `docs/interfaces.md` "정한 세부 규칙", `tests/unit/test_http_response.c`. 시그니처는 그대로. 서버(#7)·프록시(#9)는 고정 문자열·`mime_type()` 상수만 넘기므로 영향 없음 (건호 확인)

## 2026-11-20 · 프로젝트 노션 주소 변경

- **결정**: 프로젝트 노션 주소를 https://app.notion.com/p/jungle-webserver-41efacdfe6e480e59bc8e888ca466fb6 ("jungle-webserver" 페이지) 로 바꾼다. 아래 "프로젝트 노션 연결" 항목의 이전 주소는 더 이상 쓰지 않는다. 사용 범위(이 페이지와 하위 페이지만, 쓰기 전 확인)는 그대로
- **이유**: 노션 페이지 주소가 바뀜. 새 주소로 페이지가 열리는 것을 확인함
- **영향**: CLAUDE.md 노션 절·"아직 하지 않은 것", README

## 2026-11-20 · 인터페이스 리뷰 반영 (PR #18)

- **결정**:
  1. 431 을 가장 먼저 판정한다 (앞 8KB 안에 `\r\n\r\n` 이 없으면 431)
  2. `app_match` 는 `proxy_match` 와 같이 디코딩 전 `target` 을 본다. 정적 파일만 디코딩한 `path`
  3. 프록시는 클라이언트 `Connection` 값에 적힌 헤더 이름도 뺀다 (RFC 7230 6.1)
  4. 클라이언트가 보낸 `X-Forwarded-For` 는 버리고 새로 쓴다
  5. `http_append_error` 에 `extra_headers` 인자를 추가하고, 405 `Allow` 는 호출자가 넣는다
  6. 디코딩한 경로의 NUL·제어 문자(0x01-0x1F, 0x7F)·잘못된 `%XX` 는 400
  7. `buffer_mark`/`buffer_rollback` 추가. 실패 시 `len` 을 직접 복원하지 않는다
- **이유**: 1 TCP 분할에 따라 414/431 이 갈림 (상철). 2 `/%61pi/add` 가 프록시를 건너뛰고 앱으로 감 (상철 지적, 혜성 제안). 3·4 표준 준수, 클라이언트가 지어낸 값 차단 (상철). 5 앱 오류에 `X-Instance` 가 없고 echo 의 405 `Allow` 가 틀림 (혜성). 6 `static_open` 이 믿을 보장이 헤더에 없었음 (혜성). 7 덧붙이는 도중 compact 되면 저장한 `len` 이 어긋남 (혜성)
- **영향**: `http_request.h/.c`, `app.h/.c`, `proxy.h/.c`, `http_response.h/.c`, `static_file.h`, `buffer.h/.c`, `docs/interfaces.md`, `test_buffer.c`, `test_contract.c`. 이슈 #5 #6 #9 #10

## 2026-11-20 · 모듈 인터페이스 확정안 (이슈 #4)

- **결정**: 응답·프록시 요청은 고정 배열 대신 공통 `buffer`(연결이 소유, 늘어남, `off` 가 송신 진행 상태)에 덧붙인다. `http_request` 에 원본 헤더 블록(`headers`)과 `method_str` 추가, `headers`/`body` 는 입력 버퍼를 가리키므로 처리 후 consume. 반환값은 0/-1 로 통일(결과는 buffer 에). `http_append_header` 는 `http_response_head` 구조체 + `extra_headers`. `handler_route` 가 매칭된 라우트를 돌려주고 `app_match` 분리. 프록시 매칭은 디코딩 전 경로 + 경로 경계. 타임아웃 상수(클라이언트 10초, 업스트림 5초)는 `server.h`
- **이유**: 초안대로면 프록시가 원본 헤더를 전달할 수 없고, 1MB 본문을 담을 출력 버퍼의 크기·소유자가 없고, 입력 버퍼를 당기면 body 포인터가 무효가 되고, 모듈마다 반환값 형태가 달라 연결 코드가 복잡해짐
- **영향**: `include/*.h` 전부, `src/buffer.c`(신규), 스텁 시그니처, `docs/interfaces.md`(신규), `tests/unit/test_buffer.c`, `test_contract.c`, CODEOWNERS

## 2026-11-20 · 단위 테스트·sanitizer 기반 (이슈 #4)

- **결정**: `tests/unit/test_*.c` 파일 하나가 실행 파일 하나 (main.c 를 뺀 모든 모듈과 링크). 외부 프레임워크 없이 `tests/unit/test.h` 매크로만 쓴다. `make test` 가 단위 테스트까지 돌리므로 CI 5개·docker-test 에 자동 포함. `make asan` 은 `-fsanitize=address,undefined` 로 `build/asan/` 에 따로 빌드하고, CI 에는 필수가 아닌 `ubuntu / asan+ubsan` 잡으로 추가
- **이유**: 세 트랙이 소켓 없이 각자 모듈을 검증하기 위해. sanitizer 는 마지막 날이 아니라 PR 마다 돌린다. Alpine(musl)은 ASan 을 지원하지 않아 Ubuntu 에서만. 필수 검사로 넣으면 보호 규칙 변경이 필요해서 일단 권고로 둠
- **영향**: Makefile, `tests/unit/`, `.github/workflows/ci.yml`, CLAUDE.md 검증 흐름, README

## 2026-11-20 · 프로젝트 노션 연결 (이 프로젝트 전용)

- **결정**: 프로젝트 노션은 https://app.notion.com/p/41efacdfe6e480ee9f8ccec38a3af93c (팀원 워크스페이스). 저장소 `.mcp.json` 으로 이 프로젝트에서만 Notion MCP 를 켠다. Claude는 이 페이지와 하위 페이지만 읽고 쓰며, 쓰기 전에는 사용자에게 확인
- **이유**: 노션은 이 프로젝트에서만 쓰기로 함. 같은 계정의 개인 노션을 건드리지 않기 위해
- **영향**: `.mcp.json`, CLAUDE.md 노션 절, README

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
