/*
 * ============================================================================
 *  queue.h  —  Ring Buffer Kuyruk İşlemleri
 * ============================================================================
 *
 *  task_queue_t ve prefetch_queue_t için push/pop işlemleri.
 *  Kapasiteler 2'nin kuvveti olduğundan modulo yerine bitmask (& (CAP-1))
 *  kullanılır — bu, bölme işleminden çok daha ucuzdur ve dallanma tahminine
 *  (branch prediction) daha dostudur.
 * ============================================================================
 */

#ifndef QUEUE_H
#define QUEUE_H

#include "scheduler.h"

/* run_queue işlemleri (worker'ların tükettiği kuyruk) */
int   task_queue_init(task_queue_t *q);
void  task_queue_destroy(task_queue_t *q);
bool  task_queue_push(task_queue_t *q, task_t *task);   /* üretici (epoll callback) çağırır */
task_t *task_queue_pop_blocking(task_queue_t *q, volatile bool *shutdown_flag);

/* prefetch_queue işlemleri (henüz RAM'e çekilmemiş görevler) */
int   prefetch_queue_init(prefetch_queue_t *q);
void  prefetch_queue_destroy(prefetch_queue_t *q);
bool  prefetch_queue_push(prefetch_queue_t *q, task_t *task);
task_t *prefetch_queue_pop(prefetch_queue_t *q);

#endif /* QUEUE_H */
