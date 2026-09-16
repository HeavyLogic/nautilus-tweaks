all: install

install:
	@./install.sh

uninstall:
	@echo "==> Удаление всех модулей Nautilus Tweaks..."
	@rm -f /usr/lib/nautilus/extensions-4/libnautilus-tweaks-*.so
	@rm -f libnautilus-tweaks-*.so
	@echo "==> Успешно удалено."

clean:
	@rm -f libnautilus-tweaks-*.so
	@echo "==> Папка очищена."

.PHONY: all install uninstall clean