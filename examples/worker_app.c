#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>

#include "../include/cluster.h"

// f(x) = 4 / (1 + x^2) 
static double integrand(double x)
{
    return 4.0 / (1.0 + x * x);
}


typedef struct {
    double   a;
    double   b;
    uint64_t n;       
    double   result;
} ThreadWork;

static void *thread_integrate(void *arg)
{
    ThreadWork *t = (ThreadWork *)arg;
    double h   = (t->b - t->a) / (double)t->n;
    double sum = 0.0;
    for (uint64_t i = 0; i < t->n; i++) {
        double x = t->a + ((double)i + 0.5) * h;
        sum += integrand(x);
    }
    t->result = sum * h;
    return NULL;
}

// подзадача
static double compute_partial(double   range_start,
                               double   range_end,
                               uint64_t num_intervals,
                               int      num_cores)
{
    if (num_cores <= 1 || num_intervals == 0) {
        ThreadWork w = { range_start, range_end, num_intervals, 0.0 };
        thread_integrate(&w);
        return w.result;
    }

    pthread_t  *threads = malloc((size_t)num_cores * sizeof(pthread_t));
    ThreadWork *work    = malloc((size_t)num_cores * sizeof(ThreadWork));
    if (!threads || !work) {
        free(threads); free(work);
        return 0.0;
    }

    double   width  = (range_end - range_start) / (double)num_cores;
    uint64_t chunk  = num_intervals / (uint64_t)num_cores;

    for (int i = 0; i < num_cores; i++) {
        work[i].a = range_start + (double)i * width;
        work[i].b = (i == num_cores - 1) ? range_end : work[i].a + width;
        work[i].n = (i == num_cores - 1)
                    ? num_intervals - chunk * (uint64_t)(num_cores - 1)
                    : chunk;
        if (work[i].n == 0) work[i].n = 1;
        work[i].result = 0.0;
        pthread_create(&threads[i], NULL, thread_integrate, &work[i]);
    }

    double total = 0.0;
    for (int i = 0; i < num_cores; i++) {
        pthread_join(threads[i], NULL);
        total += work[i].result;
    }

    free(threads);
    free(work);
    return total;
}


int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <port> <max_cores> <timeout_sec>\n", argv[0]);
        return EXIT_FAILURE;
    }
    uint16_t port    = (uint16_t)atoi(argv[1]);
    int      cores   = atoi(argv[2]);
    int      timeout = atoi(argv[3]);

    if (cores < 1)   { fprintf(stderr, "max_cores must be >= 1\n"); return EXIT_FAILURE; }
    if (timeout < 0) { fprintf(stderr, "timeout_sec must be >= 0\n"); return EXIT_FAILURE; }

    ClusterWorker *w = cluster_worker_create(cores, timeout, port);
    if (!w) return EXIT_FAILURE;

    bool ok = cluster_worker_run(w, compute_partial);
    cluster_worker_destroy(w);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
