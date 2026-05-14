#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "../include/cluster.h"

// Usage:
//   control_app <num_workers_required> <timeout_sec> <host:port> [<host:port> …]
//
// Example (two workers on localhost):
//   control_app 2 60 127.0.0.1:9001 127.0.0.1:9002
//
// The program computes  int (4/(1+x^2) dx)  which equals pi.

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

    for (int i = 3; i < argc; i++) {
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

    // вычисления
    const double A       = 0.0;
    const double B       = 1.0;
    const double EPSILON = 1e-9;
    const uint64_t N0    = 1000ULL;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    double result = cluster_control_compute_integral(ctrl, A, B, EPSILON, N0);

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
    printf("pi         = %.15f\n", M_PI);
    printf("Error     = %.2e\n",  fabs(result - M_PI));
    printf("Time   = %.3f s\n", elapsed);

    cluster_control_destroy(ctrl);
    return EXIT_SUCCESS;
}
