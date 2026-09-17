#include <nautilus-extension.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "tweaks-config.h"
#include "tweaks-log.h"
#include "tweaks-remote.h"

/* Counter for generating unique menu item IDs */
static guint g_mount_action_counter = 0;

/* Pointer to active selection dialog window (Single Instance) */
static GtkWidget *g_active_mount_window = NULL;

/* -------------------------------------------------------------------------- */
/* Data structures                                                            */
/* -------------------------------------------------------------------------- */

typedef struct {
    gchar *target_path;
    gchar *server_name;
    gchar *cmd_line;
} MountFinishData;

typedef struct {
    GtkWidget          *window;
    GtkWidget          *btn_connect;
    gchar              *target_path;
    GList              *servers;
    TweaksRemoteServer *selected_server;
} MountDialogWidgets;

typedef struct {
    TweaksRemoteServer *server;
    gchar              *target_path;
} SshCheckData;

typedef struct {
    TweaksRemoteServer *server;
    gchar              *target_path;
    GtkWidget          *window;
    GtkWidget          *entry_password;
} SshPasswordDialog;

/* -------------------------------------------------------------------------- */
/* GObject plugin structure declaration                                       */
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
/* Window helper functions                                                    */
/* -------------------------------------------------------------------------- */

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
/* Path permission check                                                      */
/* -------------------------------------------------------------------------- */

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
            return TRUE;
    }

    return FALSE;
}

static gboolean
is_path_allowed_for_mount (const gchar *path, TweaksConfig *config)
{
    if (tweaks_mount_is_inside_mount (path))
        return FALSE;

    if (tweaks_mount_has_submounts (path))
        return FALSE;

    return is_path_in_remote_dirs (path, config);
}

/* -------------------------------------------------------------------------- */
/* Mounting servers and handling results                                      */
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

    log_debug ("Command: %s", data->cmd_line);
    log_debug ("Result: exit_code=%d, success=%s", exit_code, success ? "TRUE" : "FALSE");
    if (err)
        log_debug ("GError: %s", err->message);
    if (stdout_buf && strlen (stdout_buf) > 0)
        log_debug ("STDOUT:\n%s", stdout_buf);
    if (stderr_buf && strlen (stderr_buf) > 0)
        log_debug ("STDERR:\n%s", stderr_buf);

    if (success)
    {
        log_debug ("Mount successful: %s -> %s", data->server_name, data->target_path);
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
        g_autofree gchar *err_msg = NULL;
        if (stderr_buf && strlen (g_strstrip (stderr_buf)) > 0)
            err_msg = g_strdup (stderr_buf);
        else if (err)
            err_msg = g_strdup (err->message);
        else
            err_msg = g_strdup (_("Process exited with an error (non-zero exit code). See ~/.config/nautilus-tweaks/debug.log"));

        log_debug ("Mount error for %s: %s", data->server_name, err_msg);

        const gchar *notify_argv[] = {
            "notify-send",
            "-u", "critical",
            "-i", "dialog-error",
            _("Connection Error"),
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
start_mount_server_rclone (TweaksRemoteServer *target_server, const gchar *target_path)
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
        log_debug ("Error launching rclone: %s", spawn_err->message);
        return;
    }

    log_debug ("Launching rclone: %s", cmd_desc);

    MountFinishData *mf_data = g_new0 (MountFinishData, 1);
    mf_data->target_path = g_strdup (target_path);
    mf_data->server_name = g_strdup (target_server->name);
    mf_data->cmd_line    = g_steal_pointer (&cmd_desc);

    g_subprocess_communicate_utf8_async (mount_proc, NULL, NULL, on_mount_communicated, mf_data);
    g_object_unref (mount_proc);
}

static void
start_mount_server_sshfs (TweaksRemoteServer *target_server, const gchar *target_path, const gchar *password)
{
    g_autofree gchar *remote_spec = target_server->remote_path
        ? g_strdup_printf ("%s:%s", target_server->name, target_server->remote_path)
        : g_strdup_printf ("%s:/", target_server->name);

    g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (
        G_SUBPROCESS_FLAGS_STDIN_PIPE |
        G_SUBPROCESS_FLAGS_STDOUT_PIPE |
        G_SUBPROCESS_FLAGS_STDERR_PIPE
    );

    g_subprocess_launcher_setenv (launcher, "LC_ALL", "C", TRUE);
    g_subprocess_launcher_setenv (launcher, "SSH_ASKPASS_REQUIRE", "never", TRUE);

    gboolean has_password = (password && strlen (password) > 0);
    const gchar *sshfs_opts = tweaks_remote_get_sshfs_options (has_password);

    g_autofree gchar *cmd_desc = g_strdup_printf ("sshfs %s %s -o %s", remote_spec, target_path, sshfs_opts);
    g_autofree gchar *pass_nl = has_password ? g_strdup_printf ("%s\n", password) : NULL;

    g_autoptr (GError) spawn_err = NULL;
    GSubprocess *mount_proc = g_subprocess_launcher_spawn (
        launcher,
        &spawn_err,
        "sshfs", remote_spec, target_path,
        "-o", sshfs_opts,
        NULL
    );

    if (spawn_err)
    {
        log_debug ("Error spawning sshfs: %s", spawn_err->message);
        return;
    }

    log_debug ("Launching sshfs: %s (password provided: %s)", cmd_desc, has_password ? "YES" : "NO");

    MountFinishData *mf_data = g_new0 (MountFinishData, 1);
    mf_data->target_path = g_strdup (target_path);
    mf_data->server_name = g_strdup (target_server->name);
    mf_data->cmd_line    = g_steal_pointer (&cmd_desc);

    g_subprocess_communicate_utf8_async (mount_proc, pass_nl, NULL, on_mount_communicated, mf_data);
    g_object_unref (mount_proc);
}

/* -------------------------------------------------------------------------- */
/* Native child SSH password dialog                                           */
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
    tweaks_remote_server_free (pd->server);
    g_free (pd->target_path);
    g_free (pd);
}

static void
show_ssh_password_dialog (TweaksRemoteServer *server, const gchar *target_path)
{
    SshPasswordDialog *pd = g_new0 (SshPasswordDialog, 1);
    pd->server = tweaks_remote_server_copy (server);
    pd->target_path = g_strdup (target_path);

    pd->window = gtk_window_new ();
    g_autofree gchar *title = g_strdup_printf (_("Authentication: %s"), server->name);
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

    g_autofree gchar *prompt = g_strdup_printf (_("Enter password to connect to \"%s\":"), server->name);
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

    GtkWidget *btn_cancel = gtk_button_new_with_label (_("Cancel"));
    GtkWidget *btn_connect = gtk_button_new_with_label (_("Connect"));
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

    log_debug ("SSH pre-check for '%s': exit_code=%d, success=%s",
               data->server->name, exit_code, success ? "TRUE" : "FALSE");
    if (stderr_buf && strlen (g_strstrip (stderr_buf)) > 0)
        log_debug ("SSH pre-check STDERR: %s", stderr_buf);

    if (success)
    {
        log_debug ("SSH key accepted. Mounting without password...");
        start_mount_server_sshfs (data->server, data->target_path, NULL);
    }
    else
    {
        log_debug ("SSH key rejected (exit code %d). Prompting password dialog.", exit_code);
        show_ssh_password_dialog (data->server, data->target_path);
    }

    tweaks_remote_server_free (data->server);
    g_free (data->target_path);
    g_free (data);
}

/* -------------------------------------------------------------------------- */
/* GTK4 server selection dialog                                               */
/* -------------------------------------------------------------------------- */

static void
on_mount_row_selected (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    MountDialogWidgets *d = (MountDialogWidgets *) user_data;
    if (row)
    {
        d->selected_server = (TweaksRemoteServer *) g_object_get_data (G_OBJECT (row), "server");
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
        check_data->server = tweaks_remote_server_copy (d->selected_server);
        check_data->target_path = g_strdup (d->target_path);

        g_autoptr (GError) err = NULL;
        GSubprocess *check_proc = tweaks_remote_ssh_spawn (
            check_data->server->name,
            "exit",
            2,
            G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
            &err
        );

        if (check_proc)
        {
            log_debug ("Checking SSH key for '%s'...", check_data->server->name);
            g_subprocess_communicate_utf8_async (check_proc, NULL, NULL, on_ssh_check_finished, check_data);
            g_object_unref (check_proc);
        }
        else
        {
            log_debug ("Failed to launch ssh pre-check: %s. Showing password dialog.", err ? err->message : "unknown");
            show_ssh_password_dialog (check_data->server, check_data->target_path);
            tweaks_remote_server_free (check_data->server);
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
    g_list_free_full (d->servers, (GDestroyNotify) tweaks_remote_server_free);
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

    GList *servers = tweaks_remote_get_available_servers ();
    if (!servers)
    {
        const gchar *notify_argv[] = {
            "notify-send",
            "-u", "normal",
            "-i", "dialog-information",
            _("Remote Servers"),
            _("All configured servers are already connected or none were found in configurations"),
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
    gtk_window_set_title (GTK_WINDOW (d->window), _("Mount Server"));
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

    g_autofree gchar *target_str = g_strdup_printf (_("Mount point: %s"), target_path);
    GtkWidget *lbl_target = gtk_label_new (target_str);
    gtk_widget_set_halign (lbl_target, GTK_ALIGN_START);
    gtk_label_set_ellipsize (GTK_LABEL (lbl_target), PANGO_ELLIPSIZE_START);
    gtk_widget_add_css_class (lbl_target, "dim-label");
    gtk_box_append (GTK_BOX (main_box), lbl_target);

    GtkWidget *lbl_title = gtk_label_new (_("Select server to connect:"));
    gtk_widget_set_halign (lbl_title, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (main_box), lbl_title);

    GtkWidget *scrolled = gtk_scrolled_window_new ();
    gtk_widget_set_vexpand (scrolled, TRUE);

    GtkWidget *list_box = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (list_box), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (list_box), FALSE);

    for (GList *l = servers; l != NULL; l = l->next)
    {
        TweaksRemoteServer *s = (TweaksRemoteServer *) l->data;
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

    GtkWidget *btn_cancel = gtk_button_new_with_label (_("Cancel"));
    d->btn_connect = gtk_button_new_with_label (_("Connect"));
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

    log_debug ("Unmounting mount point: %s", path);
    g_spawn_command_line_sync (cmd, NULL, &err_out, &exit_status, NULL);

    if (exit_status != 0)
    {
        const gchar *msg = (err_out && strlen (g_strstrip (err_out)) > 0)
                           ? err_out
                           : _("Failed to unmount point (directory might be busy with another process)");

        log_debug ("Error in fusermount (exit code %d): %s", exit_status, msg);

        const gchar *notify_argv[] = {
            "notify-send",
            "-u", "critical",
            "-i", "dialog-error",
            _("Unmount Error"),
            msg,
            NULL
        };
        g_spawn_async (NULL, (gchar **) notify_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }
    else
    {
        log_debug ("Mount point %s unmounted successfully", path);
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
/* Context menu                                                               */
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

    if (tweaks_mount_is_path_mounted (target_path))
    {
        g_autofree gchar *unmount_id = g_strdup_printf ("NautilusTweaks::Unmount_%u", ++g_mount_action_counter);
        NautilusMenuItem *unmount_item = nautilus_menu_item_new (
            unmount_id,
            _("Unmount"),
            _("Unmount remote server"),
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
            _("Mount Server..."),
            _("Select server from ~/.ssh/config or ~/.config/rclone/rclone.conf and mount"),
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