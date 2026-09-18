#ifndef TWEAKS_REMOTE_H
#define TWEAKS_REMOTE_H

#include <glib.h>
#include <gio/gio.h>

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
    gchar    *type_label;   /* "SFTP", "FTP", "WEBDAV", "S3", etc. */
    gchar    *remote_path;  /* Custom starting directory from # RemotePath: */
    gboolean  is_rclone;    /* TRUE for rclone, FALSE for sshfs */
} TweaksRemoteServer;

/* -------------------------------------------------------------------------- */
/* Mount Information and Path Helpers                                         */
/* -------------------------------------------------------------------------- */

TweaksMountInfo *tweaks_mount_info_get_for_path (const gchar *path);
void             tweaks_mount_info_free          (TweaksMountInfo *info);
gchar           *tweaks_mount_translate_to_remote (const TweaksMountInfo *info, const gchar *local_path);

gboolean         tweaks_mount_is_path_mounted    (const gchar *path);
gboolean         tweaks_mount_is_inside_mount    (const gchar *path);
gboolean         tweaks_mount_has_submounts      (const gchar *path);
gboolean         tweaks_mount_is_server_mounted  (const TweaksRemoteServer *server);

/* Resolves local path to remote server path taking into account RemotePath from ~/.ssh/config.
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

/* -------------------------------------------------------------------------- */
/* SSH Command Execution & Options                                            */
/* -------------------------------------------------------------------------- */

/* Returns default sshfs mount options string including performance caching */
const gchar *tweaks_remote_get_sshfs_options (gboolean password_stdin);

/* Spawn on-demand SSH command via GSubprocess with BatchMode */
GSubprocess *tweaks_remote_ssh_spawn (const gchar       *host,
                                      const gchar       *command,
                                      guint              timeout_sec,
                                      GSubprocessFlags   flags,
                                      GError           **error);

/* Synchronous on-demand SSH command execution helper */
gboolean tweaks_remote_ssh_exec_sync (const gchar  *host,
                                      const gchar  *command,
                                      guint         timeout_sec,
                                      gchar       **stdout_buf,
                                      gchar       **stderr_buf,
                                      gint         *exit_code,
                                      GError      **error);

#endif /* TWEAKS_REMOTE_H */