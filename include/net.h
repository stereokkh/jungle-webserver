/*
 * net.h — listen 소켓 생성, 논블로킹 설정
 *
 * CS:APP 대응: open_listenfd() (csapp.c), Open_listenfd 래퍼
 * 담당: 트랙 A · 이슈 #7
 * 현대식으로 바뀌는 점: 에러 시 exit 하지 않고 -1 반환, O_NONBLOCK 설정
 */
#ifndef NET_H
#define NET_H

/* port 로 listen 소켓을 만든다 (getaddrinfo + SO_REUSEADDR + bind + listen).
 * 성공: fd, 실패: -1 (errno 유지) */
int net_listen(const char *port);

/* fd 를 논블로킹으로 바꾼다 (fcntl O_NONBLOCK). 성공 0, 실패 -1 */
int net_set_nonblocking(int fd);

/* TCP 연결 fd 에 TCP_NODELAY 를 켠다 (Nagle 끔). 성공 0, 실패 -1.
 * 응답을 여러 번 write 하는 keep-alive 연결에서 지연 ACK 대기(약 40ms)를 없앤다 */
int net_set_nodelay(int fd);

#endif /* NET_H */
