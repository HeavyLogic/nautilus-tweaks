#include <nautilus-extension.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <gdk/gdk.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

#include "tweaks-config.h"
#include "tweaks-remote.h"

/* Counter for generating unique menu item IDs */
static guint g_action_counter = 0;

/* -------------------------------------------------------------------------- */
/* GObject plugin structure declaration                                       */
/* -------------------------------------------------------------------------- */

typedef struct _NautilusTweaksActions {
    GObject parent_instance;
} NautilusTweaksActions;

typedef struct _NautilusTweaksActionsClass {
    GObjectClass parent_class;
} NautilusTweaksActionsClass;

static GType nautilus_tweaks_actions_get_type (void);
static void nautilus_tweaks_actions_menu_provider_iface_init (NautilusMenuProviderInterface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (NautilusTweaksActions, nautilus_tweaks_actions, G_TYPE_OBJECT, 0,
    G_IMPLEMENT_INTERFACE_DYNAMIC (NAUTILUS_TYPE_MENU_PROVIDER,
                                   nautilus_tweaks_actions_menu_provider_iface_init))

static void nautilus_tweaks_actions_class_init (NautilusTweaksActionsClass *klass) {}
static void nautilus_tweaks_actions_init (NautilusTweaksActions *self) {}
static void nautilus_tweaks_actions_class_finalize (NautilusTweaksActionsClass *klass) {}

/* -------------------------------------------------------------------------- */
/* Helper: Execute root editor (TUI template or GUI admin://)                 */
/* -------------------------------------------------------------------------- */

static void
execute_root_editor (TweaksConfig *config, const gchar *path)
{
    if (g_ascii_strcasecmp (config->root_editor_mode, "admin") == 0)
    {
        /* GUI editor with admin:// protocol */
        g_autofree gchar *admin_uri = g_strdup_printf ("admin://%s", path);
        const gchar *gui_app = (config->root_editor_gui && strlen (config->root_editor_gui) > 0)
                               ? config->root_editor_gui
                               : "gnome-text-editor";
        const gchar *argv[] = { gui_app, admin_uri, NULL };
        g_spawn_async (NULL, (gchar **) argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }
    else
    {
        /* TUI editor via command template */
        const gchar *tmpl = (config->root_editor_cmd && strlen (config->root_editor_cmd) > 0)
                            ? config->root_editor_cmd
                            : "kgx -e sudo micro %f";

        g_autofree gchar *quoted_path = g_shell_quote (path);
        g_autofree gchar *final_cmd = NULL;

        if (strstr (tmpl, "%f"))
        {
            gchar **tokens = g_strsplit (tmpl, "%f", -1);
            final_cmd = g_strjoinv (quoted_path, tokens);
            g_strfreev (tokens);
        }
        else
        {
            final_cmd = g_strdup_printf ("%s %s", tmpl, quoted_path);
        }

        g_spawn_command_line_async (final_cmd, NULL);
    }
}

/* -------------------------------------------------------------------------- */
/* 1. Action: Copy path                                                       */
/* -------------------------------------------------------------------------- */

static void
on_copy_path_activated (NautilusMenuItem *item, gpointer user_data)
{
    GList *files = (GList *) user_data;
    TweaksConfig *config = tweaks_config_load ();

    const gchar *home = g_get_home_dir ();
    gsize home_len = home ? strlen (home) : 0;
    GString *text = g_string_new (NULL);
    GList *l;
    gboolean first = TRUE;

    for (l = files; l != NULL; l = l->next)
    {
        NautilusFileInfo *file = NAUTILUS_FILE_INFO (l->data);
        g_autoptr (GFile) location = nautilus_file_info_get_location (file);
        if (!location)
            continue;

        /* 1. Resolve remote server path if enabled */
        g_autofree gchar *remote_path = NULL;
        if (config->resolve_remotes)
            remote_path = tweaks_remote_resolve_location_path (location);

        if (remote_path)
        {
            if (!first)
                g_string_append_c (text, '\n');
            first = FALSE;

            g_string_append (text, remote_path);
            continue;
        }

        /* 2. Local path processing */
        g_autofree gchar *path = g_file_get_path (location);
        if (!path)
            continue;

        g_autofree gchar *target_path = NULL;

        /* Resolve symlink if requested */
        if (config->resolve_symlinks && g_file_test (path, G_FILE_TEST_IS_SYMLINK))
        {
            char *resolved = realpath (path, NULL);
            if (resolved)
            {
                target_path = g_strdup (resolved);
                free (resolved);
            }
            else
            {
                g_autofree gchar *raw_link = g_file_read_link (path, NULL);
                if (raw_link)
                {
                    if (g_path_is_absolute (raw_link))
                    {
                        target_path = g_strdup (raw_link);
                    }
                    else
                    {
                        g_autofree gchar *parent_dir = g_path_get_dirname (path);
                        target_path = g_build_filename (parent_dir, raw_link, NULL);
                    }

                    const gchar *notify_argv[] = {
                        "notify-send",
                        "-u", "critical",
                        "-i", "dialog-warning",
                        _("Broken Symlink"),
                        _("Target file does not exist, but link path was copied to clipboard"),
                        NULL
                    };
                    g_spawn_async (NULL, (gchar **) notify_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
                }
            }
        }

        const gchar *candidate_path = target_path ? target_path : path;

        if (!first)
            g_string_append_c (text, '\n');
        first = FALSE;

        /* Local path: apply shorten $HOME to ~ */
        if (config->shorten_home && home && g_strcmp0 (candidate_path, home) == 0)
        {
            g_string_append (text, "~");
        }
        else if (config->shorten_home && home && g_str_has_prefix (candidate_path, home) && candidate_path[home_len] == '/')
        {
            g_string_append_c (text, '~');
            g_string_append (text, candidate_path + home_len);
        }
        else
        {
            g_string_append (text, candidate_path);
        }
    }

    if (text->len > 0)
    {
        GdkDisplay *display = gdk_display_get_default ();
        if (display)
        {
            GdkClipboard *clipboard = gdk_display_get_clipboard (display);
            gdk_clipboard_set_text (clipboard, text->str);
        }
    }

    g_string_free (text, TRUE);
    tweaks_config_free (config);
}

/* -------------------------------------------------------------------------- */
/* Action: Open folder in Terminal (Local CWD or Remote SSH)                  */
/* -------------------------------------------------------------------------- */

static void
launch_terminal_ssh (const gchar *term, const gchar *host, const gchar *remote_cmd)
{
    gint term_argc = 0;
    gchar **term_argv = NULL;

    if (!g_shell_parse_argv (term, &term_argc, &term_argv, NULL) || term_argc == 0)
    {
        const gchar *fallback[] = { "kgx", "-e", "ssh", "-t", host, remote_cmd, NULL };
        g_spawn_async (NULL, (gchar **) fallback, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        return;
    }

    g_autofree gchar *last_token = g_path_get_basename (term_argv[term_argc - 1]);
    gboolean is_terminator = (g_strcmp0 (last_token, "terminator") == 0);
    gboolean is_dash_dash = (g_strcmp0 (last_token, "gnome-terminal") == 0 || g_strcmp0 (last_token, "ptyxis") == 0);

    GPtrArray *args = g_ptr_array_new ();
    for (int i = 0; i < term_argc; i++)
        g_ptr_array_add (args, term_argv[i]);

    if (is_terminator)
    {
        g_ptr_array_add (args, "-u");
        g_ptr_array_add (args, "-x");
    }
    else if (is_dash_dash)
    {
        g_ptr_array_add (args, "--");
    }
    else
    {
        g_ptr_array_add (args, "-e");
    }

    g_ptr_array_add (args, "ssh");
    g_ptr_array_add (args, "-t");
    g_ptr_array_add (args, (gchar *) host);
    g_ptr_array_add (args, (gchar *) remote_cmd);
    g_ptr_array_add (args, NULL);

    g_spawn_async (NULL, (gchar **) args->pdata, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);

    g_ptr_array_free (args, TRUE);
    g_strfreev (term_argv);
}

static void
on_open_in_terminal_activated (NautilusMenuItem *item, gpointer user_data)
{
    GFile *location = G_FILE (user_data);
    if (!location)
        return;

    TweaksConfig *config = tweaks_config_load ();
    TweaksMountInfo *info = tweaks_mount_info_get_for_location (location);
    const gchar *term = (config->terminal && strlen (config->terminal) > 0) ? config->terminal : "kgx";

    if (info->mode == TWEAKS_FS_SSHFS && info->ssh_host)
    {
        /* Remote SSH server: open terminal with interactive SSH session in remote folder */
        g_autofree gchar *remote_path = tweaks_remote_resolve_location_path (location);
        const gchar *target_dir = (remote_path && strlen (remote_path) > 0) ? remote_path : "/";
        g_autofree gchar *quoted_dir = g_shell_quote (target_dir);
        g_autofree gchar *remote_cmd = g_strdup_printf ("cd %s; exec ${SHELL:-bash}", quoted_dir);

        launch_terminal_ssh (term, info->ssh_host, remote_cmd);
    }
    else if (info->mode == TWEAKS_FS_LOCAL)
    {
        /* Local folder: open terminal with CWD */
        g_autofree gchar *path = g_file_get_path (location);
        if (path)
        {
            gint term_argc = 0;
            gchar **term_argv = NULL;
            if (g_shell_parse_argv (term, &term_argc, &term_argv, NULL) && term_argc > 0)
            {
                g_spawn_async (path, term_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
                g_strfreev (term_argv);
            }
            else
            {
                const gchar *argv[] = { term, NULL };
                g_spawn_async (path, (gchar **) argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
            }
        }
    }

    tweaks_mount_info_free (info);
    tweaks_config_free (config);
}

/* -------------------------------------------------------------------------- */
/* 2. Action: Open folder in IDE                                              */
/* -------------------------------------------------------------------------- */

static void
on_open_in_ide_activated (NautilusMenuItem *item, gpointer user_data)
{
    gchar *path = (gchar *) user_data;
    TweaksConfig *config = tweaks_config_load ();
    const gchar *ide = (config->ide && strlen (config->ide) > 0) ? config->ide : "code";

    gint ide_argc = 0;
    gchar **ide_argv = NULL;
    if (g_shell_parse_argv (ide, &ide_argc, &ide_argv, NULL) && ide_argc > 0)
    {
        GPtrArray *argv_array = g_ptr_array_new ();
        for (int i = 0; i < ide_argc; i++)
            g_ptr_array_add (argv_array, ide_argv[i]);
        g_ptr_array_add (argv_array, path);
        g_ptr_array_add (argv_array, NULL);

        g_spawn_async (NULL, (gchar **) argv_array->pdata, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        g_ptr_array_free (argv_array, TRUE);
        g_strfreev (ide_argv);
    }
    else
    {
        const gchar *argv[] = { ide, path, NULL };
        g_spawn_async (NULL, (gchar **) argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }

    tweaks_config_free (config);
}

/* -------------------------------------------------------------------------- */
/* 3. Action: Open / Edit as root                                             */
/* -------------------------------------------------------------------------- */

static void
on_open_as_root_activated (NautilusMenuItem *item, gpointer user_data)
{
    NautilusFileInfo *file = NAUTILUS_FILE_INFO (user_data);
    g_autoptr (GFile) location = nautilus_file_info_get_location (file);
    if (!location)
        return;

    g_autofree gchar *path = g_file_get_path (location);
    if (!path)
        return;

    gboolean is_dir = nautilus_file_info_is_directory (file);

    if (is_dir)
    {
        g_autofree gchar *admin_uri = g_strdup_printf ("admin://%s", path);
        const gchar *argv[] = { "nautilus", admin_uri, NULL };
        g_spawn_async (NULL, (gchar **) argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }
    else
    {
        TweaksConfig *config = tweaks_config_load ();
        execute_root_editor (config, path);
        tweaks_config_free (config);
    }
}

/* -------------------------------------------------------------------------- */
/* 4. Context menu for selected files/folders                                 */
/* -------------------------------------------------------------------------- */

static GList *
nautilus_tweaks_actions_get_file_items (NautilusMenuProvider *provider, GList *files)
{
    GList *items = NULL;
    guint len = g_list_length (files);

    if (len == 0)
        return NULL;

    /* --- Item 1: Copy path --- */
    g_autofree gchar *copy_label = NULL;
    if (len == 1)
    {
        NautilusFileInfo *first_file = NAUTILUS_FILE_INFO (files->data);
        if (nautilus_file_info_is_directory (first_file))
            copy_label = g_strdup (_("Copy Folder Path"));
        else
            copy_label = g_strdup (_("Copy File Path"));
    }
    else
    {
        copy_label = g_strdup_printf (_("Copy Paths (%u)"), len);
    }

    g_autofree gchar *copy_id = g_strdup_printf ("NautilusTweaks::CopyPath_%u", ++g_action_counter);
    NautilusMenuItem *copy_item = nautilus_menu_item_new (
        copy_id,
        copy_label,
        _("Copy path to clipboard"),
        "edit-copy-symbolic"
    );

    g_signal_connect_data (copy_item, "activate",
                           G_CALLBACK (on_copy_path_activated),
                           nautilus_file_info_list_copy (files),
                           (GClosureNotify) nautilus_file_info_list_free, 0);

    items = g_list_append (items, copy_item);

    /* --- Block for single selection --- */
    if (len == 1)
    {
        NautilusFileInfo *first_file = NAUTILUS_FILE_INFO (files->data);
        gboolean is_dir = nautilus_file_info_is_directory (first_file);
        g_autoptr (GFile) location = nautilus_file_info_get_location (first_file);
        g_autofree gchar *target_path = location ? g_file_get_path (location) : NULL;

        TweaksMountInfo *mount_info = location ? tweaks_mount_info_get_for_location (location) : NULL;
        gboolean is_ftp = (mount_info && mount_info->mode == TWEAKS_FS_RCLONE);

        /* --- Item 2: Open in Terminal (directories only, hidden on FTP) --- */
        if (is_dir && location && !is_ftp)
        {
            g_autofree gchar *term_id = g_strdup_printf ("NautilusTweaks::OpenInTerm_%u", ++g_action_counter);
            NautilusMenuItem *term_item = nautilus_menu_item_new (
                term_id,
                _("Open in Terminal"),
                _("Open this directory in your preferred terminal"),
                "utilities-terminal-symbolic"
            );

            g_signal_connect_data (term_item, "activate",
                                   G_CALLBACK (on_open_in_terminal_activated),
                                   g_object_ref (location),
                                   (GClosureNotify) g_object_unref, 0);

            items = g_list_append (items, term_item);
        }

        /* --- Item 3: Open in IDE (directories only) --- */
        if (is_dir && target_path)
        {
            g_autofree gchar *code_id = g_strdup_printf ("NautilusTweaks::OpenInIde_%u", ++g_action_counter);
            NautilusMenuItem *code_item = nautilus_menu_item_new (
                code_id,
                _("Open in IDE"),
                _("Open this directory as a project in IDE"),
                "applications-development-symbolic"
            );

            g_signal_connect_data (code_item, "activate",
                                   G_CALLBACK (on_open_in_ide_activated),
                                   g_strdup (target_path),
                                   (GClosureNotify) g_free, 0);

            items = g_list_append (items, code_item);
        }

        /* --- Item 4: Open / Edit as root (Local paths ONLY) --- */
        if (location && !tweaks_remote_is_file_remote (location))
        {
            g_autofree gchar *root_label = is_dir ? g_strdup (_("Open as Root"))
                                                 : g_strdup (_("Edit as Root"));

            const gchar *root_tip   = is_dir ? _("Open this folder in Nautilus with administrator privileges")
                                             : _("Edit file with administrator privileges");
            const gchar *root_icon  = is_dir ? "folder-remote-symbolic" : "accessories-text-editor-symbolic";

            g_autofree gchar *root_id = g_strdup_printf ("NautilusTweaks::OpenAsRoot_%u", ++g_action_counter);
            NautilusMenuItem *root_item = nautilus_menu_item_new (
                root_id,
                root_label,
                root_tip,
                root_icon
            );

            g_signal_connect_data (root_item, "activate",
                                   G_CALLBACK (on_open_as_root_activated),
                                   g_object_ref (first_file),
                                   (GClosureNotify) g_object_unref, 0);

            items = g_list_append (items, root_item);
        }

        if (mount_info)
            tweaks_mount_info_free (mount_info);
    }

    return items;
}

/* -------------------------------------------------------------------------- */
/* 5. Background context menu                                                 */
/* -------------------------------------------------------------------------- */

static GList *
nautilus_tweaks_actions_get_background_items (NautilusMenuProvider *provider,
                                              NautilusFileInfo     *current_folder)
{
    if (!current_folder)
        return NULL;

    g_autoptr (GFile) location = nautilus_file_info_get_location (current_folder);
    if (!location)
        return NULL;

    g_autofree gchar *target_path = g_file_get_path (location);

    TweaksMountInfo *mount_info = tweaks_mount_info_get_for_location (location);
    gboolean is_ftp = (mount_info && mount_info->mode == TWEAKS_FS_RCLONE);

    GList *items = NULL;

    /* 1. Copy current folder path */
    g_autofree gchar *bg_copy_id = g_strdup_printf ("NautilusTweaks::BgCopyPath_%u", ++g_action_counter);
    NautilusMenuItem *copy_item = nautilus_menu_item_new (
        bg_copy_id,
        _("Copy Folder Path"),
        _("Copy current folder path to clipboard"),
        "edit-copy-symbolic"
    );

    GList *single_list = g_list_append (NULL, current_folder);
    g_signal_connect_data (copy_item, "activate",
                           G_CALLBACK (on_copy_path_activated),
                           nautilus_file_info_list_copy (single_list),
                           (GClosureNotify) nautilus_file_info_list_free, 0);
    g_list_free (single_list);
    items = g_list_append (items, copy_item);

    /* 2. Open current folder in Terminal (hidden on FTP) */
    if (!is_ftp)
    {
        g_autofree gchar *bg_term_id = g_strdup_printf ("NautilusTweaks::BgOpenInTerm_%u", ++g_action_counter);
        NautilusMenuItem *term_item = nautilus_menu_item_new (
            bg_term_id,
            _("Open in Terminal"),
            _("Open current directory in your preferred terminal"),
            "utilities-terminal-symbolic"
        );

        g_signal_connect_data (term_item, "activate",
                               G_CALLBACK (on_open_in_terminal_activated),
                               g_object_ref (location),
                               (GClosureNotify) g_object_unref, 0);
        items = g_list_append (items, term_item);
    }

    /* 3. Open current folder in IDE */
    if (target_path)
    {
        g_autofree gchar *bg_code_id = g_strdup_printf ("NautilusTweaks::BgOpenInIde_%u", ++g_action_counter);
        NautilusMenuItem *code_item = nautilus_menu_item_new (
            bg_code_id,
            _("Open in IDE"),
            _("Open current directory as a project in IDE"),
            "applications-development-symbolic"
        );

        g_signal_connect_data (code_item, "activate",
                               G_CALLBACK (on_open_in_ide_activated),
                               g_strdup (target_path),
                               (GClosureNotify) g_free, 0);
        items = g_list_append (items, code_item);
    }

    /* 4. Open current folder as root (Local folders ONLY) */
    if (!tweaks_remote_is_file_remote (location))
    {
        g_autofree gchar *bg_root_id = g_strdup_printf ("NautilusTweaks::BgOpenAsRoot_%u", ++g_action_counter);
        NautilusMenuItem *root_item = nautilus_menu_item_new (
            bg_root_id,
            _("Open as Root"),
            _("Open current folder in Nautilus with administrator privileges"),
            "folder-remote-symbolic"
        );

        g_signal_connect_data (root_item, "activate",
                               G_CALLBACK (on_open_as_root_activated),
                               g_object_ref (current_folder),
                               (GClosureNotify) g_object_unref, 0);
        items = g_list_append (items, root_item);
    }

    if (mount_info)
        tweaks_mount_info_free (mount_info);

    return items;
}

/* -------------------------------------------------------------------------- */
/* Nautilus extension module initialization                                   */
/* -------------------------------------------------------------------------- */

static void
nautilus_tweaks_actions_menu_provider_iface_init (NautilusMenuProviderInterface *iface)
{
    iface->get_file_items = nautilus_tweaks_actions_get_file_items;
    iface->get_background_items = nautilus_tweaks_actions_get_background_items;
}

void
nautilus_module_initialize (GTypeModule *module)
{
    nautilus_tweaks_actions_register_type (module);
}

void
nautilus_module_shutdown (void)
{
}

void
nautilus_module_list_types (const GType **types, int *num_types)
{
    static GType type_list[1];
    type_list[0] = nautilus_tweaks_actions_get_type ();
    *types = type_list;
    *num_types = 1;
}