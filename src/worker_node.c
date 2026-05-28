#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>

#include "../include/cluster.h"
#include "protocol.h"
#include "net_utils.h"


typedef struct {
    cluster_subtask_fn_t fn;
    int      idx;
    int      total;
    uint64_t n;
    double   result;
} WPoolTask;

typedef struct {
    pthread_t       *tids;
    WPoolTask       *tasks;
    int              nthreads;
    pthread_mutex_t  mu;
    pthread_cond_t   work_cond;
    pthread_cond_t   done_cond;
    int              n_tasks;
    int              next_task;
    int              n_done;
    bool             shutdown;
} WThreadPool;

static void *wpool_thread(void *arg)
{
    WThreadPool *pool = arg;
    for (;;) {
        pthread_mutex_lock(&pool->mu);
        // спим, пока нет задач и не пришёл shutdown
        while (pool->next_task >= pool->n_tasks && !pool->shutdown)
            pthread_cond_wait(&pool->work_cond, &pool->mu);
        // выход, если shutdown
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->mu);
            return NULL;
        }

        // берём следующую задачу
        int i = pool->next_task++;
        WPoolTask t = pool->tasks[i];
        pthread_mutex_unlock(&pool->mu);

        double res = t.fn(t.idx, t.total, t.n);

        pthread_mutex_lock(&pool->mu);
        pool->tasks[i].result = res;
        if (++pool->n_done == pool->n_tasks)
            pthread_cond_signal(&pool->done_cond);
        pthread_mutex_unlock(&pool->mu);
    }
}

static bool wpool_init(WThreadPool *pool, int nthreads)
{
    pool->tids  = malloc((size_t)nthreads * sizeof(pthread_t));
    pool->tasks = malloc((size_t)nthreads * sizeof(WPoolTask));
    if (!pool->tids || !pool->tasks) {
        free(pool->tids); free(pool->tasks); return false;
    }
    pool->nthreads  = 0;
    pool->n_tasks   = 0;
    pool->next_task = 0;
    pool->n_done    = 0;
    pool->shutdown  = false;
    pthread_mutex_init(&pool->mu, NULL);
    pthread_cond_init(&pool->work_cond, NULL);
    pthread_cond_init(&pool->done_cond, NULL);
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&pool->tids[i], NULL, wpool_thread, pool) != 0) {
            perror("[worker] pthread_create");
            pool->shutdown = true;
            pthread_cond_broadcast(&pool->work_cond);
            for (int j = 0; j < pool->nthreads; j++)
                pthread_join(pool->tids[j], NULL);
            free(pool->tids); free(pool->tasks);
            return false;
        }
        pool->nthreads++;
    }
    return true;
}

static void wpool_destroy(WThreadPool *pool)
{
    pthread_mutex_lock(&pool->mu);
    pool->shutdown = true;
    pthread_cond_broadcast(&pool->work_cond);
    pthread_mutex_unlock(&pool->mu);
    for (int i = 0; i < pool->nthreads; i++)
        pthread_join(pool->tids[i], NULL);
    pthread_mutex_destroy(&pool->mu);
    pthread_cond_destroy(&pool->work_cond);
    pthread_cond_destroy(&pool->done_cond);
    free(pool->tids);
    free(pool->tasks);
}

/* Запускает fn параллельно в nthreads потоках, возвращает сумму результатов.
 * Каждый поток получает уточнённые (idx*nw+t, total*nw, n). */
static double wpool_run(WThreadPool *pool, cluster_subtask_fn_t fn,
                         int idx, int total, uint64_t n)
{
    int nw = pool->nthreads;

    pthread_mutex_lock(&pool->mu);
    pool->n_tasks   = nw;
    pool->next_task = 0;
    pool->n_done    = 0;
    for (int t = 0; t < nw; t++) {
        pool->tasks[t].fn     = fn;
        pool->tasks[t].idx    = idx * nw + t;
        pool->tasks[t].total  = total * nw;
        pool->tasks[t].n      = n;
        pool->tasks[t].result = 0.0;
    }
    pthread_cond_broadcast(&pool->work_cond);

    while (pool->n_done < pool->n_tasks)
        //отпускает мьютекс
        pthread_cond_wait(&pool->done_cond, &pool->mu);

    double sum = 0.0;
    for (int t = 0; t < nw; t++)
        sum += pool->tasks[t].result;
    pthread_mutex_unlock(&pool->mu);
    return sum;
}


struct ClusterWorker {
    int          max_cores;
    int          timeout_sec;
    uint16_t     port;
    int          listen_fd;
    int          conn_fd;
    WThreadPool  pool;
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
    if (max_cores < 1) max_cores = 1;
    ClusterWorker *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->max_cores   = max_cores;
    w->timeout_sec = timeout_sec;
    w->port        = port;
    w->listen_fd   = -1;
    w->conn_fd     = -1;
    if (!wpool_init(&w->pool, max_cores)) {
        free(w);
        return NULL;
    }
    return w;
}

bool cluster_worker_run(ClusterWorker *w, cluster_subtask_fn_t fn)
{
    signal(SIGPIPE, SIG_IGN);

    if (!worker_open_listen(w)) return false;

    fprintf(stderr, "[worker port=%u] Listening\n", w->port);

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
            fprintf(stderr, "[worker port=%u] Control disconnected unexpectedly\n", w->port);
            ok = false;
            break;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
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

        TaskMsg task;
        task.tag = tag;
        if (!recv_all(w->conn_fd, (char *)&task + 1, sizeof(task) - 1)) {
            fprintf(stderr, "[worker port=%u] Failed to receive task body\n", w->port);
            ok = false;
            break;
        }

        fprintf(stderr,
                "[worker port=%u] Task idx=%d/%d n=%lu cores=%d\n",
                w->port, task.idx, task.total, (unsigned long)task.n, w->max_cores);

        double result = wpool_run(&w->pool, fn, task.idx, task.total, task.n);

        ResultMsg rmsg = {
            .tag    = MSG_RESULT,
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

    close(w->conn_fd);   w->conn_fd   = -1;
    close(w->listen_fd); w->listen_fd = -1;
    return ok;
}

void cluster_worker_destroy(ClusterWorker *w)
{
    if (!w) return;
    wpool_destroy(&w->pool);
    if (w->conn_fd   >= 0) close(w->conn_fd);
    if (w->listen_fd >= 0) close(w->listen_fd);
    free(w);
}
