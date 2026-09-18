#include "tweaks-remote.h"
#include "tweaks-config.h"
#include "tweaks-log.h"

#include <stdio.h>
#include <string.h>

static GtkWidget *g_active_chooser_window = NULL;

/* -------------------------------------------------------------------------- */
/* Mount Information and Path Helpers                                         */
/* -------------------------------------------------------------------------- */

void
tweaks_mount_info_free (TweaksMountInfo *info)
{
    if (!info)
        return;
    g_free (info->ssh_host);
    g_free (info->remote_base_path);
    g_free (info->mount_point);
    g_free (info);
}

TweaksMountInfo *
tweaks_mount_info_get_for_path (const gchar *path)
{
    TweaksMountInfo *info = g_new0 (TweaksMountInfo, 1);
    info->mode = TWEAKS_FS_LOCAL;

    if (!path)
        return info;

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
                        info->mode = TWEAKS_FS_RCLONE;
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
                    else if (g_str_has_prefix (fstype, "fuse.sshfs") || g_strcmp0 (fstype, "sshfs") == 0)
                    {
                        info->mode = TWEAKS_FS_SSHFS;
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
                        info->mode = TWEAKS_FS_LOCAL;
                    }
                }
            }
        }
    }
    fclose (fp);
    return info;
}

gboolean
tweaks_mount_is_remote (const gchar *path)
{
    if (!path)
        return FALSE;

    TweaksMountInfo *info = tweaks_mount_info_get_for_path (path);
    gboolean is_remote = (info && info->mode != TWEAKS_FS_LOCAL);
    tweaks_mount_info_free (info);
    return is_remote;
}

gchar *
tweaks_mount_translate_to_remote (const TweaksMountInfo *info, const gchar *local_path)
{
    if (!info || info->mode == TWEAKS_FS_LOCAL || !info->mount_point || !local_path)
        return g_strdup (local_path ? local_path : "");

    gsize mnt_len = strlen (info->mount_point);
    const gchar *subpath = local_path + mnt_len;
    while (*subpath == '/')
        subpath++;

    const gchar *base = info->remote_base_path;
    gchar *result = NULL;

    if (!base || g_strcmp0 (base, "/") == 0 || strlen (base) == 0)
    {
        if (strlen (subpath) == 0)
            result = g_strdup ("/");
        else
            result = g_strdup_printf ("/%s", subpath);
    }
    else
    {
        if (strlen (subpath) == 0)
            result = g_strdup (base);
        else
            result = g_build_filename (base, subpath, NULL);
    }

    if (result && result[0] != '/')
    {
        gchar *tmp = g_strdup_printf ("/%s", result);
        g_free (result);
        result = tmp;
    }

    return result;
}

gchar *
tweaks_remote_get_configured_remote_path (const gchar *target_host)
{
    if (!target_host || strlen (target_host) == 0)
        return NULL;

    const gchar *home = g_get_home_dir ();
    g_autofree gchar *ssh_cfg = g_build_filename (home, ".ssh", "config", NULL);
    FILE *fp = fopen (ssh_cfg, "r");
    if (!fp)
        return NULL;

    char line[1024];
    gchar *current_host = NULL;
    gchar *pending_path = NULL;
    gchar *result = NULL;

    while (fgets (line, sizeof (line), fp))
    {
        gchar *trimmed = g_strstrip (line);

        if (trimmed[0] == '#')
        {
            if (g_ascii_strncasecmp (trimmed, "# RemotePath:", 13) == 0)
            {
                const gchar *val = g_strstrip (trimmed + 13);
                if (current_host && g_strcmp0 (current_host, target_host) == 0)
                {
                    result = g_strdup (val);
                    break;
                }
                g_free (pending_path);
                pending_path = g_strdup (val);
            }
            continue;
        }

        if (g_ascii_strncasecmp (trimmed, "Host ", 5) == 0)
        {
            g_free (current_host);
            current_host = g_strdup (g_strstrip (trimmed + 5));

            if (current_host && g_strcmp0 (current_host, target_host) == 0 && pending_path)
            {
                result = g_strdup (pending_path);
                break;
            }
            g_clear_pointer (&pending_path, g_free);
        }
    }

    g_free (current_host);
    g_free (pending_path);
    fclose (fp);
    return result;
}

static gchar *
tweaks_remote_get_rclone_remote_path (const gchar *remote_name)
{
    if (!remote_name || strlen (remote_name) == 0)
        return NULL;

    const gchar *config_dir = g_get_user_config_dir ();
    g_autofree gchar *rclone_cfg = g_build_filename (config_dir, "rclone", "rclone.conf", NULL);
    g_autoptr (GKeyFile) keyfile = g_key_file_new ();
    if (g_key_file_load_from_file (keyfile, rclone_cfg, G_KEY_FILE_NONE, NULL))
    {
        gchar *rpath = g_key_file_get_string (keyfile, remote_name, "remote_path", NULL);
        if (rpath && strlen (g_strstrip (rpath)) > 0)
            return rpath;
        g_free (rpath);
    }
    return NULL;
}

gchar *
tweaks_remote_resolve_path (const gchar *local_path)
{
    if (!local_path)
        return NULL;

    TweaksMountInfo *info = tweaks_mount_info_get_for_path (local_path);
    if (!info || info->mode == TWEAKS_FS_LOCAL || !info->mount_point)
    {
        tweaks_mount_info_free (info);
        return NULL;
    }

    if (info->mode == TWEAKS_FS_SSHFS && info->ssh_host)
    {
        if (!info->remote_base_path || g_strcmp0 (info->remote_base_path, "/") == 0)
        {
            g_autofree gchar *cfg_remote = tweaks_remote_get_configured_remote_path (info->ssh_host);
            if (cfg_remote && strlen (cfg_remote) > 0)
            {
                g_free (info->remote_base_path);
                info->remote_base_path = g_steal_pointer (&cfg_remote);
            }
        }
    }
    else if (info->mode == TWEAKS_FS_RCLONE && info->ssh_host)
    {
        if (!info->remote_base_path || g_strcmp0 (info->remote_base_path, "/") == 0)
        {
            g_autofree gchar *cfg_remote = tweaks_remote_get_rclone_remote_path (info->ssh_host);
            if (cfg_remote && strlen (cfg_remote) > 0)
            {
                g_free (info->remote_base_path);
                info->remote_base_path = g_steal_pointer (&cfg_remote);
            }
        }
    }

    gchar *remote = tweaks_mount_translate_to_remote (info, local_path);
    tweaks_mount_info_free (info);
    return remote;
}

gboolean
tweaks_mount_is_path_mounted (const gchar *path)
{
    if (!path)
        return FALSE;

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

gboolean
tweaks_mount_is_inside_mount (const gchar *path)
{
    if (!path)
        return FALSE;

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

gboolean
tweaks_mount_has_submounts (const gchar *path)
{
    if (!path)
        return FALSE;

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

gboolean
tweaks_mount_is_server_mounted (const TweaksRemoteServer *server)
{
    if (!server || !server->name)
        return FALSE;

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
            if (server->is_rclone && (g_str_has_prefix (fstype, "fuse.rclone") || g_strcmp0 (fstype, "rclone") == 0))
            {
                g_autofree gchar *prefix = g_strdup_printf ("%s:", server->name);
                if (g_str_has_prefix (dev, prefix) || g_strcmp0 (dev, server->name) == 0)
                {
                    mounted = TRUE;
                    break;
                }
            }
            else if (!server->is_rclone && (g_str_has_prefix (fstype, "fuse.sshfs") || g_strcmp0 (fstype, "sshfs") == 0))
            {
                char *colon = strchr (dev, ':');
                if (colon)
                {
                    g_autofree gchar *host_part = g_strndup (dev, colon - dev);
                    if (g_strcmp0 (host_part, server->name) == 0 || g_str_has_suffix (host_part, server->name))
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
/* Remote Servers Discovery (~/.ssh/config & rclone.conf)                     */
/* -------------------------------------------------------------------------- */

void
tweaks_remote_server_free (TweaksRemoteServer *server)
{
    if (!server)
        return;
    g_free (server->name);
    g_free (server->type_label);
    g_free (server->host);
    g_free (server->user);
    g_free (server->remote_path);
    g_free (server);
}

TweaksRemoteServer *
tweaks_remote_server_copy (const TweaksRemoteServer *server)
{
    if (!server)
        return NULL;

    TweaksRemoteServer *copy = g_new0 (TweaksRemoteServer, 1);
    copy->name        = g_strdup (server->name);
    copy->type_label  = g_strdup (server->type_label);
    copy->host        = g_strdup (server->host);
    copy->user        = g_strdup (server->user);
    copy->port        = server->port;
    copy->remote_path = g_strdup (server->remote_path);
    copy->is_rclone   = server->is_rclone;
    return copy;
}

GList *
tweaks_remote_get_available_servers (void)
{
    GList *list = NULL;
    const gchar *home = g_get_home_dir ();
    const gchar *config_dir = g_get_user_config_dir ();

    /* 1. ~/.ssh/config (SFTP via sshfs) */
    g_autofree gchar *ssh_cfg = g_build_filename (home, ".ssh", "config", NULL);
    FILE *fp = fopen (ssh_cfg, "r");
    if (fp)
    {
        char line[1024];
        TweaksRemoteServer *cur = NULL;

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
                    cur = g_new0 (TweaksRemoteServer, 1);
                    cur->name       = g_strdup (host_name);
                    cur->type_label = g_strdup ("SFTP");
                    cur->is_rclone  = FALSE;

                    if (!tweaks_mount_is_server_mounted (cur))
                    {
                        list = g_list_append (list, cur);
                    }
                    else
                    {
                        tweaks_remote_server_free (cur);
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

    /* 2. ~/.config/rclone/rclone.conf (Only FTP servers) */
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

            if (!type || g_ascii_strcasecmp (type, "ftp") != 0)
            {
                g_free (type);
                continue;
            }
            g_free (type);

            TweaksRemoteServer *cur = g_new0 (TweaksRemoteServer, 1);
            cur->name       = g_strdup (group_name);
            cur->type_label = g_strdup ("FTP");
            cur->is_rclone  = TRUE;

            cur->host = g_key_file_get_string (keyfile, group_name, "host", NULL);
            cur->user = g_key_file_get_string (keyfile, group_name, "user", NULL);

            gint port_val = g_key_file_get_integer (keyfile, group_name, "port", NULL);
            cur->port = (port_val > 0) ? (guint) port_val : 0;

            gchar *rpath = g_key_file_get_string (keyfile, group_name, "remote_path", NULL);
            if (rpath && strlen (g_strstrip (rpath)) > 0)
                cur->remote_path = rpath;
            else
                g_free (rpath);

            if (!tweaks_mount_is_server_mounted (cur))
            {
                list = g_list_append (list, cur);
            }
            else
            {
                tweaks_remote_server_free (cur);
            }
        }
        g_strfreev (groups);
    }

    return list;
}

/* -------------------------------------------------------------------------- */
/* Universal Server Selection Dialog                                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    GtkWidget                    *window;
    GtkWidget                    *btn_connect;
    GList                        *servers;
    TweaksRemoteServer           *selected_server;
    TweaksServerSelectedCallback  callback;
    gpointer                      user_data;
} ServerChooserWidgets;

static void
on_chooser_row_selected (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    ServerChooserWidgets *d = (ServerChooserWidgets *) user_data;
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
on_chooser_connect_clicked (GtkButton *btn, gpointer user_data)
{
    ServerChooserWidgets *d = (ServerChooserWidgets *) user_data;
    if (!d->selected_server || !d->callback)
        return;

    TweaksRemoteServer *copy = tweaks_remote_server_copy (d->selected_server);
    TweaksServerSelectedCallback cb = d->callback;
    gpointer ud = d->user_data;

    gtk_window_destroy (GTK_WINDOW (d->window));
    cb (copy, ud);
}

static void
on_chooser_row_activated (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    on_chooser_connect_clicked (NULL, user_data);
}

static void
on_chooser_dialog_destroyed (gpointer data, GObject *where_the_object_was)
{
    ServerChooserWidgets *d = (ServerChooserWidgets *) data;
    g_active_chooser_window = NULL;

    g_list_free_full (d->servers, (GDestroyNotify) tweaks_remote_server_free);
    g_free (d);
}

void
tweaks_remote_show_server_chooser (GtkWindow                   *parent,
                                   const gchar                 *title,
                                   const gchar                 *target_label_text,
                                   gboolean                     only_sftp,
                                   TweaksServerSelectedCallback callback,
                                   gpointer                    user_data)
{
    if (g_active_chooser_window != NULL)
    {
        gtk_window_present (GTK_WINDOW (g_active_chooser_window));
        return;
    }

    GList *all_servers = tweaks_remote_get_available_servers ();
    GList *filtered = NULL;

    for (GList *l = all_servers; l != NULL; l = l->next)
    {
        TweaksRemoteServer *s = (TweaksRemoteServer *) l->data;
        if (only_sftp && s->is_rclone)
        {
            tweaks_remote_server_free (s);
        }
        else
        {
            filtered = g_list_append (filtered, s);
        }
    }
    g_list_free (all_servers);

    if (!filtered)
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

    ServerChooserWidgets *d = g_new0 (ServerChooserWidgets, 1);
    d->servers = filtered;
    d->callback = callback;
    d->user_data = user_data;

    d->window = gtk_window_new ();
    gtk_window_set_title (GTK_WINDOW (d->window), title ? title : _("Connect to Server"));
    gtk_window_set_default_size (GTK_WINDOW (d->window), 390, 420);
    gtk_window_set_resizable (GTK_WINDOW (d->window), FALSE);

    if (parent)
    {
        gtk_window_set_transient_for (GTK_WINDOW (d->window), parent);
        gtk_window_set_modal (GTK_WINDOW (d->window), TRUE);
    }

    g_active_chooser_window = d->window;
    g_object_weak_ref (G_OBJECT (d->window), on_chooser_dialog_destroyed, d);

    GtkWidget *main_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start (main_box, 16);
    gtk_widget_set_margin_end (main_box, 16);
    gtk_widget_set_margin_top (main_box, 16);
    gtk_widget_set_margin_bottom (main_box, 16);
    gtk_window_set_child (GTK_WINDOW (d->window), main_box);

    if (target_label_text && strlen (target_label_text) > 0)
    {
        GtkWidget *lbl_target = gtk_label_new (target_label_text);
        gtk_widget_set_halign (lbl_target, GTK_ALIGN_START);
        gtk_label_set_ellipsize (GTK_LABEL (lbl_target), PANGO_ELLIPSIZE_START);
        gtk_widget_add_css_class (lbl_target, "dim-label");
        gtk_box_append (GTK_BOX (main_box), lbl_target);
    }

    GtkWidget *lbl_title = gtk_label_new (_("Select server to connect:"));
    gtk_widget_set_halign (lbl_title, GTK_ALIGN_START);
    gtk_box_append (GTK_BOX (main_box), lbl_title);

    GtkWidget *scrolled = gtk_scrolled_window_new ();
    gtk_widget_set_vexpand (scrolled, TRUE);

    GtkWidget *list_box = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (list_box), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (list_box), FALSE);

    for (GList *l = filtered; l != NULL; l = l->next)
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
    g_signal_connect (d->btn_connect, "clicked", G_CALLBACK (on_chooser_connect_clicked), d);

    gtk_box_append (GTK_BOX (btn_box), btn_cancel);
    gtk_box_append (GTK_BOX (btn_box), d->btn_connect);
    gtk_box_append (GTK_BOX (main_box), btn_box);

    gtk_list_box_unselect_all (GTK_LIST_BOX (list_box));

    g_signal_connect (list_box, "row-selected", G_CALLBACK (on_chooser_row_selected), d);
    g_signal_connect (list_box, "row-activated", G_CALLBACK (on_chooser_row_activated), d);

    gtk_window_set_focus (GTK_WINDOW (d->window), btn_cancel);
    gtk_window_present (GTK_WINDOW (d->window));
}

/* -------------------------------------------------------------------------- */
/* SSH Command Execution & Options                                            */
/* -------------------------------------------------------------------------- */

const gchar *
tweaks_remote_get_sshfs_options (gboolean password_stdin)
{
    if (password_stdin)
    {
        return "reconnect,ServerAliveInterval=15,ServerAliveCountMax=3,"
               "follow_symlinks,StrictHostKeyChecking=accept-new,"
               "password_stdin";
    }

    return "reconnect,ServerAliveInterval=15,ServerAliveCountMax=3,"
           "follow_symlinks,StrictHostKeyChecking=accept-new";
}

GSubprocess *
tweaks_remote_ssh_spawn (const gchar       *host,
                         const gchar       *command,
                         guint              timeout_sec,
                         GSubprocessFlags   flags,
                         GError           **error)
{
    if (!host || !command)
    {
        g_set_error (error, G_SPAWN_ERROR, G_SPAWN_ERROR_FAILED, "Host or command cannot be NULL");
        return NULL;
    }

    g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (flags);
    g_subprocess_launcher_setenv (launcher, "LC_ALL", "C", TRUE);

    g_autofree gchar *timeout_opt = g_strdup_printf ("ConnectTimeout=%u", timeout_sec > 0 ? timeout_sec : 5);

    return g_subprocess_launcher_spawn (
        launcher,
        error,
        "ssh",
        "-o", "BatchMode=yes",
        "-o", "StrictHostKeyChecking=accept-new",
        "-o", timeout_opt,
        host,
        command,
        NULL
    );
}

gboolean
tweaks_remote_ssh_exec_sync (const gchar  *host,
                             const gchar  *command,
                             guint         timeout_sec,
                             gchar       **stdout_buf,
                             gchar       **stderr_buf,
                             gint         *exit_code,
                             GError      **error)
{
    g_autoptr (GSubprocess) proc = tweaks_remote_ssh_spawn (
        host,
        command,
        timeout_sec,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
        error
    );

    if (!proc)
        return FALSE;

    if (!g_subprocess_communicate_utf8 (proc, NULL, NULL, stdout_buf, stderr_buf, error))
        return FALSE;

    if (exit_code)
        *exit_code = g_subprocess_get_exit_status (proc);

    return g_subprocess_get_successful (proc);
}

gchar *
tweaks_remote_server_build_gvfs_uri (const TweaksRemoteServer *server)
{
    if (!server)
        return NULL;

    const gchar *subpath = server->remote_path ? server->remote_path : "";
    while (*subpath == '/')
        subpath++;

    if (!server->is_rclone)
    {
        /* SFTP: GVfs understands OpenSSH Host aliases natively */
        if (strlen (subpath) > 0)
            return g_strdup_printf ("sftp://%s/%s", server->name, subpath);
        else
            return g_strdup_printf ("sftp://%s/", server->name);
    }
    else
    {
        /* FTP: construct ftp://[user@]host[:port]/path */
        const gchar *host = (server->host && strlen (server->host) > 0) ? server->host : server->name;
        GString *uri = g_string_new ("ftp://");

        if (server->user && strlen (server->user) > 0)
        {
            g_autofree gchar *esc_user = g_uri_escape_string (server->user, NULL, TRUE);
            g_string_append_printf (uri, "%s@", esc_user);
        }

        g_string_append (uri, host);

        if (server->port > 0 && server->port != 21)
            g_string_append_printf (uri, ":%u", server->port);

        if (strlen (subpath) > 0)
            g_string_append_printf (uri, "/%s", subpath);
        else
            g_string_append_c (uri, '/');

        return g_string_free (uri, FALSE);
    }
}

gboolean
tweaks_remote_is_file_remote (GFile *location)
{
    if (!location)
        return FALSE;

    g_autofree gchar *uri = g_file_get_uri (location);
    if (uri && (g_str_has_prefix (uri, "sftp://") || 
                g_str_has_prefix (uri, "ftp://")  || 
                g_str_has_prefix (uri, "smb://")  ||
                g_str_has_prefix (uri, "dav://")  ||
                g_str_has_prefix (uri, "davs://")))
    {
        return TRUE;
    }

    g_autofree gchar *path = g_file_get_path (location);
    if (path)
    {
        if (g_str_has_prefix (path, "/run/user/") && strstr (path, "/gvfs/"))
            return TRUE;

        return tweaks_mount_is_remote (path);
    }

    return FALSE;
}

gchar *
tweaks_remote_resolve_location_path (GFile *location)
{
    if (!location)
        return NULL;

    g_autofree gchar *uri = g_file_get_uri (location);
    if (uri && (g_str_has_prefix (uri, "sftp://") || g_str_has_prefix (uri, "ftp://")))
    {
        g_autoptr (GUri) guri = g_uri_parse (uri, G_URI_FLAGS_NONE, NULL);
        if (guri)
        {
            const gchar *path_part = g_uri_get_path (guri);
            if (path_part && strlen (path_part) > 0)
                return g_strdup (path_part);
            return g_strdup ("/");
        }
    }

    g_autofree gchar *path = g_file_get_path (location);
    if (path)
    {
        /* Check if it's inside gvfs FUSE path */
        if (g_str_has_prefix (path, "/run/user/") && strstr (path, "/gvfs/"))
        {
            const gchar *gvfs_sub = strstr (path, "/gvfs/");
            const gchar *slash_after_mount = strchr (gvfs_sub + 6, '/');
            if (slash_after_mount)
                return g_strdup (slash_after_mount);
            return g_strdup ("/");
        }

        return tweaks_remote_resolve_path (path);
    }

    return NULL;
}

TweaksMountInfo *
tweaks_mount_info_get_for_location (GFile *location)
{
    if (!location)
        return g_new0 (TweaksMountInfo, 1);

    g_autofree gchar *uri = g_file_get_uri (location);
    if (uri)
    {
        if (g_str_has_prefix (uri, "sftp://"))
        {
            g_autoptr (GUri) guri = g_uri_parse (uri, G_URI_FLAGS_NONE, NULL);
            if (guri)
            {
                TweaksMountInfo *info = g_new0 (TweaksMountInfo, 1);
                info->mode = TWEAKS_FS_SSHFS;
                info->ssh_host = g_strdup (g_uri_get_host (guri));
                info->remote_base_path = g_strdup ("/");
                return info;
            }
        }
        else if (g_str_has_prefix (uri, "ftp://"))
        {
            TweaksMountInfo *info = g_new0 (TweaksMountInfo, 1);
            info->mode = TWEAKS_FS_RCLONE; /* Treat FTP as non-SSH remote */
            return info;
        }
    }

    g_autofree gchar *path = g_file_get_path (location);
    if (path)
    {
        /* Handle /run/user/1000/gvfs/ paths */
        if (g_str_has_prefix (path, "/run/user/") && strstr (path, "/gvfs/"))
        {
            if (strstr (path, "/sftp:"))
            {
                TweaksMountInfo *info = g_new0 (TweaksMountInfo, 1);
                info->mode = TWEAKS_FS_SSHFS;
                const gchar *host_start = strstr (path, "host=");
                if (host_start)
                {
                    host_start += 5;
                    const gchar *host_end = strpbrk (host_start, ",/");
                    if (host_end)
                        info->ssh_host = g_strndup (host_start, host_end - host_start);
                    else
                        info->ssh_host = g_strdup (host_start);
                }
                info->remote_base_path = g_strdup ("/");
                return info;
            }
            else
            {
                TweaksMountInfo *info = g_new0 (TweaksMountInfo, 1);
                info->mode = TWEAKS_FS_RCLONE; /* FTP / SMB */
                return info;
            }
        }

        return tweaks_mount_info_get_for_path (path);
    }

    return g_new0 (TweaksMountInfo, 1);
}