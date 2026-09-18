#include "tweaks-config.h"
#include <gio/gio.h>
#include <string.h>

#define LOCALEDIR "/usr/share/locale"

void
tweaks_i18n_init (void)
{
    static gsize initialized = 0;

    /* Thread-safe one-time initialization */
    if (g_once_init_enter (&initialized))
    {
        bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
        bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
        g_once_init_leave (&initialized, 1);
    }
}

static void
ensure_default_config_exists (const gchar *config_path)
{
    if (g_file_test (config_path, G_FILE_TEST_EXISTS))
        return;

    g_autofree gchar *dir = g_path_get_dirname (config_path);
    g_mkdir_with_parents (dir, 0755);

    const gchar *default_content =
        "[General]\n"
        "# Terminal emulator (kgx, ptyxis, ghostty, alacritty, kitty, foot; supports wrappers)\n"
        "terminal = kgx\n\n"
        "# IDE / code editor for opening projects (code, zed, pycharm, subl)\n"
        "ide = code\n\n"
        "# Shorten $HOME to ~ when copying paths (true / false)\n"
        "shorten_home = true\n\n"
        "# Resolve symlinks to the real target file path (true / false)\n"
        "resolve_symlinks = true\n\n"
        "# Resolve paths on mounted remote servers to real remote server paths (true / false)\n"
        "resolve_remotes = true\n\n"
        "# Mode for editing files as root: \"tui\" (terminal editor) or \"admin\" (GUI editor via admin://)\n"
        "# Note: very few GUI editors support admin:// protocol. Use gnome-text-editor to avoid issues.\n"
        "root_editor_mode = tui\n\n"
        "# Command template for editing files as root in TUI mode (%f will be replaced by quoted path)\n"
        "root_editor_cmd = kgx -e sudo micro %f\n\n"
        "# GUI text editor for editing files as root in \"admin\" mode (must support admin://, e.g. gnome-text-editor)\n"
        "root_editor_gui = gnome-text-editor\n\n"
        "# SFTP connection backend: \"gvfs\" (native GNOME network location) or \"sshfs\" (mounts into folder)\n"
        "sftp_backend = gvfs\n\n"
        "# Comma-separated list of allowed mount directories for sshfs (leave empty to disable restriction)\n"
        "remote_dirs = /mnt/Remote\n\n"
        "# Emblem for mounted directories in sshfs mode (globe, web, shared, default, favorite, system)\n"
        "emblem = globe\n\n"
        "# Enable debug logging to ~/.config/nautilus-tweaks/debug.log (true / false)\n"
        "debug = false\n";

    g_file_set_contents (config_path, default_content, -1, NULL);
}

TweaksConfig *
tweaks_config_load (void)
{
    tweaks_i18n_init ();

    TweaksConfig *config = g_new0 (TweaksConfig, 1);
    config->ide              = g_strdup ("code");
    config->terminal         = g_strdup ("kgx");
    config->shorten_home     = TRUE;
    config->resolve_symlinks = TRUE;
    config->resolve_remotes  = TRUE;
    config->root_editor_mode = g_strdup ("tui");
    config->root_editor_cmd  = g_strdup ("kgx -e sudo micro %f");
    config->root_editor_gui  = g_strdup ("gnome-text-editor");
    config->sftp_backend     = g_strdup ("gvfs");
    config->emblem           = g_strdup ("globe");
    config->remote_dirs      = g_strsplit ("/mnt/Remote", ",", -1);
    config->debug            = FALSE;

    const gchar *config_dir = g_get_user_config_dir ();
    g_autofree gchar *config_path = g_build_filename (config_dir, "nautilus-tweaks", "config.ini", NULL);

    ensure_default_config_exists (config_path);

    g_autoptr (GKeyFile) keyfile = g_key_file_new ();
    if (g_key_file_load_from_file (keyfile, config_path, G_KEY_FILE_NONE, NULL))
    {
        gchar *val = NULL;

        val = g_key_file_get_string (keyfile, "General", "ide", NULL);
        if (val && strlen (g_strstrip (val)) > 0)
        {
            g_free (config->ide);
            config->ide = val;
        }
        else
        {
            g_free (val);
        }

        val = g_key_file_get_string (keyfile, "General", "terminal", NULL);
        if (val && strlen (g_strstrip (val)) > 0)
        {
            g_free (config->terminal);
            config->terminal = val;
        }
        else
        {
            g_free (val);
        }

        GError *err = NULL;
        gboolean sh = g_key_file_get_boolean (keyfile, "General", "shorten_home", &err);
        if (!err)
            config->shorten_home = sh;
        g_clear_error (&err);

        gboolean rs = g_key_file_get_boolean (keyfile, "General", "resolve_symlinks", &err);
        if (!err)
            config->resolve_symlinks = rs;
        g_clear_error (&err);

        gboolean rr = g_key_file_get_boolean (keyfile, "General", "resolve_remotes", &err);
        if (!err)
            config->resolve_remotes = rr;
        g_clear_error (&err);

        val = g_key_file_get_string (keyfile, "General", "root_editor_mode", NULL);
        if (val && strlen (g_strstrip (val)) > 0)
        {
            g_free (config->root_editor_mode);
            config->root_editor_mode = val;
        }
        else
        {
            g_free (val);
        }

        val = g_key_file_get_string (keyfile, "General", "root_editor_cmd", NULL);
        if (val && strlen (g_strstrip (val)) > 0)
        {
            g_free (config->root_editor_cmd);
            config->root_editor_cmd = val;
        }
        else
        {
            g_free (val);
        }

        val = g_key_file_get_string (keyfile, "General", "root_editor_gui", NULL);
        if (val && strlen (g_strstrip (val)) > 0)
        {
            g_free (config->root_editor_gui);
            config->root_editor_gui = val;
        }
        else
        {
            g_free (val);
        }

        val = g_key_file_get_string (keyfile, "General", "sftp_backend", NULL);
        if (val && strlen (g_strstrip (val)) > 0)
        {
            g_free (config->sftp_backend);
            config->sftp_backend = val;
        }
        else
        {
            g_free (val);
        }

        gchar *rd = g_key_file_get_string (keyfile, "General", "remote_dirs", NULL);
        if (rd && strlen (g_strstrip (rd)) > 0)
        {
            g_strfreev (config->remote_dirs);
            config->remote_dirs = g_strsplit (rd, ",", -1);
            for (int i = 0; config->remote_dirs[i] != NULL; i++)
                g_strstrip (config->remote_dirs[i]);
            g_free (rd);
        }
        else
        {
            g_free (rd);
        }

        gchar *emb = g_key_file_get_string (keyfile, "General", "emblem", NULL);
        if (emb && strlen (g_strstrip (emb)) > 0)
        {
            g_free (config->emblem);
            config->emblem = emb;
        }
        else
        {
            g_free (emb);
        }

        gboolean dbg = g_key_file_get_boolean (keyfile, "General", "debug", &err);
        if (!err)
            config->debug = dbg;
        g_clear_error (&err);
    }

    return config;
}

void
tweaks_config_free (TweaksConfig *config)
{
    if (!config)
        return;
    g_free (config->ide);
    g_free (config->terminal);
    g_free (config->root_editor_mode);
    g_free (config->root_editor_cmd);
    g_free (config->root_editor_gui);
    g_free (config->sftp_backend);
    g_free (config->emblem);
    g_strfreev (config->remote_dirs);
    g_free (config);
}