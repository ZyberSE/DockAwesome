#ifndef AMIGA_FOLDER_H
#define AMIGA_FOLDER_H

#include <stddef.h>

typedef struct {
    char name[108];
    int drawer;
    int custom_icon;
} DockFolderEntry;

/* Returns a sorted snapshot. The caller frees *entries. Empty folders succeed. */
int amiga_folder_read(const char *path, DockFolderEntry **entries, size_t *count);

#endif
