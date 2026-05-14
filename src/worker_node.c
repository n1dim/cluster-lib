#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>

#include "../include/cluster.h"
#include "protocol.h"
#include "net_utils.h"

struct ClusterWorker {
    int      max_cores;
    int      timeout_sec;
    uint16_t port;
    int      listen_fd;
    int      conn_fd;
};

static bool worker_open_listen(ClusterWorker *w)
{
    w->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (w->listen_fd < 0) {
        perror("[worker] socket");
        return false;
    }

    int yes = 1;
    setsockopt(w->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(w->port),
    };
    if (bind(w->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[worker] bind");
        close(w->listen_fd);
        return false;
    }
    if (listen(w->listen_fd, 1) < 0) {
        perror("[worker] listen");
        close(w->listen_fd);
        return false;
    }
    return true;
}

static void worker_send_abort(ClusterWorker *w)
{
    if (w->conn_fd < 0) return;
    ControlMsg msg = { .tag = MSG_ABORT };
    send_all(w->conn_fd, &msg, sizeof(msg));
}


ClusterWorker *cluster_worker_create(int max_cores, int timeout_sec, uint16_t port)
{
    ClusterWorker *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->max_cores   = max_cores;
    w->timeout_sec = timeout_sec;
    w->port        = port;
    w->listen_fd   = -1;
    w->conn_fd     = -1;
    return w;
}

bool cluster_worker_run(ClusterWorker *w, cluster_subtask_fn_t fn)
{
    // чтобы процесс не умер, когда пишет в закрытый сокет
    signal(SIGPIPE, SIG_IGN);

    if (!worker_open_listen(w)) return false;

    fprintf(stderr, "[worker port=%u] Listening\n", w->port);

    // Ждём подключения с тайм-аутом. SO_RCVTIMEO действует только на conn_fd
    // после accept(), поэтому используем poll() на listen_fd
    if (w->timeout_sec > 0) {
        struct pollfd pfd = { .fd = w->listen_fd, .events = POLLIN };
        int ready = poll(&pfd, 1, w->timeout_sec * 1000);
        if (ready == 0) {
            fprintf(stderr, "[worker port=%u] Timeout waiting for control node\n", w->port);
            close(w->listen_fd);
            return false;
        }
        if (ready < 0) {
            perror("[worker] poll");
            close(w->listen_fd);
            return false;
        }
    }

    w->conn_fd = accept(w->listen_fd, NULL, NULL);
    if (w->conn_fd < 0) {
        perror("[worker] accept");
        close(w->listen_fd);
        return false;
    }

    tune_socket(w->conn_fd, w->timeout_sec);
    fprintf(stderr, "[worker port=%u] Control node connected\n", w->port);

    bool ok = true;

    while (true) {
        uint8_t tag;
        ssize_t n = read(w->conn_fd, &tag, 1);

        if (n == 0) {
            // сокет закрылся на той стороне
            fprintf(stderr, "[worker port=%u] Control disconnected unexpectedly\n", w->port);
            ok = false;
            break;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                // по таймауту
                fprintf(stderr, "[worker port=%u] Timeout waiting for task\n", w->port);
            else
                perror("[worker] read tag");
            ok = false;
            break;
        }

        if (tag == MSG_DONE) {
            fprintf(stderr, "[worker port=%u] Received DONE – shutting down\n", w->port);
            ok = true;
            break;
        }
        if (tag == MSG_ABORT) {
            fprintf(stderr, "[worker port=%u] Received ABORT from control\n", w->port);
            ok = false;
            break;
        }
        if (tag != MSG_TASK) {
            fprintf(stderr, "[worker port=%u] Unexpected tag 0x%02x\n", w->port, tag);
            ok = false;
            break;
        }

        // читаем оставшиеся байты TaskMsg
        TaskMsg task;
        task.tag = tag;
        if (!recv_all(w->conn_fd, (char *)&task + 1, sizeof(task) - 1)) {
            fprintf(stderr, "[worker port=%u] Failed to receive task body\n", w->port);
            ok = false;
            break;
        }

        int cores = w->max_cores;
        if (cores < 1) cores = 1;

        fprintf(stderr,
                "[worker port=%u] Task [%.6f, %.6f] intervals=%lu cores=%d\n",
                w->port,
                task.range_start, task.range_end,
                (unsigned long)task.num_intervals, cores);

        double result = fn(task.range_start, task.range_end, task.num_intervals, cores);

        ResultMsg rmsg = {
            .tag = MSG_RESULT,
            .result = result,
            .status = 0
        };

        if (!send_all(w->conn_fd, &rmsg, sizeof(rmsg))) {
            fprintf(stderr, "[worker port=%u] Failed to send result\n", w->port);
            ok = false;
            break;
        }

        fprintf(stderr, "[worker port=%u] Result sent: %.10f\n", w->port, result);
    }

    if (!ok) worker_send_abort(w);

    close(w->conn_fd);  w->conn_fd   = -1;
    close(w->listen_fd); w->listen_fd = -1;
    return ok;
}

void cluster_worker_destroy(ClusterWorker *w)
{
    if (!w) return;
    if (w->conn_fd   >= 0) close(w->conn_fd);
    if (w->listen_fd >= 0) close(w->listen_fd);
    free(w);
}
