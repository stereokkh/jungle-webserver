/*
 * main.c — CLI 인자 처리 후 server_run() 호출
 *
 * CS:APP 대응: tiny.c main() 의 인자 검사 부분 (이벤트 루프는 server.c)
 * 담당: 트랙 A · 이슈 #7
 * TODO(#7): -p <port> -r <docroot> [-c <proxy.conf>] 파싱 → server_config → server_run().
 *           tests/run.sh 도 같은 PR 에서 백그라운드 실행 방식으로 바꾼다.
 */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return EXIT_FAILURE;
    }

    printf("webserver: listening on port %s (not implemented yet)\n", argv[1]);
    return EXIT_SUCCESS;
}
