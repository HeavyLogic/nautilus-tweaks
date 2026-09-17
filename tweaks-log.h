#ifndef TWEAKS_LOG_H
#define TWEAKS_LOG_H

#include <glib.h>

void log_debug (const gchar *format, ...) G_GNUC_PRINTF (1, 2);

#endif /* TWEAKS_LOG_H */