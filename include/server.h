/*
 * server.h — poll() 이벤트 루프, 연결 수명주기, 시그널/종료
 *
 * CS:APP 대응: tiny.c main() 의 while(1) { Accept; doit; Close; } 반복 루프
 * 담당: 트랙 A · 이슈 #7 (업스트림 상태 통합은 #9, B와 협업)
 * 현대식으로 바뀌는 점: 한 번에 한 연결 → poll 로 여러 연결, keep-alive, 타임아웃,
 *                       SIGINT/SIGTERM 시 모든 자원 정리 후 종료
 */
#ifndef SERVER_H
#define SERVER_H

typedef struct {
    const char *port;        /* -p */
    const char *docroot;     /* -r */
    const char *proxy_conf;  /* -c (없으면 NULL) */
} server_config;

/* 이벤트 루프를 돌린다. 시그널로 정상 종료하면 0, 초기화 실패 시 -1 */
int server_run(const server_config *cfg);

#endif /* SERVER_H */
