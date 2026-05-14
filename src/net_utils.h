#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

static inline bool send_all(int fd, const void *buf, size_t size)
{
    const char *p = (const char *)buf;
    size_t left = size;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n <= 0) return false;
        p    += n;
        left -= (size_t)n;
    }
    return true;
}

static inline bool recv_all(int fd, void *buf, size_t size)
{
    char *p = (char *)buf;
    size_t left = size;
    while (left > 0) {
        ssize_t n = read(fd, p, left);
        if (n <= 0) return false;
        p    += n;
        left -= (size_t)n;
    }
    return true;
}

static inline void tune_socket(int fd, int timeout_sec)
{
    int yes = 1;

    setsockopt(fd, SOL_SOCKET,  SO_KEEPALIVE,  &yes,  sizeof(yes));
    int idle = 5, intvl = 2, cnt = 3;
    // через сколько секунд простоя начать посылать пакеты проверки соединения
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
    // интервал между пакетами в сек
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    // сколько пакетов без ответа считать обрывом соединения
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));

    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,   &yes,  sizeof(yes));

    if (timeout_sec > 0) {

        struct timeval tv = {
            .tv_sec = timeout_sec,
            .tv_usec = 0
        };

        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
}
