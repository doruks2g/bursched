/*
 * ============================================================================
 *  scheduler.c  —  Ana Orkestratör (implementasyon)
 * ============================================================================
 *
 *  scheduler_run() tek bekleme noktasıdır: epoll_wait(timeout = -1) ile
 *  kernel seviyesinde tam blok olur. Sadece iki fd izlenir:
 *    1. inotify_fd     — dizine dosya düştüğünde EPOLLIN tetiklenir
 *    2. wake_event_fd  — worker'lardan gelen "iş bitti" sinyali
 *
 *  Böylece ana thread, veri gelene kadar CPU'da görünür bir iz bırakmaz
 *  (~%0), veri geldiği an ise en düşük gecikmeyle run_queue'ya görev ekleyip
 *  worker'ları cond_broadcast ile ayağa kaldırır.
 * ============================================================================
 */

#define _DEFAULT_SOURCE

#include "scheduler.h"   /* _DEFAULT_SOURCE tanımını diğer sistem header'larından önce içermeli */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <ftw.h>
#include "queue.h"
#include "pool.h"
#include "prefetch.h"

/* ==========================================================================
 *  Statik yardımcılar
 * ========================================================================== */

static int epoll_add_fd(int epoll_fd, int fd, uint32_t events)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        fprintf(stderr, "[scheduler] epoll_ctl(ADD, fd=%d) failed: %s\n",
                fd, strerror(errno));
        return -1;
    }
    return 0;
}

/* nftw() callback imzası ek bir "context" parametresi almadığından, geçici
 * olarak taranmakta olan scheduler'a bu tek static işaretçi ile erişilir.
 * Bu, yalnızca scheduler_watch_directory() çağrısı sırasında, tek thread'de
 * kullanılır — eşzamanlılık riski yoktur. */
static scheduler_t *g_watch_target = NULL;

static int watch_table_add(scheduler_t *sched, int wd, const char *path)
{
    if (sched->watch_count >= SCHED_MAX_WATCHES) {
        fprintf(stderr, "[scheduler] watch table full (max=%d), skipping '%s'\n",
                SCHED_MAX_WATCHES, path);
        return -1;
    }

    watch_entry_t *entry = &sched->watches[sched->watch_count];
    entry->wd = wd;

    int written = snprintf(entry->path, sizeof(entry->path), "%s", path);
    if (written < 0 || (size_t)written >= sizeof(entry->path)) {
        fprintf(stderr, "[scheduler] watch path truncated: '%s'\n", path);
    }

    sched->watch_count++;
    return 0;
}

static const char *watch_table_lookup(scheduler_t *sched, int wd)
{
    for (int i = 0; i < sched->watch_count; i++) {
        if (sched->watches[i].wd == wd) {
            return sched->watches[i].path;
        }
    }
    return NULL;
}

/* nftw() her dizin/dosya için bu callback'i çağırır. Yalnızca dizinlere
 * (FTW_D) watch ekleriz; sembolik linkler FTW_PHYS bayrağı ile takip
 * edilmediğinden döngü riski yoktur. İzin hatası (EACCES) alınan alt
 * dizinler sessizce atlanır — /home altında başka kullanıcıların izin
 * vermediği klasörler tipik örnektir; bu tekil hata tüm taramayı
 * durdurmamalıdır. */
static int nftw_add_watch_callback(const char *fpath, const struct stat *sb,
                                    int typeflag, struct FTW *ftwbuf)
{
    (void)sb;
    (void)ftwbuf;

    if (typeflag != FTW_D) {
        return 0;   /* yalnızca dizinler izlenir */
    }

    int wd = inotify_add_watch(g_watch_target->inotify_fd, fpath,
                                IN_CLOSE_WRITE | IN_MOVED_TO);
    if (wd < 0) {
        if (errno == EACCES) {
            return 0;   /* izin yok — bu dalı atla, taramaya devam et */
        }
        fprintf(stderr, "[scheduler] inotify_add_watch('%s') failed: %s\n",
                fpath, strerror(errno));
        return 0;
    }

    watch_table_add(g_watch_target, wd, fpath);
    return 0;
}

/* Yeni bir task_t heap'te ayırır ve temel alanlarını doldurur.
 * NOT: Görev ömrü worker tarafından DONE olduğunda serbest bırakılır
 * (bkz. handle_inotify_event -> free sorumluluğu run_queue tüketicisinde). */
static task_t *task_create(const char *dir_path, const char *filename)
{
    task_t *task = calloc(1, sizeof(task_t));
    if (task == NULL) {
        fprintf(stderr, "[scheduler] calloc failed for task '%s/%s'\n",
                dir_path, filename);
        return NULL;
    }

    int written = snprintf(task->path, sizeof(task->path), "%s/%s", dir_path, filename);
    if (written < 0 || (size_t)written >= sizeof(task->path)) {
        fprintf(stderr, "[scheduler] path too long, truncated: '%s/%s'\n",
                dir_path, filename);
    }

    int fd = open(task->path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[scheduler] open('%s') failed: %s\n",
                task->path, strerror(errno));
        free(task);
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        fprintf(stderr, "[scheduler] fstat('%s') failed: %s\n",
                task->path, strerror(errno));
        close(fd);
        free(task);
        return NULL;
    }

    task->hot.fd     = fd;
    task->hot.offset = 0;
    task->hot.size   = (size_t)st.st_size;
    task->hot.state  = TASK_STATE_PENDING;

    return task;
}

/* inotify_fd üzerinde okunabilir veri olduğunda çağrılır. Gelen event'leri
 * ayrıştırır, ev->wd ile watch tablosundan ilgili dizin yolunu bulur, her
 * yeni dosya için bir task_t oluşturur, prefetch ipucu verir ve run_queue'ya
 * ekler. */
static void handle_inotify_events(scheduler_t *sched)
{
    char buf[SCHED_INOTIFY_BUF_LEN] __attribute__((aligned(__alignof__(struct inotify_event))));

    for (;;) {
        ssize_t len = read(sched->inotify_fd, buf, sizeof(buf));
        if (len < 0) {
            if (errno == EAGAIN) {
                break;   /* tüm mevcut event'ler tüketildi */
            }
            fprintf(stderr, "[scheduler] inotify read failed: %s\n", strerror(errno));
            break;
        }
        if (len == 0) {
            break;
        }

        ssize_t offset = 0;
        while (offset < len) {
            struct inotify_event *ev = (struct inotify_event *)(buf + offset);

            if ((ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) && ev->len > 0) {
                const char *dir_path = watch_table_lookup(sched, ev->wd);
                if (dir_path == NULL) {
                    fprintf(stderr,
                            "[scheduler] unknown wd=%d for file '%s', skipping\n",
                            ev->wd, ev->name);
                } else {
                    task_t *task = task_create(dir_path, ev->name);
                    if (task != NULL) {
                        /* Pik moda geçmeden önce kernel'e "yakında okunacak" ipucu ver. */
                        prefetch_hint_willneed(task);

                        if (!task_queue_push(&sched->run_queue, task)) {
                            fprintf(stderr,
                                    "[scheduler] run_queue full, dropping task '%s'\n",
                                    task->path);
                            close(task->hot.fd);
                            free(task);
                        }
                    }
                }
            }

            offset += (ssize_t)(sizeof(struct inotify_event) + ev->len);
        }
    }
}

/* wake_event_fd üzerinde okunabilir veri olduğunda çağrılır (worker'lardan
 * gelen tamamlanma sinyali). Şu an için yalnızca sayaç tüketir; ileride
 * istatistik/telemetri için genişletilebilir. */
static void handle_wake_event(scheduler_t *sched)
{
    uint64_t count;
    ssize_t  r = read(sched->wake_event_fd, &count, sizeof(count));
    (void)r;
    (void)sched;
}

/* ==========================================================================
 *  Genel API
 * ========================================================================== */

int scheduler_init(scheduler_t *sched, int worker_count)
{
    memset(sched, 0, sizeof(*sched));

    sched->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (sched->epoll_fd < 0) {
        fprintf(stderr, "[scheduler] epoll_create1 failed: %s\n", strerror(errno));
        return -1;
    }

    sched->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (sched->inotify_fd < 0) {
        fprintf(stderr, "[scheduler] inotify_init1 failed: %s\n", strerror(errno));
        close(sched->epoll_fd);
        return -1;
    }

    sched->wake_event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (sched->wake_event_fd < 0) {
        fprintf(stderr, "[scheduler] eventfd failed: %s\n", strerror(errno));
        close(sched->inotify_fd);
        close(sched->epoll_fd);
        return -1;
    }

    if (epoll_add_fd(sched->epoll_fd, sched->inotify_fd, EPOLLIN) != 0 ||
        epoll_add_fd(sched->epoll_fd, sched->wake_event_fd, EPOLLIN) != 0) {
        close(sched->wake_event_fd);
        close(sched->inotify_fd);
        close(sched->epoll_fd);
        return -1;
    }

    if (task_queue_init(&sched->run_queue) != 0) {
        fprintf(stderr, "[scheduler] task_queue_init failed\n");
        goto fail_fds;
    }

    if (prefetch_queue_init(&sched->prefetch_queue) != 0) {
        fprintf(stderr, "[scheduler] prefetch_queue_init failed\n");
        task_queue_destroy(&sched->run_queue);
        goto fail_fds;
    }

    if (thread_pool_init(&sched->pool, sched, worker_count) != 0) {
        fprintf(stderr, "[scheduler] thread_pool_init failed\n");
        prefetch_queue_destroy(&sched->prefetch_queue);
        task_queue_destroy(&sched->run_queue);
        goto fail_fds;
    }

    sched->running = true;
    return 0;

fail_fds:
    close(sched->wake_event_fd);
    close(sched->inotify_fd);
    close(sched->epoll_fd);
    return -1;
}

int scheduler_watch_directory(scheduler_t *sched, const char *root_path)
{
    /* nftw() callback'i context parametresi almadığı için g_watch_target
     * kullanılır. FTW_PHYS: sembolik linkleri takip etme (döngü riskini
     * ortadan kaldırır). FTW_MOUNT eklenmedi — kasıtlı olarak, /home altında
     * ayrı mount noktaları da (varsa) izlenebilsin diye; gerekirse
     * eklenebilir bilinen bir genişletme noktasıdır. */
    g_watch_target = sched;

    int rc = nftw(root_path, nftw_add_watch_callback, 16, FTW_PHYS);

    g_watch_target = NULL;

    if (rc != 0) {
        fprintf(stderr, "[scheduler] nftw('%s') failed: %s\n",
                root_path, strerror(errno));
        return -1;
    }

    if (sched->watch_count == 0) {
        fprintf(stderr, "[scheduler] no directories watched under '%s'\n", root_path);
        return -1;
    }

    printf("[scheduler] recursively watching %d directories under '%s'\n",
           sched->watch_count, root_path);

    return sched->watch_count;
}

int scheduler_run(scheduler_t *sched)
{
    struct epoll_event events[SCHED_MAX_EPOLL_EVENTS];

    while (sched->running) {
        /* timeout = -1: kernel seviyesinde tam blok. Veri gelene kadar bu
         * thread scheduler'dan tamamen çıkar, CPU'da ölçülebilir bir iz
         * bırakmaz. */
        int n = epoll_wait(sched->epoll_fd, events, SCHED_MAX_EPOLL_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "[scheduler] epoll_wait failed: %s\n", strerror(errno));
            return -1;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == sched->inotify_fd) {
                handle_inotify_events(sched);
            } else if (events[i].data.fd == sched->wake_event_fd) {
                handle_wake_event(sched);
            }
        }
    }

    return 0;
}

void scheduler_shutdown(scheduler_t *sched)
{
    sched->running = false;
    thread_pool_shutdown(&sched->pool, &sched->run_queue);
    thread_pool_join(&sched->pool);
}

void scheduler_destroy(scheduler_t *sched)
{
    prefetch_queue_destroy(&sched->prefetch_queue);
    task_queue_destroy(&sched->run_queue);

    close(sched->wake_event_fd);
    close(sched->inotify_fd);
    close(sched->epoll_fd);
}
