CFLAGS ?= -O2 -Wall -Wextra
BINDIR := $(HOME)/.local/bin
UNITDIR := $(HOME)/.config/systemd/user
APPDIR := $(HOME)/.local/share/applications

CORE := rgb-core.c rgb-core.h
GUI_PKGS := libadwaita-1 gtk4
DAEMON_PKGS := glib-2.0 libpulse-simple libpulse

GPU_USER ?= $(or $(SUDO_USER),$(USER))
SYSTEM_UNITDIR := /etc/systemd/system
MODULES_CONF := /etc/modules-load.d/colorful-gpu-led.conf

all: colorful-rgb-gui colorful-rgbd colorful-gpu-ledd

colorful-gpu-ledd: colorful-gpu-ledd.c
	$(CC) $(CFLAGS) -o $@ $<

colorful-rgb-gui: colorful-rgb-gui.c $(CORE)
	$(CC) $(CFLAGS) $(shell pkg-config --cflags $(GUI_PKGS)) -o $@ colorful-rgb-gui.c rgb-core.c $(shell pkg-config --libs $(GUI_PKGS))

colorful-rgbd: colorful-rgbd.c $(CORE)
	$(CC) $(CFLAGS) $(shell pkg-config --cflags $(DAEMON_PKGS)) -o $@ colorful-rgbd.c rgb-core.c $(shell pkg-config --libs $(DAEMON_PKGS)) -lm

install: all
	install -Dm755 colorful-rgb-gui $(BINDIR)/colorful-rgb-gui
	install -Dm755 colorful-rgbd $(BINDIR)/colorful-rgbd
	install -Dm644 colorful-rgbd.service $(UNITDIR)/colorful-rgbd.service
	install -d $(APPDIR)
	sed 's|@BINDIR@|$(BINDIR)|' colorful-rgb.desktop.in > $(APPDIR)/colorful-rgb.desktop
	systemctl --user daemon-reload
	systemctl --user enable colorful-rgbd.service
	systemctl --user restart colorful-rgbd.service

uninstall:
	-systemctl --user disable --now colorful-rgbd.service
	rm -f $(BINDIR)/colorful-rgb-gui $(BINDIR)/colorful-rgbd $(UNITDIR)/colorful-rgbd.service $(APPDIR)/colorful-rgb.desktop
	systemctl --user daemon-reload

# Run with sudo after building as your own user.
install-gpu: colorful-gpu-ledd
	install -Dm755 colorful-gpu-ledd /usr/local/bin/colorful-gpu-ledd
	install -Dm644 colorful-gpu-led.service $(SYSTEM_UNITDIR)/colorful-gpu-led.service
	sed 's|@USER@|$(GPU_USER)|' colorful-gpu-led.socket.in > $(SYSTEM_UNITDIR)/colorful-gpu-led.socket
	echo i2c-dev > $(MODULES_CONF)
	modprobe i2c-dev
	systemctl daemon-reload
	systemctl enable --now colorful-gpu-led.socket
	systemctl try-restart colorful-gpu-led.service

uninstall-gpu:
	-systemctl disable --now colorful-gpu-led.socket colorful-gpu-led.service
	rm -f /usr/local/bin/colorful-gpu-ledd $(SYSTEM_UNITDIR)/colorful-gpu-led.service $(SYSTEM_UNITDIR)/colorful-gpu-led.socket $(MODULES_CONF)
	systemctl daemon-reload

clean:
	rm -f colorful-rgb-gui colorful-rgbd colorful-gpu-ledd

.PHONY: all install uninstall install-gpu uninstall-gpu clean
