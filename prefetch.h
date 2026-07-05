/*
 * ============================================================================
 *  prefetch.h  —  Önceden Veri Hazırlama (Pre-Fetching)
 * ============================================================================
 *
 *  Bir task'ın gerçek işlenme anından önce verisini page cache'e (fadvise)
 *  veya doğrudan sürecin adres alanına (mmap) çekmek için kullanılır.
 *  Amaç: worker thread pik moda geçtiğinde disk I/O'suna takılmamak.
 * ============================================================================
 */

#ifndef PREFETCH_H
#define PREFETCH_H

#include "scheduler.h"

/* Dosyayı page cache'e çekmesi için kernel'e ipucu verir (asenkron, non-blocking). */
int  prefetch_hint_willneed(task_t *task);

/* Dosyayı doğrudan mmap ile sürecin adres alanına haritalar (READY durumuna geçirir). */
int  prefetch_map_into_memory(task_t *task);

/* mmap edilmiş bölgeyi serbest bırakır. */
void prefetch_release(task_t *task);

#endif /* PREFETCH_H */
