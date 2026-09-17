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
    gchar    *terminal;
    gchar    *editor;
    gboolean  shorten_home;
    gboolean  resolve_symlinks;

    /* Mount settings */
    gchar   **remote_dirs;
    gchar    *emblem;

    /* Permission settings */
    gchar   **extra_users;
    gchar   **extra_groups;

    /* Debug settings */
    gboolean  debug;
} TweaksConfig;

void          tweaks_i18n_init   (void);
TweaksConfig *tweaks_config_load (void);
void          tweaks_config_free (TweaksConfig *config);

#endif /* TWEAKS_CONFIG_H */