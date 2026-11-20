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

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean run test classic docker-build docker-test docker-test-all docker-shell

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(WARN) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

run: $(TARGET)
	./$(TARGET) 8080

test: $(TARGET)
	sh tests/run.sh

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
