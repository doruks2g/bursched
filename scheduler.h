/*
 * ============================================================================
 *  scheduler.h  —  I/O-Driven Asenkron Task Scheduler
 * ============================================================================
 *
 *  AŞAMA 1: Sistem mimarisi ve temel veri yapıları.
 *
 *  Tasarım özeti:
 *    - epoll tek merkezi bekleme noktasıdır. inotify fd'si ve worker->main
 *      sinyal eventfd'si epoll'un izlediği iki kaynaktır. Ana thread bu ikisi
 *      dışında hiçbir şeyi poll etmez; kernel seviyesinde tam blok (EPOLLWAIT
 *      timeout = -1) ile uyur.
 *    - Task struct'ı worker'ların sık erişeceği alanları (fd, offset, size,
 *      status) tek bir cache line'a (64 byte) sığdıracak şekilde
 *      hizalanmıştır.
 *    - Pre-fetch kuyruğu (henüz hazırlanmamış görevler) ile çalışma kuyruğu
 *      (worker'ların tükettiği görevler) ayrı struct'lardır; aralarında
 *      padding var, böylece bir kuyruğun head/tail sayaçları diğerinin
 *      cache line'ına taşıp false-sharing yaratmaz.
 *    - Thread pool senkronizasyonu: mutex + pthread_cond_t. Worker'lar boşta
 *      cond_wait üzerinde bloke; ana thread epoll olayı aldığında
 *      cond_broadcast ile uyandırır.
 *
 * ============================================================================
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#define _DEFAULT_SOURCE   /* inotify, eventfd için gerekli */
#define _GNU_SOURCE        /* pthread_setaffinity_np gibi GNU uzantıları */

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/inotify.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

/* ========================================================================
 *  Konfigürasyon Makroları
 * ======================================================================== */

#define SCHED_CACHE_LINE_SIZE   64
#define SCHED_MAX_WORKERS       32
#define SCHED_TASK_QUEUE_CAP    1024      /* 2'nin kuvveti — modulo yerine bitmask */
#define SCHED_PREFETCH_CAP      256
#define SCHED_MAX_EPOLL_EVENTS  16
#define SCHED_INOTIFY_BUF_LEN   (64 * (sizeof(struct inotify_event) + NAME_MAX + 1))
#define SCHED_MAX_WATCHES       8192       /* /home gibi geniş ağaçlar için recursive watch limiti */
#define SCHED_PATH_MAX          512

#define SCHED_ALIGNED           __attribute__((aligned(SCHED_CACHE_LINE_SIZE)))

/* ========================================================================
 *  Watch Tablosu — wd (watch descriptor) -> path eşlemesi
 *
 *  inotify her izlenen dizin için bir tam sayı (wd) döndürür; event geldiğinde
 *  yalnızca bu wd ve dosya adı (relative) verilir, tam yol verilmez. Recursive
 *  izleme (örn. /home altındaki tüm alt dizinler) için bu tabloyu doğrusal
 *  arama ile tarıyoruz — SCHED_MAX_WATCHES ölçeğinde (birkaç bin), doğrusal
 *  arama pratikte yeterlidir; hash tablosuna geçiş yalnızca çok daha büyük
 *  ağaçlarda gerekli olur.
 * ======================================================================== */

typedef struct {
    int     wd;                      /* inotify_add_watch dönüş değeri, -1 = boş slot */
    char    path[SCHED_PATH_MAX];    /* bu wd'nin karşılık geldiği tam dizin yolu     */
} watch_entry_t;

/* ========================================================================
 *  Task — Tek bir iş birimi
 *
 *  Struct ikiye bölünmüştür:
 *    - task_hot_t  : worker'ın her tüketimde dokunduğu alanlar (fd, offset,
 *                    size, state, mapped_base, mapped_len). Tam olarak tek
 *                    64 byte'lık cache line'a sığar.
 *    - task_t      : hot alanı + soğuk alan (path). path yalnızca log/hata
 *                    ayıklama sırasında okunur; worker'ın sıcak yoluna dahil
 *                    edilirse gereksiz yere cache line'ı şişirir.
 *
 *  Böylece bir worker bir Task'ı işlemek için yalnızca tek bir cache line
 *  (task_t.hot) okur; path alanı ayrı satır(lar)da kalır ve sıcak yolu
 *  etkilemez.
 * ======================================================================== */

typedef enum {
    TASK_STATE_PENDING = 0,   /* pre-fetch kuyruğunda, henüz RAM'de hazır değil */
    TASK_STATE_READY,         /* posix_fadvise/mmap tamamlandı, işlenmeye hazır */
    TASK_STATE_RUNNING,       /* bir worker tarafından işleniyor */
    TASK_STATE_DONE           /* tamamlandı */
} task_state_t;

typedef struct {
    int             fd;              /* açık dosya tanımlayıcı              */
    off_t           offset;          /* işlenecek bölgenin başlangıcı       */
    size_t          size;            /* işlenecek veri boyutu               */
    task_state_t    state;           /* mevcut durum                        */

    void           *mapped_base;     /* mmap ile RAM'e çekilmiş bölge       */
    size_t          mapped_len;      /* mmap uzunluğu (munmap için gerekli) */
} task_hot_t;

typedef struct {
    task_hot_t      hot;             /* sıcak alanlar — tek cache line      */
    char            path[192];       /* soğuk alan — yalnızca log/tanılama  */
} SCHED_ALIGNED task_t;

/* ========================================================================
 *  Ring Buffer tabanlı Görev Kuyruğu
 *
 *  head/tail sayaçları ayrı cache line'lara hizalanmıştır: worker'lar
 *  sürekli head'i güncellerken üretici (epoll callback) tail'i güncelliyor.
 *  Aynı cache line'da olsalardı her iki taraf da birbirinin yazdığı
 *  satırı sürekli invalidate ederdi (false sharing).
 * ======================================================================== */

typedef struct {
    task_t   *slots[SCHED_TASK_QUEUE_CAP];

    SCHED_ALIGNED volatile uint32_t head;   /* worker'ların tükettiği uç    */
    SCHED_ALIGNED volatile uint32_t tail;   /* üreticinin eklediği uç       */

    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
} task_queue_t;

/* ========================================================================
 *  Pre-fetch Kuyruğu
 *
 *  Zamanı yaklaşan ama henüz RAM'e çekilmemiş görevleri tutar. Ayrı bir
 *  struct olmasının sebebi: bu kuyruğa erişim frekansı çalışma kuyruğuna
 *  göre çok daha düşüktür (yalnızca zamanlayıcı taraması sırasında
 *  dokunulur), bu yüzden worker'ların sıcak yoluyla aynı cache line'ı
 *  paylaşmasını istemeyiz.
 * ======================================================================== */

typedef struct {
    task_t   *slots[SCHED_PREFETCH_CAP];

    SCHED_ALIGNED volatile uint32_t head;
    SCHED_ALIGNED volatile uint32_t tail;

    pthread_mutex_t lock;
} prefetch_queue_t;

/* ========================================================================
 *  Worker Thread Argümanları
 *
 *  Her worker kendi struct kopyasını alır (id, pool referansı). Padding,
 *  bir worker'ın argüman struct'ının bitişik worker'ın struct'ıyla aynı
 *  cache line'a düşmesini engeller — her worker kendi id'sini sürekli
 *  okuyacağından bu satır kendine ait kalmalı.
 * ======================================================================== */

typedef struct scheduler scheduler_t;

typedef struct {
    int             worker_id;
    scheduler_t    *sched;
    pthread_t       thread_handle;

    char            _pad[SCHED_CACHE_LINE_SIZE - (sizeof(int) + sizeof(void *) + sizeof(pthread_t)) % SCHED_CACHE_LINE_SIZE];
} SCHED_ALIGNED worker_arg_t;

/* ========================================================================
 *  Thread Pool
 * ======================================================================== */

typedef struct {
    worker_arg_t    workers[SCHED_MAX_WORKERS];
    int             worker_count;

    volatile bool   shutdown_requested;
} thread_pool_t;

/* ========================================================================
 *  Scheduler — Ana Orkestratör
 *
 *  epoll_fd tek bekleme noktasıdır. inotify_fd ve wake_event_fd bu epoll'a
 *  EPOLLIN ile eklenir. main_loop bu ikisi dışında hiçbir fd'yi izlemez.
 * ======================================================================== */

struct scheduler {
    int                 epoll_fd;
    int                 inotify_fd;
    int                 wake_event_fd;    /* worker -> main sinyal kanalı   */

    task_queue_t        run_queue;
    prefetch_queue_t     prefetch_queue;

    thread_pool_t        pool;

    volatile bool        running;

    /* Recursive izleme: her alt dizin ayrı bir wd alır, bu tabloda saklanır.
     * handle_inotify_events() olay geldiğinde ev->wd ile bu tabloda arama
     * yapıp tam dizin yolunu bulur, ardından ev->name ile birleştirir. */
    watch_entry_t        watches[SCHED_MAX_WATCHES];
    int                  watch_count;
};

/* ========================================================================
 *  Genel API (Aşama 1 kapsamında yalnızca imzalar)
 * ======================================================================== */

int   scheduler_init(scheduler_t *sched, int worker_count);

/* Verilen kök dizini ve TÜM alt dizinlerini recursive olarak izlemeye alır.
 * Sembolik linkler takip edilmez (döngü riski). Erişim izni olmayan alt
 * dizinler atlanır, hata olarak sayılmaz (örn. /home altında başka
 * kullanıcıların izin vermediği klasörler). */
int   scheduler_watch_directory(scheduler_t *sched, const char *root_path);

int   scheduler_run(scheduler_t *sched);
void  scheduler_shutdown(scheduler_t *sched);
void  scheduler_destroy(scheduler_t *sched);

#endif /* SCHEDULER_H */
