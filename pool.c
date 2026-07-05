/*
 * ============================================================================
 *  pool.c  —  POSIX Thread Havuzu (implementasyon)
 * ============================================================================
 *
 *  Strateji: worker_count kadar thread önceden (pre-allocated) oluşturulur,
 *  yaşam boyu (lifetime) boyunca yeniden yaratılmaz. Her worker sonsuz
 *  döngüde run_queue'dan blocking pop yapar; kuyruk boşken kernel seviyesinde
 *  uyur (busy-wait yok), görev geldiğinde en düşük gecikmeyle işler.
 * ============================================================================
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "pool.h"
#include "queue.h"
#include "prefetch.h"

/* Her worker'ın gerçek çalışma fonksiyonu. Task içeriğini işler — bu iskelet
 * aşamada "işleme" yalnızca mmap edilmiş veriyi dokunup READY->DONE'a
 * taşımaktır; gerçek iş yükü (örn. arama, sıkıştırma) üst katmanda eklenir. */
static void process_task(task_t *task)
{
    if (task->hot.state == TASK_STATE_PENDING) {
        /* Prefetch aşaması atlanmışsa (doğrudan run_queue'ya düşen görev),
         * worker kendi bloke eden mmap'ini burada yapar. Bu, "yavaş yol"dur;
         * asıl hız kazancı pre-fetch kuyruğunun bu duruma düşmemesini
         * sağlamaktan gelir. */
        prefetch_map_into_memory(task);
    }

    task->hot.state = TASK_STATE_RUNNING;

    /* --- Gerçek iş yükü buraya eklenecek (Aşama 5+) --- */

    task->hot.state = TASK_STATE_DONE;

    prefetch_release(task);
}

static void *worker_main(void *arg)
{
    worker_arg_t *self  = (worker_arg_t *)arg;
    scheduler_t  *sched = self->sched;

    for (;;) {
        task_t *task = task_queue_pop_blocking(&sched->run_queue,
                                                &sched->pool.shutdown_requested);
        if (task == NULL) {
            /* shutdown_requested true ve kuyruk boş — worker çıkışa gider. */
            break;
        }

        process_task(task);

        /* Ana thread'e "iş bitti" sinyali — eventfd üzerinden. Ana thread bu
         * sinyali epoll ile bekleyen üçüncü kaynak olarak görür (opsiyonel;
         * shutdown senkronizasyonu ve istatistik için kullanılabilir). */
        uint64_t one = 1;
        ssize_t  written = write(sched->wake_event_fd, &one, sizeof(one));
        (void)written; /* eventfd yazımı 8 byte'tan küçük olamaz; hata ihmal edilebilir düzeyde nadir */
    }

    return NULL;
}

int thread_pool_init(thread_pool_t *pool, scheduler_t *sched, int worker_count)
{
    if (worker_count <= 0 || worker_count > SCHED_MAX_WORKERS) {
        fprintf(stderr, "[pool] invalid worker_count=%d (max=%d)\n",
                worker_count, SCHED_MAX_WORKERS);
        return -1;
    }

    pool->worker_count      = worker_count;
    pool->shutdown_requested = false;

    for (int i = 0; i < worker_count; i++) {
        pool->workers[i].worker_id = i;
        pool->workers[i].sched     = sched;

        int rc = pthread_create(&pool->workers[i].thread_handle, NULL,
                                 worker_main, &pool->workers[i]);
        if (rc != 0) {
            fprintf(stderr, "[pool] pthread_create failed at worker %d: %d\n", i, rc);

            /* Kısmi başarısızlıkta zaten oluşturulmuş thread'leri temiz kapat. */
            pool->shutdown_requested = true;
            pthread_cond_broadcast(&sched->run_queue.not_empty);
            for (int j = 0; j < i; j++) {
                pthread_join(pool->workers[j].thread_handle, NULL);
            }
            return -1;
        }
    }

    return 0;
}

void thread_pool_shutdown(thread_pool_t *pool, task_queue_t *queue)
{
    pthread_mutex_lock(&queue->lock);
    pool->shutdown_requested = true;
    pthread_cond_broadcast(&queue->not_empty);   /* tüm uyuyan worker'ları uyandır */
    pthread_mutex_unlock(&queue->lock);
}

void thread_pool_join(thread_pool_t *pool)
{
    for (int i = 0; i < pool->worker_count; i++) {
        pthread_join(pool->workers[i].thread_handle, NULL);
    }
}
