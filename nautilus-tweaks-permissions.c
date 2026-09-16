#include <nautilus-extension.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>

#include "tweaks-config.h"

static guint g_permissions_action_counter = 0;
static GtkWidget *g_active_dialog_window = NULL;

/* -------------------------------------------------------------------------- */
/* Структура виджетов диалога                                                 */
/* -------------------------------------------------------------------------- */

typedef struct {
    GtkWidget *window;
    GList     *target_paths;

    /* Выпадающие списки */
    GtkWidget *combo_owner;
    GtkWidget *combo_group;

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
/* GObject регистрация модуля                                                 */
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
/* Живой пересчёт Octal <-> Чекбоксы                                          */
/* -------------------------------------------------------------------------- */

static void
update_octal_from_checkboxes (PermissionsDialogWidgets *w)
{
    if (w->updating_from_code)
        return;

    w->updating_from_code = TRUE;

    guint mode = 0;

    /* Специальные биты */
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_u_suid)))   mode |= 04000;
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_g_sgid)))   mode |= 02000;
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_o_sticky))) mode |= 01000;

    /* Owner */
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_u_r))) mode |= 0400;
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_u_w))) mode |= 0200;
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_u_x))) mode |= 0100;

    /* Group */
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_g_r))) mode |= 0040;
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_g_w))) mode |= 0020;
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_g_x))) mode |= 0010;

    /* Others */
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

    w->updating_from_code = FALSE;
}

/* -------------------------------------------------------------------------- */
/* Кнопки диалога (Apply / Cancel)                                            */
/* -------------------------------------------------------------------------- */

static void
on_apply_clicked (GtkButton *btn, gpointer user_data)
{
    PermissionsDialogWidgets *w = (PermissionsDialogWidgets *) user_data;

    const char *octal = gtk_editable_get_text (GTK_EDITABLE (w->entry_octal));
    gboolean add_x = gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_add_x));
    gboolean recursive = gtk_check_button_get_active (GTK_CHECK_BUTTON (w->chk_recursive));

    guint owner_idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (w->combo_owner));
    guint group_idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (w->combo_group));

    g_autofree gchar *debug_msg = g_strdup_printf (
        "Выбранные параметры:\n"
        "• Octal: %s\n"
        "• Owner index: %u\n"
        "• Group index: %u\n"
        "• Add X to dirs: %s\n"
        "• Recursive: %s\n"
        "• Объектов: %u",
        octal, owner_idx, group_idx,
        add_x ? "Да" : "Нет",
        recursive ? "Да" : "Нет",
        g_list_length (w->target_paths)
    );

    const gchar *notify_argv[] = {
        "notify-send",
        "-u", "normal",
        "-i", "dialog-information",
        "Nautilus Tweaks: Права (UI готов)",
        debug_msg,
        NULL
    };
    g_spawn_async (NULL, (gchar **) notify_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);

    /* ---------------------------------------------------------------------- */
    /* TODO: Сюда позже добавим chmod / chown                                 */
    /* ---------------------------------------------------------------------- */

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

    /* Извлекаем пути к файлам */
    for (GList *l = files; l != NULL; l = l->next)
    {
        NautilusFileInfo *file = NAUTILUS_FILE_INFO (l->data);
        g_autoptr (GFile) loc = nautilus_file_info_get_location (file);
        if (loc)
        {
            gchar *path = g_file_get_path (loc);
            if (path)
                w->target_paths = g_list_append (w->target_paths, path);
        }
    }

    /* Создаём окно */
    w->window = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (w->window), "Permissions");
    gtk_window_set_resizable (GTK_WINDOW (w->window), FALSE);
    gtk_window_set_default_size (GTK_WINDOW (w->window), 420, -1);

    g_object_weak_ref (G_OBJECT (w->window), on_dialog_destroyed, w);

    GtkWidget *main_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start (main_box, 16);
    gtk_widget_set_margin_end (main_box, 16);
    gtk_widget_set_margin_top (main_box, 16);
    gtk_widget_set_margin_bottom (main_box, 16);
    gtk_window_set_child (GTK_WINDOW (w->window), main_box);

    /* 1. Блок Owner / Group (Вверху) */
    GtkWidget *grid_top = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (grid_top), 12);
    gtk_grid_set_row_spacing (GTK_GRID (grid_top), 8);

    GtkWidget *lbl_owner = gtk_label_new ("Owner:");
    gtk_widget_set_halign (lbl_owner, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grid_top), lbl_owner, 0, 0, 1, 1);

    const char * const default_owners[] = { "root [0]", g_get_user_name (), NULL };
    w->combo_owner = gtk_drop_down_new_from_strings (default_owners);
    gtk_widget_set_hexpand (w->combo_owner, TRUE);
    gtk_grid_attach (GTK_GRID (grid_top), w->combo_owner, 1, 0, 1, 1);

    GtkWidget *lbl_group = gtk_label_new ("Group:");
    gtk_widget_set_halign (lbl_group, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grid_top), lbl_group, 0, 1, 1, 1);

    const char * const default_groups[] = { "root [0]", "wheel", "users", NULL };
    w->combo_group = gtk_drop_down_new_from_strings (default_groups);
    gtk_widget_set_hexpand (w->combo_group, TRUE);
    gtk_grid_attach (GTK_GRID (grid_top), w->combo_group, 1, 1, 1, 1);

    gtk_box_append (GTK_BOX (main_box), grid_top);
    gtk_box_append (GTK_BOX (main_box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    /* 2. Блок Permissions (Сетка со скриншота) */
    GtkWidget *grid_perm = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (grid_perm), 12);
    gtk_grid_set_row_spacing (GTK_GRID (grid_perm), 6);

    GtkWidget *lbl_perm_title = gtk_label_new ("Permissions:");
    gtk_widget_set_halign (lbl_perm_title, GTK_ALIGN_START);
    gtk_widget_set_valign (lbl_perm_title, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grid_perm), lbl_perm_title, 0, 0, 1, 5);

    /* Заголовки строк */
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

    /* Row 0: Owner (R=1, W=1, X=1, SUID=0) */
    w->chk_u_r    = gtk_check_button_new_with_label ("R");
    w->chk_u_w    = gtk_check_button_new_with_label ("W");
    w->chk_u_x    = gtk_check_button_new_with_label ("X");
    w->chk_u_suid = gtk_check_button_new_with_label ("Set UID");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_u_r), TRUE);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_u_w), TRUE);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_u_x), TRUE);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_u_r,    2, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_u_w,    3, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_u_x,    4, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_u_suid, 5, 0, 1, 1);

    /* Row 1: Group (R=1, W=0, X=1, SGID=0) */
    w->chk_g_r    = gtk_check_button_new_with_label ("R");
    w->chk_g_w    = gtk_check_button_new_with_label ("W");
    w->chk_g_x    = gtk_check_button_new_with_label ("X");
    w->chk_g_sgid = gtk_check_button_new_with_label ("Set GID");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_g_r), TRUE);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_g_w), FALSE);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_g_x), TRUE);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_g_r,    2, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_g_w,    3, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_g_x,    4, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_g_sgid, 5, 1, 1, 1);

    /* Row 2: Others (R=1, W=0, X=0, Sticky=0) -> Сумма даёт 0754 */
    w->chk_o_r      = gtk_check_button_new_with_label ("R");
    w->chk_o_w      = gtk_check_button_new_with_label ("W");
    w->chk_o_x      = gtk_check_button_new_with_label ("X");
    w->chk_o_sticky = gtk_check_button_new_with_label ("Sticky bit");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_o_r), TRUE);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_o_w), FALSE);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (w->chk_o_x), FALSE);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_o_r,      2, 2, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_o_w,      3, 2, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_o_x,      4, 2, 1, 1);
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_o_sticky, 5, 2, 1, 1);

    /* Row 3: Octal Entry (0754) */
    w->entry_octal = gtk_entry_new ();
    gtk_editable_set_text (GTK_EDITABLE (w->entry_octal), "0754");
    gtk_entry_set_max_length (GTK_ENTRY (w->entry_octal), 5);
    gtk_grid_attach (GTK_GRID (grid_perm), w->entry_octal, 2, 3, 2, 1);

    /* Row 4: Add X to directories */
    w->chk_add_x = gtk_check_button_new_with_mnemonic ("Add _X to directories");
    gtk_grid_attach (GTK_GRID (grid_perm), w->chk_add_x, 2, 4, 4, 1);

    gtk_box_append (GTK_BOX (main_box), grid_perm);
    gtk_box_append (GTK_BOX (main_box), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

    /* 3. Чекбокс рекурсивности */
    w->chk_recursive = gtk_check_button_new_with_mnemonic ("Set owner, group and permissions _recursively");
    gtk_box_append (GTK_BOX (main_box), w->chk_recursive);

    /* 4. Кнопки Cancel / Apply */
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

    /* Подключаем сигналы пересчёта Octal */
    GtkWidget *all_checks[] = {
        w->chk_u_r, w->chk_u_w, w->chk_u_x, w->chk_u_suid,
        w->chk_g_r, w->chk_g_w, w->chk_g_x, w->chk_g_sgid,
        w->chk_o_r, w->chk_o_w, w->chk_o_x, w->chk_o_sticky,
        NULL
    };

    for (int i = 0; all_checks[i] != NULL; i++)
        g_signal_connect (all_checks[i], "toggled", G_CALLBACK (on_perm_checkbox_toggled), w);

    g_signal_connect (w->entry_octal, "changed", G_CALLBACK (on_octal_entry_changed), w);

    return w->window;
}

/* -------------------------------------------------------------------------- */
/* Вызов меню                                                                 */
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

static void
nautilus_tweaks_permissions_menu_provider_iface_init (NautilusMenuProviderInterface *iface)
{
    iface->get_file_items = nautilus_tweaks_permissions_get_file_items;
    iface->get_background_items = NULL;
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