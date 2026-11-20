# 검증용 Alpine 환경 (musl libc 대표)
FROM alpine:3.20

RUN apk add --no-cache build-base clang make curl valgrind netcat-openbsd

WORKDIR /work
