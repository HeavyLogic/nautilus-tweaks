all: install

install:
	@./install.sh

uninstall:
	@echo "==> Removing Nautilus Tweaks..."
	@rm -f /usr/lib/nautilus/extensions-4/libnautilus-tweaks-*.so
	@rm -f /usr/share/locale/*/LC_MESSAGES/nautilus-tweaks.mo
	@rm -f libnautilus-tweaks-*.so
	@echo "==> Successfully uninstalled."

clean:
	@rm -f libnautilus-tweaks-*.so
	@echo "==> Directory cleaned."

.PHONY: all install uninstall clean