/*
 * ============================================================================
 *  pool.h  —  POSIX Thread Havuzu
 * ============================================================================
 *
 *  Worker'lar run_queue üzerinden görev tüketir. İş yokken cond_wait ile
 *  derin uykuya geçerler (bkz. queue.c: task_queue_pop_blocking). Bir görev
 *  geldiğinde uyanır, işler, tekrar uykuya döner.
 * ============================================================================
 */

#ifndef POOL_H
#define POOL_H

#include "scheduler.h"

int   thread_pool_init(thread_pool_t *pool, scheduler_t *sched, int worker_count);
void  thread_pool_shutdown(thread_pool_t *pool, task_queue_t *queue);
void  thread_pool_join(thread_pool_t *pool);

#endif /* POOL_H */
