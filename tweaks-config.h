#ifndef TWEAKS_CONFIG_H
#define TWEAKS_CONFIG_H

#include <glib.h>

/* Setup gettext domain for the whole project */
#ifndef GETTEXT_PACKAGE
#define GETTEXT_PACKAGE "nautilus-tweaks"
#endif

#include <glib/gi18n-lib.h>

typedef struct {
    /* Action settings */
    gchar    *ide;
    gboolean  shorten_home;
    gboolean  resolve_symlinks;
    gboolean  resolve_remotes;
    gchar    *root_editor_mode;   /* "tui" or "admin" */
    gchar    *root_editor_cmd;    /* Template for TUI, e.g. "ghostty -e sudo micro %f" */
    gchar    *root_editor_gui;    /* GUI editor for admin://, e.g. "gnome-text-editor" */

    /* Mount settings */
    gchar    *sftp_backend;       /* "gvfs" (native network) or "sshfs" (local folder mount) */
    gchar   **remote_dirs;
    gchar    *emblem;

    /* Debug settings */
    gboolean  debug;
} TweaksConfig;

void          tweaks_i18n_init   (void);
TweaksConfig *tweaks_config_load (void);
void          tweaks_config_free (TweaksConfig *config);

#endif /* TWEAKS_CONFIG_H */