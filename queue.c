/*
 * ============================================================================
 *  queue.c  —  Ring Buffer Kuyruk İşlemleri (implementasyon)
 * ============================================================================
 *
 *  Strateji: head/tail sayaçları hiç geri sarılmaz (monotonic artan uint32_t),
 *  indeksleme her erişimde (& (CAP-1)) ile yapılır. Bu klasik "Lamport
 *  circular buffer" yaklaşımıdır — overflow durumunda uint32_t doğal olarak
 *  wrap eder ve bitmask hâlâ doğru sonucu verir (CAP 2'nin kuvveti olduğu
 *  sürece).
 * ============================================================================
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "queue.h"

/* ==========================================================================
 *  run_queue (task_queue_t) — worker'ların tükettiği kuyruk
 * ========================================================================== */

int task_queue_init(task_queue_t *q)
{
    memset(q->slots, 0, sizeof(q->slots));
    q->head = 0;
    q->tail = 0;

    int rc = pthread_mutex_init(&q->lock, NULL);
    if (rc != 0) {
        return -1;
    }

    rc = pthread_cond_init(&q->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&q->lock);
        return -1;
    }

    return 0;
}

void task_queue_destroy(task_queue_t *q)
{
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
}

bool task_queue_push(task_queue_t *q, task_t *task)
{
    pthread_mutex_lock(&q->lock);

    uint32_t used = q->tail - q->head;
    if (used >= SCHED_TASK_QUEUE_CAP) {
        /* Kuyruk dolu — üretici tarafı (epoll callback) geri basınç uygular. */
        pthread_mutex_unlock(&q->lock);
        return false;
    }

    uint32_t idx = q->tail & (SCHED_TASK_QUEUE_CAP - 1);
    q->slots[idx] = task;
    q->tail++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return true;
}

task_t *task_queue_pop_blocking(task_queue_t *q, volatile bool *shutdown_flag)
{
    pthread_mutex_lock(&q->lock);

    while (q->head == q->tail && !*shutdown_flag) {
        /* Kuyruk boş: worker burada derin uykuya geçer. Bu satır, sistemin
         * "iş yokken CPU yakmama" garantisinin worker tarafındaki ayağıdır —
         * pthread_cond_wait kernel seviyesinde bloke eder, busy-loop yapmaz. */
        pthread_cond_wait(&q->not_empty, &q->lock);
    }

    if (q->head == q->tail && *shutdown_flag) {
        pthread_mutex_unlock(&q->lock);
        return NULL;
    }

    uint32_t idx = q->head & (SCHED_TASK_QUEUE_CAP - 1);
    task_t *task = q->slots[idx];
    q->slots[idx] = NULL;
    q->head++;

    pthread_mutex_unlock(&q->lock);
    return task;
}

/* ==========================================================================
 *  prefetch_queue (prefetch_queue_t) — düşük frekanslı, zamanlayıcı taramalı
 * ========================================================================== */

int prefetch_queue_init(prefetch_queue_t *q)
{
    memset(q->slots, 0, sizeof(q->slots));
    q->head = 0;
    q->tail = 0;
    return pthread_mutex_init(&q->lock, NULL) == 0 ? 0 : -1;
}

void prefetch_queue_destroy(prefetch_queue_t *q)
{
    pthread_mutex_destroy(&q->lock);
}

bool prefetch_queue_push(prefetch_queue_t *q, task_t *task)
{
    pthread_mutex_lock(&q->lock);

    uint32_t used = q->tail - q->head;
    if (used >= SCHED_PREFETCH_CAP) {
        pthread_mutex_unlock(&q->lock);
        return false;
    }

    uint32_t idx = q->tail & (SCHED_PREFETCH_CAP - 1);
    q->slots[idx] = task;
    q->tail++;

    pthread_mutex_unlock(&q->lock);
    return true;
}

task_t *prefetch_queue_pop(prefetch_queue_t *q)
{
    pthread_mutex_lock(&q->lock);

    if (q->head == q->tail) {
        pthread_mutex_unlock(&q->lock);
        return NULL;
    }

    uint32_t idx = q->head & (SCHED_PREFETCH_CAP - 1);
    task_t *task = q->slots[idx];
    q->slots[idx] = NULL;
    q->head++;

    pthread_mutex_unlock(&q->lock);
    return task;
}
