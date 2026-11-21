/*
 * main.c — CLI 인자 처리 후 server_run() 호출
 *
 * CS:APP 대응: tiny.c main() 의 인자 검사 부분 (이벤트 루프는 server.c)
 *   Tiny 는 포트 하나만 받고 docroot 는 현재 디렉터리(".")로 고정이다.
 *   여기서는 docroot 를 -r 로 받고, 서버를 띄우기 전에 잘못된 인자는 바로 exit 1 한다.
 * 담당: 트랙 A · 이슈 #7
 */
#include "server.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s -p <port> -r <docroot> [-c <proxy.conf>]\n", prog);
}

/* 1..65535 의 10진수만 받는다 ("80x", "", "+80", "0", "08080" 거절).
 * 포트 문자열은 #10 에서 X-Instance 헤더 값으로 그대로 쓰므로 표기를 하나로 맞춘다
 * (선행 0 거절은 #26 프록시 설정 파일과 같은 규칙) */
static int valid_port(const char *s)
{
    long v = 0;

    if (*s == '\0' || *s == '0')
        return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return 0;
        v = v * 10 + (*s - '0');
        if (v > 65535)
            return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    server_config cfg = { NULL, NULL, NULL };
    struct stat st;
    int opt;

    while ((opt = getopt(argc, argv, "p:r:c:")) != -1) {
        switch (opt) {
        case 'p': cfg.port = optarg; break;
        case 'r': cfg.docroot = optarg; break;
        case 'c': cfg.proxy_conf = optarg; break;
        default:  usage(argv[0]); return EXIT_FAILURE;   /* getopt 가 이미 이유를 출력함 */
        }
    }
    if (optind != argc || cfg.port == NULL || cfg.docroot == NULL) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (!valid_port(cfg.port)) {
        fprintf(stderr, "webserver: invalid port '%s' (1-65535)\n", cfg.port);
        return EXIT_FAILURE;
    }
    if (stat(cfg.docroot, &st) < 0) {
        fprintf(stderr, "webserver: docroot '%s': %s\n", cfg.docroot, strerror(errno));
        return EXIT_FAILURE;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "webserver: docroot '%s' is not a directory\n", cfg.docroot);
        return EXIT_FAILURE;
    }

    return server_run(&cfg) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
