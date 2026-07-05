# bursched

ÖNEMLİ make diyerek derlediğinizde otomatik olarak # ROOT gücünde systemd servisi olarak çalışır.Bunu göze alarak çalıştırınız.Hiçbir profesyonel yapım değildir tamamen hobi yapımıdır.EL YAPIMI DEĞİLDİR

I/O-driven, asenkron bir görev zamanlayıcısı (task scheduler). Bir dizini
(ve tüm alt dizinlerini) `inotify` ile izler, yeni bir dosya oluştuğunda
bunu bir POSIX thread havuzuna (worker pool) dağıtır. Ana thread `epoll`
üzerinde tam blok olur; iş yokken CPU'da ölçülebilir bir iz bırakmaz.



## Mimari

```
                         ┌─────────────────────┐
                         │   epoll_wait(-1)     │  ← ana thread, tam blok
                         └──────────┬───────────┘
                    ┌───────────────┴───────────────┐
                    │                                │
            ┌───────▼────────┐              ┌────────▼────────┐
            │  inotify_fd     │              │  wake_event_fd  │
            │ (dosya olayları)│              │ (worker→main)   │
            └───────┬────────┘              └─────────────────┘
                    │
              task_t oluştur
                    │
            ┌───────▼────────┐
            │  run_queue      │  ring buffer, mutex + cond_wait
            │ (task_queue_t)  │
            └───────┬────────┘
                    │
        ┌───────────┼───────────┐
        ▼           ▼           ▼
   worker #0    worker #1   worker #N   ← pthread havuzu, önceden oluşturulmuş
```

- **epoll**: tek merkezi bekleme noktası. Yalnızca `inotify_fd` ve
  `wake_event_fd` izlenir.
- **inotify**: `IN_CLOSE_WRITE | IN_MOVED_TO` olaylarını dinler, izlenen kök
  dizinin altındaki tüm alt dizinleri recursive olarak (nftw ile) tarar.
- **Thread pool**: worker sayısı sabit ve önceden oluşturulmuştur; görev
  yokken `pthread_cond_wait` ile derin uykuya geçer (busy-wait yok).
- **Prefetch**: bir görev işlenmeden önce `posix_fadvise` / `mmap` +
  `madvise(MADV_WILLNEED)` ile verinin page cache'e çekilmesi hedeflenir.
- **Cache-line farkındalığı**: `task_hot_t` tek bir 64 byte'lık cache
  line'a sığacak şekilde ayrılmıştır; kuyruk `head`/`tail` sayaçları
  false-sharing'i önlemek için ayrı hizalanmıştır.

## Dosyalar

| Dosya          | Sorumluluk                                          |
|----------------|------------------------------------------------------|
| `main.c`       | Test/demo girişi, sinyal (SIGINT) yönetimi           |
| `scheduler.c/h`| Ana orkestratör: epoll, inotify, watch tablosu       |
| `queue.c/h`    | Ring buffer tabanlı `run_queue` ve `prefetch_queue`  |
| `pool.c/h`     | POSIX thread havuzu, worker döngüsü                  |
| `prefetch.c/h` | `fadvise` / `mmap` ile önceden veri hazırlama        |

## Build

```bash
make            # yalnızca derler → build/scheduler
```

## Manuel çalıştırma (foreground, test amaçlı)

```bash
./build/scheduler <izlenecek-dizin> [worker_count]
# örnek:
./build/scheduler /home/doruk/downloads 4
```

`Ctrl+C` ile `SIGINT` gönderildiğinde tüm worker'lar temiz şekilde
sonlandırılır.

## systemd kurulumu — ⚠️ ÖNEMLİ UYARILAR

`make install` komutu, binary'yi derleyip **root yetkisiyle, sistem
genelinde çalışan bir systemd servisi** olarak kurar:

```bash
sudo make install
```

Bu komutun **gerçekte yaptıkları**:

1. Binary'yi `/usr/local/bin/bursched`'e kopyalar.
2. `/etc/systemd/system/bursched.service` dosyasını oluşturur.
3. `systemctl enable` ile servisi **her boot'ta otomatik başlayacak**
   şekilde etkinleştirir.
4. Servisi hemen başlatır.

### Bilmen gereken riskler

- **Varsayılan izlenen dizin `/home`'dur.** Makefile'daki `WATCH_DIR`
  değişkeni değiştirilmeden kurulursa, sistemdeki **tüm kullanıcıların ev
  dizinleri** recursive olarak izlenmeye başlar. Bu, hem performans hem de
  gizlilik açısından ciddi bir karardır — kurulum öncesi mutlaka gözden
  geçir:
  ```makefile
  WATCH_DIR := /home        # ihtiyacına göre değiştir
  ```
- **Servis root olarak çalışır.** `sudo cp` ve `sudo tee` ile kurulduğu
  ve systemd birim dosyasında bir `User=` direktifi *olmadığı* için,
  servis varsayılan olarak **root** kullanıcısı altında çalışır. Bu,
  izlenen dizin altındaki *her* dosyayı okuma (ve `open()` başarılı
  olduğu için potansiyel olarak `mmap` ile belleğe alma) yetkisine sahip
  olduğu anlamına gelir. Prodüksiyonda daha kısıtlı bir kullanıcı ile
  çalıştırman (systemd birimine `User=` / `DynamicUser=yes` ekleyerek)
  şiddetle önerilir.
- **`/home` gibi geniş ağaçlarda `SCHED_MAX_WATCHES` (8192) aşılabilir.**
  Bu limit aşılırsa fazla alt dizinler sessizce izlenmeden kalır (yalnızca
  stderr'e log düşer, servis çökmez) — `journalctl` ile kontrol et.
- **Görev işleme mantığı henüz iskelet halindedir** (`pool.c` içindeki
  `process_task()` — "Gerçek iş yükü buraya eklenecek" yorumuna bak).
  Yani servis şu an dosyaları yakalayıp prefetch/mmap yapıyor ama üzerinde
  gerçek bir iş (arama, sıkıştırma, vs.) yapmıyor. Prodüksiyona almadan
  önce bu kısmı doldurman gerekiyor.
- **`make uninstall` servisi tamamen kaldırır** (durdurur, disable eder,
  birim dosyasını ve binary'yi siler) — geri dönüşü yok, dikkatli kullan.

### Servis yönetimi

```bash
make status     # servis durumunu gösterir
make logs       # journalctl -u bursched -f (canlı log takibi)
make uninstall  # servisi tamamen kaldırır
```

### Öneri: production'a almadan önce

- `WATCH_DIR`'i gerçek ihtiyacına göre daraltılmış bir dizine ayarla.
- Systemd birimine `User=`/`Group=` (mümkünse ayrıcalıksız bir kullanıcı)
  ve `ProtectSystem=strict`, `NoNewPrivileges=yes` gibi sandbox
  direktifleri eklemeyi değerlendir.
- `process_task()` içine gerçek iş yükünü ekleyip test etmeden `/home`
  gibi geniş/hassas dizinleri izlemeye almayı erteler.

## Lisans

Kişisel proje — lisans belirtilmemiş.
