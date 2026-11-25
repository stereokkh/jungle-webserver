# 모듈 인터페이스 (이슈 #4)

세 트랙이 서로의 코드를 기다리지 않고 동시에 작업하기 위한 약속이다. 함수별 자세한 계약은 각 `include/*.h` 주석에 있고, 여기에는 **모듈을 어떻게 이어 붙이는지**만 적는다. 바꾸려면 관련 담당자와 합의한 뒤 헤더와 이 문서를 같은 PR에서 고친다.

## 한눈에 보기

```
 클라이언트
    │ 바이트가 들어옴
    ▼
┌─ A: server.c (poll 루프, conn 소유) ────────────────────────────────────┐
│ 1. read() → conn.in 버퍼에 덧붙임                                       │
│ 2. http_parse_request(in)             ──▶ B: http_request.c            │
│      NEED_MORE → 더 읽기 / ERROR → http_append_error / DONE → 3        │
│ 3. handler_route(req, cfg, &route)    ──▶ A: handler.c                 │
│      ├ ROUTE_PROXY  → proxy_build_request, proxy_connect_start ──▶ B   │
│      ├ ROUTE_APP    → app_handle(req, port, &out)              ──▶ C   │
│      └ ROUTE_STATIC → static_open + http_append_header         ──▶ C   │
│ 4. buffer_consume(in, consumed)  ← 응답을 다 만든 뒤에만 (프록시 제외)   │
│ 5. POLLOUT → write(out) → (정적이면) static_send_some 반복              │
│ 6. keep_alive 면 1 로, 아니면 conn_close                                │
└─────────────────────────────────────────────────────────────────────────┘
```

CS:APP Tiny의 `doit()` 한 함수가 하던 일을 나눈 것이다. A는 `main`의 반복문과 `doit`의 뼈대, B는 요청 읽기(`read_requesthdrs`, `parse_uri`)와 프록시, C는 응답 쓰기(`clienterror`, `serve_static`, `serve_dynamic`)를 맡는다.

## 모듈 사이를 오가는 것

| 무엇 | 만드는 쪽 | 쓰는 쪽 | 정의 |
|---|---|---|---|
| `http_request` (파싱 결과) | B `http_parse_request` | A 라우팅, C 응답, B 프록시 | `http_request.h` |
| `buffer` (응답·요청 바이트) | A `conn` 이 소유 | C·B 가 덧붙임, A 가 송신 | `buffer.h` |
| `http_response_head` (헤더 값) | C·A 가 채움 | C `http_append_header` | `http_response.h` |
| `static_file` (열린 파일, 보낸 위치) | C `static_open` | A 가 `static_send_some` 반복 호출 | `static_file.h` |
| `proxy_route`, `proxy_backend` | B `proxy_config_load` | A 라우팅, B 중계 | `proxy_config.h` |

## 소유권: 누가 만들고 누가 정리하나

| 자원 | 소유자 | 정리 |
|---|---|---|
| 클라이언트 소켓, `conn` 구조체 | A `server.c` | `conn_close` 하나에서만 |
| `conn.in`, `conn.out`, `conn.up_out` 버퍼 | A `conn` | `conn_close` 의 `buffer_free` |
| 열린 정적 파일 (`static_file.fd`) | `conn.file` | `static_close` (`conn_close` 가 호출) |
| 업스트림 소켓 | `conn.upstream_fd` | 응답 끝·실패 때 `up_release`, 아니면 `conn_close` 가 `close` |
| `conn.relay` (`proxy_relay`, 머리 대기 버퍼 포함) | A `conn` | `proxy_relay_free` (`up_release`, `conn_close`). 두 번 불러도 안전 |
| `proxy_config` | `server_run` 지역 변수 (서버 수명) | 프로그램 종료 시 (힙 할당 없음) |

규칙:

- **다른 모듈은 받은 `buffer` 에 덧붙이기만 한다.** free, clear, consume 하지 않는다
- **덧붙이기가 실패하면 버퍼는 호출 전 그대로다.** 반쯤 쓴 응답이 나가지 않게. 여러 번 나눠 덧붙이는 함수는 시작할 때 `buffer_mark`, 실패하면 `buffer_rollback` 한다 (`out->len` 을 직접 저장·복원하지 않는다. 도중에 compact 되면 어긋남)
- **`http_request.headers`, `.body` 는 `conn.in` 안을 가리킨다.** 응답 생성이 끝난 뒤에 `buffer_consume(&in, consumed)` 한다. 그 전에 `in` 에 read/append/compact 하지 않는다. 프록시 요청은 예외다: 페일오버가 요청을 다른 백엔드용으로 다시 쓰므로(`proxy_build_request` 가 `req` 를 다시 읽는다) 연결이 닫힐 때까지 consume 하지 않는다. 프록시 중에는 클라이언트 입력을 읽지 않고(`CONN_UP_*`), 응답 뒤 `conn_linger` 가 `in` 을 비운다
- **`NEED_MORE` 의 `consumed` 는 "필요한 총 길이"다.** 헤더가 끝나고 본문만 덜 왔으면 요청 전체 길이(`head_len + body_len`, DONE 때의 `consumed` 와 같은 값), 헤더가 아직이면 0 (본문이 잘게 오면 바이트마다 헤더를 다시 파싱하고 유휴 타이머도 리셋되므로, PR #24 리뷰)
  - 비교 기준은 `in.len` 이 아니라 **`buffer_pending(&in)`** (아직 consume 안 한 바이트, 즉 지금 요청의 시작부터). 앞 요청을 consume 한 뒤 `off` 가 남아 있어도 맞다
  - 서버는 0 이 아니면 이 값을 기억하고, `buffer_pending(&in)` 이 이 값 이상이 될 때까지 파서만 다시 부르지 않는다. 소켓 읽기, 유휴 타임아웃, 연결 종료·오류 처리는 그대로 한다
  - 이 요청의 파싱이 `DONE` 이나 `ERROR` 로 끝나면 기억한 값을 지운다 (다음 요청에 남기면 이미 다 온 짧은 요청을 기다리게 된다)
  - 이 값은 consume 하지 않는다
- 모듈은 자기가 열지 않은 fd 를 닫지 않는다
- 정리 함수(`buffer_free`, `static_close`, `conn_close`)는 두 번 불러도 안전해야 한다. 실패 경로에서 중복 호출이 생기기 쉽다

## 반환값 규칙

| 형태 | 의미 | 예 |
|---|---|---|
| `int` 0 / -1 | 성공 / 실패 (결과는 인자로 받은 buffer·구조체에) | `buffer_append`, `http_append_*`, `app_handle`, `proxy_build_request`, `static_open` |
| `int` 1 / 0 | 예 / 아니오 | `app_match` |
| `int` 1 / 0 / -1 | 끝남 / 아직 남음 (EAGAIN 포함) / 오류 | `static_send_some` |
| `enum` | 세 가지 이상 결과 | `http_parse_request` → `parse_result`, `handler_route` → `route_kind`, `proxy_relay_input`·`proxy_relay_eof` → `proxy_relay_result` |
| fd 또는 -1 | 연 소켓 / 실패 (`errno` 유지) | `net_listen` |
| fd 또는 -1 또는 -2 | 연 소켓 / connect 가 동기 실패 / 이쪽 문제로 연결 못 함 (`PROXY_CONNECT_LOCAL`: socket() 자원 고갈, 논블로킹 설정 실패, connect 의 `EADDRNOTAVAIL`. 백엔드에 죽은 표시를 붙이지 않는다). 둘 다 `errno` 유지 | `proxy_connect_start` |
| `int` 0 / errno | 성공 / 실패한 errno | `proxy_connect_result` |

4xx·5xx 응답을 **쓴 것**은 성공(0)이다. -1 은 응답조차 쓰지 못했을 때(메모리 부족)만 쓰고, 그때 서버는 연결을 닫는다.

## 연결 상태와 타임아웃 (`server.c`, `server.h`)

| 상태 | poll 이벤트 | 다음 |
|---|---|---|
| `CONN_READING` | 클라이언트 POLLIN | 요청 완성 → 응답 준비 후 `CONN_WRITING` |
| `CONN_WRITING` | 클라이언트 POLLOUT | out 다 보냄 → 파일 있으면 `CONN_SENDING_FILE`, 아니면 keep-alive 면 `CONN_READING` / 아니면 `CONN_LINGERING` |
| `CONN_SENDING_FILE` | 클라이언트 POLLOUT | `static_send_some` 이 1 → `static_close` → keep-alive 판단 |
| `CONN_UP_CONNECTING` | 업스트림 POLLOUT | `proxy_connect_result`(SO_ERROR) 가 0 이면 `CONN_UP_SENDING`, 아니면 502. 라우트에 `failover=connect` 가 있으면 502 대신 업스트림 fd 를 닫고 다음 백엔드로 다시 connect (상태는 그대로). `failover=timeout` 이면 `PROXY_CONNECT_TIMEOUT_MS` 가 지나도 같다 (못 넘기면 502 / 시간 초과면 504). 요청 바이트는 `CONN_UP_SENDING` 부터 나가므로 넘기는 것은 여기서만 |
| `CONN_UP_SENDING` | 업스트림 POLLOUT | `up_out`(다시 쓴 요청)을 다 보내면 `CONN_UP_RECEIVING`. 쓰기 오류는 502 |
| `CONN_UP_RECEIVING` | 업스트림 POLLIN, 응답 시작 후 out 에 바이트가 있으면 클라이언트 POLLOUT | 응답 끝·업스트림 조기 종료 → `CONN_WRITING` (남은 out 을 보내고 닫기). 시작 전 실패는 502/504 |
| `CONN_CLOSING` | 클라이언트 POLLOUT | out 다 보내면 `CONN_LINGERING` (파서 오류 응답 후, `Connection: close`) |
| `CONN_LINGERING` | 클라이언트 POLLIN | `shutdown(SHUT_WR)` 뒤 받은 바이트는 버림. EOF·오류·마감이면 `conn_close` |

| 타임아웃 | 값 | 넘으면 |
|---|---|---|
| `CLIENT_IDLE_TIMEOUT_MS` | 10초 | 연결 닫기 (요청을 덜 보낸 느린 클라이언트, keep-alive 유휴) |
| `CLIENT_REQUEST_TIMEOUT_MS` | 30초 | 요청 하나의 첫 바이트부터 완성까지. 넘으면 응답 없이 닫기 (1바이트씩 보내 유휴 타이머를 계속 리셋하는 slow loris) |
| `CLIENT_LINGER_TIMEOUT_MS` | 2초 | `CONN_LINGERING` 의 상한. 넘으면 닫기 |
| `PROXY_CONNECT_TIMEOUT_MS` | 1초 (`UPSTREAM_TIMEOUT_MS` 를 1초 이하로 줄여 빌드하면 그 절반) | 라우트에 `failover=timeout` 이 있을 때만. 한 번의 connect 가 이 시간 안에 안 끝나면 그 연결을 닫고 다음 백엔드로 (없으면 504). `UPSTREAM_TIMEOUT_MS` 보다 작아야 한다 (빌드에서 검사) |
| `PROXY_BACKEND_DOWN_MS` (`proxy_config.h`) | 10초 | connect 오류·시간 초과가 난 백엔드를 이 시간 동안 고르지 않는다 (페일오버가 켜진 라우트만). 시간으로만 풀린다 |
| `UPSTREAM_TIMEOUT_MS` | 5초 | 시계가 둘이다. 응답 시작 전: 요청을 프록시에 넘긴 때부터 머리가 다 올 때까지 (페일오버로 다른 백엔드를 시도해도 첫 시도부터 한 번만 잰다), 넘으면 504. 시작 후: 업스트림을 읽으려고 기다리는 동안 바이트가 안 오는 시간, 넘으면 이미 받은 out 을 보내고 닫기. 느린 클라이언트 때문에 읽기를 멈춘 시간은 세지 않는다 (그건 `CLIENT_IDLE_TIMEOUT_MS`) |

- 같은 연결의 다음 요청(파이프라이닝)은 현재 응답을 다 보낸 뒤 `in` 에 남은 바이트로 처리한다. 응답을 보내는 동안에는 클라이언트 소켓을 읽지 않는다 (POLLOUT 만 감시)
- 연결을 닫는 응답(`keep_alive == 0`, 오류 응답)은 바로 `close` 하지 않고 lingering close 한다. 읽지 않은 입력이 남은 채 `close` 하면 커널이 RST 를 보내 클라이언트가 아직 읽지 않은 응답까지 버려질 수 있다
- 아직 스텁인 경로: `ROUTE_APP` 은 `app_handle` 이 -1 이면 500 + 닫기 (#10 전까지). `ROUTE_PROXY` 는 스텁이 아니다 (#9)
- 프록시 경로 (#9): `respond()` 는 `RESPOND_PROXY` 를 돌려주고 out 을 비워 둔 채 `CONN_UP_CONNECTING` 으로 간다. 오류 코드는 다시 쓴 요청 실패 431, `proxy_connect_start` 실패 502, 시작 전 깨진·끊긴 머리 502, 시작 전 시간 초과 504 (전체 마감을 이미 넘겼으면 위 502·431 도 504)
  - 응답은 머리(상태 줄과 `Content-Length`)만 해석하고 받은 바이트를 그대로 `out` 에 넣는다. 백엔드에는 늘 `Connection: close` 를 보내므로 프록시 응답 뒤 클라이언트 연결은 항상 닫는다 (`keep_alive = 0`)
  - 중계 상한: 클라이언트 `out` 에 대기한 바이트가 `PROXY_RELAY_MAX`(64KB) 이상이면 업스트림을 읽지 않는다 (역압력). 그래서 `BUFFER_MAX` 보다 큰 응답도 모으지 않고 흘려보낸다
  - 응답 시작 = 머리 해석이 끝나 상태 줄 검사를 통과하고 머리 바이트를 `out` 에 넣은 때. 그 전 실패는 502/504, 그 뒤는 `out` 에 있는 것까지 보내고 닫는다 (요청을 보낸 뒤의 재시도는 없다. 응답 시작 전 connect 단계의 페일오버만 아래 "프록시 페일오버")
  - 본문 없음: 요청이 `HEAD` 이거나 상태가 204·304 이면 `Content-Length` 가 있어도 본문이 없다. `Transfer-Encoding` 이 있으면 업스트림 EOF 까지가 본문이다
  - 한 연결이 pollfd 를 최대 둘 쓴다 (클라이언트, 업스트림). 프록시 중에는 클라이언트 입력을 읽지 않고 (`events` 0, 끊김만 감지), 응답이 시작돼 `out` 에 바이트가 있을 때만 클라이언트 POLLOUT 을 본다
  - 업스트림 fd 는 `conn_close` 뿐 아니라 응답 끝·실패 때 `up_release` 가 닫는다 (`upstream_fd = -1` 로 두 번 닫기를 막는다)
- `SIGINT`/`SIGTERM`: 새 연결을 받지 않고 모든 `conn` 을 `conn_close` 한 뒤 `server_run` 이 0 을 반환한다

## 정한 세부 규칙

- **요청 크기**: 헤더 8KB(431), URI 2KB(414), 본문 1MB(413). `Transfer-Encoding` 은 501
- **431 우선**: 요청 시작부터 빈 줄까지가 8KB 를 넘으면 다른 오류보다 먼저 431. TCP 분할과 상관없이 같은 요청은 같은 코드
- **경로의 금지 바이트**: 디코딩 결과에 NUL·제어 문자(0x01-0x1F, 0x7F)·`/`(`%2F`)·`\` 가 있거나 `%XX` 가 잘못되면 파서가 400 (아래 "경로 정규형"). `static_open` 은 이를 전제로 한다
- **경로 정규형 (#31)**: 파서가 `target` 을 **정규화된 요청 대상**(정규형 경로 + 원본의 `?` 부분)으로 채운다. 원본 문자열은 남기지 않는다. 라우팅(`proxy_match`·`app_match`·`app_handle`)과 백엔드 전달(`proxy_build_request`)이 모두 이 `target` 을, 정적 파일은 이를 끝까지 디코딩한 `path` 를 쓰므로 표기만 바꿔 라우트를 건너뛸 수 없다. 만드는 순서 (하나라도 걸리면 400):
  1. `%XX` 문법 오류
  2. 인코딩 안 된 `\`, 디코딩하면 `/` `\` NUL·제어 문자(0x01-0x1F, 0x7F)가 되는 `%XX`
  3. 빈 조각 `//` (`--merge-slashes` = `http_parse_options.merge_slashes` 면 하나로 합침. 끝 `/` 하나는 허용)
  4. 비예약 문자(`A-Z a-z 0-9 - . _ ~`)의 `%XX` 만 글자로, 나머지 `%XX` 는 16진수 대문자 (`%eb` → `%EB`)
  5. 디코딩한 `path` 의 `.`·`..` 조각 (`/a/../b`, `/a/%2E%2E/b` 모두)

  디코딩은 한 번만 (`%252F` 는 `path` 에서 글자 `%2F`). `query` 는 원본 그대로. `static_open` 의 `..` 정리·403 은 두 번째 안전장치로 남는다. 순서·이유는 `docs/decisions.md` 2026-11-23
- **파서 옵션**: `http_parse_request(buf, len, opt, ...)` 의 `opt` 는 `http_parse_options` (NULL = 기본값). 서버는 시작할 때 `server_config` 에서 한 번 채워 요청마다 같은 것을 넘긴다. 옵션이 늘면 인자 대신 필드를 추가한다 (#35)
- **프록시 경로 매칭**: 정규화된 `target` 의 `?` 앞 경로로, 경로 경계에서만 (`/api` 는 `/apix` 에 안 맞음). 가장 긴 접두사가 이긴다. 경로·쿼리는 정규화된 `target` 그대로 전달한다 (접두사를 떼지 않음)
- **헬스체크 경로** (2026-11-25): 정규화된 `target` 의 `?` 앞이 정확히 `/healthz` (`app_health_match`). `handler_route` 가 프록시보다 먼저 보고 `ROUTE_HEALTH` 를 돌려주며, 서버는 `app_health` 로 응답한다. 그래서 프런트든 백엔드든 자기 자신이 답한다
- **앱 경로**: 정규화된 `target` 이 `/api/` 로 시작 (프록시 매칭과 같은 문자열). 프록시 경로가 먼저라서 프록시 인스턴스에서는 백엔드로 가고, 백엔드 인스턴스(`-c` 없음)에서는 앱이 처리한다. `/%61pi/echo` 는 정규형이 `/api/echo` 라 같은 곳으로 간다
- **프록시 헤더**: hop-by-hop 헤더, `Connection` 값에 적힌 이름, `Expect` 는 빼고 전달한다. `Host`(백엔드 이름)·`X-Forwarded-For`(실제 클라이언트 IP)·`Content-Length`(원래 있었을 때만, 본문 길이로)·`Connection: close` 는 프록시가 새로 쓰고 클라이언트 값은 버린다. 요청마다 `Via: 1.1 jungle-webserver` 도 붙인다 (RFC 9110 7.6.3). `Connection` 에 적힌 이름이 이 줄들을 지우지는 못한다. 다시 쓴 머리가 `HTTP_MAX_HEADER`(8192)를 넘거나 헤더 줄이 파서 기준으로 깨졌으면 `proxy_build_request` 는 -1 이다 (같은 바이너리 백엔드가 어차피 431 로 거절할 길이). 서버는 이 -1 을 431 로 답한다 (파서·설정 로더가 막아 주므로 현실적인 원인은 머리 길이 초과, 그 밖에 메모리 부족)
- **프록시 응답 해석** (`proxy_relay_*`, #9): 상태 줄은 `HTTP/1.` 숫자 + 세 자리 상태(100..599)여야 하고 1xx 는 깨진 머리로 본다 (요청에서 `Expect`·`Upgrade` 를 뺐으므로). 머리는 CRLF 줄, obs-fold 없음, 빈 줄까지 8192바이트 이하. `Content-Length` 는 숫자만 (2^53 이하), 여러 줄이면 값이 같아야 한다. 어기면 시작 전 502
- **Round-Robin** (`proxy_rr_begin`): 라우트마다 `next` 를 요청마다 한 칸 돌려 주 백엔드끼리 0, 1, ..., n-1, 0 순 (백업은 세지 않는다). 페일오버가 꺼진 라우트는 예전과 같은 순서다
- **프록시 페일오버** (`proxy_pick_backend`, `proxy_mark_down`, `server.c` 의 `up_start_attempt`·`up_connect_failed`): 결정은 `docs/decisions.md` 2026-11-25. 요청 하나는 시작에 `proxy_rr_begin` 으로 RR 시작 위치를 받고, 시도마다 `proxy_pick_backend(route, now, tried_mask, rr_start)` 로 안 해 본 백엔드를 고른다 (RR 한 바퀴의 주 백엔드 → 백업. 죽은 표시 중인 것은 건너뛰되 전부 죽은 표시면 무시). 시도 횟수 한도(`route->tries`)는 호출자가 센다. 실패하면(`CONN_UP_CONNECTING` 에서 connect 오류, 또는 `timeout` 이 켜졌을 때 connect 시계 초과) `proxy_mark_down` 으로 표시하고 `up_out` 을 새 백엔드용으로 다시 만들어(`Host`) 다시 connect 한다. 프록시 요청의 `in` 은 이 재작성 때문에 연결이 닫힐 때까지 consume 하지 않는다 (`c->req` 가 `in` 을 가리킨다)
- **프록시 설정 파일 (`proxy_config_load`)**: 한 줄에 `접두사 host:port... [옵션...]` (IPv6 는 `[::1]:8001`), `#` 주석, 빈 파일은 오류. 옵션(`=` 가 있는 토큰) `backup=`, `failover=`, `tries=` 의 규칙은 `docs/api.md`, 주 + 백업이 8개를 넘거나 주 백엔드가 없으면 오류. 접두사는 `/` 로 시작하는 정규형이어야 한다: `%`·`?`·`#`·제어 문자·비 ASCII 없음, 빈 조각 `//` 없음(끝 `/` 하나는 괜찮음), `.`·`..` 조각 없음 (정규화된 `target` 에 나올 수 없는 모양이라 어떤 요청에도 안 맞으므로). 한도: 줄 1024바이트, 접두사 127, 백엔드 토큰 63, 라우트 16, 라우트당 백엔드 8, 포트 1-65535 (앞자리 0 없는 10진수, 숫자 호스트도 앞자리 0 없는 점 넷 IPv4). 이름은 시작할 때 한 번 해석하고 IPv4 를 먼저 고른다 (`localhost` 가 `::1` 로 풀려 연결이 거절되는 것을 피함. 백엔드 인스턴스는 IPv4 loopback 을 받아야 한다). 오류는 -1 과 stderr `파일:줄: 이유` (줄이 없는 파일 전체 오류는 `파일: 이유`) 이고 서버는 시작하지 않는다
- **오류 응답의 추가 헤더**: `http_append_error` 의 `extra_headers` 로 넣는다. 405 `Allow` 는 호출자가 넣고(정적 `GET, HEAD`, echo `POST`), 앱의 오류에도 `X-Instance` 를 붙인다
- **HEAD**: 헤더는 GET 과 같게(Content-Length 포함), 본문은 보내지 않는다
- **응답 헤더 모양** (`http_append_header`): 상태 코드는 100~599 (밖이면 -1). 줄 순서는 상태 줄, `Date`, `Server: jungle-webserver`, `Content-Type`, `Content-Length`, `Connection`, `extra_headers`, 빈 줄로 고정. `Date` 가 들어가므로 응답 모듈은 소켓과 무관하지만 결과가 현재 시각에 따라 달라진다. 응답을 검사하는 테스트는 전체 바이트 비교 대신 필요한 줄을 `strstr` 로 찾는다
- **길이를 모르는 응답**: `content_length < 0` 이면 `Content-Length` 를 생략하고, `keep_alive` 와 상관없이 `Connection: close` 를 쓴다 (클라이언트가 응답 끝을 연결 종료로만 알 수 있음)
- **`extra_headers` 형식**: `NULL`, `""`, 또는 CRLF 로 끝나는 줄들 (빈 줄·짝 없는 CR·LF 없이, 응답 분할 방지). `content_type` 에 CR·LF 가 있어도 -1. 아니면 `http_append_*` 가 -1 (호출 코드 실수 검사). 서버가 만든 값만 넣고 요청에서 온 값은 넣지 않는다
- **오류 본문**: `<html><body><h1>404 Not Found</h1></body></html>` 처럼 상태 코드와 문구만. 요청 경로 등 요청 내용은 넣지 않는다 (Tiny `clienterror` 반사형 XSS, #3)
- **정적 파일 메서드**: GET, HEAD 외에는 서버가 405 (`extra_headers` = `"Allow: GET, HEAD\r\n"`)
- **정적 파일 경로 판정** (`static_open`): `.`·`..`·`//` 를 먼저 글자로 정리하고, 루트 위로 가면 파일 존재와 상관없이 403. 그다음 `realpath` 결과가 docroot 밖이면(심볼릭 링크) 403. 디렉터리는 `index.html`(없으면 404), `/파일/` 처럼 파일 뒤 `/` 는 404, 일반 파일이 아니면 403, docroot 를 못 찾으면 500
- **큰 배열은 스택에 두지 않는다**: `http_request`, `conn` 은 힙(calloc). `static_send_some` 의 지역 버퍼는 16KB 이하
