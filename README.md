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

## Как компилировать .so

#### Компилируем
```bash
gcc -shared -fPIC -O2 nautilus-custom-actions.c -o libcustom_actions.so $(pkg-config --cflags --libs libnautilus-extension-4 gtk4)
```

#### Копируем в папку плагинов
*Для текущего пользователя:*
```bash
mkdir -p ~/.local/share/nautilus/extensions-4/
cp libcustom_actions.so ~/.local/share/nautilus/extensions-4/
```
*Или глобально для системы:*
```bash
sudo cp libcustom_actions.so /usr/lib/nautilus/extensions-4/
```

#### Перезапускаем Nautilus
```bash
nautilus -q
```

## Зависимости

### Для сборки:
- `gcc`
- `pkgconf`
- `libnautilus-extension-4` (пакет `libnautilus-extension`)
- `gtk4`

### Для работы (Runtime):
- `zenity` — графическое меню выбора серверов и запрос паролей
- `sshfs` — монтирование SFTP/SSH серверов
- `rclone` — быстрое монтирование FTP/WebDAV/облачных хранилищ с VFS-кэшированием
- `fuse3` — системная утилита размонтирования (`fusermount3` / `fusermount`)
- `openbsd-netcat` — проброс соединений через `ProxyCommand` в обход VPN (опционально)