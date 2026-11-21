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
│ 4. buffer_consume(in, consumed)  ← 응답·프록시 요청을 다 만든 뒤에만     │
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
| 업스트림 소켓 | `conn.upstream_fd` | `conn_close` 가 `close` |
| `proxy_config` | `server_run` 지역 변수 (서버 수명) | 프로그램 종료 시 (힙 할당 없음) |

규칙:

- **다른 모듈은 받은 `buffer` 에 덧붙이기만 한다.** free, clear, consume 하지 않는다
- **덧붙이기가 실패하면 버퍼는 호출 전 그대로다.** 반쯤 쓴 응답이 나가지 않게. 여러 번 나눠 덧붙이는 함수는 시작할 때 `buffer_mark`, 실패하면 `buffer_rollback` 한다 (`out->len` 을 직접 저장·복원하지 않는다. 도중에 compact 되면 어긋남)
- **`http_request.headers`, `.body` 는 `conn.in` 안을 가리킨다.** 응답 생성과 프록시 요청 생성이 끝난 뒤에 `buffer_consume(&in, consumed)` 한다. 그 전에 `in` 에 read/append/compact 하지 않는다
- 모듈은 자기가 열지 않은 fd 를 닫지 않는다
- 정리 함수(`buffer_free`, `static_close`, `conn_close`)는 두 번 불러도 안전해야 한다. 실패 경로에서 중복 호출이 생기기 쉽다

## 반환값 규칙

| 형태 | 의미 | 예 |
|---|---|---|
| `int` 0 / -1 | 성공 / 실패 (결과는 인자로 받은 buffer·구조체에) | `buffer_append`, `http_append_*`, `app_handle`, `proxy_build_request`, `static_open` |
| `int` 1 / 0 | 예 / 아니오 | `app_match` |
| `int` 1 / 0 / -1 | 끝남 / 아직 남음 (EAGAIN 포함) / 오류 | `static_send_some` |
| `enum` | 세 가지 이상 결과 | `http_parse_request` → `parse_result`, `handler_route` → `route_kind` |
| fd 또는 -1 | 연 소켓 / 실패 (`errno` 유지) | `net_listen`, `proxy_connect_start` |

4xx·5xx 응답을 **쓴 것**은 성공(0)이다. -1 은 응답조차 쓰지 못했을 때(메모리 부족)만 쓰고, 그때 서버는 연결을 닫는다.

## 연결 상태와 타임아웃 (`server.c`, `server.h`)

| 상태 | poll 이벤트 | 다음 |
|---|---|---|
| `CONN_READING` | 클라이언트 POLLIN | 요청 완성 → 응답 준비 후 `CONN_WRITING` |
| `CONN_WRITING` | 클라이언트 POLLOUT | out 다 보냄 → 파일 있으면 `CONN_SENDING_FILE`, 아니면 keep-alive 면 `CONN_READING` / 아니면 닫기 |
| `CONN_SENDING_FILE` | 클라이언트 POLLOUT | `static_send_some` 이 1 → `static_close` → keep-alive 판단 |
| `CONN_UPSTREAM` | 업스트림 POLLOUT/POLLIN | #9 에서 세분화 (연결 중 / 요청 송신 / 응답 중계) |
| `CONN_CLOSING` | 클라이언트 POLLOUT | out 다 보내면 닫기 (오류 응답 후, `Connection: close`) |

| 타임아웃 | 값 | 넘으면 |
|---|---|---|
| `CLIENT_IDLE_TIMEOUT_MS` | 10초 | 연결 닫기 (요청을 덜 보낸 느린 클라이언트, keep-alive 유휴) |
| `UPSTREAM_TIMEOUT_MS` | 5초 | 응답 시작 전이면 504, 시작 후면 연결 닫기 |

- 같은 연결의 다음 요청(파이프라이닝)은 현재 응답을 다 보낸 뒤 `in` 에 남은 바이트로 처리한다
- `SIGINT`/`SIGTERM`: 새 연결을 받지 않고 모든 `conn` 을 `conn_close` 한 뒤 `server_run` 이 0 을 반환한다

## 정한 세부 규칙

- **요청 크기**: 헤더 8KB(431), URI 2KB(414), 본문 1MB(413). `Transfer-Encoding` 은 501
- **431 우선**: 요청 시작부터 빈 줄까지가 8KB 를 넘으면 다른 오류보다 먼저 431. TCP 분할과 상관없이 같은 요청은 같은 코드
- **경로의 금지 바이트**: 디코딩 결과에 NUL·제어 문자(0x01-0x1F, 0x7F)가 있거나 `%XX` 가 잘못되면 파서가 400. `static_open` 은 이를 전제로 한다
- **프록시 경로 매칭**: 디코딩 전 `target` 의 `?` 앞 경로로, 경로 경계에서만 (`/api` 는 `/apix` 에 안 맞음)
- **앱 경로**: 디코딩 전 `target` 이 `/api/` 로 시작 (프록시 매칭과 같은 기준). 프록시 경로가 먼저라서 프록시 인스턴스에서는 백엔드로 가고, 백엔드 인스턴스(`-c` 없음)에서는 앱이 처리한다. `/%61pi/add` 처럼 표기를 바꾼 요청은 둘 다 안 맞아 정적 파일(404)로 간다. 정적 파일만 디코딩한 `path` 를 쓴다
- **프록시 헤더**: hop-by-hop 헤더와 클라이언트 `Connection` 값에 적힌 이름은 빼고 전달. 클라이언트가 보낸 `X-Forwarded-For` 는 버리고 실제 클라이언트 IP 로 새로 쓴다
- **오류 응답의 추가 헤더**: `http_append_error` 의 `extra_headers` 로 넣는다. 405 `Allow` 는 호출자가 넣고(정적 `GET, HEAD`, echo `POST`), 앱의 오류에도 `X-Instance` 를 붙인다
- **HEAD**: 헤더는 GET 과 같게(Content-Length 포함), 본문은 보내지 않는다
- **응답 헤더 모양** (`http_append_header`): 상태 코드는 100~599 (밖이면 -1). 줄 순서는 상태 줄, `Date`, `Server: jungle-webserver`, `Content-Type`, `Content-Length`, `Connection`, `extra_headers`, 빈 줄로 고정. `Date` 가 들어가므로 응답 모듈은 소켓과 무관하지만 결과가 현재 시각에 따라 달라진다. 응답을 검사하는 테스트는 전체 바이트 비교 대신 필요한 줄을 `strstr` 로 찾는다
- **길이를 모르는 응답**: `content_length < 0` 이면 `Content-Length` 를 생략하고, `keep_alive` 와 상관없이 `Connection: close` 를 쓴다 (클라이언트가 응답 끝을 연결 종료로만 알 수 있음)
- **`extra_headers` 형식**: `NULL`, `""`, 또는 CRLF 로 끝나는 줄들 (빈 줄·짝 없는 CR·LF 없이, 응답 분할 방지). `content_type` 에 CR·LF 가 있어도 -1. 아니면 `http_append_*` 가 -1 (호출 코드 실수 검사). 서버가 만든 값만 넣고 요청에서 온 값은 넣지 않는다
- **오류 본문**: `<html><body><h1>404 Not Found</h1></body></html>` 처럼 상태 코드와 문구만. 요청 경로 등 요청 내용은 넣지 않는다 (Tiny `clienterror` 반사형 XSS, #3)
- **정적 파일 메서드**: GET, HEAD 외에는 서버가 405 (`extra_headers` = `"Allow: GET, HEAD\r\n"`)
- **정적 파일 경로 판정** (`static_open`): `.`·`..`·`//` 를 먼저 글자로 정리하고, 루트 위로 가면 파일 존재와 상관없이 403. 그다음 `realpath` 결과가 docroot 밖이면(심볼릭 링크) 403. 디렉터리는 `index.html`(없으면 404), `/파일/` 처럼 파일 뒤 `/` 는 404, 일반 파일이 아니면 403, docroot 를 못 찾으면 500
- **큰 배열은 스택에 두지 않는다**: `http_request`, `conn` 은 힙(calloc). `static_send_some` 의 지역 버퍼는 16KB 이하
