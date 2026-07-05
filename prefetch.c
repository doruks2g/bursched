/*
 * ============================================================================
 *  prefetch.c  —  Önceden Veri Hazırlama (implementasyon)
 * ============================================================================
 */

#include "prefetch.h"   /* scheduler.h -> _DEFAULT_SOURCE, diğer include'lardan önce gelmeli */

#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

int prefetch_hint_willneed(task_t *task)
{
    /* posix_fadvise() asenkron bir ipucudur: çağıran thread'i bloke etmez,
     * kernel'e "bu aralığı yakında okuyacağım" der ve page cache'i arka
     * planda doldurmasını tetikler. */
    int rc = posix_fadvise(task->hot.fd, task->hot.offset,
                            (off_t)task->hot.size, POSIX_FADV_WILLNEED);
    if (rc != 0) {
        fprintf(stderr, "[prefetch] fadvise failed for '%s': %s\n",
                task->path, strerror(rc));
        return -1;
    }
    return 0;
}

int prefetch_map_into_memory(task_t *task)
{
    if (task->hot.size == 0) {
        fprintf(stderr, "[prefetch] refusing to mmap zero-length task '%s'\n",
                task->path);
        return -1;
    }

    void *addr = mmap(NULL, task->hot.size, PROT_READ, MAP_PRIVATE,
                       task->hot.fd, task->hot.offset);
    if (addr == MAP_FAILED) {
        fprintf(stderr, "[prefetch] mmap failed for '%s': %s\n",
                task->path, strerror(errno));
        return -1;
    }

    /* MADV_WILLNEED: mmap sonrası sayfaların hemen hazırlanmasını (readahead)
     * kernel'e hatırlatır. fadvise'dan farklı olarak bu, mmap edilmiş bölge
     * üzerinde çalışır. */
    madvise(addr, task->hot.size, MADV_WILLNEED);

    task->hot.mapped_base = addr;
    task->hot.mapped_len  = task->hot.size;
    task->hot.state       = TASK_STATE_READY;

    return 0;
}

void prefetch_release(task_t *task)
{
    if (task->hot.mapped_base != NULL && task->hot.mapped_len > 0) {
        munmap(task->hot.mapped_base, task->hot.mapped_len);
        task->hot.mapped_base = NULL;
        task->hot.mapped_len  = 0;
    }
}
