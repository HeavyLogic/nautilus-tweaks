#include <nautilus-extension.h>
#include <gio/gio.h>
#include <gdk/gdk.h>
#include <string.h>

/* Объявляем структуру GObject-плагина */
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
/* 1. Действие: Копирование пути (с раскрытием симлинков)                      */
/* -------------------------------------------------------------------------- */
static void
on_copy_path_activated (NautilusMenuItem *item, gpointer user_data)
{
    GList *files = (GList *) user_data;
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

        /* Проверяем: является ли объект симлинком */
        if (g_file_test (path, G_FILE_TEST_IS_SYMLINK))
        {
            /* 1. Пытаемся получить полный канонический путь цели через realpath */
            char *resolved = realpath (path, NULL);
            if (resolved)
            {
                target_path = g_strdup (resolved);
                free (resolved);
            }
            else
            {
                /* 2. Fallback: если ссылка "битая" (файла назначения нет), 
                      всё равно читаем путь, на который она указывает */
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
                }
            }
        }

        /* Если это обычный файл/папка или резолв не удался — берем исходный путь */
        const gchar *final_path = target_path ? target_path : path;

        if (!first)
            g_string_append_c (text, '\n');
        first = FALSE;

        /* Сокращаем $HOME до ~ (работает и для раскрытых симлинков) */
        if (home && g_strcmp0 (final_path, home) == 0)
        {
            g_string_append (text, "~");
        }
        else if (home && g_str_has_prefix (final_path, home) && final_path[home_len] == '/')
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
}

/* -------------------------------------------------------------------------- */
/* 2. Действие: Открыть / Редактировать как root (через kgx + micro)           */
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
        /* Папки открываем в Nautilus с правами root */
        g_autofree gchar *admin_uri = g_strdup_printf ("admin://%s", path);
        const gchar *argv[] = { "nautilus", admin_uri, NULL };
        g_spawn_async (NULL, (gchar **) argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }
    else
    {
        /* Файлы открываем в micro через kgx от имени root */
        g_autofree gchar *exec_cmd = g_strdup_printf ("sudo micro \"%s\"", path);
        const gchar *argv[] = { "kgx", "-e", exec_cmd, NULL };

        g_spawn_async (NULL, (gchar **) argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }
}

/* -------------------------------------------------------------------------- */
/* Формирование меню Nautilus                                                 */
/* -------------------------------------------------------------------------- */
static GList *
custom_actions_get_file_items (NautilusMenuProvider *provider, GList *files)
{
    GList *items = NULL;
    guint len = g_list_length (files);

    if (len == 0)
        return NULL;

    /* --- Пункт: Копировать путь --- */
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

    /* --- Пункт: Открыть / Редактировать как root --- */
    if (len == 1)
    {
        NautilusFileInfo *first_file = NAUTILUS_FILE_INFO (files->data);
        gboolean is_dir = nautilus_file_info_is_directory (first_file);

        const gchar *root_label = is_dir ? "Открыть как root" : "Редактировать как root";
        const gchar *root_tip   = is_dir ? "Открыть эту папку в Nautilus с правами администратора"
                                         : "Редактировать этот файл в Sublime Text с правами администратора";
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
    }

    return items;
}

/* -------------------------------------------------------------------------- */
/* Инициализация модуля Nautilus                                              */
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