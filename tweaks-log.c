#include "tweaks-log.h"
#include "tweaks-config.h"

#include <stdio.h>
#include <stdarg.h>

static gboolean
is_debug_enabled (void)
{
    static gint64 last_check = 0;
    static gboolean cached_debug = FALSE;
    gint64 now = g_get_monotonic_time ();

    /* Cache for 2 seconds to avoid reading config from disk on every log call */
    if (now - last_check < 2 * G_USEC_PER_SEC)
        return cached_debug;

    last_check = now;
    TweaksConfig *cfg = tweaks_config_load ();
    if (cfg)
    {
        cached_debug = cfg->debug;
        tweaks_config_free (cfg);
    }
    return cached_debug;
}

/* -------------------------------------------------------------------------- */
/* Logging to ~/.config/nautilus-tweaks/debug.log                             */
/* -------------------------------------------------------------------------- */

void
log_debug (const gchar *format, ...)
{
    if (!is_debug_enabled ())
        return;

    const gchar *config_dir = g_get_user_config_dir ();
    g_autofree gchar *log_dir = g_build_filename (config_dir, "nautilus-tweaks", NULL);
    g_mkdir_with_parents (log_dir, 0755);
    g_autofree gchar *log_path = g_build_filename (log_dir, "debug.log", NULL);

    FILE *fp = fopen (log_path, "a");
    if (!fp)
        return;

    GDateTime *now = g_date_time_new_now_local ();
    g_autofree gchar *time_str = g_date_time_format (now, "%Y-%m-%d %H:%M:%S");
    g_date_time_unref (now);

    va_list args;
    va_start (args, format);
    g_autofree gchar *msg = g_strdup_vprintf (format, args);
    va_end (args);

    fprintf (fp, "[%s] %s\n", time_str, msg);
    fflush (fp);
    fclose (fp);
}