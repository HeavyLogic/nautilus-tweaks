# Nautilus Tweaks

A Nautilus extension that adds various useful context menu actions.

## Why

- Python-based extensions for Nautilus context menus are sluggish.
- Standard Nautilus scripts don't allow conditionally showing or hiding menu items.
- A native C extension is the fastest, cleanest, and most flexible approach.

## Features

1. **Copy Path.** Automatically replaces `$HOME` with `~/` (configurable), resolves symlinks, and translates paths on mounted servers to their real remote paths (taking `# RemotePath:` into account).
2. **Open folder as root.** Uses the modern GNOME `admin://` GVfs backend (hidden on remote mounts).
3. **Edit file as root.** Supports both TUI editors via a customizable command template (`root_editor_cmd`, e.g. `ghostty -e sudo micro %f`) and GUI editors via `admin://`. Automatically hidden on remote mounts.
4. **Mount SSH (SFTP).** Requires `sshfs` and `fuse3`. Reads host configurations directly from `~/.ssh/config`. Mounts the remote server root `/` (or the path defined in `# RemotePath: /var/www`) with built-in directory caching.
5. **Mount FTP.** Requires `rclone` and `fuse3`. Reads FTP server configurations from `~/.config/rclone/rclone.conf`.
6. **Open in IDE.** Open any folder as a project in your preferred IDE (`code`, `zed`, `subl`, etc., configurable).
7. **Manage Permissions (chmod / chown).** Works seamlessly on both local files and remote SSH servers. Full real-time sync between checkboxes, octal (`0755`), and symbolic (`rwxr-xr-x`) notation. Supports recursive applying and a separate `+X` for directories.

## Roadmap

[ ] Remove mount icons on Nautilus start  
[ ] Open in terminal of choice  
[ ] Disableable actions

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
# our custom option: initial remote directory
remote_path = /home/c/cl11917
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