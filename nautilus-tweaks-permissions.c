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
/* Структура данных виджетов                                                  */
/* -------------------------------------------------------------------------- */

typedef struct {
    GtkWidget *window;
    GList     *target_paths;

    /* Переключаемый блок Owner */
    GtkWidget      *stack_owner;
    GtkWidget      *combo_owner;
    GtkWidget      *entry_owner;
    GtkWidget      *toggle_owner;
    GtkStringList  *owners_model;

    /* Переключаемый блок Group */
    GtkWidget      *stack_group;
    GtkWidget      *combo_group;
    GtkWidget      *entry_group;
    GtkWidget      *toggle_group;
    GtkStringList  *groups_model;

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
is_whitelisted_system_user (const gchar *name)
{
    const gchar *whitelist[] = {
        "http", "www-data", "nginx", "ftp", "git", "phpmyadmin", "nobody", NULL
    };
    for (int i = 0; whitelist[i] != NULL; i++)
    {
        if (g_strcmp0 (name, whitelist[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

static gboolean
is_whitelisted_system_group (const gchar *name)
{
    const gchar *whitelist[] = {
        "wheel", "sudo", "users", "storage", "http", "www-data", 
        "nginx", "docker", "ftp", "phpmyadmin", "nobody", NULL
    };
    for (int i = 0; whitelist[i] != NULL; i++)
    {
        if (g_strcmp0 (name, whitelist[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

static GtkStringList *
build_users_string_list (TweaksConfig *config)
{
    GtkStringList *model = gtk_string_list_new (NULL);
    GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    #define ADD_USER(name, uid_str) do { \
        if (name && strlen(name) > 0 && !g_hash_table_contains (seen, name)) { \
            g_hash_table_add (seen, g_strdup (name)); \
            g_autofree gchar *entry_str = g_strdup_printf ("%s [%s]", name, uid_str); \
            gtk_string_list_append (model, entry_str); \
        } \
    } while (0)

    ADD_USER ("root", "0");

    const gchar *cur_user = g_get_user_name ();
    g_autofree gchar *cur_uid_str = g_strdup_printf ("%u", (guint) getuid ());
    ADD_USER (cur_user, cur_uid_str);

    struct passwd *pw;
    setpwent ();
    while ((pw = getpwent ()) != NULL)
    {
        gboolean is_real_user = (pw->pw_uid >= 1000 && pw->pw_uid != 65534);
        gboolean is_server_acc = is_whitelisted_system_user (pw->pw_name);

        if (is_real_user || is_server_acc)
        {
            g_autofree gchar *u_str = g_strdup_printf ("%u", (guint) pw->pw_uid);
            ADD_USER (pw->pw_name, u_str);
        }
    }
    endpwent ();

    if (config && config->extra_users)
    {
        for (int i = 0; config->extra_users[i] != NULL; i++)
        {
            const gchar *extra = config->extra_users[i];
            if (strlen (extra) > 0)
            {
                struct passwd *p = getpwnam (extra);
                if (p)
                {
                    g_autofree gchar *u_str = g_strdup_printf ("%u", (guint) p->pw_uid);
                    ADD_USER (p->pw_name, u_str);
                }
                else
                {
                    ADD_USER (extra, "custom");
                }
            }
        }
    }

    #undef ADD_USER
    g_hash_table_destroy (seen);
    return model;
}

static GtkStringList *
build_groups_string_list (TweaksConfig *config)
{
    GtkStringList *model = gtk_string_list_new (NULL);
    GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

    #define ADD_GROUP(name, gid_str) do { \
        if (name && strlen(name) > 0 && !g_hash_table_contains (seen, name)) { \
            g_hash_table_add (seen, g_strdup (name)); \
            g_autofree gchar *entry_str = g_strdup_printf ("%s [%s]", name, gid_str); \
            gtk_string_list_append (model, entry_str); \
        } \
    } while (0)

    ADD_GROUP ("root", "0");

    gid_t cur_gid = getgid ();
    struct group *cur_gr = getgrgid (cur_gid);
    if (cur_gr)
    {
        g_autofree gchar *g_str = g_strdup_printf ("%u", (guint) cur_gid);
        ADD_GROUP (cur_gr->gr_name, g_str);
    }

    int ngroups = 0;
    getgrouplist (g_get_user_name (), cur_gid, NULL, &ngroups);
    if (ngroups > 0)
    {
        gid_t *groups = g_new0 (gid_t, ngroups);
        if (getgrouplist (g_get_user_name (), cur_gid, groups, &ngroups) != -1)
        {
            for (int i = 0; i < ngroups; i++)
            {
                struct group *g = getgrgid (groups[i]);
                if (g)
                {
                    g_autofree gchar *g_str = g_strdup_printf ("%u", (guint) g->gr_gid);
                    ADD_GROUP (g->gr_name, g_str);
                }
            }
        }
        g_free (groups);
    }

    struct group *gr;
    setgrent ();
    while ((gr = getgrent ()) != NULL)
    {
        gboolean is_user_group = (gr->gr_gid >= 1000 && gr->gr_gid != 65534);
        gboolean is_server_group = is_whitelisted_system_group (gr->gr_name);

        if (is_user_group || is_server_group)
        {
            g_autofree gchar *g_str = g_strdup_printf ("%u", (guint) gr->gr_gid);
            ADD_GROUP (gr->gr_name, g_str);
        }
    }
    endgrent ();

    if (config && config->extra_groups)
    {
        for (int i = 0; config->extra_groups[i] != NULL; i++)
        {
            const gchar *extra = config->extra_groups[i];
            if (strlen (extra) > 0)
            {
                struct group *g = getgrnam (extra);
                if (g)
                {
                    g_autofree gchar *g_str = g_strdup_printf ("%u", (guint) g->gr_gid);
                    ADD_GROUP (g->gr_name, g_str);
                }
                else
                {
                    ADD_GROUP (extra, "custom");
                }
            }
        }
    }

    #undef ADD_GROUP
    g_hash_table_destroy (seen);
    return model;
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

/* -------------------------------------------------------------------------- */
/* Обработчики переключения режима (Список <-> Ручной ввод)                   */
/* -------------------------------------------------------------------------- */

static void
on_owner_toggle_toggled (GtkToggleButton *btn, gpointer user_data)
{
    PermissionsDialogWidgets *w = (PermissionsDialogWidgets *) user_data;
    gboolean is_manual = gtk_toggle_button_get_active (btn);

    if (is_manual)
    {
        /* При переключении на ручной ввод подставляем текущий выбранный логин */
        guint idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (w->combo_owner));
        const gchar *item_str = gtk_string_list_get_string (w->owners_model, idx);
        g_autofree gchar *name = clean_entry_value (item_str);
        gtk_editable_set_text (GTK_EDITABLE (w->entry_owner), name);

        gtk_stack_set_visible_child_name (GTK_STACK (w->stack_owner), "entry");
        gtk_widget_set_tooltip_text (GTK_WIDGET (btn), "Выбрать из списка");
    }
    else
    {
        gtk_stack_set_visible_child_name (GTK_STACK (w->stack_owner), "dropdown");
        gtk_widget_set_tooltip_text (GTK_WIDGET (btn), "Ввести вручную");
    }
}

static void
on_group_toggle_toggled (GtkToggleButton *btn, gpointer user_data)
{
    PermissionsDialogWidgets *w = (PermissionsDialogWidgets *) user_data;
    gboolean is_manual = gtk_toggle_button_get_active (btn);

    if (is_manual)
    {
        guint idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (w->combo_group));
        const gchar *item_str = gtk_string_list_get_string (w->groups_model, idx);
        g_autofree gchar *name = clean_entry_value (item_str);
        gtk_editable_set_text (GTK_EDITABLE (w->entry_group), name);

        gtk_stack_set_visible_child_name (GTK_STACK (w->stack_group), "entry");
        gtk_widget_set_tooltip_text (GTK_WIDGET (btn), "Выбрать из списка");
    }
    else
    {
        gtk_stack_set_visible_child_name (GTK_STACK (w->stack_group), "dropdown");
        gtk_widget_set_tooltip_text (GTK_WIDGET (btn), "Ввести вручную");
    }
}

/* -------------------------------------------------------------------------- */
/* Асинхронное выполнение команды                                             */
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

    /* Получение Owner (в зависимости от состояния кнопки-карандаша) */
    g_autofree gchar *owner_name = NULL;
    if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w->toggle_owner)))
    {
        owner_name = clean_entry_value (gtk_editable_get_text (GTK_EDITABLE (w->entry_owner)));
    }
    else
    {
        guint idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (w->combo_owner));
        const gchar *raw_owner = gtk_string_list_get_string (w->owners_model, idx);
        owner_name = clean_entry_value (raw_owner);
    }

    /* Получение Group (в зависимости от состояния кнопки-карандаша) */
    g_autofree gchar *group_name = NULL;
    if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (w->toggle_group)))
    {
        group_name = clean_entry_value (gtk_editable_get_text (GTK_EDITABLE (w->entry_group)));
    }
    else
    {
        guint idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (w->combo_group));
        const gchar *raw_group = gtk_string_list_get_string (w->groups_model, idx);
        group_name = clean_entry_value (raw_group);
    }

    long file_mode = base_mode;
    long dir_mode  = base_mode;

    if (add_x)
    {
        if (file_mode & 0400) dir_mode |= 0100;
        if (file_mode & 0040) dir_mode |= 0010;
        if (file_mode & 0004) dir_mode |= 0001;
    }

    GString *cmd = g_string_new (NULL);
    gboolean first = TRUE;

    for (GList *l = w->target_paths; l != NULL; l = l->next)
    {
        const gchar *path = (const gchar *) l->data;
        g_autofree gchar *quoted_path = g_shell_quote (path);

        if (!first)
            g_string_append (cmd, " && ");
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
                g_string_append_printf (cmd, "chown -R %s %s && ", chown_target, quoted_path);
            else
                g_string_append_printf (cmd, "chown %s %s && ", chown_target, quoted_path);
        }

        /* 2. chmod */
        if (recursive)
        {
            if (add_x)
            {
                g_string_append_printf (cmd, "find %s -type d -exec chmod %04lo {} + && ", quoted_path, dir_mode);
                g_string_append_printf (cmd, "find %s -type f -exec chmod %04lo {} +", quoted_path, file_mode);
            }
            else
            {
                g_string_append_printf (cmd, "chmod -R %04lo %s", base_mode, quoted_path);
            }
        }
        else
        {
            if (g_file_test (path, G_FILE_TEST_IS_DIR) && add_x)
                g_string_append_printf (cmd, "chmod %04lo %s", dir_mode, quoted_path);
            else
                g_string_append_printf (cmd, "chmod %04lo %s", file_mode, quoted_path);
        }
    }

    log_debug ("[APPLY] Mode(Owner=%s, Group=%s), Command: pkexec sh -c \"%s\"",
               owner_name, group_name, cmd->str);

    g_autoptr (GError) spawn_err = NULL;
    g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE
    );

    GSubprocess *proc = g_subprocess_launcher_spawn (
        launcher,
        &spawn_err,
        "pkexec", "sh", "-c", cmd->str,
        NULL
    );

    g_string_free (cmd, TRUE);

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
    g_free (w);
}

/* -------------------------------------------------------------------------- */
/* Построение окна интерфейса                                                 */
/* -------------------------------------------------------------------------- */

static GtkWidget *
create_permissions_window (GList *files)
{
    PermissionsDialogWidgets *w = g_new0 (PermissionsDialogWidgets, 1);
    w->updating_from_code = FALSE;

    TweaksConfig *config = tweaks_config_load ();

    uid_t initial_uid = 0;
    gid_t initial_gid = 0;
    mode_t initial_mode = 0755;
    gboolean got_stat = FALSE;
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

                if (!got_stat)
                {
                    struct stat st;
                    if (stat (path, &st) == 0)
                    {
                        initial_uid = st.st_uid;
                        initial_gid = st.st_gid;
                        initial_mode = st.st_mode & 07777;
                        got_stat = TRUE;
                        log_debug ("[STAT SUCCESS] '%s' -> UID=%u, GID=%u, Mode=%04o",
                                   path, (guint) st.st_uid, (guint) st.st_gid, (guint) initial_mode);
                    }
                }
                w->target_paths = g_list_append (w->target_paths, path);
            }
        }
    }

    w->window = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (w->window), "Permissions");
    gtk_window_set_resizable (GTK_WINDOW (w->window), FALSE);
    gtk_window_set_default_size (GTK_WINDOW (w->window), 450, -1);

    g_object_weak_ref (G_OBJECT (w->window), on_dialog_destroyed, w);

    GtkWidget *main_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start (main_box, 16);
    gtk_widget_set_margin_end (main_box, 16);
    gtk_widget_set_margin_top (main_box, 16);
    gtk_widget_set_margin_bottom (main_box, 16);
    gtk_window_set_child (GTK_WINDOW (w->window), main_box);

    /* 0. Путь к целевому файлу/папке */
    g_autofree gchar *target_label_text = NULL;
    guint target_count = g_list_length (w->target_paths);
    if (target_count == 1 && first_path)
    {
        target_label_text = g_strdup_printf ("Target: %s", first_path);
    }
    else
    {
        target_label_text = g_strdup_printf ("Selected: %u items", target_count);
    }

    GtkWidget *lbl_target_path = gtk_label_new (target_label_text);
    gtk_widget_set_halign (lbl_target_path, GTK_ALIGN_START);
    gtk_label_set_ellipsize (GTK_LABEL (lbl_target_path), PANGO_ELLIPSIZE_START);
    gtk_widget_add_css_class (lbl_target_path, "dim-label");
    gtk_box_append (GTK_BOX (main_box), lbl_target_path);

    /* 1. Блок Owner / Group (GtkStack с GtkDropDown и GtkEntry + Кнопка Карандаша) */
    GtkWidget *grid_top = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (grid_top), 8);
    gtk_grid_set_row_spacing (GTK_GRID (grid_top), 8);

    /* --- Owner --- */
    GtkWidget *lbl_owner = gtk_label_new ("Owner:");
    gtk_widget_set_halign (lbl_owner, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grid_top), lbl_owner, 0, 0, 1, 1);

    w->owners_model = build_users_string_list (config);
    w->combo_owner  = gtk_drop_down_new (G_LIST_MODEL (w->owners_model), NULL);
    w->entry_owner  = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (w->entry_owner), "Логин или UID пользователя");

    w->stack_owner  = gtk_stack_new ();
    gtk_stack_set_transition_type (GTK_STACK (w->stack_owner), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_hexpand (w->stack_owner, TRUE);
    gtk_stack_add_named (GTK_STACK (w->stack_owner), w->combo_owner, "dropdown");
    gtk_stack_add_named (GTK_STACK (w->stack_owner), w->entry_owner, "entry");
    gtk_grid_attach (GTK_GRID (grid_top), w->stack_owner, 1, 0, 1, 1);

    w->toggle_owner = gtk_toggle_button_new ();
    gtk_button_set_icon_name (GTK_BUTTON (w->toggle_owner), "document-edit-symbolic");
    gtk_widget_set_tooltip_text (w->toggle_owner, "Ввести вручную");
    g_signal_connect (w->toggle_owner, "toggled", G_CALLBACK (on_owner_toggle_toggled), w);
    gtk_grid_attach (GTK_GRID (grid_top), w->toggle_owner, 2, 0, 1, 1);

    /* --- Group --- */
    GtkWidget *lbl_group = gtk_label_new ("Group:");
    gtk_widget_set_halign (lbl_group, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grid_top), lbl_group, 0, 1, 1, 1);

    w->groups_model = build_groups_string_list (config);
    w->combo_group  = gtk_drop_down_new (G_LIST_MODEL (w->groups_model), NULL);
    w->entry_group  = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (w->entry_group), "Имя группы или GID");

    w->stack_group  = gtk_stack_new ();
    gtk_stack_set_transition_type (GTK_STACK (w->stack_group), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_hexpand (w->stack_group, TRUE);
    gtk_stack_add_named (GTK_STACK (w->stack_group), w->combo_group, "dropdown");
    gtk_stack_add_named (GTK_STACK (w->stack_group), w->entry_group, "entry");
    gtk_grid_attach (GTK_GRID (grid_top), w->stack_group, 1, 1, 1, 1);

    w->toggle_group = gtk_toggle_button_new ();
    gtk_button_set_icon_name (GTK_BUTTON (w->toggle_group), "document-edit-symbolic");
    gtk_widget_set_tooltip_text (w->toggle_group, "Ввести вручную");
    g_signal_connect (w->toggle_group, "toggled", G_CALLBACK (on_group_toggle_toggled), w);
    gtk_grid_attach (GTK_GRID (grid_top), w->toggle_group, 2, 1, 1, 1);

    /* Автовыбор текущих Owner / Group */
    if (got_stat)
    {
        struct passwd *pw = getpwuid (initial_uid);
        if (pw)
        {
            guint n_items = g_list_model_get_n_items (G_LIST_MODEL (w->owners_model));
            for (guint i = 0; i < n_items; i++)
            {
                const gchar *item_str = gtk_string_list_get_string (w->owners_model, i);
                g_autofree gchar *name = clean_entry_value (item_str);
                if (g_strcmp0 (name, pw->pw_name) == 0)
                {
                    gtk_drop_down_set_selected (GTK_DROP_DOWN (w->combo_owner), i);
                    break;
                }
            }
        }

        struct group *gr = getgrgid (initial_gid);
        if (gr)
        {
            guint n_items = g_list_model_get_n_items (G_LIST_MODEL (w->groups_model));
            for (guint i = 0; i < n_items; i++)
            {
                const gchar *item_str = gtk_string_list_get_string (w->groups_model, i);
                g_autofree gchar *name = clean_entry_value (item_str);
                if (g_strcmp0 (name, gr->gr_name) == 0)
                {
                    gtk_drop_down_set_selected (GTK_DROP_DOWN (w->combo_group), i);
                    break;
                }
            }
        }
    }

    gtk_box_append (GTK_BOX (main_box), grid_top);
    gtk_box_append (GTK_BOX (main_box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    /* 2. Блок Permissions */
    GtkWidget *grid_perm = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (grid_perm), 12);
    gtk_grid_set_row_spacing (GTK_GRID (grid_perm), 6);

    GtkWidget *lbl_perm_title = gtk_label_new ("Permissions:");
    gtk_widget_set_halign (lbl_perm_title, GTK_ALIGN_START);
    gtk_widget_set_valign (lbl_perm_title, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grid_perm), lbl_perm_title, 0, 0, 1, 5);

    /* Метки строк */
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

    /* Поле Octal */
    w->entry_octal = gtk_entry_new ();
    gtk_entry_set_max_length (GTK_ENTRY (w->entry_octal), 5);
    gtk_grid_attach (GTK_GRID (grid_perm), w->entry_octal, 2, 3, 2, 1);

    /* Add X to directories */
    w->chk_add_x = gtk_check_button_new_with_mnemonic ("Add _X to directories");
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_add_x, 2, 4, 4, 1);

    gtk_box_append (GTK_BOX (main_box), grid_perm);
    gtk_box_append (GTK_BOX (main_box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    /* 3. Чекбокс рекурсивности */
    w->chk_recursive = gtk_check_button_new_with_mnemonic ("Set owner, group and permissions _recursively");
    gtk_box_append (GTK_BOX (main_box), w->chk_recursive);

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
    gtk_box_append (GTK_BOX (main_box), btn_box);

    /* Выставляем считанные права */
    apply_mode_to_checkboxes (w, initial_mode);

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

    tweaks_config_free (config);
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