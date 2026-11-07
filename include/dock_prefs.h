#ifndef DOCK_PREFS_H
#define DOCK_PREFS_H

#include "dock_geometry.h"
#include <stdio.h>

#define DOCK_PREFS_PATH_CAP 512

typedef struct {
    int size, zoom, levels;
    int scale_percent; /* Zero when zoom is an explicit pixel size. */
    DockEdge edge;
    int snapshot, pip_mode, pip_x_offset, pip_y_offset;
    int count;
    char folder[DOCK_PREFS_PATH_CAP]; /* Empty disables the drawer shortcut. */
    char paths[DOCK_MAX_ITEMS][DOCK_PREFS_PATH_CAP];
} DockPrefs;

void dock_prefs_defaults(DockPrefs *prefs);
/* Read a version-1 JSON object. A failed read leaves prefs unchanged. */
int dock_prefs_read(FILE *file, DockPrefs *prefs);
/* Write a complete JSON object. The caller handles flushing and replacement. */
int dock_prefs_write(FILE *file, const DockPrefs *prefs);

#endif
