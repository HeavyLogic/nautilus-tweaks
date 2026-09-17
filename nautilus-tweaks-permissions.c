#include <nautilus-extension.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>
#include <errno.h>

#include "tweaks-log.h"
#include "tweaks-config.h"

static guint g_permissions_action_counter = 0;
static GtkWidget *g_active_dialog_window = NULL;

/* -------------------------------------------------------------------------- */
/* Filesystem types and mount data                                            */
/* -------------------------------------------------------------------------- */

typedef enum {
    FS_MODE_LOCAL,
    FS_MODE_SSHFS,
    FS_MODE_RCLONE
} FsMode;

typedef struct {
    FsMode mode;
    gchar *ssh_host;
    gchar *remote_base_path;
    gchar *mount_point;
} MountInfo;

static void
mount_info_free (MountInfo *info)
{
    if (!info)
        return;
    g_free (info->ssh_host);
    g_free (info->remote_base_path);
    g_free (info->mount_point);
    g_free (info);
}

/* -------------------------------------------------------------------------- */
/* Path helper functions                                                      */
/* -------------------------------------------------------------------------- */

static gchar *
format_path_for_display (const gchar *path)
{
    if (!path)
        return g_strdup ("");

    const gchar *home = g_get_home_dir ();
    if (home && g_str_has_prefix (path, home))
    {
        gsize home_len = strlen (home);
        if (path[home_len] == '/' || path[home_len] == '\0')
            return g_strdup_printf ("~%s", path + home_len);
    }
    return g_strdup (path);
}

/* -------------------------------------------------------------------------- */
/* Filesystem mode detection via /proc/mounts                                 */
/* -------------------------------------------------------------------------- */

static MountInfo *
get_mount_info_for_path (const gchar *path)
{
    MountInfo *info = g_new0 (MountInfo, 1);
    info->mode = FS_MODE_LOCAL;

    FILE *fp = fopen ("/proc/mounts", "r");
    if (!fp)
        return info;

    char line[2048];
    gsize best_match_len = 0;

    while (fgets (line, sizeof (line), fp))
    {
        char dev[512], mnt[1024], fstype[64];
        if (sscanf (line, "%511s %1023s %63s", dev, mnt, fstype) >= 3)
        {
            gsize mnt_len = strlen (mnt);
            if (g_str_has_prefix (path, mnt) && (path[mnt_len] == '/' || path[mnt_len] == '\0' || mnt_len == 1))
            {
                if (mnt_len > best_match_len)
                {
                    best_match_len = mnt_len;
                    g_free (info->mount_point);
                    info->mount_point = g_strdup (mnt);

                    if (g_str_has_prefix (fstype, "fuse.rclone") || g_strcmp0 (fstype, "rclone") == 0)
                    {
                        info->mode = FS_MODE_RCLONE;
                    }
                    else if (g_str_has_prefix (fstype, "fuse.sshfs") || g_strcmp0 (fstype, "sshfs") == 0)
                    {
                        info->mode = FS_MODE_SSHFS;
                        g_free (info->ssh_host);
                        g_free (info->remote_base_path);

                        char *colon = strchr (dev, ':');
                        if (colon)
                        {
                            info->ssh_host = g_strndup (dev, colon - dev);
                            info->remote_base_path = g_strdup (colon + 1);
                            if (strlen (info->remote_base_path) == 0)
                            {
                                g_free (info->remote_base_path);
                                info->remote_base_path = g_strdup ("/");
                            }
                        }
                        else
                        {
                            info->ssh_host = g_strdup (dev);
                            info->remote_base_path = g_strdup ("/");
                        }
                    }
                    else
                    {
                        info->mode = FS_MODE_LOCAL;
                    }
                }
            }
        }
    }
    fclose (fp);
    return info;
}

static gchar *
translate_to_remote_path (const gchar *local_path, MountInfo *info)
{
    if (info->mode != FS_MODE_SSHFS || !info->mount_point)
        return g_strdup (local_path);

    gsize mnt_len = strlen (info->mount_point);
    const gchar *subpath = local_path + mnt_len;
    while (*subpath == '/')
        subpath++;

    if (g_strcmp0 (info->remote_base_path, "/") == 0 || strlen (info->remote_base_path) == 0)
        return g_strdup_printf ("/%s", subpath);

    return g_build_filename (info->remote_base_path, subpath, NULL);
}

/* -------------------------------------------------------------------------- */
/* Widget data structure                                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    GtkWidget *window;
    GList     *target_paths;
    MountInfo *mount_info;

    /* Stack switching (Loading <-> Form) */
    GtkWidget *stack_pages;
    GtkWidget *spinner;
    GtkWidget *lbl_loading;

    GtkWidget *lbl_target_path;

    /* Dropdown selectors */
    GtkWidget     *combo_owner;
    GtkStringList *owners_model;

    GtkWidget     *combo_group;
    GtkStringList *groups_model;

    /* Permission checkboxes (3x4) */
    GtkWidget *chk_u_r;
    GtkWidget *chk_u_w;
    GtkWidget *chk_u_x;
    GtkWidget *chk_u_suid;

    GtkWidget *chk_g_r;
    GtkWidget *chk_g_w;
    GtkWidget *chk_g_x;
    GtkWidget *chk_g_sgid;

    GtkWidget *chk_o_r;
    GtkWidget *chk_o_w;
    GtkWidget *chk_o_x;
    GtkWidget *chk_o_sticky;

    /* Octal entry and extra options */
    GtkWidget *entry_octal;
    GtkWidget *chk_add_x;
    GtkWidget *chk_recursive;

    gboolean   updating_from_code;
} PermissionsDialogWidgets;

/* -------------------------------------------------------------------------- */
/* GObject plugin declaration                                                 */
/* -------------------------------------------------------------------------- */

typedef struct _NautilusTweaksPermissions {
    GObject parent_instance;
} NautilusTweaksPermissions;

typedef struct _NautilusTweaksPermissionsClass {
    GObjectClass parent_class;
} NautilusTweaksPermissionsClass;

static GType nautilus_tweaks_permissions_get_type (void);
static void nautilus_tweaks_permissions_menu_provider_iface_init (NautilusMenuProviderInterface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (NautilusTweaksPermissions, nautilus_tweaks_permissions, G_TYPE_OBJECT, 0,
    G_IMPLEMENT_INTERFACE_DYNAMIC (NAUTILUS_TYPE_MENU_PROVIDER,
                                   nautilus_tweaks_permissions_menu_provider_iface_init))

static void nautilus_tweaks_permissions_class_init (NautilusTweaksPermissionsClass *klass) {}
static void nautilus_tweaks_permissions_init (NautilusTweaksPermissions *self) {}
static void nautilus_tweaks_permissions_class_finalize (NautilusTweaksPermissionsClass *klass) {}

/* -------------------------------------------------------------------------- */
/* Reload Nautilus views                                                      */
/* -------------------------------------------------------------------------- */

static void
reload_nautilus_views (void)
{
    GApplication *app = g_application_get_default ();
    if (app && GTK_IS_APPLICATION (app))
    {
        GList *windows = gtk_application_get_windows (GTK_APPLICATION (app));
        for (GList *w = windows; w != NULL; w = w->next)
        {
            if (GTK_IS_WINDOW (w->data))
            {
                gtk_widget_activate_action (GTK_WIDGET (w->data), "slot.reload", NULL);
                GtkWidget *focus = gtk_window_get_focus (GTK_WINDOW (w->data));
                if (focus)
                    gtk_widget_activate_action (focus, "slot.reload", NULL);
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Live sync between Octal and Checkboxes                                     */
/* -------------------------------------------------------------------------- */

static void
update_octal_from_checkboxes (PermissionsDialogWidgets *w)
{
    if (w->updating_from_code)
        return;

    w->updating_from_code = TRUE;

    guint mode = 0;

    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_u_suid)))   mode |= 04000;
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_g_sgid)))   mode |= 02000;
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_o_sticky))) mode |= 01000;

    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_u_r))) mode |= 0400;
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_u_w))) mode |= 0200;
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_u_x))) mode |= 0100;

    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_g_r))) mode |= 0040;
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_g_w))) mode |= 0020;
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_g_x))) mode |= 0010;

    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_o_r))) mode |= 0004;
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_o_w))) mode |= 0002;
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_o_x))) mode |= 0001;

    char octal_str[16];
    snprintf (octal_str, sizeof (octal_str), "%04o", mode);
    gtk_editable_set_text (GTK_EDITABLE (w->entry_octal), octal_str);

    w->updating_from_code = FALSE;
}

static void
on_perm_checkbox_toggled (GtkCheckButton *btn, gpointer user_data)
{
    PermissionsDialogWidgets *w = (PermissionsDialogWidgets *) user_data;
    update_octal_from_checkboxes (w);
}

static void
apply_mode_to_checkboxes (PermissionsDialogWidgets *w, long mode)
{
    w->updating_from_code = TRUE;

    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_u_suid),   (mode & 04000) != 0);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_g_sgid),   (mode & 02000) != 0);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_o_sticky), (mode & 01000) != 0);

    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_u_r), (mode & 0400) != 0);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_u_w), (mode & 0200) != 0);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_u_x), (mode & 0100) != 0);

    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_g_r), (mode & 0040) != 0);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_g_w), (mode & 0020) != 0);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_g_x), (mode & 0010) != 0);

    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_o_r), (mode & 0004) != 0);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_o_w), (mode & 0002) != 0);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_o_x), (mode & 0001) != 0);

    char octal_str[16];
    snprintf (octal_str, sizeof (octal_str), "%04lo", mode);
    gtk_editable_set_text (GTK_EDITABLE (w->entry_octal), octal_str);

    w->updating_from_code = FALSE;
}

static void
on_octal_entry_changed (GtkEditable *editable, gpointer user_data)
{
    PermissionsDialogWidgets *w = (PermissionsDialogWidgets *) user_data;
    if (w->updating_from_code)
        return;

    const char *text = gtk_editable_get_text (editable);
    if (strlen (text) == 0)
        return;

    char *endptr = NULL;
    long mode = strtol (text, &endptr, 8);
    if (*endptr != '\0' || mode < 0 || mode > 07777)
        return;

    apply_mode_to_checkboxes (w, mode);
}

/* -------------------------------------------------------------------------- */
/* Building user and group lists                                              */
/* -------------------------------------------------------------------------- */

static gchar *
clean_entry_value (const gchar *input_str)
{
    if (!input_str)
        return g_strdup ("");

    gchar *trimmed = g_strstrip (g_strdup (input_str));
    gchar *bracket = strchr (trimmed, ' ');
    if (bracket)
        *bracket = '\0';

    return trimmed;
}

static void
select_in_string_list (GtkWidget *dropdown, GtkStringList *model, const gchar *target_str)
{
    if (!target_str || !model)
        return;

    g_autofree gchar *target_name = clean_entry_value (target_str);
    guint n = g_list_model_get_n_items (G_LIST_MODEL (model));
    for (guint i = 0; i < n; i++)
    {
        const gchar *s = gtk_string_list_get_string (model, i);
        g_autofree gchar *name = clean_entry_value (s);
        if (g_strcmp0 (name, target_name) == 0)
        {
            gtk_drop_down_set_selected (GTK_DROP_DOWN (dropdown), i);
            return;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Create searchable GtkDropDown (GtkPropertyExpression)                      */
/* -------------------------------------------------------------------------- */

static GtkWidget *
create_searchable_dropdown (GtkStringList *model)
{
    GtkExpression *expr = gtk_property_expression_new (GTK_TYPE_STRING_OBJECT, NULL, "string");
    GtkWidget *dropdown = gtk_drop_down_new (G_LIST_MODEL (model), expr);
    gtk_drop_down_set_enable_search (GTK_DROP_DOWN (dropdown), TRUE);
    gtk_widget_set_hexpand (dropdown, TRUE);
    return dropdown;
}

/* -------------------------------------------------------------------------- */
/* Asynchronous execution of permission change command                        */
/* -------------------------------------------------------------------------- */

static void
on_permissions_proc_finished (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    GSubprocess *proc = G_SUBPROCESS (source_object);
    g_autofree gchar *stdout_buf = NULL;
    g_autofree gchar *stderr_buf = NULL;
    g_autoptr (GError) err = NULL;

    g_subprocess_communicate_utf8_finish (proc, res, &stdout_buf, &stderr_buf, &err);

    gint exit_code = g_subprocess_get_exit_status (proc);
    log_debug ("[EXEC FINISH] exit_code=%d, stdout='%s', stderr='%s', err='%s'",
               exit_code,
               stdout_buf ? stdout_buf : "",
               stderr_buf ? stderr_buf : "",
               err ? err->message : "none");

    if (!err && g_subprocess_get_successful (proc))
    {
        log_debug ("[SUCCESS] Permissions updated successfully");
        const gchar *notify_argv[] = {
            "notify-send",
            "-u", "normal",
            "-i", "dialog-information",
            _("Permissions"),
            _("Permissions and owner updated successfully"),
            NULL
        };
        g_spawn_async (NULL, (gchar **) notify_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);

        reload_nautilus_views ();
    }
    else
    {
        g_autofree gchar *err_msg = NULL;
        if (stderr_buf && strlen (g_strstrip (stderr_buf)) > 0)
            err_msg = g_strdup (stderr_buf);
        else if (err)
            err_msg = g_strdup (err->message);
        else
            err_msg = g_strdup (_("Operation cancelled by user or permission denied"));

        log_debug ("[ERROR] %s", err_msg);

        const gchar *notify_argv[] = {
            "notify-send",
            "-u", "critical",
            "-i", "dialog-error",
            _("Error Changing Permissions"),
            err_msg,
            NULL
        };
        g_spawn_async (NULL, (gchar **) notify_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }
}

static void
on_apply_clicked (GtkButton *btn, gpointer user_data)
{
    PermissionsDialogWidgets *w = (PermissionsDialogWidgets *) user_data;

    const char *octal_text = gtk_editable_get_text (GTK_EDITABLE (w->entry_octal));
    char *endptr = NULL;
    long base_mode = strtol (octal_text, &endptr, 8);
    if (*endptr != '\0' || base_mode < 0 || base_mode > 07777)
    {
        base_mode = 0755;
    }

    gboolean add_x = gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_add_x));
    gboolean recursive = gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_recursive));

    guint owner_idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (w->combo_owner));
    const gchar *raw_owner = gtk_string_list_get_string (w->owners_model, owner_idx);
    g_autofree gchar *owner_name = clean_entry_value (raw_owner);

    guint group_idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (w->combo_group));
    const gchar *raw_group = gtk_string_list_get_string (w->groups_model, group_idx);
    g_autofree gchar *group_name = clean_entry_value (raw_group);

    long file_mode = base_mode;
    long dir_mode  = base_mode;

    if (add_x)
    {
        if (file_mode & 0400) dir_mode |= 0100;
        if (file_mode & 0040) dir_mode |= 0010;
        if (file_mode & 0004) dir_mode |= 0001;
    }

    GString *inner_cmd = g_string_new (NULL);
    gboolean first = TRUE;

    for (GList *l = w->target_paths; l != NULL; l = l->next)
    {
        const gchar *local_path = (const gchar *) l->data;
        g_autofree gchar *target_path = translate_to_remote_path (local_path, w->mount_info);
        g_autofree gchar *quoted_path = g_shell_quote (target_path);

        if (!first)
            g_string_append (inner_cmd, " && ");
        first = FALSE;

        /* 1. chown */
        if (strlen (owner_name) > 0 || strlen (group_name) > 0)
        {
            g_autofree gchar *chown_target = NULL;
            if (strlen (owner_name) > 0 && strlen (group_name) > 0)
                chown_target = g_strdup_printf ("%s:%s", owner_name, group_name);
            else if (strlen (owner_name) > 0)
                chown_target = g_strdup (owner_name);
            else
                chown_target = g_strdup_printf (":%s", group_name);

            if (recursive)
                g_string_append_printf (inner_cmd, "chown -R %s %s && ", chown_target, quoted_path);
            else
                g_string_append_printf (inner_cmd, "chown %s %s && ", chown_target, quoted_path);
        }

        /* 2. chmod */
        if (recursive)
        {
            if (add_x)
            {
                g_string_append_printf (inner_cmd, "find %s -type d -exec chmod %04lo {} + && ", quoted_path, dir_mode);
                g_string_append_printf (inner_cmd, "find %s -type f -exec chmod %04lo {} +", quoted_path, file_mode);
            }
            else
            {
                g_string_append_printf (inner_cmd, "chmod -R %04lo %s", base_mode, quoted_path);
            }
        }
        else
        {
            if (g_file_test (local_path, G_FILE_TEST_IS_DIR) && add_x)
                g_string_append_printf (inner_cmd, "chmod %04lo %s", dir_mode, quoted_path);
            else
                g_string_append_printf (inner_cmd, "chmod %04lo %s", file_mode, quoted_path);
        }
    }

    g_autoptr (GError) spawn_err = NULL;
    g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE
    );
    GSubprocess *proc = NULL;

    if (w->mount_info->mode == FS_MODE_SSHFS && w->mount_info->ssh_host)
    {
        log_debug ("[APPLY REMOTE] Server: %s, Command: %s", w->mount_info->ssh_host, inner_cmd->str);
        proc = g_subprocess_launcher_spawn (
            launcher,
            &spawn_err,
            "ssh", "-o", "ConnectTimeout=10", "-o", "BatchMode=yes", w->mount_info->ssh_host, inner_cmd->str,
            NULL
        );
    }
    else
    {
        log_debug ("[APPLY LOCAL] Command: pkexec sh -c \"%s\"", inner_cmd->str);
        proc = g_subprocess_launcher_spawn (
            launcher,
            &spawn_err,
            "pkexec", "sh", "-c", inner_cmd->str,
            NULL
        );
    }

    g_string_free (inner_cmd, TRUE);

    if (spawn_err)
    {
        log_debug ("[SPAWN ERROR] %s", spawn_err->message);
    }
    else if (proc)
    {
        g_subprocess_communicate_utf8_async (proc, NULL, NULL, on_permissions_proc_finished, NULL);
        g_object_unref (proc);
    }

    gtk_window_destroy (GTK_WINDOW (w->window));
}

static void
on_cancel_clicked (GtkButton *btn, gpointer user_data)
{
    PermissionsDialogWidgets *w = (PermissionsDialogWidgets *) user_data;
    log_debug ("[UI] Cancel clicked, destroying window");
    gtk_window_destroy (GTK_WINDOW (w->window));
}

static void
on_dialog_destroyed (gpointer data, GObject *where_the_object_was)
{
    log_debug ("[DESTROY] on_dialog_destroyed started");
    PermissionsDialogWidgets *w = (PermissionsDialogWidgets *) data;
    g_active_dialog_window = NULL;

    if (w)
    {
        log_debug ("[DESTROY] freeing target_paths");
        g_list_free_full (w->target_paths, g_free);

        log_debug ("[DESTROY] freeing mount_info");
        mount_info_free (w->mount_info);

        /* Do NOT call g_clear_object for models here: GTK handles them on widget dispose */

        log_debug ("[DESTROY] freeing struct w");
        g_free (w);
    }
    log_debug ("[DESTROY] on_dialog_destroyed finished");
}

/* -------------------------------------------------------------------------- */
/* Asynchronous data loading (Stat + Passwd + Groups)                         */
/* -------------------------------------------------------------------------- */

static void
populate_models_from_parsed_data (PermissionsDialogWidgets *w,
                                  const gchar *passwd_part,
                                  const gchar *group_part,
                                  const gchar *initial_owner,
                                  const gchar *initial_group)
{
    GHashTable *seen_u = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *seen_g = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    #define ADD_U(model, seen, name, uid) do { \
        if ((name) && strlen(name) > 0 && !g_hash_table_contains(seen, (name))) { \
            g_hash_table_add(seen, g_strdup(name)); \
            g_autofree gchar *entry_str = g_strdup_printf("%s [%s]", (name), (uid)); \
            gtk_string_list_append(model, entry_str); \
        } \
    } while (0)

    #define ADD_G(model, seen, name, gid) do { \
        if ((name) && strlen(name) > 0 && !g_hash_table_contains(seen, (name))) { \
            g_hash_table_add(seen, g_strdup(name)); \
            g_autofree gchar *entry_str = g_strdup_printf("%s [%s]", (name), (gid)); \
            gtk_string_list_append(model, entry_str); \
        } \
    } while (0)

    ADD_U (w->owners_model, seen_u, "root", "0");
    ADD_G (w->groups_model, seen_g, "root", "0");

    if (initial_owner && strlen (initial_owner) > 0)
        ADD_U (w->owners_model, seen_u, initial_owner, "current");

    if (initial_group && strlen (initial_group) > 0)
        ADD_G (w->groups_model, seen_g, initial_group, "current");

    if (passwd_part)
    {
        gchar **lines = g_strsplit (passwd_part, "\n", -1);
        for (int i = 0; lines[i] != NULL; i++)
        {
            gchar *line = lines[i];
            if (strlen (line) == 0) continue;
            gchar **parts = g_strsplit (line, ":", 4);
            if (parts[0] && parts[1] && parts[2])
            {
                const gchar *u_name = parts[0];
                const gchar *u_uid = parts[2];
                ADD_U (w->owners_model, seen_u, u_name, u_uid);
            }
            g_strfreev (parts);
        }
        g_strfreev (lines);
    }

    if (group_part)
    {
        gchar **lines = g_strsplit (group_part, "\n", -1);
        for (int i = 0; lines[i] != NULL; i++)
        {
            gchar *line = lines[i];
            if (strlen (line) == 0) continue;
            gchar **parts = g_strsplit (line, ":", 4);
            if (parts[0] && parts[1] && parts[2])
            {
                const gchar *g_name = parts[0];
                const gchar *g_gid = parts[2];
                ADD_G (w->groups_model, seen_g, g_name, g_gid);
            }
            g_strfreev (parts);
        }
        g_strfreev (lines);
    }

    /* Custom entries from config.ini */
    TweaksConfig *config = tweaks_config_load ();
    if (config && config->extra_users)
    {
        for (int i = 0; config->extra_users[i] != NULL; i++)
        {
            const gchar *extra = config->extra_users[i];
            if (strlen (extra) > 0)
                ADD_U (w->owners_model, seen_u, extra, "custom");
        }
    }
    if (config && config->extra_groups)
    {
        for (int i = 0; config->extra_groups[i] != NULL; i++)
        {
            const gchar *extra = config->extra_groups[i];
            if (strlen (extra) > 0)
                ADD_G (w->groups_model, seen_g, extra, "custom");
        }
    }
    tweaks_config_free (config);

    #undef ADD_U
    #undef ADD_G
    g_hash_table_destroy (seen_u);
    g_hash_table_destroy (seen_g);

    /* Select active user and group */
    select_in_string_list (w->combo_owner, w->owners_model, initial_owner ? initial_owner : "root");
    select_in_string_list (w->combo_group, w->groups_model, initial_group ? initial_group : "root");

    /* Stop spinner and display form */
    gtk_spinner_stop (GTK_SPINNER (w->spinner));
    gtk_stack_set_visible_child_name (GTK_STACK (w->stack_pages), "form");
}

static void
on_remote_load_finished (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    PermissionsDialogWidgets *w = (PermissionsDialogWidgets *) user_data;
    GSubprocess *proc = G_SUBPROCESS (source_object);
    g_autofree gchar *stdout_buf = NULL;
    g_autoptr (GError) err = NULL;

    g_subprocess_communicate_utf8_finish (proc, res, &stdout_buf, NULL, &err);

    g_autofree gchar *initial_owner = NULL;
    g_autofree gchar *initial_group = NULL;
    mode_t initial_mode = 0755;

    gchar *passwd_part = NULL;
    gchar *group_part = NULL;

    if (!err && g_subprocess_get_successful (proc) && stdout_buf)
    {
        gchar **sections = g_strsplit (stdout_buf, "===PASSWD===\n", 2);
        gchar *stat_part = sections[0];
        gchar *rest = sections[1];

        if (stat_part && strlen (g_strstrip (stat_part)) > 0)
        {
            gchar **stat_tokens = g_strsplit (stat_part, ":", 5);
            if (stat_tokens[0] && stat_tokens[1] && stat_tokens[2] && stat_tokens[3] && stat_tokens[4])
            {
                initial_owner = g_strdup (stat_tokens[3]);
                initial_group = g_strdup (stat_tokens[4]);
                initial_mode = (mode_t) strtol (stat_tokens[2], NULL, 8);
                log_debug ("[REMOTE STAT ASYNC] Owner: %s, Group: %s, Mode: %04o",
                           initial_owner, initial_group, (guint) initial_mode);
            }
            g_strfreev (stat_tokens);
        }

        gchar **g_split = NULL;
        if (rest)
        {
            g_split = g_strsplit (rest, "===GROUPS===\n", 2);
            passwd_part = g_split[0];
            group_part = g_split[1];
        }

        apply_mode_to_checkboxes (w, initial_mode);
        populate_models_from_parsed_data (w, passwd_part, group_part, initial_owner, initial_group);

        if (g_split)
            g_strfreev (g_split);
        g_strfreev (sections);
    }
    else
    {
        log_debug ("[REMOTE LOAD FAILED] %s", err ? err->message : "unknown error");
        populate_models_from_parsed_data (w, NULL, NULL, "root", "root");
    }
}

static void
start_async_data_load (PermissionsDialogWidgets *w, const gchar *first_path)
{
    if (w->mount_info->mode == FS_MODE_SSHFS && w->mount_info->ssh_host)
    {
        g_autofree gchar *rem_path = translate_to_remote_path (first_path, w->mount_info);
        g_autofree gchar *quoted_rem = g_shell_quote (rem_path);

        log_debug ("[ASYNC SSH] Starting data fetch from %s", w->mount_info->ssh_host);

        g_autoptr (GError) err = NULL;
        g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (
            G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE
        );

        g_autofree gchar *remote_cmd = g_strdup_printf (
            "stat -c '%%u:%%g:%%a:%%U:%%G' %s 2>/dev/null; echo '===PASSWD==='; getent passwd; echo '===GROUPS==='; getent group",
            quoted_rem
        );

        GSubprocess *proc = g_subprocess_launcher_spawn (
            launcher,
            &err,
            "ssh", "-o", "ConnectTimeout=5", "-o", "BatchMode=yes", w->mount_info->ssh_host, remote_cmd,
            NULL
        );

        if (proc)
        {
            g_subprocess_communicate_utf8_async (proc, NULL, NULL, on_remote_load_finished, w);
            g_object_unref (proc);
            return;
        }
    }

    /* Local data fetch */
    g_autofree gchar *initial_owner = NULL;
    g_autofree gchar *initial_group = NULL;
    mode_t initial_mode = 0755;

    if (first_path)
    {
        struct stat st;
        if (stat (first_path, &st) == 0)
        {
            initial_mode = st.st_mode & 07777;
            struct passwd *pw = getpwuid (st.st_uid);
            if (pw) initial_owner = g_strdup (pw->pw_name);
            struct group *gr = getgrgid (st.st_gid);
            if (gr) initial_group = g_strdup (gr->gr_name);
        }
    }

    apply_mode_to_checkboxes (w, initial_mode);

    g_autofree gchar *passwd_out = NULL;
    g_autofree gchar *group_out = NULL;
    g_spawn_command_line_sync ("getent passwd", &passwd_out, NULL, NULL, NULL);
    g_spawn_command_line_sync ("getent group", &group_out, NULL, NULL, NULL);

    populate_models_from_parsed_data (w, passwd_out, group_out, initial_owner, initial_group);
}

/* -------------------------------------------------------------------------- */
/* UI window construction                                                     */
/* -------------------------------------------------------------------------- */

static GtkWidget *
create_permissions_window (GList *files)
{
    PermissionsDialogWidgets *w = g_new0 (PermissionsDialogWidgets, 1);
    w->updating_from_code = FALSE;

    g_autofree gchar *first_path = NULL;

    for (GList *l = files; l != NULL; l = l->next)
    {
        NautilusFileInfo *file = NAUTILUS_FILE_INFO (l->data);
        g_autoptr (GFile) loc = nautilus_file_info_get_location (file);
        if (loc)
        {
            gchar *path = g_file_get_path (loc);
            if (path)
            {
                if (!first_path)
                    first_path = g_strdup (path);
                w->target_paths = g_list_append (w->target_paths, path);
            }
        }
    }

    if (first_path)
        w->mount_info = get_mount_info_for_path (first_path);
    else
        w->mount_info = g_new0 (MountInfo, 1);

    w->owners_model = gtk_string_list_new (NULL);
    w->groups_model = gtk_string_list_new (NULL);

    w->window = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (w->window), _("Permissions"));
    gtk_window_set_resizable (GTK_WINDOW (w->window), FALSE);
    gtk_window_set_default_size (GTK_WINDOW (w->window), 450, -1);

    g_object_weak_ref (G_OBJECT (w->window), on_dialog_destroyed, w);

    /* Stack: Loading vs Form */
    w->stack_pages = gtk_stack_new ();
    gtk_stack_set_transition_type (GTK_STACK (w->stack_pages), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_window_set_child (GTK_WINDOW (w->window), w->stack_pages);

    /* --- PAGE 1: Loading (Spinner) --- */
    GtkWidget *loading_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_valign (loading_box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign (loading_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top (loading_box, 60);
    gtk_widget_set_margin_bottom (loading_box, 60);
    gtk_widget_set_margin_start (loading_box, 40);
    gtk_widget_set_margin_end (loading_box, 40);

    w->spinner = gtk_spinner_new ();
    gtk_widget_set_size_request (w->spinner, 36, 36);
    gtk_spinner_start (GTK_SPINNER (w->spinner));
    gtk_box_append (GTK_BOX (loading_box), w->spinner);

    w->lbl_loading = gtk_label_new (
        (w->mount_info->mode == FS_MODE_SSHFS)
        ? _("Loading users and permissions from server...")
        : _("Reading permissions...")
    );
    gtk_widget_add_css_class (w->lbl_loading, "dim-label");
    gtk_box_append (GTK_BOX (loading_box), w->lbl_loading);

    gtk_stack_add_named (GTK_STACK (w->stack_pages), loading_box, "loading");

    /* --- PAGE 2: Main Form --- */
    GtkWidget *form_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start (form_box, 16);
    gtk_widget_set_margin_end (form_box, 16);
    gtk_widget_set_margin_top (form_box, 16);
    gtk_widget_set_margin_bottom (form_box, 16);
    gtk_stack_add_named (GTK_STACK (w->stack_pages), form_box, "form");

    /* 0. Target path header */
    g_autofree gchar *target_label_text = NULL;
    guint target_count = g_list_length (w->target_paths);
    if (target_count == 1 && first_path)
    {
        if (w->mount_info->mode == FS_MODE_SSHFS && w->mount_info->ssh_host)
        {
            g_autofree gchar *rem = translate_to_remote_path (first_path, w->mount_info);
            target_label_text = g_strdup_printf (_("Remote: %s"), rem);
        }
        else
        {
            g_autofree gchar *disp_path = format_path_for_display (first_path);
            target_label_text = g_strdup_printf (_("Target: %s"), disp_path);
        }
    }
    else
    {
        target_label_text = g_strdup_printf (_("Selected: %u items"), target_count);
    }

    w->lbl_target_path = gtk_label_new (target_label_text);
    gtk_widget_set_halign (w->lbl_target_path, GTK_ALIGN_START);
    gtk_label_set_ellipsize (GTK_LABEL (w->lbl_target_path), PANGO_ELLIPSIZE_START);
    gtk_widget_add_css_class (w->lbl_target_path, "dim-label");
    gtk_box_append (GTK_BOX (form_box), w->lbl_target_path);

    /* 1. Owner / Group (Single searchable dropdowns) */
    GtkWidget *grid_top = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (grid_top), 12);
    gtk_grid_set_row_spacing (GTK_GRID (grid_top), 8);

    GtkWidget *lbl_owner = gtk_label_new (_("Owner:"));
    gtk_widget_set_halign (lbl_owner, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grid_top), lbl_owner, 0, 0, 1, 1);

    w->combo_owner = create_searchable_dropdown (w->owners_model);
    gtk_grid_attach (GTK_GRID (grid_top), w->combo_owner, 1, 0, 1, 1);

    GtkWidget *lbl_group = gtk_label_new (_("Group:"));
    gtk_widget_set_halign (lbl_group, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grid_top), lbl_group, 0, 1, 1, 1);

    w->combo_group = create_searchable_dropdown (w->groups_model);
    gtk_grid_attach (GTK_GRID (grid_top), w->combo_group, 1, 1, 1, 1);

    gtk_box_append (GTK_BOX (form_box), grid_top);
    gtk_box_append (GTK_BOX (form_box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    /* 2. Permissions block */
    GtkWidget *grid_perm = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (grid_perm), 12);
    gtk_grid_set_row_spacing (GTK_GRID (grid_perm), 6);

    GtkWidget *lbl_u = gtk_label_new_with_mnemonic (_("_Owner"));
    GtkWidget *lbl_g = gtk_label_new_with_mnemonic (_("_Group"));
    GtkWidget *lbl_o = gtk_label_new_with_mnemonic (_("Ot_hers"));
    GtkWidget *lbl_octal = gtk_label_new_with_mnemonic (_("O_ctal:"));
    gtk_widget_set_halign (lbl_u, GTK_ALIGN_START);
    gtk_widget_set_halign (lbl_g, GTK_ALIGN_START);
    gtk_widget_set_halign (lbl_o, GTK_ALIGN_START);
    gtk_widget_set_halign (lbl_octal, GTK_ALIGN_START);

    gtk_grid_attach (GTK_GRID (grid_perm), lbl_u, 0, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), lbl_g, 0, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), lbl_o, 0, 2, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), lbl_octal, 0, 3, 1, 1);

    /* Owner checkboxes */
    w->chk_u_r    = gtk_check_button_new_with_label ("R");
    w->chk_u_w    = gtk_check_button_new_with_label ("W");
    w->chk_u_x    = gtk_check_button_new_with_label ("X");
    w->chk_u_suid = gtk_check_button_new_with_label (_("Set UID"));
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_u_r,    1, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_u_w,    2, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_u_x,    3, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_u_suid, 4, 0, 1, 1);

    /* Group checkboxes */
    w->chk_g_r    = gtk_check_button_new_with_label ("R");
    w->chk_g_w    = gtk_check_button_new_with_label ("W");
    w->chk_g_x    = gtk_check_button_new_with_label ("X");
    w->chk_g_sgid = gtk_check_button_new_with_label (_("Set GID"));
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_g_r,    1, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_g_w,    2, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_g_x,    3, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_g_sgid, 4, 1, 1, 1);

    /* Others checkboxes */
    w->chk_o_r      = gtk_check_button_new_with_label ("R");
    w->chk_o_w      = gtk_check_button_new_with_label ("W");
    w->chk_o_x      = gtk_check_button_new_with_label ("X");
    w->chk_o_sticky = gtk_check_button_new_with_label (_("Sticky bit"));
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_o_r,      1, 2, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_o_w,      2, 2, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_o_x,      3, 2, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_o_sticky, 4, 2, 1, 1);

    /* Octal */
    w->entry_octal = gtk_entry_new ();
    gtk_entry_set_max_length (GTK_ENTRY (w->entry_octal), 5);
    gtk_label_set_mnemonic_widget (GTK_LABEL (lbl_octal), w->entry_octal);
    gtk_grid_attach (GTK_GRID (grid_perm), w->entry_octal, 1, 3, 2, 1);

    /* Add X to directories (left-aligned across columns) */
    w->chk_add_x = gtk_check_button_new_with_mnemonic (_("Add _X to directories"));
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_add_x, 0, 4, 5, 1);

    gtk_box_append (GTK_BOX (form_box), grid_perm);
    gtk_box_append (GTK_BOX (form_box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    /* 3. Recursive checkbox */
    w->chk_recursive = gtk_check_button_new_with_mnemonic (_("Set owner, group and permissions _recursively"));
    gtk_box_append (GTK_BOX (form_box), w->chk_recursive);

    /* 4. Cancel / Apply buttons */
    GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign (btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top (btn_box, 8);

    GtkWidget *btn_cancel = gtk_button_new_with_label (_("Cancel"));
    GtkWidget *btn_apply  = gtk_button_new_with_label (_("Apply"));
    gtk_widget_add_css_class (btn_apply, "suggested-action");

    g_signal_connect (btn_cancel, "clicked", G_CALLBACK (on_cancel_clicked), w);
    g_signal_connect (btn_apply, "clicked", G_CALLBACK (on_apply_clicked), w);

    gtk_box_append (GTK_BOX (btn_box), btn_cancel);
    gtk_box_append (GTK_BOX (btn_box), btn_apply);
    gtk_box_append (GTK_BOX (form_box), btn_box);

    /* Connect recalculation signals */
    GtkWidget *all_checks[] = {
        w->chk_u_r, w->chk_u_w, w->chk_u_x, w->chk_u_suid,
        w->chk_g_r, w->chk_g_w, w->chk_g_x, w->chk_g_sgid,
        w->chk_o_r, w->chk_o_w, w->chk_o_x, w->chk_o_sticky,
        NULL
    };

    for (int i = 0; all_checks[i] != NULL; i++)
        g_signal_connect (all_checks[i], "toggled", G_CALLBACK (on_perm_checkbox_toggled), w);

    g_signal_connect (w->entry_octal, "changed", G_CALLBACK (on_octal_entry_changed), w);

    /* Start background data fetch */
    start_async_data_load (w, first_path);

    return w->window;
}

/* -------------------------------------------------------------------------- */
/* Context menu for selected files/folders                                    */
/* -------------------------------------------------------------------------- */

static void
on_permissions_activated (NautilusMenuItem *item, gpointer user_data)
{
    GList *files = (GList *) user_data;

    if (g_active_dialog_window != NULL)
    {
        gtk_window_present (GTK_WINDOW (g_active_dialog_window));
        return;
    }

    g_active_dialog_window = create_permissions_window (files);
    gtk_window_present (GTK_WINDOW (g_active_dialog_window));
}

static GList *
nautilus_tweaks_permissions_get_file_items (NautilusMenuProvider *provider, GList *files)
{
    if (g_list_length (files) == 0)
        return NULL;

    NautilusFileInfo *first_file = NAUTILUS_FILE_INFO (files->data);
    g_autoptr (GFile) loc = nautilus_file_info_get_location (first_file);
    if (loc)
    {
        g_autofree gchar *path = g_file_get_path (loc);
        if (path)
        {
            MountInfo *info = get_mount_info_for_path (path);
            if (info->mode == FS_MODE_RCLONE)
            {
                mount_info_free (info);
                return NULL;
            }
            mount_info_free (info);
        }
    }

    GList *items = NULL;
    g_autofree gchar *perm_id = g_strdup_printf ("NautilusTweaks::Permissions_%u", ++g_permissions_action_counter);

    NautilusMenuItem *perm_item = nautilus_menu_item_new (
        perm_id,
        _("Permissions..."),
        _("Change owner, group and permissions (chmod/chown)"),
        "dialog-password-symbolic"
    );

    g_signal_connect_data (perm_item, "activate",
                           G_CALLBACK (on_permissions_activated),
                           nautilus_file_info_list_copy (files),
                           (GClosureNotify) (GCallback) (GDestroyNotify) nautilus_file_info_list_free, 0);

    items = g_list_append (items, perm_item);
    return items;
}

/* -------------------------------------------------------------------------- */
/* Background context menu                                                    */
/* -------------------------------------------------------------------------- */

static GList *
nautilus_tweaks_permissions_get_background_items (NautilusMenuProvider *provider,
                                                  NautilusFileInfo     *current_folder)
{
    if (!current_folder)
        return NULL;

    g_autoptr (GFile) location = nautilus_file_info_get_location (current_folder);
    if (!location)
        return NULL;

    g_autofree gchar *target_path = g_file_get_path (location);
    if (!target_path)
        return NULL;

    MountInfo *info = get_mount_info_for_path (target_path);
    if (info->mode == FS_MODE_RCLONE)
    {
        mount_info_free (info);
        return NULL;
    }
    mount_info_free (info);

    GList *items = NULL;
    g_autofree gchar *bg_perm_id = g_strdup_printf ("NautilusTweaks::BgPermissions_%u", ++g_permissions_action_counter);

    NautilusMenuItem *perm_item = nautilus_menu_item_new (
        bg_perm_id,
        _("Permissions..."),
        _("Change owner, group and permissions of current folder (chmod/chown)"),
        "dialog-password-symbolic"
    );

    GList *single_list = g_list_append (NULL, current_folder);
    g_signal_connect_data (perm_item, "activate",
                           G_CALLBACK (on_permissions_activated),
                           nautilus_file_info_list_copy (single_list),
                           (GClosureNotify) (GCallback) (GDestroyNotify) nautilus_file_info_list_free, 0);
    g_list_free (single_list);

    items = g_list_append (items, perm_item);
    return items;
}

/* -------------------------------------------------------------------------- */
/* Nautilus extension module initialization                                   */
/* -------------------------------------------------------------------------- */

static void
nautilus_tweaks_permissions_menu_provider_iface_init (NautilusMenuProviderInterface *iface)
{
    iface->get_file_items = nautilus_tweaks_permissions_get_file_items;
    iface->get_background_items = nautilus_tweaks_permissions_get_background_items;
}

void
nautilus_module_initialize (GTypeModule *module)
{
    nautilus_tweaks_permissions_register_type (module);
}

void
nautilus_module_shutdown (void)
{
}

void
nautilus_module_list_types (const GType **types, int *num_types)
{
    static GType type_list[1];
    type_list[0] = nautilus_tweaks_permissions_get_type ();
    *types = type_list;
    *num_types = 1;
}