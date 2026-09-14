#include <nautilus-extension.h>
#include <gio/gio.h>
#include <gdk/gdk.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

/* Счётчик для генерации уникальных ID пунктов меню (защита от коллизий в GTK/GAction) */
static guint g_action_counter = 0;

/* Указатель на текущий активный процесс Zenity (для реализации Single Instance) */
static GSubprocess *g_active_zenity_proc = NULL;

/* -------------------------------------------------------------------------- */
/* Структура и управление конфигурацией                                       */
/* -------------------------------------------------------------------------- */

typedef struct {
    gchar    *terminal;         /* Терминал для запуска редактора (default: "kgx") */
    gchar    *editor;           /* Консольный редактор (default: "micro") */
    gboolean  shorten_home;     /* Заменять $HOME на ~ (default: TRUE) */
    gboolean  resolve_symlinks; /* Раскрывать симлинки (default: TRUE) */
} AppConfig;

static void
app_config_free (AppConfig *config)
{
    if (!config)
        return;
    g_free (config->terminal);
    g_free (config->editor);
    g_free (config);
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
        "# Эмулятор терминала (kgx, gnome-terminal, ptyxis, ghostty, alacritty, foot, kitty)\n"
        "terminal = kgx\n\n"
        "# Консольный текстовый редактор для root (micro, nano, nvim, vim)\n"
        "editor = micro\n\n"
        "# Сокращать $HOME до ~ при копировании путей (true / false)\n"
        "shorten_home = true\n\n"
        "# Раскрывать симлинки до реального пути целевого файла (true / false)\n"
        "resolve_symlinks = true\n";

    g_file_set_contents (config_path, default_content, -1, NULL);
}

static AppConfig *
app_config_load (void)
{
    AppConfig *config = g_new0 (AppConfig, 1);
    config->terminal         = g_strdup ("kgx");
    config->editor           = g_strdup ("micro");
    config->shorten_home     = TRUE;
    config->resolve_symlinks = TRUE;

    const gchar *config_dir = g_get_user_config_dir ();
    g_autofree gchar *config_path = g_build_filename (config_dir, "nautilus-custom-actions", "config.ini", NULL);

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
    }

    return config;
}

/* -------------------------------------------------------------------------- */
/* Объявление структуры GObject-плагина                                       */
/* -------------------------------------------------------------------------- */

typedef struct _CustomActions {
    GObject parent_instance;
} CustomActions;

typedef struct _CustomActionsClass {
    GObjectClass parent_class;
} CustomActionsClass;

static GType custom_actions_get_type (void);
static void custom_actions_menu_provider_iface_init (NautilusMenuProviderInterface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (CustomActions, custom_actions, G_TYPE_OBJECT, 0,
    G_IMPLEMENT_INTERFACE_DYNAMIC (NAUTILUS_TYPE_MENU_PROVIDER,
                                   custom_actions_menu_provider_iface_init))

static void custom_actions_class_init (CustomActionsClass *klass) {}
static void custom_actions_init (CustomActions *self) {}
static void custom_actions_class_finalize (CustomActionsClass *klass) {}

/* -------------------------------------------------------------------------- */
/* Вспомогательный хелпер: Запуск команд в терминале                           */
/* -------------------------------------------------------------------------- */
static void
launch_in_terminal (const gchar *terminal, const gchar *command)
{
    gint term_argc = 0;
    gchar **term_argv = NULL;

    if (!g_shell_parse_argv (terminal, &term_argc, &term_argv, NULL) || term_argc == 0)
    {
        const gchar *fallback_argv[] = { "kgx", "--", "sh", "-c", command, NULL };
        g_spawn_async (NULL, (gchar **) fallback_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        return;
    }

    GPtrArray *argv_array = g_ptr_array_new ();
    for (int i = 0; i < term_argc; i++)
        g_ptr_array_add (argv_array, term_argv[i]);

    const gchar *last_token = term_argv[term_argc - 1];

    /* kgx / gnome-terminal / ptyxis используют разделитель '--' */
    if (g_strcmp0 (last_token, "kgx") == 0 ||
        g_strcmp0 (last_token, "gnome-console") == 0 ||
        g_strcmp0 (last_token, "gnome-terminal") == 0 ||
        g_strcmp0 (last_token, "ptyxis") == 0)
    {
        g_ptr_array_add (argv_array, "--");
        g_ptr_array_add (argv_array, "sh");
        g_ptr_array_add (argv_array, "-c");
        g_ptr_array_add (argv_array, (gchar *) command);
    }
    /* Terminator принимает всю команду целиком после флага -e */
    else if (g_strcmp0 (last_token, "terminator") == 0)
    {
        g_ptr_array_add (argv_array, "-e");
        g_ptr_array_add (argv_array, (gchar *) command);
    }
    /* kitty и foot принимают команду напрямую */
    else if (g_strcmp0 (last_token, "kitty") == 0 || g_strcmp0 (last_token, "foot") == 0)
    {
        g_ptr_array_add (argv_array, "sh");
        g_ptr_array_add (argv_array, "-c");
        g_ptr_array_add (argv_array, (gchar *) command);
    }
    /* ghostty / alacritty */
    else
    {
        g_ptr_array_add (argv_array, "-e");
        g_ptr_array_add (argv_array, "sh");
        g_ptr_array_add (argv_array, "-c");
        g_ptr_array_add (argv_array, (gchar *) command);
    }

    g_ptr_array_add (argv_array, NULL);

    g_spawn_async (NULL, (gchar **) argv_array->pdata, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);

    g_ptr_array_free (argv_array, TRUE);
    g_strfreev (term_argv);
}

/* -------------------------------------------------------------------------- */
/* Вспомогательные функции: SSHFS и парсинг хостов                             */
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

static GList *
get_ssh_hosts (void)
{
    GList *hosts = NULL;
    const gchar *home = g_get_home_dir ();
    g_autofree gchar *config_path = g_build_filename (home, ".ssh", "config", NULL);

    FILE *fp = fopen (config_path, "r");
    if (!fp)
        return NULL;

    char line[1024];
    while (fgets (line, sizeof (line), fp))
    {
        gchar *trimmed = g_strstrip (line);
        if (g_ascii_strncasecmp (trimmed, "Host ", 5) == 0)
        {
            gchar **tokens = g_strsplit_set (trimmed + 5, " \t", -1);
            for (int i = 0; tokens[i] != NULL; i++)
            {
                gchar *t = g_strstrip (tokens[i]);
                if (strlen (t) > 0 && !strchr (t, '*') && !strchr (t, '?'))
                {
                    hosts = g_list_append (hosts, g_strdup (t));
                }
            }
            g_strfreev (tokens);
        }
    }
    fclose (fp);
    return hosts;
}

/* Структура данных для асинхронного диалога Zenity */
typedef struct {
    gchar *target_path;
} ZenityDialogData;

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
        gchar *selected_host = g_strstrip (stdout_buf);
        if (strlen (selected_host) > 0)
        {
            /* 
             * 1. SSH_ASKPASS вызывает графическое окно пароля Zenity
             * 2. StrictHostKeyChecking=accept-new сам подтверждает новые сертификаты
             */
            g_autofree gchar *cmd = g_strdup_printf (
                "env SSH_ASKPASS_REQUIRE=force SSH_ASKPASS=zenity-askpass-wrapper "
                "sshfs '%s:' '%s' -o reconnect,ServerAliveInterval=15,ServerAliveCountMax=3,follow_symlinks,StrictHostKeyChecking=accept-new",
                selected_host, data->target_path
            );
            
            g_spawn_command_line_async (cmd, NULL);

            g_spawn_command_line_async (cmd, NULL);
        }
    }

    g_free (data->target_path);
    g_free (data);
}
/**
 * on_mount_dialog_activated:
 * Асинхронный вызов Zenity с поддержкой Single Instance.
 */
static void
on_mount_dialog_activated (NautilusMenuItem *item, gpointer user_data)
{
    gchar *target_path = (gchar *) user_data;

    /* Single Instance: закрываем предыдущий экземпляр Zenity, если он уже открыт */
    if (g_active_zenity_proc != NULL)
    {
        g_subprocess_force_exit (g_active_zenity_proc);
        g_clear_object (&g_active_zenity_proc);
    }

    GList *ssh_hosts = get_ssh_hosts ();
    if (!ssh_hosts)
    {
        const gchar *notify_argv[] = {
            "notify-send",
            "-u", "normal",
            "-i", "dialog-information",
            "SSHFS",
            "В ~/.ssh/config не найдено настроенных хостов",
            NULL
        };
        g_spawn_async (NULL, (gchar **) notify_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        return;
    }

    GPtrArray *argv_array = g_ptr_array_new ();
    g_ptr_array_add (argv_array, "zenity");
    g_ptr_array_add (argv_array, "--list");
    g_ptr_array_add (argv_array, "--title=Монтирование SSHFS");
    g_ptr_array_add (argv_array, "--text=Выберите сервер для монтирования:");
    g_ptr_array_add (argv_array, "--column=Сервер");
    g_ptr_array_add (argv_array, "--width=400");
    g_ptr_array_add (argv_array, "--height=450");

    for (GList *h = ssh_hosts; h != NULL; h = h->next)
    {
        g_ptr_array_add (argv_array, (gchar *) h->data);
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

    g_ptr_array_free (argv_array, TRUE);
    g_list_free_full (ssh_hosts, g_free);

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
}

/* -------------------------------------------------------------------------- */
/* 1. Действие: Копирование пути                                              */
/* -------------------------------------------------------------------------- */

static void
on_copy_path_activated (NautilusMenuItem *item, gpointer user_data)
{
    GList *files = (GList *) user_data;
    AppConfig *config = app_config_load ();

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

        g_autofree gchar *path = g_file_get_path (location);
        if (!path)
            continue;

        g_autofree gchar *target_path = NULL;

        /* Резолв симлинка */
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
                        "Битый симлинк",
                        "Конечный файл не существует, но ссылка всё равно скопирована в буфер",
                        NULL
                    };
                    g_spawn_async (NULL, (gchar **) notify_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
                }
            }
        }

        const gchar *final_path = target_path ? target_path : path;

        if (!first)
            g_string_append_c (text, '\n');
        first = FALSE;

        /* Сокращение $HOME до ~ */
        if (config->shorten_home && home && g_strcmp0 (final_path, home) == 0)
        {
            g_string_append (text, "~");
        }
        else if (config->shorten_home && home && g_str_has_prefix (final_path, home) && final_path[home_len] == '/')
        {
            g_string_append_c (text, '~');
            g_string_append (text, final_path + home_len);
        }
        else
        {
            g_string_append (text, final_path);
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
    app_config_free (config);
}

/* -------------------------------------------------------------------------- */
/* 2. Действие: Открыть папку в VS Code                                       */
/* -------------------------------------------------------------------------- */

static void
on_open_in_code_activated (NautilusMenuItem *item, gpointer user_data)
{
    gchar *path = (gchar *) user_data;
    const gchar *argv[] = { "code", path, NULL };
    g_spawn_async (NULL, (gchar **) argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

/* -------------------------------------------------------------------------- */
/* 3. Действие: Открыть / Редактировать как root                              */
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
        AppConfig *config = app_config_load ();
        g_autofree gchar *cmd = g_strdup_printf ("sudo %s \"%s\"", config->editor, path);
        launch_in_terminal (config->terminal, cmd);
        app_config_free (config);
    }
}

/* -------------------------------------------------------------------------- */
/* 4. Контекстное меню для выбранных файлов/папок                             */
/* -------------------------------------------------------------------------- */

static GList *
custom_actions_get_file_items (NautilusMenuProvider *provider, GList *files)
{
    GList *items = NULL;
    guint len = g_list_length (files);

    if (len == 0)
        return NULL;

    AppConfig *config = app_config_load ();

    /* --- Пункт 1: Копировать путь --- */
    g_autofree gchar *copy_label = NULL;
    if (len == 1)
    {
        NautilusFileInfo *first_file = NAUTILUS_FILE_INFO (files->data);
        if (nautilus_file_info_is_directory (first_file))
            copy_label = g_strdup ("Копировать путь к папке");
        else
            copy_label = g_strdup ("Копировать путь к файлу");
    }
    else
    {
        copy_label = g_strdup_printf ("Копировать пути (%u)", len);
    }

    g_autofree gchar *copy_id = g_strdup_printf ("CustomActions::CopyPath_%u", ++g_action_counter);
    NautilusMenuItem *copy_item = nautilus_menu_item_new (
        copy_id,
        copy_label,
        "Копирует путь в буфер обмена",
        "edit-copy-symbolic"
    );

    g_signal_connect_data (copy_item, "activate",
                           G_CALLBACK (on_copy_path_activated),
                           nautilus_file_info_list_copy (files),
                           (GClosureNotify) nautilus_file_info_list_free, 0);

    items = g_list_append (items, copy_item);

    /* --- Блок для одного выбранного объекта --- */
    if (len == 1)
    {
        NautilusFileInfo *first_file = NAUTILUS_FILE_INFO (files->data);
        gboolean is_dir = nautilus_file_info_is_directory (first_file);
        g_autoptr (GFile) location = nautilus_file_info_get_location (first_file);
        g_autofree gchar *target_path = location ? g_file_get_path (location) : NULL;

        /* --- Пункт 2: Открыть папку в VS Code (только для папок) --- */
        if (is_dir && target_path)
        {
            g_autofree gchar *code_id = g_strdup_printf ("CustomActions::OpenInCode_%u", ++g_action_counter);
            NautilusMenuItem *code_item = nautilus_menu_item_new (
                code_id,
                "Открыть папку в VS Code",
                "Открыть эту директорию как проект в VS Code",
                "com.visualstudio.code"
            );

            g_signal_connect_data (code_item, "activate",
                                   G_CALLBACK (on_open_in_code_activated),
                                   g_strdup (target_path),
                                   (GClosureNotify) g_free, 0);

            items = g_list_append (items, code_item);
        }

        /* --- Пункт 3: Открыть / Редактировать как root --- */
        g_autofree gchar *root_label = NULL;
        if (is_dir)
            root_label = g_strdup ("Открыть как root");
        else
            root_label = g_strdup_printf ("Редактировать как root (%s)", config->editor);

        const gchar *root_tip   = is_dir ? "Открыть эту папку в Nautilus с правами администратора"
                                         : "Редактировать файл в консольном редакторе от имени root";
        const gchar *root_icon  = is_dir ? "folder-remote-symbolic" : "accessories-text-editor-symbolic";

        g_autofree gchar *root_id = g_strdup_printf ("CustomActions::OpenAsRoot_%u", ++g_action_counter);
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

        /* --- Пункт 4: Монтирование / Размонтирование SSHFS (ТОЛЬКО для папок) --- */
        if (is_dir && target_path)
        {
            if (is_path_mounted (target_path))
            {
                g_autofree gchar *unmount_id = g_strdup_printf ("CustomActions::UnmountSSH_%u", ++g_action_counter);
                NautilusMenuItem *unmount_item = nautilus_menu_item_new (
                    unmount_id,
                    "Отмонтировать (SSHFS)",
                    "Отмонтировать удалённый сервер",
                    "media-eject-symbolic"
                );

                g_signal_connect_data (unmount_item, "activate",
                                       G_CALLBACK (on_unmount_ssh_activated),
                                       g_strdup (target_path),
                                       (GClosureNotify) g_free, 0);

                items = g_list_append (items, unmount_item);
            }
            else
            {
                g_autofree gchar *mount_id = g_strdup_printf ("CustomActions::MountSSH_%u", ++g_action_counter);
                NautilusMenuItem *mount_item = nautilus_menu_item_new (
                    mount_id,
                    "Примонтировать сервер...",
                    "Выбрать сервер из ~/.ssh/config и примонтировать через SSHFS",
                    "network-server-symbolic"
                );

                g_signal_connect_data (mount_item, "activate",
                                       G_CALLBACK (on_mount_dialog_activated),
                                       g_strdup (target_path),
                                       (GClosureNotify) g_free, 0);

                items = g_list_append (items, mount_item);
            }
        }
    }

    app_config_free (config);
    return items;
}

/* -------------------------------------------------------------------------- */
/* 5. Контекстное меню пустого пространства (Background Menu)                  */
/* -------------------------------------------------------------------------- */

static GList *
custom_actions_get_background_items (NautilusMenuProvider *provider,
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

    GList *items = NULL;

    /* 1. Копировать путь к текущей папке */
    g_autofree gchar *bg_copy_id = g_strdup_printf ("CustomActions::BgCopyPath_%u", ++g_action_counter);
    NautilusMenuItem *copy_item = nautilus_menu_item_new (
        bg_copy_id,
        "Копировать путь к папке",
        "Копирует путь текущей папки в буфер обмена",
        "edit-copy-symbolic"
    );

    GList *single_list = g_list_append (NULL, current_folder);
    g_signal_connect_data (copy_item, "activate",
                           G_CALLBACK (on_copy_path_activated),
                           nautilus_file_info_list_copy (single_list),
                           (GClosureNotify) nautilus_file_info_list_free, 0);
    g_list_free (single_list);
    items = g_list_append (items, copy_item);

    /* 2. Открыть текущую папку в VS Code */
    g_autofree gchar *bg_code_id = g_strdup_printf ("CustomActions::BgOpenInCode_%u", ++g_action_counter);
    NautilusMenuItem *code_item = nautilus_menu_item_new (
        bg_code_id,
        "Открыть папку в VS Code",
        "Открыть текущую директорию как проект в VS Code",
        "com.visualstudio.code"
    );

    g_signal_connect_data (code_item, "activate",
                           G_CALLBACK (on_open_in_code_activated),
                           g_strdup (target_path),
                           (GClosureNotify) g_free, 0);
    items = g_list_append (items, code_item);

    /* 3. Открыть текущую папку как root */
    g_autofree gchar *bg_root_id = g_strdup_printf ("CustomActions::BgOpenAsRoot_%u", ++g_action_counter);
    NautilusMenuItem *root_item = nautilus_menu_item_new (
        bg_root_id,
        "Открыть как root",
        "Открыть текущую папку в Nautilus с правами администратора",
        "folder-remote-symbolic"
    );

    g_signal_connect_data (root_item, "activate",
                           G_CALLBACK (on_open_as_root_activated),
                           g_object_ref (current_folder),
                           (GClosureNotify) g_object_unref, 0);
    items = g_list_append (items, root_item);

    return items;
}

/* -------------------------------------------------------------------------- */
/* Инициализация модуля расширения Nautilus                                   */
/* -------------------------------------------------------------------------- */

static void
custom_actions_menu_provider_iface_init (NautilusMenuProviderInterface *iface)
{
    iface->get_file_items = custom_actions_get_file_items;
    iface->get_background_items = custom_actions_get_background_items;
}

void
nautilus_module_initialize (GTypeModule *module)
{
    custom_actions_register_type (module);
}

void
nautilus_module_shutdown (void)
{
}

void
nautilus_module_list_types (const GType **types, int *num_types)
{
    static GType type_list[1];
    type_list[0] = custom_actions_get_type ();
    *types = type_list;
    *num_types = 1;
}