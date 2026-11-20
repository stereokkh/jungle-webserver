# 검증용 Ubuntu 환경 (glibc 대표)
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential clang make curl ca-certificates valgrind netcat-openbsd \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /work
