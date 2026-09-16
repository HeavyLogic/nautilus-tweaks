#include <nautilus-extension.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "tweaks-config.h"

/* Счётчик для генерации уникальных ID пунктов меню */
static guint g_mount_action_counter = 0;

/* Указатель на текущий активный процесс Zenity (Single Instance) */
static GSubprocess *g_active_zenity_proc = NULL;

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
} ZenityDialogData;

typedef struct {
    gchar *target_path;
} MountFinishData;

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
/* Освобождение памяти                                                        */
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

/* -------------------------------------------------------------------------- */
/* Вспомогательные функции: Проверка монтирования и защита дерева             */
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

        /* Исключаем саму базовую папку-контейнер (например, /mnt/Remote) */
        if (g_strcmp0 (path, allowed_dir) == 0)
            return FALSE;

        /* Разрешаем строго подпапки (например, /mnt/Remote/itcrus) */
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

/* -------------------------------------------------------------------------- */
/* Парсинг ~/.ssh/config и ~/.config/rclone/rclone.conf                       */
/* -------------------------------------------------------------------------- */

static GList *
get_all_remote_servers (void)
{
    GList *list = NULL;
    const gchar *home = g_get_home_dir ();
    const gchar *config_dir = g_get_user_config_dir ();

    /* 1. Парсим ~/.ssh/config (OpenSSH / SFTP) */
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
                    list = g_list_append (list, cur);
                }
                else
                {
                    cur = NULL;
                }
            }
        }
        fclose (fp);
    }

    /* 2. Парсим ~/.config/rclone/rclone.conf (FTP, WebDAV, S3 и др.) */
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

            list = g_list_append (list, cur);
        }

        g_strfreev (groups);
    }

    return list;
}

/**
 * reload_nautilus_views:
 * Программно вызывает системное действие F5 (slot.reload) в окнах Nautilus.
 */
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
/* Асинхронное монтирование                                                   */
/* -------------------------------------------------------------------------- */

static void
on_mount_finished (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    MountFinishData *data = (MountFinishData *) user_data;
    GSubprocess *proc = G_SUBPROCESS (source_object);
    g_autoptr (GError) err = NULL;

    g_subprocess_wait_finish (proc, res, &err);

    if (!err && g_subprocess_get_successful (proc))
    {
        /* 1. Устанавливаем системную эмблему через GIO metadata */
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

        /* 2. Обновляем вид Nautilus */
        reload_nautilus_views ();
    }

    g_free (data->target_path);
    g_free (data);
}

static void
on_zenity_dialog_finished (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    ZenityDialogData *data = (ZenityDialogData *) user_data;
    GSubprocess *proc = G_SUBPROCESS (source_object);
    g_autofree gchar *stdout_buf = NULL;
    g_autoptr (GError) err = NULL;

    g_subprocess_communicate_utf8_finish (proc, res, &stdout_buf, NULL, &err);

    if (g_active_zenity_proc == proc)
        g_clear_object (&g_active_zenity_proc);

    if (!err && g_subprocess_get_successful (proc) && stdout_buf)
    {
        gchar *selected_uid = g_strstrip (stdout_buf);
        gchar **parts = g_strsplit (selected_uid, ":", 2);

        if (parts[0] != NULL && parts[1] != NULL)
        {
            gboolean is_rclone = (g_strcmp0 (parts[0], "rclone") == 0);
            const gchar *selected_name = parts[1];

            GList *servers = get_all_remote_servers ();
            RemoteServer *target_server = NULL;

            for (GList *l = servers; l != NULL; l = l->next)
            {
                RemoteServer *s = (RemoteServer *) l->data;
                if (s->is_rclone == is_rclone && g_strcmp0 (s->name, selected_name) == 0)
                {
                    target_server = s;
                    break;
                }
            }

            if (target_server)
            {
                GSubprocess *mount_proc = NULL;
                g_autoptr (GError) spawn_err = NULL;

                /* 1. Монтирование через Rclone */
                if (target_server->is_rclone)
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

                    mount_proc = g_subprocess_new (
                        G_SUBPROCESS_FLAGS_NONE,
                        &spawn_err,
                        "rclone", "mount", remote_spec, data->target_path,
                        "--vfs-cache-mode", "writes",
                        "--daemon",
                        NULL
                    );
                }
                /* 2. Монтирование через SSHFS */
                else
                {
                    g_autofree gchar *remote_spec = NULL;
                    if (target_server->remote_path)
                        remote_spec = g_strdup_printf ("%s:%s", target_server->name, target_server->remote_path);
                    else
                        remote_spec = g_strdup_printf ("%s:/", target_server->name);

                    g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
                    g_subprocess_launcher_setenv (launcher, "SSH_ASKPASS_REQUIRE", "force", TRUE);
                    g_subprocess_launcher_setenv (launcher, "SSH_ASKPASS", "zenity-askpass-wrapper", TRUE);

                    mount_proc = g_subprocess_launcher_spawn (
                        launcher,
                        &spawn_err,
                        "sshfs", remote_spec, data->target_path,
                        "-o", "reconnect,ServerAliveInterval=15,ServerAliveCountMax=3,follow_symlinks,StrictHostKeyChecking=accept-new",
                        NULL
                    );
                }

                if (mount_proc)
                {
                    MountFinishData *mf_data = g_new0 (MountFinishData, 1);
                    mf_data->target_path = g_strdup (data->target_path);

                    g_subprocess_wait_async (mount_proc, NULL, on_mount_finished, mf_data);
                    g_object_unref (mount_proc);
                }
            }

            g_list_free_full (servers, (GDestroyNotify) remote_server_free);
        }
        g_strfreev (parts);
    }

    g_free (data->target_path);
    g_free (data);
}

static void
on_mount_dialog_activated (NautilusMenuItem *item, gpointer user_data)
{
    gchar *target_path = (gchar *) user_data;

    if (g_active_zenity_proc != NULL)
    {
        g_subprocess_force_exit (g_active_zenity_proc);
        g_clear_object (&g_active_zenity_proc);
    }

    GList *servers = get_all_remote_servers ();
    if (!servers)
    {
        const gchar *notify_argv[] = {
            "notify-send",
            "-u", "normal",
            "-i", "dialog-information",
            "Удалённые серверы",
            "Не найдено серверов в ~/.ssh/config или ~/.config/rclone/rclone.conf",
            NULL
        };
        g_spawn_async (NULL, (gchar **) notify_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        return;
    }

    GPtrArray *argv_array = g_ptr_array_new ();
    g_ptr_array_add (argv_array, "zenity");
    g_ptr_array_add (argv_array, "--list");
    g_ptr_array_add (argv_array, "--title=Монтирование сервера");
    g_ptr_array_add (argv_array, "--text=Выберите сервер для подключения:");
    g_ptr_array_add (argv_array, "--column=ID");
    g_ptr_array_add (argv_array, "--hide-column=1");
    g_ptr_array_add (argv_array, "--column=Сервер");
    g_ptr_array_add (argv_array, "--column=Протокол");
    g_ptr_array_add (argv_array, "--width=420");
    g_ptr_array_add (argv_array, "--height=450");

    for (GList *l = servers; l != NULL; l = l->next)
    {
        RemoteServer *s = (RemoteServer *) l->data;
        gchar *uid = g_strdup_printf ("%s:%s", s->is_rclone ? "rclone" : "sftp", s->name);

        g_ptr_array_add (argv_array, uid);
        g_ptr_array_add (argv_array, s->name);
        g_ptr_array_add (argv_array, s->type_label);
    }
    g_ptr_array_add (argv_array, NULL);

    g_autoptr (GError) err = NULL;
    g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE
    );

    GSubprocess *proc = g_subprocess_launcher_spawnv (
        launcher,
        (const gchar * const *) argv_array->pdata,
        &err
    );

    for (guint i = 10; i < argv_array->len - 1; i += 3)
        g_free (g_ptr_array_index (argv_array, i));

    g_ptr_array_free (argv_array, TRUE);
    g_list_free_full (servers, (GDestroyNotify) remote_server_free);

    if (err)
    {
        const gchar *notify_argv[] = {
            "notify-send",
            "-u", "critical",
            "-i", "dialog-error",
            "Ошибка запуска Zenity",
            "Убедитесь, что пакет zenity установлен (sudo pacman -S zenity)",
            NULL
        };
        g_spawn_async (NULL, (gchar **) notify_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        return;
    }

    g_active_zenity_proc = g_object_ref (proc);

    ZenityDialogData *data = g_new0 (ZenityDialogData, 1);
    data->target_path = g_strdup (target_path);

    g_subprocess_communicate_utf8_async (proc, NULL, NULL, on_zenity_dialog_finished, data);
}

static void
on_unmount_ssh_activated (NautilusMenuItem *item, gpointer user_data)
{
    gchar *path = (gchar *) user_data;
    g_autofree gchar *cmd = g_strdup_printf ("fusermount -u \"%s\"", path);

    g_autofree gchar *err_out = NULL;
    gint exit_status = 0;

    g_spawn_command_line_sync (cmd, NULL, &err_out, &exit_status, NULL);

    if (exit_status != 0)
    {
        const gchar *msg = (err_out && strlen (g_strstrip (err_out)) > 0)
                           ? err_out
                           : "Не удалось отмонтировать точку (возможно, каталог занят другим процессом)";

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
                               (GClosureNotify) g_free, 0);

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
                               (GClosureNotify) g_free, 0);

        items = g_list_append (items, mount_item);
    }

    tweaks_config_free (config);
    return items;
}

/* -------------------------------------------------------------------------- */
/* Инициализация модуля расширения Nautilus                                   */
/* -------------------------------------------------------------------------- */

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