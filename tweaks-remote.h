#ifndef TWEAKS_REMOTE_H
#define TWEAKS_REMOTE_H

#include <glib.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

/* -------------------------------------------------------------------------- */
/* Types and Data Structures                                                  */
/* -------------------------------------------------------------------------- */

typedef enum {
    TWEAKS_FS_LOCAL,
    TWEAKS_FS_SSHFS,
    TWEAKS_FS_RCLONE
} TweaksFsMode;

typedef struct {
    TweaksFsMode mode;
    gchar       *ssh_host;
    gchar       *remote_base_path;
    gchar       *mount_point;
} TweaksMountInfo;

typedef struct {
    gchar    *name;         /* Server / Host name */
    gchar    *type_label;   /* "SFTP", "FTP", etc. */
    gchar    *host;         /* Hostname or IP for FTP */
    gchar    *user;         /* Username for FTP */
    guint     port;         /* Port for FTP (0 if default) */
    gchar    *remote_path;  /* Custom starting directory */
    gboolean  is_rclone;    /* TRUE for rclone/FTP, FALSE for sshfs/SFTP */
} TweaksRemoteServer;

typedef void (*TweaksServerSelectedCallback) (TweaksRemoteServer *server, gpointer user_data);

/* -------------------------------------------------------------------------- */
/* Mount Information and Path Helpers                                         */
/* -------------------------------------------------------------------------- */

TweaksMountInfo *tweaks_mount_info_get_for_path (const gchar *path);
void             tweaks_mount_info_free          (TweaksMountInfo *info);
gchar           *tweaks_mount_translate_to_remote (const TweaksMountInfo *info, const gchar *local_path);
gchar           *tweaks_remote_server_build_gvfs_uri (const TweaksRemoteServer *server);

gboolean         tweaks_mount_is_path_mounted    (const gchar *path);
gboolean         tweaks_mount_is_inside_mount    (const gchar *path);
gboolean         tweaks_mount_has_submounts      (const gchar *path);
gboolean         tweaks_mount_is_server_mounted  (const TweaksRemoteServer *server);
gboolean         tweaks_mount_is_remote          (const gchar *path);

/* Resolves local path to remote server path taking into account RemotePath.
 * Returns newly-allocated remote path string, or NULL if path is not on a mounted remote server. */
gchar           *tweaks_remote_resolve_path      (const gchar *local_path);

/* Reads # RemotePath: for given Host from ~/.ssh/config if specified */
gchar           *tweaks_remote_get_configured_remote_path (const gchar *host_name);

/* -------------------------------------------------------------------------- */
/* Remote Servers Discovery (~/.ssh/config & rclone.conf)                     */
/* -------------------------------------------------------------------------- */

TweaksRemoteServer *tweaks_remote_server_copy (const TweaksRemoteServer *server);
void                tweaks_remote_server_free (TweaksRemoteServer *server);
GList              *tweaks_remote_get_available_servers (void);

/* Universal GTK4 server selection dialog */
void tweaks_remote_show_server_chooser (GtkWindow                   *parent,
                                        const gchar                 *title,
                                        const gchar                 *target_label_text,
                                        gboolean                     only_sftp,
                                        TweaksServerSelectedCallback callback,
                                        gpointer                     user_data);

/* -------------------------------------------------------------------------- */
/* SSH Command Execution & Options                                            */
/* -------------------------------------------------------------------------- */

const gchar *tweaks_remote_get_sshfs_options (gboolean password_stdin);

GSubprocess *tweaks_remote_ssh_spawn (const gchar       *host,
                                      const gchar       *command,
                                      guint              timeout_sec,
                                      GSubprocessFlags   flags,
                                      GError           **error);

gboolean tweaks_remote_ssh_exec_sync (const gchar  *host,
                                      const gchar  *command,
                                      guint         timeout_sec,
                                      gchar       **stdout_buf,
                                      gchar       **stderr_buf,
                                      gint         *exit_code,
                                      GError      **error);

#endif /* TWEAKS_REMOTE_H */