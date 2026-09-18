#include "tweaks-remote.h"
#include "tweaks-log.h"

#include <stdio.h>
#include <string.h>

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

    /* Ensure the path starts with '/' */
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

    /* SSHFS: check # RemotePath: from ~/.ssh/config */
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
    /* RCLONE: check remote_path from rclone.conf */
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

            /* Strict check: allow ONLY ftp from rclone; SFTP is handled via sshfs */
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

            /* Read custom remote_path if configured */
            gchar *rpath = g_key_file_get_string (keyfile, group_name, "remote_path", NULL);
            if (rpath && strlen (g_strstrip (rpath)) > 0)
            {
                cur->remote_path = rpath;
            }
            else
            {
                g_free (rpath);
            }

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