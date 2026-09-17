# Nautilus Tweaks

A Nautilus extension that adds various useful context menu actions.

## Why

- Python-based extensions for Nautilus context menus are sluggish.
- Standard Nautilus scripts don't allow conditionally showing or hiding menu items.
- A native C extension is the fastest, cleanest, and most flexible approach.

## Features

1. **Copy Path.** Automatically replaces `$HOME` with `~/` (configurable) and resolves symlinks to their actual target paths.
2. **Open folder as root.** Uses the modern GNOME `admin://` GVfs backend.
3. **Edit file as root.** Launches a TUI text editor (defaults to `micro`) inside your preferred terminal emulator. GNOME Text Editor via `admin://` lacks multi-cursor support, and running GUI applications as root (such as Sublime Text) is blocked under Wayland by design.
4. **Mount SSH (SFTP).** Requires `sshfs` and `fuse3`. Reads host configurations directly from `~/.ssh/config`. By default mounts the remote server root `/` (or the path defined in `# RemotePath: /var/www`).
5. **Mount FTP / WebDAV / S3.** Requires `rclone` and `fuse3`. Reads server configurations from `~/.config/rclone/rclone.conf`.
6. **Open folder in VS Code.**

## Roadmap
1. Change owner and group
2. Recursively apply permissions
3. Research how FileZilla determines mount paths
4. CLI commands for testing SSH and FTP mounts from the terminal
5. Editing mode switch — TUI vs `admin://`

## Configuration

The extension stores its configuration at:
`~/.config/nautilus-tweaks/config.ini`

Available parameters:
- `terminal` — Terminal emulator (`kgx`, `gnome-terminal`, `ptyxis`, `ghostty`, `terminator`, etc.; supports wrappers).
- `editor` — Console editor for root (`micro`, `nano`, `nvim`, `vim`).
- `shorten_home` — Replace `$HOME` with `~` when copying (`true` / `false`).
- `resolve_symlinks` — Resolve symlinks to their real target path (`true` / `false`).
- `remote_dirs` — Comma-separated list of allowed base directories inside which mounting is permitted (defaults to `/mnt/Remote`).
- `emblem` — System icon name for mounted folders (defaults to `globe`).

### Example FTP configuration in `~/.config/rclone/rclone.conf`:
```ini
[old-shop]
type = ftp
host = 194.58.112.15
user = shop_admin
pass = zK19U8Gg-mUqZ89Q-ObscuredPass...
```
*(To obscure the password for the `pass` field, use `rclone obscure "your_password"` or run the interactive wizard `rclone config`)*.

### Example SSH configuration in `~/.ssh/config`:
```ssh
Host ruweb
    HostName 185.11.246.104
    User root
    IdentityFile "/mnt/Data/Work/SSH keys/ruweb_vds/id_rsa"
    # RemotePath: /var/www/site
```

## Building and Installation

**Install**
```bash
sudo make install
```

**Uninstall**
```bash
sudo make uninstall
```

*Nautilus will automatically quit during installation to reload the modules.*

## Dependencies

### Build dependencies:
- C compiler (`gcc` or `clang`)
- `make`
- `pkgconf` (or `pkg-config`)
- `gettext` (required for compiling `.po` translation catalogs via `msgfmt`)
- Nautilus extension development headers (`libnautilus-extension-4`):
  - **Arch Linux:** `sudo pacman -S base-devel nautilus gettext`
  - **Ubuntu / Debian:** `sudo apt install build-essential pkgconf libnautilus-extension-dev gettext`
  - **Fedora:** `sudo dnf install gcc make pkgconf nautilus-devel gettext`

### Runtime dependencies:
- **For the mounting module (`mount`):**
  - `sshfs` — for mounting SFTP/SSH remotes
  - `rclone` — for mounting FTP, WebDAV, S3, etc.
  - `fuse3` — for userspace filesystem mounting