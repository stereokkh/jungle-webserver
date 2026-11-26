CC       ?= cc
CFLAGS   ?= -O2 -g
WARN     := -std=c11 -Wall -Wextra -Wpedantic
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Iinclude
LDFLAGS  ?=

# make WERROR=1 → 경고를 에러로 처리 (CI에서 사용)
ifeq ($(WERROR),1)
WARN += -Werror
endif

SRC_DIR   := src
BUILD_DIR := build
TARGET    := webserver

# 이벤트 백엔드 (docs/event-backend.md): make [EVENT=poll|epoll|kqueue]. 비면 poll
EVENT_ALL := poll epoll kqueue
override EVENT := $(if $(strip $(EVENT)),$(strip $(EVENT)),poll)
ifeq ($(filter $(EVENT),$(EVENT_ALL)),)
$(error EVENT=$(EVENT) 은 모르는 값이다. poll, epoll, kqueue 중 하나)
endif

# 고르지 않은 백엔드 파일은 빌드에서 뺀다 (#ifdef 로 감싸면 빈 번역 단위라 -Wpedantic 경고)
SRCS := $(filter-out $(patsubst %,$(SRC_DIR)/ev_%.c,$(filter-out $(EVENT),$(EVENT_ALL))),$(wildcard $(SRC_DIR)/*.c))
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# EVENT 를 바꾸면 다시 링크되게: 마지막 EVENT 를 적은 스탬프 (내용이 바뀔 때만 다시 써서 시각이 바뀐다)
EVENT_STAMP := $(BUILD_DIR)/event.stamp
$(shell mkdir -p $(BUILD_DIR); [ "$$(cat $(EVENT_STAMP) 2>/dev/null)" = "$(EVENT)" ] || echo "$(EVENT)" > $(EVENT_STAMP))
DEPS := $(OBJS:.o=.d)

# 단위 테스트: tests/unit/test_*.c 하나가 실행 파일 하나. main.c 를 뺀 모든 모듈과 링크
UNIT_DIR  := tests/unit
UNIT_SRCS := $(wildcard $(UNIT_DIR)/test_*.c)
UNIT_BINS := $(UNIT_SRCS:$(UNIT_DIR)/%.c=$(BUILD_DIR)/unit/%)
LIB_OBJS  := $(filter-out $(BUILD_DIR)/main.o,$(OBJS))
DEPS      += $(UNIT_BINS:=.d)

# 테스트 도구: tests/tools/<이름>.c 하나가 실행 파일 하나 (서버 모듈과 링크하지 않는 독립 프로그램).
# fake_upstream 은 프록시 스모크 테스트가 502·504 를 만들 때 쓰는 가짜 백엔드 (이슈 #9)
TOOL_DIR  := tests/tools
FAKE      := $(BUILD_DIR)/tools/fake_upstream
DEPS      += $(FAKE).d

.PHONY: all clean run test unit asan classic docker-build docker-test docker-test-all docker-shell

all: $(TARGET)

$(TARGET): $(OBJS) $(EVENT_STAMP)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(WARN) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/unit/%: $(UNIT_DIR)/%.c $(LIB_OBJS) $(EVENT_STAMP) | $(BUILD_DIR)/unit
	$(CC) $(CPPFLAGS) -I$(UNIT_DIR) $(WARN) $(CFLAGS) -MMD -MP -MT $@ -MF $@.d \
		-o $@ $< $(LIB_OBJS) $(LDFLAGS)

$(BUILD_DIR)/tools/%: $(TOOL_DIR)/%.c | $(BUILD_DIR)/tools
	$(CC) $(CPPFLAGS) $(WARN) $(CFLAGS) -MMD -MP -MT $@ -MF $@.d -o $@ $< $(LDFLAGS)

$(BUILD_DIR) $(BUILD_DIR)/unit $(BUILD_DIR)/tools:
	mkdir -p $@

# make run [PORT=8080] [DOCROOT=www]
PORT    ?= 8080
DOCROOT ?= www
run: $(TARGET)
	./$(TARGET) -p "$(PORT)" -r "$(DOCROOT)"

# make unit → 단위 테스트만, make test → 단위 테스트 + 스모크 테스트 + 백엔드 2개 비교 (CI·docker-test 도 이것)
# 스모크 테스트는 서버(BIN)와 가짜 백엔드(FAKE)를 환경 변수로 받는다
# backends.sh 는 같은 바이너리를 백엔드 2개 + 프런트 1개로 띄워 직접 호출 = 프록시 경유를 본다 (#10)
unit: $(UNIT_BINS)
	@echo "== unit tests"
	@fail=0; for t in $(UNIT_BINS); do ./$$t || fail=1; done; exit $$fail

test: unit $(TARGET) $(FAKE)
	BIN=./$(TARGET) FAKE=./$(FAKE) sh tests/run.sh
	BIN=./$(TARGET) sh tests/backends.sh

# make asan → AddressSanitizer + UBSan 으로 build/asan/ 에 따로 빌드해서 make test 와 같은 검사
#   - 일반 빌드 산출물(build/*.o, ./webserver)과 섞이지 않는다
#   - 메모리 오류·정의되지 않은 동작은 즉시 실패, Linux 에서는 종료 시 누수도 검사(LeakSanitizer)
#   - Alpine(musl)은 ASan 미지원 → Ubuntu/WSL 또는 macOS 에서 실행
SAN         := address,undefined
ASAN_DIR    := $(BUILD_DIR)/asan
ASAN_CFLAGS := -O1 -g -fno-omit-frame-pointer -fsanitize=$(SAN) -fno-sanitize-recover=all

asan:
	UBSAN_OPTIONS=print_stacktrace=1 $(MAKE) --no-print-directory \
		BUILD_DIR=$(ASAN_DIR) TARGET=$(ASAN_DIR)/webserver \
		CFLAGS="$(ASAN_CFLAGS)" LDFLAGS="-fsanitize=$(SAN)" test

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

# CS:APP Tiny를 classic/ 에 받아서 빌드 (분석용, 커밋 안 함, CI 대상 아님). 이슈 #3
classic:
	sh scripts/fetch-tiny.sh

# ---------------------------------------------------------------
# Docker 검증 환경
#   make docker-test                    # Ubuntu + gcc
#   make docker-test DISTRO=alpine      # Alpine(musl) + gcc
#   make docker-test DISTRO=alpine CC=clang
#   make docker-test-all                # Ubuntu, Alpine 모두
#   make docker-shell DISTRO=alpine     # 컨테이너 안에서 직접 확인
# 소스는 읽기 전용으로 마운트하고 컨테이너 내부 복사본에서 빌드하므로
# 호스트의 build/ 와 실행 파일은 건드리지 않는다.
# ---------------------------------------------------------------
DISTRO       ?= ubuntu
DOCKER       ?= docker
DOCKER_IMAGE := jungle-webserver-$(DISTRO)
DOCKER_RUN   := $(DOCKER) run --rm -v "$(CURDIR)":/src:ro $(DOCKER_IMAGE)
IN_CONTAINER := cp -a /src/. /work/ && make clean >/dev/null && make all test CC=$(CC) WERROR=$(WERROR)

docker-build:
	$(DOCKER) build -q -f docker/$(DISTRO).Dockerfile -t $(DOCKER_IMAGE) docker

docker-test: docker-build
	$(DOCKER_RUN) sh -c '$(IN_CONTAINER)'

docker-test-all:
	$(MAKE) docker-test DISTRO=ubuntu
	$(MAKE) docker-test DISTRO=alpine

docker-shell: docker-build
	$(DOCKER) run --rm -it -v "$(CURDIR)":/src:ro $(DOCKER_IMAGE) sh -c 'cp -a /src/. /work/ && exec sh'

-include $(DEPS)
