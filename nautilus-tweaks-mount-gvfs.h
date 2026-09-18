#ifndef NAUTILUS_TWEAKS_MOUNT_GVFS_H
#define NAUTILUS_TWEAKS_MOUNT_GVFS_H

#include <glib-object.h>
#include <gmodule.h>

void  nautilus_tweaks_mount_gvfs_load (GTypeModule *module);
GType nautilus_tweaks_mount_gvfs_type (void);

#endif /* NAUTILUS_TWEAKS_MOUNT_GVFS_H */