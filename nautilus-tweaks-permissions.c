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

#include "tweaks-config.h"

static guint g_permissions_action_counter = 0;
static GtkWidget *g_active_dialog_window = NULL;

/* -------------------------------------------------------------------------- */
/* Типы файловых систем и данные монтирования                                 */
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
/* Логирование в ~/.config/nautilus-tweaks/debug.log                          */
/* -------------------------------------------------------------------------- */

static void
log_debug (const gchar *format, ...)
{
    const gchar *config_dir = g_get_user_config_dir ();
    g_autofree gchar *log_dir = g_build_filename (config_dir, "nautilus-tweaks", NULL);
    g_mkdir_with_parents (log_dir, 0755);
    g_autofree gchar *log_path = g_build_filename (log_dir, "debug.log", NULL);

    FILE *fp = fopen (log_path, "a");
    if (!fp)
        return;

    GDateTime *now = g_date_time_new_now_local ();
    g_autofree gchar *time_str = g_date_time_format (now, "%Y-%m-%d %H:%M:%S");
    g_date_time_unref (now);

    va_list args;
    va_start (args, format);
    g_autofree gchar *msg = g_strdup_vprintf (format, args);
    va_end (args);

    fprintf (fp, "[%s] %s\n", time_str, msg);
    fflush (fp);
    fclose (fp);
}

/* -------------------------------------------------------------------------- */
/* Определение режима ФС через /proc/mounts                                   */
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
/* Структура данных виджетов                                                  */
/* -------------------------------------------------------------------------- */

typedef struct {
    GtkWidget *window;
    GList     *target_paths;
    MountInfo *mount_info;

    /* Стек переключения (Загрузка <-> Форма) */
    GtkWidget *stack_pages;
    GtkWidget *spinner;
    GtkWidget *lbl_loading;

    GtkWidget *lbl_target_path;

    /* Выпадающие списки */
    GtkWidget     *combo_owner_compact;
    GtkWidget     *combo_owner_full;
    GtkStringList *owners_compact_model;
    GtkStringList *owners_full_model;

    GtkWidget     *combo_group_compact;
    GtkWidget     *combo_group_full;
    GtkStringList *groups_compact_model;
    GtkStringList *groups_full_model;

    /* Чекбокс показа всех пользователей/групп */
    GtkWidget *chk_show_all;

    /* Чекбоксы прав (3x4) */
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

    /* Octal и дополнительные опции */
    GtkWidget *entry_octal;
    GtkWidget *chk_add_x;
    GtkWidget *chk_recursive;

    gboolean   updating_from_code;
} PermissionsDialogWidgets;

/* -------------------------------------------------------------------------- */
/* GObject объявление плагина                                                 */
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
/* Обновление вида Nautilus                                                   */
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
/* Живой пересчёт Octal <-> Чекбоксы                                          */
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
/* Формирование списков пользователей и групп                                 */
/* -------------------------------------------------------------------------- */

static gboolean
is_whitelisted_service_name (const gchar *name)
{
    const gchar *whitelist[] = {
        "www-data", "http", "nginx", "phpmyadmin", "mysql", "redis",
        "ftp", "git", "docker", "wheel", "sudo", "users", "storage", "nobody", "nogroup", NULL
    };
    for (int i = 0; whitelist[i] != NULL; i++)
    {
        if (g_strcmp0 (name, whitelist[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

static GHashTable *
get_valid_shells_set (const gchar *shells_raw)
{
    GHashTable *table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    if (shells_raw && strlen (shells_raw) > 0)
    {
        gchar **lines = g_strsplit (shells_raw, "\n", -1);
        for (int i = 0; lines[i] != NULL; i++)
        {
            gchar *trimmed = g_strstrip (lines[i]);
            if (trimmed[0] != '#' && strlen (trimmed) > 0)
                g_hash_table_add (table, g_strdup (trimmed));
        }
        g_strfreev (lines);
    }
    else
    {
        FILE *fp = fopen ("/etc/shells", "r");
        if (fp)
        {
            char line[256];
            while (fgets (line, sizeof (line), fp))
            {
                gchar *trimmed = g_strstrip (line);
                if (trimmed[0] != '#' && strlen (trimmed) > 0)
                    g_hash_table_add (table, g_strdup (trimmed));
            }
            fclose (fp);
        }
    }

    g_hash_table_add (table, g_strdup ("/bin/bash"));
    g_hash_table_add (table, g_strdup ("/usr/bin/bash"));
    g_hash_table_add (table, g_strdup ("/bin/sh"));
    g_hash_table_add (table, g_strdup ("/usr/bin/sh"));
    g_hash_table_add (table, g_strdup ("/bin/zsh"));
    g_hash_table_add (table, g_strdup ("/usr/bin/zsh"));

    return table;
}

static gchar *
clean_entry_value (const gchar *input_str)
{
    if (!input_str)
        return g_strdup ("");

    gchar *trimmed = g_strstrip (g_strdup (input_str));
    gchar *bracket = strchr (trimmed, ' ');
    if (bracket)
    {
        *bracket = '\0';
    }
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
/* Создание GtkDropDown со встроенным поиском (GtkPropertyExpression)          */
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
/* Переключение между Компактным и Полным списком (без падений)               */
/* -------------------------------------------------------------------------- */

static void
on_show_all_toggled (GtkCheckButton *btn, gpointer user_data)
{
    PermissionsDialogWidgets *w = (PermissionsDialogWidgets *) user_data;
    gboolean show_all = gtk_check_button_get_active (btn);

    if (show_all)
    {
        /* Синхронизируем выбор из compact в full */
        guint u_idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (w->combo_owner_compact));
        const gchar *u_str = gtk_string_list_get_string (w->owners_compact_model, u_idx);
        select_in_string_list (w->combo_owner_full, w->owners_full_model, u_str);

        guint g_idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (w->combo_group_compact));
        const gchar *g_str = gtk_string_list_get_string (w->groups_compact_model, g_idx);
        select_in_string_list (w->combo_group_full, w->groups_full_model, g_str);
    }
    else
    {
        /* Синхронизируем выбор из full в compact */
        guint u_idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (w->combo_owner_full));
        const gchar *u_str = gtk_string_list_get_string (w->owners_full_model, u_idx);
        select_in_string_list (w->combo_owner_compact, w->owners_compact_model, u_str);

        guint g_idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (w->combo_group_full));
        const gchar *g_str = gtk_string_list_get_string (w->groups_full_model, g_idx);
        select_in_string_list (w->combo_group_compact, w->groups_compact_model, g_str);
    }

    gtk_widget_set_visible (w->combo_owner_compact, !show_all);
    gtk_widget_set_visible (w->combo_owner_full, show_all);
    gtk_widget_set_visible (w->combo_group_compact, !show_all);
    gtk_widget_set_visible (w->combo_group_full, show_all);
}

/* -------------------------------------------------------------------------- */
/* Асинхронное выполнение команды изменения прав                              */
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
        log_debug ("[SUCCESS] Права успешно обновлены");
        const gchar *notify_argv[] = {
            "notify-send",
            "-u", "normal",
            "-i", "dialog-information",
            "Права доступа",
            "Права и владелец успешно обновлены",
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
            err_msg = g_strdup ("Операция отменена пользователем или ошибка доступа");

        log_debug ("[ERROR] %s", err_msg);

        const gchar *notify_argv[] = {
            "notify-send",
            "-u", "critical",
            "-i", "dialog-error",
            "Ошибка изменения прав",
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
    gboolean show_all = gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_show_all));

    GtkWidget *active_owner_combo = show_all ? w->combo_owner_full : w->combo_owner_compact;
    GtkStringList *active_u_model = show_all ? w->owners_full_model : w->owners_compact_model;
    guint owner_idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (active_owner_combo));
    const gchar *raw_owner = gtk_string_list_get_string (active_u_model, owner_idx);
    g_autofree gchar *owner_name = clean_entry_value (raw_owner);

    GtkWidget *active_group_combo = show_all ? w->combo_group_full : w->combo_group_compact;
    GtkStringList *active_g_model = show_all ? w->groups_full_model : w->groups_compact_model;
    guint group_idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (active_group_combo));
    const gchar *raw_group = gtk_string_list_get_string (active_g_model, group_idx);
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
on_dialog_destroyed (gpointer data, GObject *where_the_object_was)
{
    PermissionsDialogWidgets *w = (PermissionsDialogWidgets *) data;
    g_active_dialog_window = NULL;

    g_list_free_full (w->target_paths, g_free);
    mount_info_free (w->mount_info);
    g_free (w);
}

/* -------------------------------------------------------------------------- */
/* Асинхронное получение данных (Stat + Passwd + Groups)                      */
/* -------------------------------------------------------------------------- */

static void
populate_models_from_parsed_data (PermissionsDialogWidgets *w,
                                  const gchar *passwd_part,
                                  const gchar *group_part,
                                  const gchar *shells_part,
                                  const gchar *initial_owner,
                                  const gchar *initial_group)
{
    GHashTable *seen_u_c = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *seen_u_f = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *seen_g_c = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *seen_g_f = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    #define ADD_U(model, seen, name, uid) do { \
        if (name && strlen(name) > 0 && !g_hash_table_contains(seen, name)) { \
            g_hash_table_add(seen, g_strdup(name)); \
            g_autofree gchar *entry_str = g_strdup_printf("%s [%s]", name, uid); \
            gtk_string_list_append(model, entry_str); \
        } \
    } while (0)

    #define ADD_G(model, seen, name, gid) do { \
        if (name && strlen(name) > 0 && !g_hash_table_contains(seen, name)) { \
            g_hash_table_add(seen, g_strdup(name)); \
            g_autofree gchar *entry_str = g_strdup_printf("%s [%s]", name, gid); \
            gtk_string_list_append(model, entry_str); \
        } \
    } while (0)

    ADD_U (w->owners_compact_model, seen_u_c, "root", "0");
    ADD_U (w->owners_full_model, seen_u_f, "root", "0");
    ADD_G (w->groups_compact_model, seen_g_c, "root", "0");
    ADD_G (w->groups_full_model, seen_g_f, "root", "0");

    if (initial_owner && strlen (initial_owner) > 0)
    {
        ADD_U (w->owners_compact_model, seen_u_c, initial_owner, "current");
        ADD_U (w->owners_full_model, seen_u_f, initial_owner, "current");
    }
    if (initial_group && strlen (initial_group) > 0)
    {
        ADD_G (w->groups_compact_model, seen_g_c, initial_group, "current");
        ADD_G (w->groups_full_model, seen_g_f, initial_group, "current");
    }

    GHashTable *valid_shells = get_valid_shells_set (shells_part);

    if (passwd_part)
    {
        gchar **lines = g_strsplit (passwd_part, "\n", -1);
        for (int i = 0; lines[i] != NULL; i++)
        {
            gchar *line = lines[i];
            if (strlen (line) == 0) continue;
            gchar **parts = g_strsplit (line, ":", 7);
            if (parts[0] && parts[2] && parts[6])
            {
                const gchar *u_name = parts[0];
                const gchar *u_uid = parts[2];
                const gchar *u_shell = parts[6];

                gboolean has_shell = g_hash_table_contains (valid_shells, u_shell);
                gboolean is_whitelist = is_whitelisted_service_name (u_name);
                gboolean is_host_user = (w->mount_info->ssh_host && g_strcmp0 (u_name, w->mount_info->ssh_host) == 0);

                ADD_U (w->owners_full_model, seen_u_f, u_name, u_uid);
                if (has_shell || is_whitelist || is_host_user)
                {
                    ADD_U (w->owners_compact_model, seen_u_c, u_name, u_uid);
                }
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
            if (parts[0] && parts[2])
            {
                const gchar *g_name = parts[0];
                const gchar *g_gid = parts[2];

                gboolean is_whitelist = is_whitelisted_service_name (g_name);
                gboolean is_user_match = g_hash_table_contains (seen_u_c, g_name);

                ADD_G (w->groups_full_model, seen_g_f, g_name, g_gid);
                if (is_whitelist || is_user_match)
                {
                    ADD_G (w->groups_compact_model, seen_g_c, g_name, g_gid);
                }
            }
            g_strfreev (parts);
        }
        g_strfreev (lines);
    }

    /* Кастомные записи из config.ini */
    TweaksConfig *config = tweaks_config_load ();
    if (config && config->extra_users)
    {
        for (int i = 0; config->extra_users[i] != NULL; i++)
        {
            const gchar *extra = config->extra_users[i];
            if (strlen (extra) > 0)
            {
                ADD_U (w->owners_compact_model, seen_u_c, extra, "custom");
                ADD_U (w->owners_full_model, seen_u_f, extra, "custom");
            }
        }
    }
    if (config && config->extra_groups)
    {
        for (int i = 0; config->extra_groups[i] != NULL; i++)
        {
            const gchar *extra = config->extra_groups[i];
            if (strlen (extra) > 0)
            {
                ADD_G (w->groups_compact_model, seen_g_c, extra, "custom");
                ADD_G (w->groups_full_model, seen_g_f, extra, "custom");
            }
        }
    }
    tweaks_config_free (config);

    #undef ADD_U
    #undef ADD_G
    g_hash_table_destroy (valid_shells);
    g_hash_table_destroy (seen_u_c);
    g_hash_table_destroy (seen_u_f);
    g_hash_table_destroy (seen_g_c);
    g_hash_table_destroy (seen_g_f);

    /* Автовыбор активного пользователя и группы */
    select_in_string_list (w->combo_owner_compact, w->owners_compact_model, initial_owner ? initial_owner : "root");
    select_in_string_list (w->combo_group_compact, w->groups_compact_model, initial_group ? initial_group : "root");

    /* Останавливаем спиннер и показываем форму */
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
    gchar *shells_part = NULL;

    if (!err && g_subprocess_get_successful (proc) && stdout_buf)
    {
        /* Парсим секции */
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

        if (rest)
        {
            gchar **g_split = g_strsplit (rest, "===GROUPS===\n", 2);
            passwd_part = g_split[0];
            if (g_split[1])
            {
                gchar **s_split = g_strsplit (g_split[1], "===SHELLS===\n", 2);
                group_part = s_split[0];
                shells_part = s_split[1];
            }
        }

        apply_mode_to_checkboxes (w, initial_mode);
        populate_models_from_parsed_data (w, passwd_part, group_part, shells_part, initial_owner, initial_group);
        g_strfreev (sections);
    }
    else
    {
        log_debug ("[REMOTE LOAD FAILED] %s", err ? err->message : "unknown error");
        populate_models_from_parsed_data (w, NULL, NULL, NULL, "root", "root");
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
            "stat -c '%%u:%%g:%%a:%%U:%%G' %s 2>/dev/null; echo '===PASSWD==='; getent passwd; echo '===GROUPS==='; getent group; echo '===SHELLS==='; cat /etc/shells 2>/dev/null",
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

    /* Локальная загрузка */
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

    populate_models_from_parsed_data (w, passwd_out, group_out, NULL, initial_owner, initial_group);
}

/* -------------------------------------------------------------------------- */
/* Построение окна интерфейса                                                 */
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

    w->owners_compact_model = gtk_string_list_new (NULL);
    w->owners_full_model    = gtk_string_list_new (NULL);
    w->groups_compact_model = gtk_string_list_new (NULL);
    w->groups_full_model    = gtk_string_list_new (NULL);

    w->window = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (w->window), "Permissions");
    gtk_window_set_resizable (GTK_WINDOW (w->window), FALSE);
    gtk_window_set_default_size (GTK_WINDOW (w->window), 450, -1);

    g_object_weak_ref (G_OBJECT (w->window), on_dialog_destroyed, w);

    /* Стек: Загрузка vs Форма */
    w->stack_pages = gtk_stack_new ();
    gtk_stack_set_transition_type (GTK_STACK (w->stack_pages), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_window_set_child (GTK_WINDOW (w->window), w->stack_pages);

    /* --- СТРАНИЦА 1: Загрузка (Spinner) --- */
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
        ? "Загрузка пользователей и прав с сервера..."
        : "Чтение прав доступа..."
    );
    gtk_widget_add_css_class (w->lbl_loading, "dim-label");
    gtk_box_append (GTK_BOX (loading_box), w->lbl_loading);

    gtk_stack_add_named (GTK_STACK (w->stack_pages), loading_box, "loading");

    /* --- СТРАНИЦА 2: Основная форма --- */
    GtkWidget *form_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start (form_box, 16);
    gtk_widget_set_margin_end (form_box, 16);
    gtk_widget_set_margin_top (form_box, 16);
    gtk_widget_set_margin_bottom (form_box, 16);
    gtk_stack_add_named (GTK_STACK (w->stack_pages), form_box, "form");

    /* 0. Целевой путь */
    g_autofree gchar *target_label_text = NULL;
    guint target_count = g_list_length (w->target_paths);
    if (target_count == 1 && first_path)
    {
        if (w->mount_info->mode == FS_MODE_SSHFS && w->mount_info->ssh_host)
        {
            g_autofree gchar *rem = translate_to_remote_path (first_path, w->mount_info);
            target_label_text = g_strdup_printf ("Target (SSH: %s): %s", w->mount_info->ssh_host, rem);
        }
        else
        {
            target_label_text = g_strdup_printf ("Target: %s", first_path);
        }
    }
    else
    {
        target_label_text = g_strdup_printf ("Selected: %u items", target_count);
    }

    w->lbl_target_path = gtk_label_new (target_label_text);
    gtk_widget_set_halign (w->lbl_target_path, GTK_ALIGN_START);
    gtk_label_set_ellipsize (GTK_LABEL (w->lbl_target_path), PANGO_ELLIPSIZE_START);
    gtk_widget_add_css_class (w->lbl_target_path, "dim-label");
    gtk_box_append (GTK_BOX (form_box), w->lbl_target_path);

    /* 1. Блок Owner / Group (Два независимых выпадающих списка с поиском) */
    GtkWidget *grid_top = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (grid_top), 12);
    gtk_grid_set_row_spacing (GTK_GRID (grid_top), 8);

    GtkWidget *lbl_owner = gtk_label_new ("Owner:");
    gtk_widget_set_halign (lbl_owner, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grid_top), lbl_owner, 0, 0, 1, 1);

    w->combo_owner_compact = create_searchable_dropdown (w->owners_compact_model);
    w->combo_owner_full    = create_searchable_dropdown (w->owners_full_model);
    gtk_widget_set_visible (w->combo_owner_full, FALSE);
    gtk_grid_attach (GTK_GRID (grid_top), w->combo_owner_compact, 1, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_top), w->combo_owner_full, 1, 0, 1, 1);

    GtkWidget *lbl_group = gtk_label_new ("Group:");
    gtk_widget_set_halign (lbl_group, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grid_top), lbl_group, 0, 1, 1, 1);

    w->combo_group_compact = create_searchable_dropdown (w->groups_compact_model);
    w->combo_group_full    = create_searchable_dropdown (w->groups_full_model);
    gtk_widget_set_visible (w->combo_group_full, FALSE);
    gtk_grid_attach (GTK_GRID (grid_top), w->combo_group_compact, 1, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_top), w->combo_group_full, 1, 1, 1, 1);

    gtk_box_append (GTK_BOX (form_box), grid_top);

    /* Чекбокс «Показать всех» */
    w->chk_show_all = gtk_check_button_new_with_label ("Показать всех пользователей и группы");
    g_signal_connect (w->chk_show_all, "toggled", G_CALLBACK (on_show_all_toggled), w);
    gtk_box_append (GTK_BOX (form_box), w->chk_show_all);

    gtk_box_append (GTK_BOX (form_box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    /* 2. Блок Permissions */
    GtkWidget *grid_perm = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (grid_perm), 12);
    gtk_grid_set_row_spacing (GTK_GRID (grid_perm), 6);

    GtkWidget *lbl_perm_title = gtk_label_new ("Permissions:");
    gtk_widget_set_halign (lbl_perm_title, GTK_ALIGN_START);
    gtk_widget_set_valign (lbl_perm_title, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grid_perm), lbl_perm_title, 0, 0, 1, 5);

    GtkWidget *lbl_u = gtk_label_new_with_mnemonic ("_Owner");
    GtkWidget *lbl_g = gtk_label_new_with_mnemonic ("_Group");
    GtkWidget *lbl_o = gtk_label_new_with_mnemonic ("Ot_hers");
    GtkWidget *lbl_octal = gtk_label_new_with_mnemonic ("O_ctal:");
    gtk_widget_set_halign (lbl_u, GTK_ALIGN_START);
    gtk_widget_set_halign (lbl_g, GTK_ALIGN_START);
    gtk_widget_set_halign (lbl_o, GTK_ALIGN_START);
    gtk_widget_set_halign (lbl_octal, GTK_ALIGN_START);

    gtk_grid_attach (GTK_GRID (grid_perm), lbl_u, 1, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), lbl_g, 1, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), lbl_o, 1, 2, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), lbl_octal, 1, 3, 1, 1);

    /* Чекбоксы Owner */
    w->chk_u_r    = gtk_check_button_new_with_label ("R");
    w->chk_u_w    = gtk_check_button_new_with_label ("W");
    w->chk_u_x    = gtk_check_button_new_with_label ("X");
    w->chk_u_suid = gtk_check_button_new_with_label ("Set UID");
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_u_r,    2, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_u_w,    3, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_u_x,    4, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_u_suid, 5, 0, 1, 1);

    /* Чекбоксы Group */
    w->chk_g_r    = gtk_check_button_new_with_label ("R");
    w->chk_g_w    = gtk_check_button_new_with_label ("W");
    w->chk_g_x    = gtk_check_button_new_with_label ("X");
    w->chk_g_sgid = gtk_check_button_new_with_label ("Set GID");
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_g_r,    2, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_g_w,    3, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_g_x,    4, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_g_sgid, 5, 1, 1, 1);

    /* Чекбоксы Others */
    w->chk_o_r      = gtk_check_button_new_with_label ("R");
    w->chk_o_w      = gtk_check_button_new_with_label ("W");
    w->chk_o_x      = gtk_check_button_new_with_label ("X");
    w->chk_o_sticky = gtk_check_button_new_with_label ("Sticky bit");
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_o_r,      2, 2, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_o_w,      3, 2, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_o_x,      4, 2, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_o_sticky, 5, 2, 1, 1);

    /* Octal */
    w->entry_octal = gtk_entry_new ();
    gtk_entry_set_max_length (GTK_ENTRY (w->entry_octal), 5);
    gtk_grid_attach (GTK_GRID (grid_perm), w->entry_octal, 2, 3, 2, 1);

    /* Add X to directories */
    w->chk_add_x = gtk_check_button_new_with_mnemonic ("Add _X to directories");
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_add_x, 2, 4, 4, 1);

    gtk_box_append (GTK_BOX (form_box), grid_perm);
    gtk_box_append (GTK_BOX (form_box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    /* 3. Чекбокс рекурсивности */
    w->chk_recursive = gtk_check_button_new_with_mnemonic ("Set owner, group and permissions _recursively");
    gtk_box_append (GTK_BOX (form_box), w->chk_recursive);

    /* 4. Кнопки Отмена / Применить */
    GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign (btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top (btn_box, 8);

    GtkWidget *btn_cancel = gtk_button_new_with_label ("Отмена");
    GtkWidget *btn_apply  = gtk_button_new_with_label ("Применить");
    gtk_widget_add_css_class (btn_apply, "suggested-action");

    g_signal_connect_swapped (btn_cancel, "clicked", G_CALLBACK (gtk_window_destroy), w->window);
    g_signal_connect (btn_apply, "clicked", G_CALLBACK (on_apply_clicked), w);

    gtk_box_append (GTK_BOX (btn_box), btn_cancel);
    gtk_box_append (GTK_BOX (btn_box), btn_apply);
    gtk_box_append (GTK_BOX (form_box), btn_box);

    /* Подключаем сигналы пересчёта */
    GtkWidget *all_checks[] = {
        w->chk_u_r, w->chk_u_w, w->chk_u_x, w->chk_u_suid,
        w->chk_g_r, w->chk_g_w, w->chk_g_x, w->chk_g_sgid,
        w->chk_o_r, w->chk_o_w, w->chk_o_x, w->chk_o_sticky,
        NULL
    };

    for (int i = 0; all_checks[i] != NULL; i++)
        g_signal_connect (all_checks[i], "toggled", G_CALLBACK (on_perm_checkbox_toggled), w);

    g_signal_connect (w->entry_octal, "changed", G_CALLBACK (on_octal_entry_changed), w);

    /* Запускаем фоновую загрузку данных */
    start_async_data_load (w, first_path);

    return w->window;
}

/* -------------------------------------------------------------------------- */
/* Контекстное меню для выбранных файлов/папок                                */
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
        "Права...",
        "Изменить владельца, группу и права доступа (chmod/chown)",
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
/* Контекстное меню пустого пространства (Background Menu)                    */
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
        "Права...",
        "Изменить владельца, группу и права доступа текущей папки (chmod/chown)",
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
/* Инициализация модуля расширения Nautilus                                   */
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