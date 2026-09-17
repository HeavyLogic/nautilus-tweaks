#include <nautilus-extension.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "tweaks-config.h"

/* Счётчик для генерации уникальных ID пунктов меню */
static guint g_mount_action_counter = 0;

/* Указатель на активное окно диалога выбора (Single Instance) */
static GtkWidget *g_active_mount_window = NULL;

/* -------------------------------------------------------------------------- */
/* Структуры данных                                                           */
/* -------------------------------------------------------------------------- */

typedef struct {
    gchar    *name;             /* Имя сервера/хоста */
    gchar    *type_label;       /* Метка типа: "SFTP", "FTP", "WEBDAV", "S3" и т.д. */
    gchar    *remote_path;      /* Кастомная стартовая папка (если задана) */
    gboolean  is_rclone;        /* TRUE для rclone, FALSE для sshfs */
} RemoteServer;

typedef struct {
    gchar *target_path;
    gchar *server_name;
    gchar *cmd_line;
} MountFinishData;

typedef struct {
    GtkWidget    *window;
    GtkWidget    *btn_connect;
    gchar        *target_path;
    GList        *servers;
    RemoteServer *selected_server;
} MountDialogWidgets;

typedef struct {
    RemoteServer *server;
    gchar        *target_path;
} SshCheckData;

typedef struct {
    RemoteServer *server;
    gchar        *target_path;
    GtkWidget    *window;
    GtkWidget    *entry_password;
} SshPasswordDialog;

/* -------------------------------------------------------------------------- */
/* Объявление структуры GObject-плагина                                       */
/* -------------------------------------------------------------------------- */

typedef struct _NautilusTweaksMount {
    GObject parent_instance;
} NautilusTweaksMount;

typedef struct _NautilusTweaksMountClass {
    GObjectClass parent_class;
} NautilusTweaksMountClass;

static GType nautilus_tweaks_mount_get_type (void);
static void nautilus_tweaks_mount_menu_provider_iface_init (NautilusMenuProviderInterface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (NautilusTweaksMount, nautilus_tweaks_mount, G_TYPE_OBJECT, 0,
    G_IMPLEMENT_INTERFACE_DYNAMIC (NAUTILUS_TYPE_MENU_PROVIDER,
                                   nautilus_tweaks_mount_menu_provider_iface_init))

static void nautilus_tweaks_mount_class_init (NautilusTweaksMountClass *klass) {}
static void nautilus_tweaks_mount_init (NautilusTweaksMount *self) {}
static void nautilus_tweaks_mount_class_finalize (NautilusTweaksMountClass *klass) {}

/* -------------------------------------------------------------------------- */
/* Логирование в ~/.config/nautilus-tweaks/debug.log                          */
/* -------------------------------------------------------------------------- */

static void
tweaks_log (const gchar *format, ...)
{
    const gchar *config_dir = g_get_user_config_dir ();
    g_autofree gchar *log_dir = g_build_filename (config_dir, "nautilus-tweaks", NULL);
    g_mkdir_with_parents (log_dir, 0755);

    g_autofree gchar *log_path = g_build_filename (log_dir, "debug.log", NULL);
    FILE *fp = fopen (log_path, "a");
    if (!fp)
        return;

    g_autoptr (GDateTime) now = g_date_time_new_now_local ();
    g_autofree gchar *time_str = now ? g_date_time_format (now, "%Y-%m-%d %H:%M:%S") : g_strdup ("---");

    va_list args;
    va_start (args, format);
    g_autofree gchar *msg = g_strdup_vprintf (format, args);
    va_end (args);

    fprintf (fp, "[%s] %s\n", time_str, msg);
    fflush (fp);
    fclose (fp);
}

/* -------------------------------------------------------------------------- */
/* Вспомогательные функции работы с памятью и окнами                          */
/* -------------------------------------------------------------------------- */

static void
remote_server_free (RemoteServer *s)
{
    if (!s)
        return;
    g_free (s->name);
    g_free (s->type_label);
    g_free (s->remote_path);
    g_free (s);
}

static RemoteServer *
remote_server_copy (const RemoteServer *s)
{
    if (!s)
        return NULL;
    RemoteServer *copy = g_new0 (RemoteServer, 1);
    copy->name        = g_strdup (s->name);
    copy->type_label  = g_strdup (s->type_label);
    copy->remote_path = g_strdup (s->remote_path);
    copy->is_rclone   = s->is_rclone;
    return copy;
}

static GtkWindow *
get_nautilus_active_window (void)
{
    GApplication *app = g_application_get_default ();
    if (app && GTK_IS_APPLICATION (app))
    {
        GList *windows = gtk_application_get_windows (GTK_APPLICATION (app));
        for (GList *w = windows; w != NULL; w = w->next)
        {
            if (GTK_IS_WINDOW (w->data) && gtk_widget_is_visible (GTK_WIDGET (w->data)))
            {
                return GTK_WINDOW (w->data);
            }
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Вспомогательные функции: Проверка монтирования                             */
/* -------------------------------------------------------------------------- */

static gboolean
is_path_mounted (const gchar *path)
{
    FILE *fp = fopen ("/proc/mounts", "r");
    if (!fp)
        return FALSE;

    char line[2048];
    gboolean mounted = FALSE;

    while (fgets (line, sizeof (line), fp))
    {
        char dev[512], mnt[1024];
        if (sscanf (line, "%511s %1023s", dev, mnt) == 2)
        {
            if (g_strcmp0 (mnt, path) == 0)
            {
                mounted = TRUE;
                break;
            }
        }
    }
    fclose (fp);
    return mounted;
}

static gboolean
is_inside_active_mount (const gchar *path)
{
    FILE *fp = fopen ("/proc/mounts", "r");
    if (!fp)
        return FALSE;

    char line[2048];
    gboolean inside = FALSE;

    while (fgets (line, sizeof (line), fp))
    {
        char dev[512], mnt[1024];
        if (sscanf (line, "%511s %1023s", dev, mnt) == 2)
        {
            if (g_strcmp0 (mnt, "/") == 0)
                continue;

            gsize mnt_len = strlen (mnt);
            if (g_str_has_prefix (path, mnt) && path[mnt_len] == '/')
            {
                inside = TRUE;
                break;
            }
        }
    }
    fclose (fp);
    return inside;
}

static gboolean
has_active_submounts (const gchar *path)
{
    FILE *fp = fopen ("/proc/mounts", "r");
    if (!fp)
        return FALSE;

    char line[2048];
    gboolean has_sub = FALSE;
    gsize path_len = strlen (path);

    while (fgets (line, sizeof (line), fp))
    {
        char dev[512], mnt[1024];
        if (sscanf (line, "%511s %1023s", dev, mnt) == 2)
        {
            if (g_str_has_prefix (mnt, path) && mnt[path_len] == '/')
            {
                has_sub = TRUE;
                break;
            }
        }
    }
    fclose (fp);
    return has_sub;
}

static gboolean
is_path_in_remote_dirs (const gchar *path, TweaksConfig *config)
{
    if (!config->remote_dirs || config->remote_dirs[0] == NULL)
        return FALSE;

    for (int i = 0; config->remote_dirs[i] != NULL; i++)
    {
        const gchar *allowed_dir = config->remote_dirs[i];
        if (strlen (allowed_dir) == 0)
            continue;

        if (g_strcmp0 (path, allowed_dir) == 0)
            return FALSE;

        gsize allowed_len = strlen (allowed_dir);
        if (g_str_has_prefix (path, allowed_dir) && path[allowed_len] == '/')
        {
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean
is_path_allowed_for_mount (const gchar *path, TweaksConfig *config)
{
    if (is_inside_active_mount (path))
        return FALSE;

    if (has_active_submounts (path))
        return FALSE;

    return is_path_in_remote_dirs (path, config);
}

static gboolean
is_server_already_mounted (RemoteServer *s)
{
    FILE *fp = fopen ("/proc/mounts", "r");
    if (!fp)
        return FALSE;

    char line[2048];
    gboolean mounted = FALSE;

    while (fgets (line, sizeof (line), fp))
    {
        char dev[512], mnt[1024], fstype[64];
        if (sscanf (line, "%511s %1023s %63s", dev, mnt, fstype) >= 3)
        {
            if (s->is_rclone && (g_str_has_prefix (fstype, "fuse.rclone") || g_strcmp0 (fstype, "rclone") == 0))
            {
                g_autofree gchar *prefix = g_strdup_printf ("%s:", s->name);
                if (g_str_has_prefix (dev, prefix) || g_strcmp0 (dev, s->name) == 0)
                {
                    mounted = TRUE;
                    break;
                }
            }
            else if (!s->is_rclone && (g_str_has_prefix (fstype, "fuse.sshfs") || g_strcmp0 (fstype, "sshfs") == 0))
            {
                char *colon = strchr (dev, ':');
                if (colon)
                {
                    g_autofree gchar *host_part = g_strndup (dev, colon - dev);
                    if (g_strcmp0 (host_part, s->name) == 0 || g_str_has_suffix (host_part, s->name))
                    {
                        mounted = TRUE;
                        break;
                    }
                }
            }
        }
    }
    fclose (fp);
    return mounted;
}

/* -------------------------------------------------------------------------- */
/* Парсинг конфигураций ~/.ssh/config и rclone.conf                           */
/* -------------------------------------------------------------------------- */

static GList *
get_available_remote_servers (void)
{
    GList *list = NULL;
    const gchar *home = g_get_home_dir ();
    const gchar *config_dir = g_get_user_config_dir ();

    /* 1. ~/.ssh/config */
    g_autofree gchar *ssh_cfg = g_build_filename (home, ".ssh", "config", NULL);
    FILE *fp = fopen (ssh_cfg, "r");
    if (fp)
    {
        char line[1024];
        RemoteServer *cur = NULL;

        while (fgets (line, sizeof (line), fp))
        {
            gchar *trimmed = g_strstrip (line);

            if (trimmed[0] == '#')
            {
                if (cur != NULL && g_ascii_strncasecmp (trimmed, "# RemotePath:", 13) == 0)
                {
                    cur->remote_path = g_strdup (g_strstrip (trimmed + 13));
                }
                continue;
            }

            if (g_ascii_strncasecmp (trimmed, "Host ", 5) == 0)
            {
                gchar *host_name = g_strstrip (trimmed + 5);
                if (strlen (host_name) > 0 && !strchr (host_name, '*') && !strchr (host_name, '?'))
                {
                    cur = g_new0 (RemoteServer, 1);
                    cur->name       = g_strdup (host_name);
                    cur->type_label = g_strdup ("SFTP");
                    cur->is_rclone  = FALSE;

                    if (!is_server_already_mounted (cur))
                    {
                        list = g_list_append (list, cur);
                    }
                    else
                    {
                        remote_server_free (cur);
                        cur = NULL;
                    }
                }
                else
                {
                    cur = NULL;
                }
            }
        }
        fclose (fp);
    }

    /* 2. ~/.config/rclone/rclone.conf */
    g_autofree gchar *rclone_cfg = g_build_filename (config_dir, "rclone", "rclone.conf", NULL);
    g_autoptr (GKeyFile) keyfile = g_key_file_new ();
    if (g_key_file_load_from_file (keyfile, rclone_cfg, G_KEY_FILE_NONE, NULL))
    {
        gsize num_groups = 0;
        gchar **groups = g_key_file_get_groups (keyfile, &num_groups);

        for (gsize i = 0; i < num_groups; i++)
        {
            gchar *group_name = groups[i];
            gchar *type = g_key_file_get_string (keyfile, group_name, "type", NULL);

            RemoteServer *cur = g_new0 (RemoteServer, 1);
            cur->name      = g_strdup (group_name);
            cur->is_rclone = TRUE;

            if (type && strlen (g_strstrip (type)) > 0)
            {
                cur->type_label = g_ascii_strup (type, -1);
                g_free (type);
            }
            else
            {
                cur->type_label = g_strdup ("RCLONE");
            }

            if (!is_server_already_mounted (cur))
            {
                list = g_list_append (list, cur);
            }
            else
            {
                remote_server_free (cur);
            }
        }
        g_strfreev (groups);
    }

    return list;
}

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
                {
                    gtk_widget_activate_action (focus, "slot.reload", NULL);
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Монтирование серверов и обработка результатов                              */
/* -------------------------------------------------------------------------- */

static void
on_mount_communicated (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    MountFinishData *data = (MountFinishData *) user_data;
    GSubprocess *proc = G_SUBPROCESS (source_object);
    g_autoptr (GError) err = NULL;
    g_autofree gchar *stdout_buf = NULL;
    g_autofree gchar *stderr_buf = NULL;

    g_subprocess_communicate_utf8_finish (proc, res, &stdout_buf, &stderr_buf, &err);

    gint exit_code = g_subprocess_get_exit_status (proc);
    gboolean success = (!err && g_subprocess_get_successful (proc));

    tweaks_log ("Команда: %s", data->cmd_line);
    tweaks_log ("Результат: exit_code=%d, success=%s", exit_code, success ? "TRUE" : "FALSE");
    if (err)
        tweaks_log ("GError: %s", err->message);
    if (stdout_buf && strlen (stdout_buf) > 0)
        tweaks_log ("STDOUT:\n%s", stdout_buf);
    if (stderr_buf && strlen (stderr_buf) > 0)
        tweaks_log ("STDERR:\n%s", stderr_buf);

    if (success)
    {
        tweaks_log ("Монтирование успешно: %s -> %s", data->server_name, data->target_path);
        TweaksConfig *config = tweaks_config_load ();
        const gchar *emblem_name = (config->emblem && strlen (config->emblem) > 0) ? config->emblem : "globe";
        const gchar *emblems[] = { emblem_name, NULL };

        g_autoptr (GFile) target_gfile = g_file_new_for_path (data->target_path);
        g_file_set_attribute (target_gfile, "metadata::emblems",
                              G_FILE_ATTRIBUTE_TYPE_STRINGV,
                              (gpointer) emblems,
                              G_FILE_QUERY_INFO_NONE,
                              NULL, NULL);
        tweaks_config_free (config);

        reload_nautilus_views ();
    }
    else
    {
        /* Извлекаем подробную причину из stderr для отображения в уведомлении */
        g_autofree gchar *err_msg = NULL;
        if (stderr_buf && strlen (g_strstrip (stderr_buf)) > 0)
        {
            err_msg = g_strdup (stderr_buf);
        }
        else if (err)
        {
            err_msg = g_strdup (err->message);
        }
        else
        {
            err_msg = g_strdup ("Процесс завершился с ошибкой (код != 0). См. ~/.config/nautilus-tweaks/debug.log");
        }

        tweaks_log ("Ошибка монтирования для %s: %s", data->server_name, err_msg);

        const gchar *notify_argv[] = {
            "notify-send",
            "-u", "critical",
            "-i", "dialog-error",
            "Ошибка подключения",
            err_msg,
            NULL
        };
        g_spawn_async (NULL, (gchar **) notify_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }

    g_free (data->target_path);
    g_free (data->server_name);
    g_free (data->cmd_line);
    g_free (data);
}

static void
start_mount_server_rclone (RemoteServer *target_server, const gchar *target_path)
{
    g_autofree gchar *remote_spec = NULL;
    if (target_server->remote_path && strlen (target_server->remote_path) > 0)
    {
        const gchar *subpath = target_server->remote_path;
        if (subpath[0] == '/')
            subpath++;
        remote_spec = g_strdup_printf ("%s:%s", target_server->name, subpath);
    }
    else
    {
        remote_spec = g_strdup_printf ("%s:", target_server->name);
    }

    g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE
    );

    g_autoptr (GError) spawn_err = NULL;
    GSubprocess *mount_proc = g_subprocess_launcher_spawn (
        launcher,
        &spawn_err,
        "rclone", "mount", remote_spec, target_path,
        "--vfs-cache-mode", "writes",
        "--daemon",
        NULL
    );

    g_autofree gchar *cmd_desc = g_strdup_printf ("rclone mount %s %s --vfs-cache-mode writes --daemon",
                                                  remote_spec, target_path);

    if (spawn_err)
    {
        tweaks_log ("Ошибка запуска rclone: %s", spawn_err->message);
        return;
    }

    tweaks_log ("Запуск rclone: %s", cmd_desc);

    MountFinishData *mf_data = g_new0 (MountFinishData, 1);
    mf_data->target_path = g_strdup (target_path);
    mf_data->server_name = g_strdup (target_server->name);
    mf_data->cmd_line    = g_steal_pointer (&cmd_desc);

    g_subprocess_communicate_utf8_async (mount_proc, NULL, NULL, on_mount_communicated, mf_data);
    g_object_unref (mount_proc);
}

static void
start_mount_server_sshfs (RemoteServer *target_server, const gchar *target_path, const gchar *password)
{
    g_autofree gchar *remote_spec = target_server->remote_path
        ? g_strdup_printf ("%s:%s", target_server->name, target_server->remote_path)
        : g_strdup_printf ("%s:/", target_server->name);

    g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (
        G_SUBPROCESS_FLAGS_STDIN_PIPE |
        G_SUBPROCESS_FLAGS_STDOUT_PIPE |
        G_SUBPROCESS_FLAGS_STDERR_PIPE
    );

    /* КРИТИЧНО: Принудительный C-locale для ssh, чтобы prompt всегда был "Password:",
     * и запрет поиска сторонних askpass */
    g_subprocess_launcher_setenv (launcher, "LC_ALL", "C", TRUE);
    g_subprocess_launcher_setenv (launcher, "SSH_ASKPASS_REQUIRE", "never", TRUE);

    g_autoptr (GError) spawn_err = NULL;
    GSubprocess *mount_proc = NULL;
    g_autofree gchar *pass_nl = NULL;
    g_autofree gchar *cmd_desc = NULL;

    if (password && strlen (password) > 0)
    {
        pass_nl = g_strdup_printf ("%s\n", password);
        cmd_desc = g_strdup_printf ("sshfs %s %s -o reconnect,ServerAliveInterval=15,ServerAliveCountMax=3,follow_symlinks,StrictHostKeyChecking=accept-new,password_stdin",
                                    remote_spec, target_path);

        mount_proc = g_subprocess_launcher_spawn (
            launcher,
            &spawn_err,
            "sshfs", remote_spec, target_path,
            "-o", "reconnect,ServerAliveInterval=15,ServerAliveCountMax=3,follow_symlinks,StrictHostKeyChecking=accept-new,password_stdin",
            NULL
        );
    }
    else
    {
        cmd_desc = g_strdup_printf ("sshfs %s %s -o reconnect,ServerAliveInterval=15,ServerAliveCountMax=3,follow_symlinks,StrictHostKeyChecking=accept-new",
                                    remote_spec, target_path);

        mount_proc = g_subprocess_launcher_spawn (
            launcher,
            &spawn_err,
            "sshfs", remote_spec, target_path,
            "-o", "reconnect,ServerAliveInterval=15,ServerAliveCountMax=3,follow_symlinks,StrictHostKeyChecking=accept-new",
            NULL
        );
    }

    if (spawn_err)
    {
        tweaks_log ("Ошибка spawn sshfs: %s", spawn_err->message);
        return;
    }

    tweaks_log ("Запуск sshfs: %s (пароль передан: %s)", cmd_desc, (password && strlen (password) > 0) ? "ДА" : "НЕТ");

    MountFinishData *mf_data = g_new0 (MountFinishData, 1);
    mf_data->target_path = g_strdup (target_path);
    mf_data->server_name = g_strdup (target_server->name);
    mf_data->cmd_line    = g_steal_pointer (&cmd_desc);

    /* Асинхронно передаем пароль в stdin, считываем stdout/stderr и ждем завершения */
    g_subprocess_communicate_utf8_async (mount_proc, pass_nl, NULL, on_mount_communicated, mf_data);
    g_object_unref (mount_proc);
}

/* -------------------------------------------------------------------------- */
/* Нативное дочернее окно запроса пароля SSH                                  */
/* -------------------------------------------------------------------------- */

static void
on_password_connect_clicked (GtkButton *btn, gpointer user_data)
{
    SshPasswordDialog *pd = (SshPasswordDialog *) user_data;
    const gchar *pass = gtk_editable_get_text (GTK_EDITABLE (pd->entry_password));

    start_mount_server_sshfs (pd->server, pd->target_path, pass);
    gtk_window_destroy (GTK_WINDOW (pd->window));
}

static void
on_password_dialog_destroyed (gpointer data, GObject *where_the_object_was)
{
    SshPasswordDialog *pd = (SshPasswordDialog *) data;
    remote_server_free (pd->server);
    g_free (pd->target_path);
    g_free (pd);
}

static void
show_ssh_password_dialog (RemoteServer *server, const gchar *target_path)
{
    SshPasswordDialog *pd = g_new0 (SshPasswordDialog, 1);
    pd->server = remote_server_copy (server);
    pd->target_path = g_strdup (target_path);

    pd->window = gtk_window_new ();
    g_autofree gchar *title = g_strdup_printf ("Аутентификация: %s", server->name);
    gtk_window_set_title (GTK_WINDOW (pd->window), title);
    gtk_window_set_default_size (GTK_WINDOW (pd->window), 380, 150);
    gtk_window_set_resizable (GTK_WINDOW (pd->window), FALSE);

    GtkWindow *parent = get_nautilus_active_window ();
    if (parent)
    {
        gtk_window_set_transient_for (GTK_WINDOW (pd->window), parent);
        gtk_window_set_modal (GTK_WINDOW (pd->window), TRUE);
    }

    g_object_weak_ref (G_OBJECT (pd->window), on_password_dialog_destroyed, pd);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start (box, 16);
    gtk_widget_set_margin_end (box, 16);
    gtk_widget_set_margin_top (box, 16);
    gtk_widget_set_margin_bottom (box, 16);
    gtk_window_set_child (GTK_WINDOW (pd->window), box);

    g_autofree gchar *prompt = g_strdup_printf ("Введите пароль для подключения к «%s»:", server->name);
    GtkWidget *lbl = gtk_label_new (prompt);
    gtk_widget_set_halign (lbl, GTK_ALIGN_START);
    gtk_label_set_wrap (GTK_LABEL (lbl), TRUE);
    gtk_box_append (GTK_BOX (box), lbl);

    pd->entry_password = gtk_password_entry_new ();
    gtk_password_entry_set_show_peek_icon (GTK_PASSWORD_ENTRY (pd->entry_password), TRUE);
    g_signal_connect_swapped (pd->entry_password, "activate", G_CALLBACK (on_password_connect_clicked), pd);
    gtk_box_append (GTK_BOX (box), pd->entry_password);

    GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign (btn_box, GTK_ALIGN_END);

    GtkWidget *btn_cancel = gtk_button_new_with_label ("Отмена");
    GtkWidget *btn_connect = gtk_button_new_with_label ("Подключить");
    gtk_widget_add_css_class (btn_connect, "suggested-action");

    g_signal_connect_swapped (btn_cancel, "clicked", G_CALLBACK (gtk_window_destroy), pd->window);
    g_signal_connect (btn_connect, "clicked", G_CALLBACK (on_password_connect_clicked), pd);

    gtk_box_append (GTK_BOX (btn_box), btn_cancel);
    gtk_box_append (GTK_BOX (btn_box), btn_connect);
    gtk_box_append (GTK_BOX (box), btn_box);

    gtk_window_set_focus (GTK_WINDOW (pd->window), pd->entry_password);
    gtk_window_present (GTK_WINDOW (pd->window));
}

static void
on_ssh_check_finished (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    SshCheckData *data = (SshCheckData *) user_data;
    GSubprocess *proc = G_SUBPROCESS (source_object);
    g_autoptr (GError) err = NULL;
    g_autofree gchar *stdout_buf = NULL;
    g_autofree gchar *stderr_buf = NULL;

    g_subprocess_communicate_utf8_finish (proc, res, &stdout_buf, &stderr_buf, &err);

    gint exit_code = g_subprocess_get_exit_status (proc);
    gboolean success = (!err && g_subprocess_get_successful (proc));

    tweaks_log ("SSH pre-check (BatchMode) для '%s': exit_code=%d, success=%s",
                data->server->name, exit_code, success ? "TRUE" : "FALSE");
    if (stderr_buf && strlen (g_strstrip (stderr_buf)) > 0)
        tweaks_log ("SSH pre-check STDERR: %s", stderr_buf);

    if (success)
    {
        tweaks_log ("SSH-ключ подошёл. Монтирование без пароля...");
        start_mount_server_sshfs (data->server, data->target_path, NULL);
    }
    else
    {
        tweaks_log ("SSH-ключ не подошёл (код %d). Открываем окно ввода пароля.", exit_code);
        show_ssh_password_dialog (data->server, data->target_path);
    }

    remote_server_free (data->server);
    g_free (data->target_path);
    g_free (data);
}

/* -------------------------------------------------------------------------- */
/* Диалоговое окно выбора сервера на GTK4                                     */
/* -------------------------------------------------------------------------- */

static void
on_mount_row_selected (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    MountDialogWidgets *d = (MountDialogWidgets *) user_data;
    if (row)
    {
        d->selected_server = (RemoteServer *) g_object_get_data (G_OBJECT (row), "server");
        gtk_widget_set_sensitive (d->btn_connect, TRUE);
    }
    else
    {
        d->selected_server = NULL;
        gtk_widget_set_sensitive (d->btn_connect, FALSE);
    }
}

static void
on_mount_connect_clicked (GtkButton *btn, gpointer user_data)
{
    MountDialogWidgets *d = (MountDialogWidgets *) user_data;
    if (!d->selected_server)
        return;

    if (d->selected_server->is_rclone)
    {
        start_mount_server_rclone (d->selected_server, d->target_path);
    }
    else
    {
        SshCheckData *check_data = g_new0 (SshCheckData, 1);
        check_data->server = remote_server_copy (d->selected_server);
        check_data->target_path = g_strdup (d->target_path);

        g_autoptr (GSubprocessLauncher) check_launcher = g_subprocess_launcher_new (
            G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE
        );
        g_subprocess_launcher_setenv (check_launcher, "LC_ALL", "C", TRUE);

        g_autoptr (GError) err = NULL;
        GSubprocess *check_proc = g_subprocess_launcher_spawn (
            check_launcher,
            &err,
            "ssh", "-o", "BatchMode=yes",
            "-o", "StrictHostKeyChecking=accept-new",
            "-o", "ConnectTimeout=2",
            check_data->server->name, "exit",
            NULL
        );

        if (check_proc)
        {
            tweaks_log ("Проверка наличия рабочего SSH-ключа для '%s'...", check_data->server->name);
            g_subprocess_communicate_utf8_async (check_proc, NULL, NULL, on_ssh_check_finished, check_data);
            g_object_unref (check_proc);
        }
        else
        {
            tweaks_log ("Не удалось запустить ssh pre-check: %s. Показываем диалог пароля.", err ? err->message : "unknown");
            show_ssh_password_dialog (check_data->server, check_data->target_path);
            remote_server_free (check_data->server);
            g_free (check_data->target_path);
            g_free (check_data);
        }
    }

    gtk_window_destroy (GTK_WINDOW (d->window));
}

static void
on_mount_row_activated (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    on_mount_connect_clicked (NULL, user_data);
}

static void
on_mount_dialog_destroyed (gpointer data, GObject *where_the_object_was)
{
    MountDialogWidgets *d = (MountDialogWidgets *) data;
    g_active_mount_window = NULL;

    g_free (d->target_path);
    g_list_free_full (d->servers, (GDestroyNotify) remote_server_free);
    g_free (d);
}

static void
on_mount_dialog_activated (NautilusMenuItem *item, gpointer user_data)
{
    gchar *target_path = (gchar *) user_data;

    if (g_active_mount_window != NULL)
    {
        gtk_window_present (GTK_WINDOW (g_active_mount_window));
        return;
    }

    GList *servers = get_available_remote_servers ();
    if (!servers)
    {
        const gchar *notify_argv[] = {
            "notify-send",
            "-u", "normal",
            "-i", "dialog-information",
            "Удалённые серверы",
            "Все настроенные серверы уже подключены или не найдены в конфигурациях",
            NULL
        };
        g_spawn_async (NULL, (gchar **) notify_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        return;
    }

    MountDialogWidgets *d = g_new0 (MountDialogWidgets, 1);
    d->target_path = g_strdup (target_path);
    d->servers = servers;
    d->selected_server = NULL;

    d->window = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (d->window), "Монтирование сервера");
    gtk_window_set_default_size (GTK_WINDOW (d->window), 390, 420);
    gtk_window_set_resizable (GTK_WINDOW (d->window), FALSE);

    GtkWindow *parent = get_nautilus_active_window ();
    if (parent)
    {
        gtk_window_set_transient_for (GTK_WINDOW (d->window), parent);
        gtk_window_set_modal (GTK_WINDOW (d->window), TRUE);
    }

    g_active_mount_window = d->window;
    g_object_weak_ref (G_OBJECT (d->window), on_mount_dialog_destroyed, d);

    GtkWidget *main_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start (main_box, 16);
    gtk_widget_set_margin_end (main_box, 16);
    gtk_widget_set_margin_top (main_box, 16);
    gtk_widget_set_margin_bottom (main_box, 16);
    gtk_window_set_child (GTK_WINDOW (d->window), main_box);

    g_autofree gchar *target_str = g_strdup_printf ("Точка: %s", target_path);
    GtkWidget *lbl_target = gtk_label_new (target_str);
    gtk_widget_set_halign (lbl_target, GTK_ALIGN_START);
    gtk_label_set_ellipsize (GTK_LABEL (lbl_target), PANGO_ELLIPSIZE_START);
    gtk_widget_add_css_class (lbl_target, "dim-label");
    gtk_box_append (GTK_BOX (main_box), lbl_target);

    GtkWidget *lbl_title = gtk_label_new ("Выберите сервер для подключения:");
    gtk_widget_set_halign (lbl_title, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (main_box), lbl_title);

    GtkWidget *scrolled = gtk_scrolled_window_new ();
    gtk_widget_set_vexpand (scrolled, TRUE);

    GtkWidget *list_box = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (list_box), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (list_box), FALSE);

    for (GList *l = servers; l != NULL; l = l->next)
    {
        RemoteServer *s = (RemoteServer *) l->data;
        GtkWidget *row = gtk_list_box_row_new ();
        g_object_set_data (G_OBJECT (row), "server", s);

        GtkWidget *row_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_start (row_box, 12);
        gtk_widget_set_margin_end (row_box, 12);
        gtk_widget_set_margin_top (row_box, 8);
        gtk_widget_set_margin_bottom (row_box, 8);

        GtkWidget *icon = gtk_image_new_from_icon_name (s->is_rclone ? "folder-remote-symbolic" : "network-server-symbolic");
        GtkWidget *name_lbl = gtk_label_new (s->name);
        gtk_widget_set_hexpand (name_lbl, TRUE);
        gtk_widget_set_halign (name_lbl, GTK_ALIGN_START);

        GtkWidget *type_lbl = gtk_label_new (s->type_label);
        gtk_widget_add_css_class (type_lbl, "dim-label");

        gtk_box_append (GTK_BOX (row_box), icon);
        gtk_box_append (GTK_BOX (row_box), name_lbl);
        gtk_box_append (GTK_BOX (row_box), type_lbl);

        gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), row_box);
        gtk_list_box_append (GTK_LIST_BOX (list_box), row);
    }

    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), list_box);
    gtk_box_append (GTK_BOX (main_box), scrolled);

    GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign (btn_box, GTK_ALIGN_END);

    GtkWidget *btn_cancel = gtk_button_new_with_label ("Отмена");
    d->btn_connect = gtk_button_new_with_label ("Подключить");
    gtk_widget_add_css_class (d->btn_connect, "suggested-action");
    gtk_widget_set_sensitive (d->btn_connect, FALSE);

    g_signal_connect_swapped (btn_cancel, "clicked", G_CALLBACK (gtk_window_destroy), d->window);
    g_signal_connect (d->btn_connect, "clicked", G_CALLBACK (on_mount_connect_clicked), d);

    gtk_box_append (GTK_BOX (btn_box), btn_cancel);
    gtk_box_append (GTK_BOX (btn_box), d->btn_connect);
    gtk_box_append (GTK_BOX (main_box), btn_box);

    gtk_list_box_unselect_all (GTK_LIST_BOX (list_box));

    g_signal_connect (list_box, "row-selected", G_CALLBACK (on_mount_row_selected), d);
    g_signal_connect (list_box, "row-activated", G_CALLBACK (on_mount_row_activated), d);

    gtk_window_set_focus (GTK_WINDOW (d->window), btn_cancel);

    gtk_window_present (GTK_WINDOW (d->window));
}

static void
on_unmount_ssh_activated (NautilusMenuItem *item, gpointer user_data)
{
    gchar *path = (gchar *) user_data;
    g_autofree gchar *cmd = g_strdup_printf ("fusermount -u \"%s\"", path);

    g_autofree gchar *err_out = NULL;
    gint exit_status = 0;

    tweaks_log ("Размонтирование точки: %s", path);
    g_spawn_command_line_sync (cmd, NULL, &err_out, &exit_status, NULL);

    if (exit_status != 0)
    {
        const gchar *msg = (err_out && strlen (g_strstrip (err_out)) > 0)
                           ? err_out
                           : "Не удалось отмонтировать точку (возможно, каталог занят другим процессом)";

        tweaks_log ("Ошибка fusermount (код %d): %s", exit_status, msg);

        const gchar *notify_argv[] = {
            "notify-send",
            "-u", "critical",
            "-i", "dialog-error",
            "Ошибка размонтирования",
            msg,
            NULL
        };
        g_spawn_async (NULL, (gchar **) notify_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }
    else
    {
        tweaks_log ("Размонтирование точки %s прошло успешно", path);
        g_autoptr (GFile) unmounted_gfile = g_file_new_for_path (path);
        g_file_set_attribute (unmounted_gfile, "metadata::emblems",
                              G_FILE_ATTRIBUTE_TYPE_INVALID,
                              NULL,
                              G_FILE_QUERY_INFO_NONE,
                              NULL, NULL);

        reload_nautilus_views ();
    }
}

/* -------------------------------------------------------------------------- */
/* Контекстное меню                                                           */
/* -------------------------------------------------------------------------- */

static GList *
nautilus_tweaks_mount_get_file_items (NautilusMenuProvider *provider, GList *files)
{
    if (g_list_length (files) != 1)
        return NULL;

    NautilusFileInfo *first_file = NAUTILUS_FILE_INFO (files->data);
    if (!nautilus_file_info_is_directory (first_file))
        return NULL;

    g_autoptr (GFile) location = nautilus_file_info_get_location (first_file);
    if (!location)
        return NULL;

    g_autofree gchar *target_path = g_file_get_path (location);
    if (!target_path)
        return NULL;

    GList *items = NULL;
    TweaksConfig *config = tweaks_config_load ();

    if (is_path_mounted (target_path))
    {
        g_autofree gchar *unmount_id = g_strdup_printf ("NautilusTweaks::Unmount_%u", ++g_mount_action_counter);
        NautilusMenuItem *unmount_item = nautilus_menu_item_new (
            unmount_id,
            "Отмонтировать",
            "Отмонтировать удалённый сервер",
            "media-eject-symbolic"
        );

        g_signal_connect_data (unmount_item, "activate",
                               G_CALLBACK (on_unmount_ssh_activated),
                               g_strdup (target_path),
                               (GClosureNotify) (GCallback) (GDestroyNotify) g_free, 0);

        items = g_list_append (items, unmount_item);
    }
    else if (is_path_allowed_for_mount (target_path, config))
    {
        g_autofree gchar *mount_id = g_strdup_printf ("NautilusTweaks::Mount_%u", ++g_mount_action_counter);
        NautilusMenuItem *mount_item = nautilus_menu_item_new (
            mount_id,
            "Примонтировать сервер...",
            "Выбрать сервер из ~/.ssh/config или ~/.config/rclone/rclone.conf и примонтировать",
            "network-server-symbolic"
        );

        g_signal_connect_data (mount_item, "activate",
                               G_CALLBACK (on_mount_dialog_activated),
                               g_strdup (target_path),
                               (GClosureNotify) (GCallback) (GDestroyNotify) g_free, 0);

        items = g_list_append (items, mount_item);
    }

    tweaks_config_free (config);
    return items;
}

static void
nautilus_tweaks_mount_menu_provider_iface_init (NautilusMenuProviderInterface *iface)
{
    iface->get_file_items = nautilus_tweaks_mount_get_file_items;
    iface->get_background_items = NULL;
}

void
nautilus_module_initialize (GTypeModule *module)
{
    nautilus_tweaks_mount_register_type (module);
}

void
nautilus_module_shutdown (void)
{
}

void
nautilus_module_list_types (const GType **types, int *num_types)
{
    static GType type_list[1];
    type_list[0] = nautilus_tweaks_mount_get_type ();
    *types = type_list;
    *num_types = 1;
}