#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "../include/cluster.h"

// Usage:
//   control_app <num_workers_required> <timeout_sec> <host:port> [<host:port> …] [n0]
//
// n0 — начальное число шагов (по умолчанию 1000, для бенчмарка используйте 500000000)
//
// Example:
//   control_app 2 60 127.0.0.1:9001 127.0.0.1:9002
//   control_app 1 60 127.0.0.1:9001 500000000

// Для интеграла: итоговый результат — сумма частичных.
static double combine_sum(const double *results, int count)
{
    double s = 0.0;
    for (int i = 0; i < count; i++) s += results[i];
    return s;
}

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr,
                "Usage: %s <num_workers_required> <timeout_sec> host:port [host:port ...]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    int num_required = atoi(argv[1]);
    int timeout      = atoi(argv[2]);

    ClusterControl *ctrl = cluster_control_create(num_required, timeout);
    if (!ctrl) return EXIT_FAILURE;

    int last_worker = argc - 1;
    if (last_worker >= 4 && !strchr(argv[last_worker], ':'))
        last_worker--;  // последний аргумент — n0, не адрес воркера

    for (int i = 3; i <= last_worker; i++) {
        char worker_host[64] = {0};
        int  worker_port     = 0;
        if (sscanf(argv[i], "%63[^:]:%d", worker_host, &worker_port) != 2) {
            fprintf(stderr, "Bad worker address '%s', expected host:port\n", argv[i]);
            cluster_control_destroy(ctrl);
            return EXIT_FAILURE;
        }
        if (!cluster_control_add_worker(ctrl, worker_host, (uint16_t)worker_port)) {
            cluster_control_destroy(ctrl);
            return EXIT_FAILURE;
        }
    }

    if (!cluster_control_connect(ctrl)) {
        cluster_control_destroy(ctrl);
        return EXIT_FAILURE;
    }

    const double EPSILON = 1e-9;
    uint64_t steps = (last_worker < argc - 1)
                     ? (uint64_t)atoll(argv[argc - 1])
                     : 1000ULL;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    double result = cluster_control_compute(ctrl, EPSILON, steps, combine_sum);

    clock_gettime(CLOCK_MONOTONIC, &t1);

    if (isnan(result)) {
        fprintf(stderr, "[control] Computation failed\n");
        cluster_control_destroy(ctrl);
        return EXIT_FAILURE;
    }

    double elapsed = (double)(t1.tv_sec  - t0.tv_sec)
                   + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;

    printf("\n");
    printf("Integral  = %.15f\n", result);
    printf("pi        = %.15f\n", M_PI);
    printf("Error     = %.2e\n",  fabs(result - M_PI));
    printf("Time      = %.3f s\n", elapsed);

    cluster_control_destroy(ctrl);
    return EXIT_SUCCESS;
}
