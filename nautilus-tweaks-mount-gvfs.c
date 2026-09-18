#include "nautilus-tweaks-mount-gvfs.h"
#include "tweaks-config.h"
#include "tweaks-log.h"
#include "tweaks-remote.h"

#include <nautilus-extension.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <string.h>

typedef struct _NautilusTweaksMountGvfs {
    GObject parent_instance;
} NautilusTweaksMountGvfs;

typedef struct _NautilusTweaksMountGvfsClass {
    GObjectClass parent_class;
} NautilusTweaksMountGvfsClass;

static guint g_gvfs_action_counter = 0;

static void nautilus_tweaks_mount_gvfs_menu_provider_iface_init (NautilusMenuProviderInterface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (NautilusTweaksMountGvfs, nautilus_tweaks_mount_gvfs, G_TYPE_OBJECT, 0,
    G_IMPLEMENT_INTERFACE_DYNAMIC (NAUTILUS_TYPE_MENU_PROVIDER,
                                   nautilus_tweaks_mount_gvfs_menu_provider_iface_init))

static void nautilus_tweaks_mount_gvfs_class_init (NautilusTweaksMountGvfsClass *klass) {}
static void nautilus_tweaks_mount_gvfs_init (NautilusTweaksMountGvfs *self) {}
static void nautilus_tweaks_mount_gvfs_class_finalize (NautilusTweaksMountGvfsClass *klass) {}

/* -------------------------------------------------------------------------- */
/* Window helper                                                              */
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
                return GTK_WINDOW (w->data);
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Navigation callback                                                        */
/* -------------------------------------------------------------------------- */

static void
on_gvfs_server_selected (TweaksRemoteServer *server, gpointer user_data)
{
    if (!server)
        return;

    g_autofree gchar *uri = tweaks_remote_server_build_gvfs_uri (server);
    log_debug ("[GVFS] Connecting to URI: %s", uri);

    /* 1. Mount location via GVfs */
    g_autofree gchar *mount_cmd = g_strdup_printf ("gio mount \"%s\"", uri);
    g_autofree gchar *err_out = NULL;
    gint exit_status = 0;

    g_spawn_command_line_sync (mount_cmd, NULL, &err_out, &exit_status, NULL);

    /* Fuse safety check: if mount failed because of a hanging/stale connection, force-reset it once */
    if (exit_status != 0 && err_out)
    {
        log_debug ("[GVFS] gio mount returned code %d: %s", exit_status, err_out);
        if (strstr (err_out, "already mounted") || strstr (err_out, "busy") || strstr (err_out, "Error"))
        {
            log_debug ("[GVFS] Forcing cleanup of stale mount for %s", uri);
            g_autofree gchar *unmount_cmd = g_strdup_printf ("gio mount -u -f \"%s\"", uri);
            g_spawn_command_line_sync (unmount_cmd, NULL, NULL, NULL, NULL);

            /* Retry clean mount */
            g_spawn_command_line_sync (mount_cmd, NULL, NULL, NULL, NULL);
        }
    }

    /* 2. Open mounted location in Nautilus */
    const gchar *open_argv[] = { "nautilus", uri, NULL };
    g_spawn_async (NULL, (gchar **) open_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);

    tweaks_remote_server_free (server);
}

static void
on_open_server_activated (NautilusMenuItem *item, gpointer user_data)
{
    GtkWindow *parent = get_nautilus_active_window ();
    tweaks_remote_show_server_chooser (
        parent,
        _("Open Server"),
        NULL,
        FALSE, /* FALSE = show BOTH SFTP and FTP servers */
        on_gvfs_server_selected,
        NULL
    );
}

/* -------------------------------------------------------------------------- */
/* Menu Provider Interface Implementation                                     */
/* -------------------------------------------------------------------------- */

static GList *
nautilus_tweaks_mount_gvfs_get_file_items (NautilusMenuProvider *provider, GList *files)
{
    /* GVfs mode does not attach to individual folder items */
    return NULL;
}

static GList *
nautilus_tweaks_mount_gvfs_get_background_items (NautilusMenuProvider *provider,
                                                 NautilusFileInfo     *current_folder)
{
    TweaksConfig *config = tweaks_config_load ();
    gboolean is_gvfs = (g_ascii_strcasecmp (config->sftp_backend, "gvfs") == 0);
    tweaks_config_free (config);

    /* Early exit if gvfs mode is disabled */
    if (!is_gvfs)
        return NULL;

    GList *items = NULL;
    g_autofree gchar *item_id = g_strdup_printf ("NautilusTweaks::OpenServer_%u", ++g_gvfs_action_counter);

    NautilusMenuItem *menu_item = nautilus_menu_item_new (
        item_id,
        _("Open Server..."),
        _("Connect and open remote server via GVfs"),
        "network-server-symbolic"
    );

    g_signal_connect (menu_item, "activate", G_CALLBACK (on_open_server_activated), NULL);

    items = g_list_append (items, menu_item);
    return items;
}

static void
nautilus_tweaks_mount_gvfs_menu_provider_iface_init (NautilusMenuProviderInterface *iface)
{
    iface->get_file_items = nautilus_tweaks_mount_gvfs_get_file_items;
    iface->get_background_items = nautilus_tweaks_mount_gvfs_get_background_items;
}

void
nautilus_tweaks_mount_gvfs_load (GTypeModule *module)
{
    nautilus_tweaks_mount_gvfs_register_type (module);
}

GType
nautilus_tweaks_mount_gvfs_type (void)
{
    return nautilus_tweaks_mount_gvfs_get_type ();
}