# Плагин для Nautilus, добавляющий пункты меню

## Зачем

- Python-расширение для создания контекстных меню работает медленно.
- Скрипты не позволяют прятать пункты меню по условной логике.
- Плагин на C — это самый нативный и гибкий способ.

## Описание функционала

1. **Копировать путь.** Автоматически подставляет `~/` (настраивается) и достаёт реальные пути из симлинков.
2. **Открыть папку как root** — использует современный для GNOME протокол `admin://`.
3. **Редактировать файл как root.** Использует TUI-редактор (по умолчанию `micro`) в выбранном эмуляторе терминала. Я не выбрал GNOME Text Editor через его `admin://`, потому что там нет поддержки мульти-кареток. А запуск GUI-приложений от `root` (типа Sublime Text) разработчики GNOME блокируют под Wayland.
4. **Примонтировать SSH (SFTP).** Требует `sshfs` и `fuse3`. Конфигурацию читает из `~/.ssh/config`. По умолчанию монтирует корень сервера `/` (или путь из директивы `# RemotePath: /var/www`).
5. **Примонтировать FTP / WebDAV / S3.** Требует `rclone` и `fuse3`. Конфигурацию серверов читает из `~/.config/rclone/rclone.conf`.
6. **Открыть папку в VS Code.**

## В планах
1. Сменить владельца и группу
2. Прописать права рекурсивно
3. Посмотреть как FileZilla определяет путь монтирования
4. Команды для тестирования монтирования SSH и FTP из консоли
5. Режим редактирования — TUI или `admin://`

## Настройка

У плагина есть свой конфиг:
`~/.config/nautilus-custom-actions/config.ini`

Доступные параметры:
- `terminal` — эмулятор терминала (`kgx`, `gnome-terminal`, `ptyxis`, `ghostty`, `terminator` и др., поддерживает обёртки).
- `editor` — консольный редактор для root (`micro`, `nano`, `nvim`, `vim`).
- `shorten_home` — заменять `$HOME` на `~` при копировании (`true` / `false`).
- `resolve_symlinks` — разворачивать симлинки до реального пути (`true` / `false`).
- `remote_dirs` — список базовых папок через запятую, внутри которых разрешено монтирование (по умолчанию `/mnt/Remote`).
- `emblem` — имя системной иконки для смонтированных папок (по умолчанию `globe`).

### Пример настройки FTP-сервера в `~/.config/rclone/rclone.conf`:
```ini
[old-shop]
type = ftp
host = 194.58.112.15
user = shop_admin
pass = zK19U8Gg-mUqZ89Q-ObscuredPass...
```
*(Зашифровать пароль для поля `pass` можно командой `rclone obscure "ваш_пароль"` или через интерактивный мастер `rclone config`)*.

### Пример настройки SSH-сервера в `~/.ssh/config`:
```ssh
Host ruweb
    HostName 185.11.246.104
    User root
    IdentityFile "/mnt/Data/Work/SSH keys/ruweb_vds/id_rsa"
    # RemotePath: /var/www/site
```

## Как компилировать

**Установить**
```bash
sudo make install
```

**Удалить**
```bash
sudo make uninstall
```

*Nautilus должен закрыться сам в процессе установки*

## Зависимости

### Для сборки:
- Компилятор C (`gcc` или `clang`)
- `pkgconf` (или `pkg-config`)
- Заголовочные файлы Nautilus (`libnautilus-extension-4`):
  - **Arch Linux:** `sudo pacman -S base-devel nautilus`
  - **Ubuntu / Debian:** `sudo apt install build-essential pkgconf libnautilus-extension-dev`
  - **Fedora:** `sudo dnf install gcc pkgconf nautilus-devel`

### Для работы (Runtime):
- **Для модуля монтирования (`mount`):**
  - `zenity` — графический диалог выбора сервера
  - `sshfs` — для подключения SFTP/SSH
  - `rclone` — для подключения FTP, WebDAV, S3 и др.