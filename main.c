/*
 * ============================================================================
 *  main.c  —  Test/Demo Girişi
 * ============================================================================
 *
 *  Kullanım: ./scheduler <izlenecek-dizin> [worker_count]
 *
 *  Ctrl+C ile SIGINT gönderildiğinde scheduler_shutdown() çağrılır ve
 *  worker'lar temiz şekilde sonlandırılır.
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "scheduler.h"

static scheduler_t g_sched;

static void handle_sigint(int signo)
{
    (void)signo;
    /* Sinyal işleyicisinde yalnızca async-signal-safe işlemler yapılmalı;
     * running bayrağını false yapmak ve epoll_wait'in EINTR ile dönmesini
     * beklemek yeterlidir. */
    g_sched.running = false;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <watch-dir> [worker_count]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *watch_dir    = argv[1];
    int         worker_count = (argc >= 3) ? atoi(argv[2]) : 4;

    if (scheduler_init(&g_sched, worker_count) != 0) {
        fprintf(stderr, "scheduler_init failed\n");
        return EXIT_FAILURE;
    }

    if (scheduler_watch_directory(&g_sched, watch_dir) < 0) {
        fprintf(stderr, "scheduler_watch_directory failed\n");
        scheduler_destroy(&g_sched);
        return EXIT_FAILURE;
    }

    signal(SIGINT, handle_sigint);

    printf("[main] watching '%s' with %d workers — sleeping until events arrive...\n",
           watch_dir, worker_count);

    scheduler_run(&g_sched);

    printf("[main] shutting down...\n");
    scheduler_shutdown(&g_sched);
    scheduler_destroy(&g_sched);

    return EXIT_SUCCESS;
}
