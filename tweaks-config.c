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
        "# Terminal emulator (kgx, gnome-terminal, ptyxis, ghostty, alacritty, foot, kitty, terminator)\n"
        "terminal = kgx\n\n"
        "# Console text editor for root (micro, nano, nvim, vim)\n"
        "editor = micro\n\n"
        "# Shorten $HOME to ~ when copying paths (true / false)\n"
        "shorten_home = true\n\n"
        "# Resolve symlinks to the real target file path (true / false)\n"
        "resolve_symlinks = true\n\n"
        "# Comma-separated list of allowed mount directories (leave empty to disable restriction)\n"
        "remote_dirs = /mnt/Remote\n\n"
        "# Emblem for mounted directories (globe, web, shared, synchronizing, default, favorite, system)\n"
        "emblem = globe\n\n"
        "# Additional users for permissions menu, comma-separated (e.g., customuser, 1005)\n"
        "extra_users = \n\n"
        "# Additional groups for permissions menu, comma-separated (e.g., mygroup, 950)\n"
        "extra_groups = \n";

    g_file_set_contents (config_path, default_content, -1, NULL);
}

TweaksConfig *
tweaks_config_load (void)
{
    /* Ensure translations are bound before anything else */
    tweaks_i18n_init ();

    TweaksConfig *config = g_new0 (TweaksConfig, 1);
    config->terminal         = g_strdup ("kgx");
    config->editor           = g_strdup ("micro");
    config->shorten_home     = TRUE;
    config->resolve_symlinks = TRUE;
    config->emblem           = g_strdup ("globe");
    config->remote_dirs      = g_strsplit ("/mnt/Remote", ",", -1);
    config->extra_users      = g_strsplit ("", ",", -1);
    config->extra_groups     = g_strsplit ("", ",", -1);

    const gchar *config_dir = g_get_user_config_dir ();
    g_autofree gchar *config_path = g_build_filename (config_dir, "nautilus-tweaks", "config.ini", NULL);

    ensure_default_config_exists (config_path);

    g_autoptr (GKeyFile) keyfile = g_key_file_new ();
    if (g_key_file_load_from_file (keyfile, config_path, G_KEY_FILE_NONE, NULL))
    {
        gchar *term = g_key_file_get_string (keyfile, "General", "terminal", NULL);
        if (term && strlen (g_strstrip (term)) > 0)
        {
            g_free (config->terminal);
            config->terminal = term;
        }
        else
        {
            g_free (term);
        }

        gchar *ed = g_key_file_get_string (keyfile, "General", "editor", NULL);
        if (ed && strlen (g_strstrip (ed)) > 0)
        {
            g_free (config->editor);
            config->editor = ed;
        }
        else
        {
            g_free (ed);
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

        gchar *eu = g_key_file_get_string (keyfile, "General", "extra_users", NULL);
        if (eu && strlen (g_strstrip (eu)) > 0)
        {
            g_strfreev (config->extra_users);
            config->extra_users = g_strsplit (eu, ",", -1);
            for (int i = 0; config->extra_users[i] != NULL; i++)
                g_strstrip (config->extra_users[i]);
            g_free (eu);
        }
        else
        {
            g_free (eu);
        }

        gchar *eg = g_key_file_get_string (keyfile, "General", "extra_groups", NULL);
        if (eg && strlen (g_strstrip (eg)) > 0)
        {
            g_strfreev (config->extra_groups);
            config->extra_groups = g_strsplit (eg, ",", -1);
            for (int i = 0; config->extra_groups[i] != NULL; i++)
                g_strstrip (config->extra_groups[i]);
            g_free (eg);
        }
        else
        {
            g_free (eg);
        }
    }

    return config;
}

void
tweaks_config_free (TweaksConfig *config)
{
    if (!config)
        return;
    g_free (config->terminal);
    g_free (config->editor);
    g_free (config->emblem);
    g_strfreev (config->remote_dirs);
    g_strfreev (config->extra_users);
    g_strfreev (config->extra_groups);
    g_free (config);
}