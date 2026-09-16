#ifndef TWEAKS_CONFIG_H
#define TWEAKS_CONFIG_H

#include <glib.h>

typedef struct {
    /* Настройки для действий */
    gchar    *terminal;
    gchar    *editor;
    gboolean  shorten_home;
    gboolean  resolve_symlinks;

    /* Настройки для монтирования */
    gchar   **remote_dirs;
    gchar    *emblem;

    /* Настройки для прав доступа */
    gchar   **extra_users;
    gchar   **extra_groups;
} TweaksConfig;

TweaksConfig *tweaks_config_load (void);
void          tweaks_config_free (TweaksConfig *config);

#endif /* TWEAKS_CONFIG_H */