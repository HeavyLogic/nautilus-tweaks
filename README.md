# Nautilus Tweaks

A Nautilus extension that adds various useful context menu actions.  
Have you missed this menu from WinSCP?  

<a href="https://i.imgur.com/NezQy9O.png" target="_blank"><img width="300" src="https://i.imgur.com/NezQy9O.png" alt=""></a>

![img](https://i.imgur.com/NezQy9O.png)

## Why build this

- Python-based extensions for Nautilus context menus are sluggish.
- Standard Nautilus scripts don't allow conditionally showing or hiding menu items.
- A native C extension is the fastest, cleanest, and most flexible approach.

## Features

1. **Copy Path.** Automatically replaces `$HOME` with `~/` (configurable), resolves symlinks, and translates paths on mounted servers to their real remote paths (taking `# RemotePath:` in `~/.ssh/config` and `remote_path` in `rclone.conf` into account).
2. **Open folder as root.** Uses the modern GNOME `admin://` GVfs backend (hidden on remote mounts).
3. **Edit file as root.** Supports both TUI editors via a customizable command template (`root_editor_cmd`, e.g. `ghostty -e sudo micro %f`) and GUI editors via `admin://`. Automatically hidden on remote mounts.
4. **Mount / Open Servers (SFTP & FTP).** Supports two interchangeable backends:
   - **GVfs mode (default):** Opens servers directly as native tabs in Nautilus (`sftp://` and `ftp://`) from the folder background menu. No local mount folders required, and servers are easily disconnected using native sidebar eject buttons.
   - **SSHFS / Rclone mode:** Mounts servers directly into local directories (defaults to `/mnt/Remote`).
   - Reads SFTP hosts from `~/.ssh/config` and FTP hosts from `~/.config/rclone/rclone.conf` (supports starting directory via `# RemotePath:` / `remote_path`).
5. **Open in Terminal.** Opens your preferred terminal emulator (configured via `terminal`, e.g. `ghostty`, `kgx`, `terminator`). When invoked on an SSH/SFTP server, automatically opens an interactive SSH session inside that remote directory (hidden on FTP).
6. **Open in IDE.** Open any folder as a project in your preferred IDE (`code`, `zed`, `subl`, etc., configurable).
7. **Manage Permissions (chmod / chown).** Works seamlessly on local files and remote SSH/SFTP servers. Full real-time sync between checkboxes, octal (`0755`), and symbolic (`rwxr-xr-x`) notation. Supports recursive applying and a separate `+X` for directories.

## Roadmap

[ ] Remove mount icons on Nautilus start  
[ ] Disableable actions

## Configuration

The extension stores its configuration at:
`~/.config/nautilus-tweaks/config.ini`

Available parameters:
- `terminal` — Preferred terminal emulator (`kgx`, `ptyxis`, `ghostty`, `terminator`, etc.; supports wrappers; defaults to `kgx`).
- `ide` — IDE / editor for opening projects (`code`, `zed`, `subl`, etc.; defaults to `code`).
- `shorten_home` — Replace `$HOME` with `~` when copying paths (`true` / `false`, defaults to `true`).
- `resolve_symlinks` — Resolve symlinks to their real target path (`true` / `false`, defaults to `true`).
- `resolve_remotes` — Translate paths on mounted servers to real remote server paths (`true` / `false`, defaults to `true`).
- `root_editor_mode` — Mode for editing files as root: `tui` (terminal editor) or `admin` (GUI editor via `admin://`, defaults to `tui`).
- `root_editor_cmd` — Command template for editing files as root in TUI mode (`%f` will be replaced by the quoted file path; defaults to `kgx -e sudo micro %f`).
- `root_editor_gui` — GUI editor for `admin://` mode (defaults to `gnome-text-editor`).
- `sftp_backend` — Connection backend: `gvfs` (native GNOME network location, defaults to `gvfs`) or `sshfs` (mounts into a local folder).
- `remote_dirs` — Comma-separated list of allowed base directories inside which mounting is permitted in `sshfs` mode (defaults to `/mnt/Remote`).
- `emblem` — System emblem name for mounted folders in `sshfs` mode (defaults to `globe`).
- `debug` — Enable debug logging to `~/.config/nautilus-tweaks/debug.log` (`true` / `false`, defaults to `false`).

### Example FTP configuration in `~/.config/rclone/rclone.conf`:
```ini
[old-shop]
type = ftp
host = 194.58.112.15
user = shop_admin
pass = zK19U8Gg-mUqZ89Q-ObscuredPass...
# Custom extension option: starting directory (defaults to /)
remote_path = /home/c/cl11917
```
*(In `gvfs` mode, passwords saved in GNOME Keyring are used automatically. In `sshfs` mode, `rclone` uses the `pass` field).*

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
- **For GVfs mode (default):**
  - Standard GNOME virtual filesystem tools (`gvfs`, `gvfs-sftp`, `gvfs-ftp`).
- **For local folder mounting mode (`sshfs`):**
  - `sshfs` — for mounting SFTP/SSH remotes into folders.
  - `rclone` — for mounting FTP remotes into folders.
  - `fuse3` — for userspace filesystem mounting.
