# ============================================================================
#  Makefile — I/O-Driven Asenkron Task Scheduler
# ============================================================================
#
#  Hedefler:
#    make            -> yalnızca derler (binary: build/scheduler)
#    make install    -> derler + /usr/local/bin'e kurar + systemd servisini
#                       kurup etkinleştirir (her açılışta otomatik başlar)
#    make uninstall  -> servisi durdurur, devre dışı bırakır, siler
#    make status     -> servis durumunu gösterir
#    make logs       -> servis loglarını canlı izler
#    make clean      -> derleme çıktılarını temizler
#
#  Kurulum sistem genelinde (root) bir systemd servisi olarak yapılır, böylece
#  belirli bir kullanıcı oturumuna bağlı kalmadan her boot'ta otomatik başlar.
# ============================================================================

CC       := gcc
STD      := -std=c11
WARN     := -Wall -Wextra -Wpedantic
OPT      := -O2
LDFLAGS  := -lpthread

SRC      := main.c scheduler.c queue.c pool.c prefetch.c
BUILD_DIR:= build
BIN      := $(BUILD_DIR)/scheduler

# --- Kurulum parametreleri (systemd servis ve izlenecek dizin) -------------
INSTALL_PATH   := /usr/local/bin/dfm-scheduler
WATCH_DIR      := /home
WORKER_COUNT   := 4
SERVICE_NAME   := dfm-scheduler
SERVICE_FILE   := /etc/systemd/system/$(SERVICE_NAME).service

.PHONY: all install uninstall status logs clean

# --- Yalnızca derleme --------------------------------------------------------
all: $(BIN)

$(BIN): $(SRC) scheduler.h queue.h pool.h prefetch.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(STD) $(WARN) $(OPT) $(SRC) -o $(BIN) $(LDFLAGS)
	@echo "[make] derleme tamam: $(BIN)"

# --- Derle + kur + systemd servisini etkinleştir ----------------------------
install: $(BIN)
	@echo "[make] binary /usr/local/bin altına kopyalanıyor..."
	sudo cp $(BIN) $(INSTALL_PATH)
	sudo chmod 755 $(INSTALL_PATH)
	@echo "[make] systemd servis dosyası yazılıyor: $(SERVICE_FILE)"
	@printf '%s\n' \
		'[Unit]' \
		'Description=I/O-Driven Async Task Scheduler (dfm-scheduler)' \
		'After=local-fs.target' \
		'' \
		'[Service]' \
		'Type=simple' \
		'ExecStart=$(INSTALL_PATH) $(WATCH_DIR) $(WORKER_COUNT)' \
		'Restart=on-failure' \
		'RestartSec=2' \
		'Nice=10' \
		'' \
		'[Install]' \
		'WantedBy=multi-user.target' \
		| sudo tee $(SERVICE_FILE) > /dev/null
	@echo "[make] systemd yeniden yükleniyor..."
	sudo systemctl daemon-reload
	@echo "[make] servis etkinleştiriliyor (her açılışta otomatik başlar)..."
	sudo systemctl enable $(SERVICE_NAME)
	@echo "[make] servis şimdi başlatılıyor..."
	sudo systemctl restart $(SERVICE_NAME)
	@echo ""
	@echo "[make] Kurulum tamamlandı."
	@echo "        Durum   : make status"
	@echo "        Loglar  : make logs"
	@echo "        Kaldır  : make uninstall"
	@sleep 1
	@sudo systemctl status $(SERVICE_NAME) --no-pager || true

# --- Servisi durdur, devre dışı bırak, dosyaları temizle --------------------
uninstall:
	@echo "[make] servis durduruluyor ve devre dışı bırakılıyor..."
	-sudo systemctl stop $(SERVICE_NAME)
	-sudo systemctl disable $(SERVICE_NAME)
	-sudo rm -f $(SERVICE_FILE)
	sudo systemctl daemon-reload
	-sudo rm -f $(INSTALL_PATH)
	@echo "[make] kaldırıldı."

status:
	sudo systemctl status $(SERVICE_NAME) --no-pager

logs:
	sudo journalctl -u $(SERVICE_NAME) -f

clean:
	rm -rf $(BUILD_DIR)
	@echo "[make] temizlendi."
