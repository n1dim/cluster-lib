#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "../include/cluster.h"

/* Прикладная функция */
static double integrand(double x)
{
    return 4.0 / (1.0 + x * x);
}

/* Коллбэк подзадачи — однопоточный.
 * Библиотека сама вызывает его параллельно из нескольких потоков,
 * передавая каждому уточнённые idx (номер потока ) /total (всего потоков). */
static double compute_partial(int idx, int total, uint64_t n)
{
    double   lo  = (double)idx       / (double)total;
    double   hi  = (double)(idx + 1) / (double)total;
    uint64_t n_sub = n / (uint64_t)total;
    if (n_sub == 0) n_sub = 1;

    double h   = (hi - lo) / (double)n_sub;
    double sum = 0.0;
    for (uint64_t i = 0; i < n_sub; i++) {
        double x = lo + ((double)i + 0.5) * h;
        sum += integrand(x);
    }
    return sum * h;
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
