#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>

#include "../include/cluster.h"
#include "protocol.h"
#include "net_utils.h"

#define MAX_WORKERS 64

typedef struct {
    char     host[64];
    uint16_t port;
    int      fd;
} WorkerEntry;

struct ClusterControl {
    int         num_workers_required;
    int         timeout_sec;
    WorkerEntry workers[MAX_WORKERS];
    int         num_workers;  // сколько воркеров задали в make
};

// дропнуть всех
static void abort_all(ClusterControl *ctrl)
{
    ControlMsg msg = { .tag = MSG_ABORT };
    for (int i = 0; i < ctrl->num_workers; i++) {
        if (ctrl->workers[i].fd >= 0)
            send_all(ctrl->workers[i].fd, &msg, sizeof(msg));
    }
}

// распределяет задачи и собирает результаты
static bool distribute_and_collect(ClusterControl *ctrl,
                                    double          a,
                                    double          b,
                                    uint64_t        num_intervals,
                                    double         *out_sum)
{
    // индексы активных воркеров
    int active[MAX_WORKERS];
    int nw = 0;
    for (int i = 0; i < ctrl->num_workers; i++) {
        if (ctrl->workers[i].fd >= 0)
            active[nw++] = i;
    }
    if (nw == 0) { fprintf(stderr, "[control] No active workers\n"); return false; }

    double   width    = (b - a) / (double)nw;
    uint64_t n_each   = num_intervals / (uint64_t)nw;
    if (n_each == 0) n_each = 1;

    // отправление задач
    for (int i = 0; i < nw; i++) {
        double ra = a + i * width;
        double rb = (i == nw - 1) ? b : ra + width;

        TaskMsg task = {
            .tag           = MSG_TASK,
            .range_start   = ra,
            .range_end     = rb,
            .num_intervals = n_each,
        };

        if (!send_all(ctrl->workers[active[i]].fd, &task, sizeof(task))) {
            fprintf(stderr, "[control] Failed to send task to worker %d\n", active[i]);
            abort_all(ctrl);
            return false;
        }
    }

    // результаты
    struct pollfd pfds[MAX_WORKERS];
    bool got[MAX_WORKERS] = {false};
    int received = 0;

    for (int i = 0; i < nw; i++) {
        pfds[i].fd     = ctrl->workers[active[i]].fd;
        pfds[i].events = POLLIN | POLLHUP | POLLERR;
    }

    int timeout_ms = (ctrl->timeout_sec > 0) ? ctrl->timeout_sec * 1000 : -1;
    double sum = 0.0;

    while (received < nw) {
        int ready = poll(pfds, (nfds_t)nw, timeout_ms);
        if (ready < 0) {
            perror("[control] poll");
            abort_all(ctrl);
            return false;
        }
        if (ready == 0) {
            fprintf(stderr, "[control] Timeout waiting for results\n");
            abort_all(ctrl);
            return false;
        }

        for (int i = 0; i < nw; i++) {
            if (got[i]) continue;
            if (pfds[i].revents == 0) continue;

            if (pfds[i].revents & (POLLHUP | POLLERR)) {
                fprintf(stderr, "[control] Worker %d disconnected\n", active[i]);
                abort_all(ctrl);
                return false;
            }
            if (!(pfds[i].revents & POLLIN)) continue;

            uint8_t tag;
            if (!recv_all(ctrl->workers[active[i]].fd, &tag, 1)) {
                fprintf(stderr, "[control] Worker %d: connection lost\n", active[i]);
                abort_all(ctrl);
                return false;
            }

            if (tag == MSG_ABORT) {
                fprintf(stderr, "[control] Worker %d sent ABORT\n", active[i]);
                abort_all(ctrl);
                return false;
            }
            if (tag != MSG_RESULT) {
                fprintf(stderr, "[control] Worker %d: unexpected tag 0x%02x\n", active[i], tag);
                abort_all(ctrl);
                return false;
            }

            ResultMsg rmsg;
            rmsg.tag = tag;
            if (!recv_all(ctrl->workers[active[i]].fd, (char *)&rmsg + 1, sizeof(rmsg) - 1)) {
                fprintf(stderr, "[control] Worker %d: failed to read result\n", active[i]);
                abort_all(ctrl);
                return false;
            }
            if (rmsg.status != 0) {
                fprintf(stderr, "[control] Worker %d reported error\n", active[i]);
                abort_all(ctrl);
                return false;
            }

            sum += rmsg.result;
            got[i] = true;
            received++;
        }
    }

    *out_sum = sum;
    return true;
}


ClusterControl *cluster_control_create(int num_workers_required, int timeout_sec)
{
    ClusterControl *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->num_workers_required = num_workers_required;
    c->timeout_sec          = timeout_sec;
    for (int i = 0; i < MAX_WORKERS; i++)
        c->workers[i].fd = -1;
    return c;
}

bool cluster_control_add_worker(ClusterControl *ctrl, const char *host, uint16_t port)
{
    if (ctrl->num_workers >= MAX_WORKERS) {
        fprintf(stderr, "[control] Too many workers (max %d)\n", MAX_WORKERS);
        return false;
    }
    WorkerEntry *e = &ctrl->workers[ctrl->num_workers++];
    strncpy(e->host, host, sizeof(e->host) - 1);
    e->port = port;
    e->fd   = -1;
    return true;
}

bool cluster_control_connect(ClusterControl *ctrl)
{
    signal(SIGPIPE, SIG_IGN);

    int connected = 0;
    for (int i = 0; i < ctrl->num_workers; i++) {
        WorkerEntry *e = &ctrl->workers[i];

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { perror("[control] socket"); continue; }

        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port   = htons(e->port),
        };
        if (inet_pton(AF_INET, e->host, &addr.sin_addr) <= 0) {
            fprintf(stderr, "[control] Invalid address: %s\n", e->host);
            close(fd);
            continue;
        }

        // Пробуем подключиться до 10 раз с паузой в секунду
        bool ok = false;
        for (int attempt = 0; attempt < 10; attempt++) {
            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                ok = true;
                break;
            }
            if (errno != ECONNREFUSED && errno != ETIMEDOUT) break;
            fprintf(stderr, "[control] Worker %s:%u not ready, retrying (%d/10)…\n",
                    e->host, e->port, attempt + 1);
            sleep(1);
        }

        if (!ok) {
            perror("[control] connect");
            close(fd);
            continue;
        }

        tune_socket(fd, ctrl->timeout_sec);
        e->fd = fd;
        connected++;
        fprintf(stderr, "[control] Connected to worker %s:%u\n", e->host, e->port);
    }


    if (connected < ctrl->num_workers_required) {
        fprintf(stderr,
                "[control] Only %d/%d required workers reachable – aborting\n",
                connected, ctrl->num_workers_required);
        return false;
    }

    return true;
}

static double elapsed_sec(struct timespec start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec  - start.tv_sec)
         + (double)(now.tv_nsec - start.tv_nsec) * 1e-9;
}

double cluster_control_compute_integral(ClusterControl *ctrl,
                                         double          a,
                                         double          b,
                                         double          epsilon,
                                        uint64_t        num_intervals_init)
{
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    uint64_t n   = num_intervals_init;
    double   r1  = 0.0, r2 = 0.0;

    if (!distribute_and_collect(ctrl, a, b, n, &r1)) return (double)NAN;
    fprintf(stderr, "[control] n=%-12lu  result=%.10f\n", (unsigned long)n, r1);

    // перебираем n, пока sum на предыдущей итерации не будет равна сумме на текущей итерации
    for (int iter = 0; iter < 60; iter++) {
        if (ctrl->timeout_sec > 0 && elapsed_sec(start) >= ctrl->timeout_sec) {
            fprintf(stderr, "[control] Computation timeout after %.1f s\n",
                    elapsed_sec(start));
            abort_all(ctrl);
            return (double)NAN;
        }

        n *= 2;
        if (!distribute_and_collect(ctrl, a, b, n, &r2)) return (double)NAN;

        fprintf(stderr, "[control] n=%-12lu  result=%.10f  Δ=%.2e\n",
                (unsigned long)n, r2, fabs(r2 - r1));

        if (fabs(r2 - r1) < epsilon) {
            ControlMsg done = { .tag = MSG_DONE };
            for (int i = 0; i < ctrl->num_workers; i++) {
                if (ctrl->workers[i].fd >= 0)
                    send_all(ctrl->workers[i].fd, &done, sizeof(done));
            }
            printf("%.15f\n", r2);
            return r2;
        }

        r1 = r2;
    }

    fprintf(stderr, "[control] Did not converge after 60 iterations\n");
    abort_all(ctrl);
    return (double)NAN;
}

void cluster_control_destroy(ClusterControl *ctrl)
{
    if (!ctrl) return;
    for (int i = 0; i < ctrl->num_workers; i++) {
        if (ctrl->workers[i].fd >= 0) {
            close(ctrl->workers[i].fd);
            ctrl->workers[i].fd = -1;
        }
    }
    free(ctrl);
}
