#include <nautilus-extension.h>
#include <gio/gio.h>
#include <gdk/gdk.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

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

/**
 * ensure_default_config_exists:
 * @config_path: Полный путь к файлу конфигурации
 *
 * Если конфига нет, создаёт директорию и дефолтный шаблон config.ini с комментариями.
 */
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

/**
 * app_config_load:
 *
 * Загружает настройки из ~/.config/nautilus-custom-actions/config.ini.
 * Возвращает: структуру AppConfig с загруженными или дефолтными значениями.
 */
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

/**
 * launch_in_terminal:
 * @terminal: Имя эмулятора терминала
 * @command: Команда для исполнения
 */
static void
launch_in_terminal (const gchar *terminal, const gchar *command)
{
    /* kgx (GNOME Console) */
    if (g_strcmp0 (terminal, "kgx") == 0 || g_strcmp0 (terminal, "gnome-console") == 0)
    {
        const gchar *argv[] = { "kgx", "-e", command, NULL };
        g_spawn_async (NULL, (gchar **) argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }
    /* gnome-terminal */
    else if (g_strcmp0 (terminal, "gnome-terminal") == 0)
    {
        const gchar *argv[] = { "gnome-terminal", "--", "sh", "-c", command, NULL };
        g_spawn_async (NULL, (gchar **) argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }
    /* Универсальный запуск через флаг -e (ptyxis, ghostty, alacritty, foot, kitty) */
    else
    {
        const gchar *argv[] = { terminal, "-e", command, NULL };
        g_spawn_async (NULL, (gchar **) argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }
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

typedef struct {
    gchar *host;
    gchar *path;
} MountPayload;

static void
mount_payload_free (MountPayload *data)
{
    if (!data) return;
    g_free (data->host);
    g_free (data->path);
    g_free (data);
}

static void
on_mount_ssh_activated (NautilusMenuItem *item, gpointer user_data)
{
    MountPayload *data = (MountPayload *) user_data;
    g_autofree gchar *cmd = g_strdup_printf (
        "sshfs \"%s:/\" \"%s\" -o reconnect,ServerAliveInterval=15,ServerAliveCountMax=3,follow_symlinks",
        data->host, data->path
    );
    g_spawn_command_line_async (cmd, NULL);
}

static void
on_unmount_ssh_activated (NautilusMenuItem *item, gpointer user_data)
{
    gchar *path = (gchar *) user_data;
    g_autofree gchar *cmd = g_strdup_printf ("fusermount -u \"%s\"", path);
    g_spawn_command_line_async (cmd, NULL);
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

        /* Резолв симлинка (если включена опция resolve_symlinks) */
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
                /* Ошибка: целевой файл битого симлинка не найден */
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

        /* Сокращение $HOME до ~ (если включена опция shorten_home) */
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
        /* Папки открываем в Nautilus через GVFS Admin */
        g_autofree gchar *admin_uri = g_strdup_printf ("admin://%s", path);
        const gchar *argv[] = { "nautilus", admin_uri, NULL };
        g_spawn_async (NULL, (gchar **) argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }
    else
    {
        /* Читаем терминал и редактор из конфига */
        AppConfig *config = app_config_load ();

        g_autofree gchar *cmd = g_strdup_printf ("sudo %s \"%s\"", config->editor, path);
        launch_in_terminal (config->terminal, cmd);

        app_config_free (config);
    }
}

/* -------------------------------------------------------------------------- */
/* 4. Формирование контекстного меню Nautilus                                 */
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

    NautilusMenuItem *copy_item = nautilus_menu_item_new (
        "CustomActions::CopyPath",
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

        /* --- Пункт 2: Открыть папку в VS Code (только для директорий) --- */
        if (is_dir && target_path)
        {
            NautilusMenuItem *code_item = nautilus_menu_item_new (
                "CustomActions::OpenInCode",
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

        NautilusMenuItem *root_item = nautilus_menu_item_new (
            "CustomActions::OpenAsRoot",
            root_label,
            root_tip,
            root_icon
        );

        g_signal_connect_data (root_item, "activate",
                               G_CALLBACK (on_open_as_root_activated),
                               g_object_ref (first_file),
                               (GClosureNotify) g_object_unref, 0);

        items = g_list_append (items, root_item);

        /* --- Пункт 4: Монтирование / Размонтирование SSHFS (только для директорий) --- */
        if (is_dir && target_path)
        {
            if (is_path_mounted (target_path))
            {
                NautilusMenuItem *unmount_item = nautilus_menu_item_new (
                    "CustomActions::UnmountSSH",
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
                GList *ssh_hosts = get_ssh_hosts ();
                if (ssh_hosts)
                {
                    NautilusMenuItem *mount_root_item = nautilus_menu_item_new (
                        "CustomActions::MountSSHRoot",
                        "Примонтировать сервер",
                        "Примонтировать хост из ~/.ssh/config через SSHFS",
                        "network-server-symbolic"
                    );

                    NautilusMenu *submenu = nautilus_menu_new ();
                    nautilus_menu_item_set_submenu (mount_root_item, submenu);

                    g_autofree gchar *folder_name = g_path_get_basename (target_path);

                    for (GList *h = ssh_hosts; h != NULL; h = h->next)
                    {
                        const gchar *host_name = (const gchar *) h->data;
                        
                        g_autofree gchar *item_id = g_strdup_printf ("CustomActions::MountSSH_%s", host_name);
                        g_autofree gchar *item_label = NULL;

                        if (g_strcmp0 (folder_name, host_name) == 0)
                            item_label = g_strdup_printf ("★ %s (по имени папки)", host_name);
                        else
                            item_label = g_strdup (host_name);

                        NautilusMenuItem *host_sub_item = nautilus_menu_item_new (
                            item_id,
                            item_label,
                            "Монтировать корень сервера",
                            "folder-remote-symbolic"
                        );

                        MountPayload *payload = g_new0 (MountPayload, 1);
                        payload->host = g_strdup (host_name);
                        payload->path = g_strdup (target_path);

                        g_signal_connect_data (host_sub_item, "activate",
                                               G_CALLBACK (on_mount_ssh_activated),
                                               payload,
                                               (GClosureNotify) mount_payload_free, 0);

                        nautilus_menu_append_item (submenu, host_sub_item);
                    }

                    g_list_free_full (ssh_hosts, g_free);
                    items = g_list_append (items, mount_root_item);
                }
            }
        }
    }

    app_config_free (config);
    return items;
}

/* -------------------------------------------------------------------------- */
/* Инициализация модуля расширения Nautilus                                   */
/* -------------------------------------------------------------------------- */

static void
custom_actions_menu_provider_iface_init (NautilusMenuProviderInterface *iface)
{
    iface->get_file_items = custom_actions_get_file_items;
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