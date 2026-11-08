#ifndef AMIGA_FOLDER_H
#define AMIGA_FOLDER_H

#include <stddef.h>

typedef struct {
    char name[108];
    int drawer;
    int custom_icon;
} DockFolderEntry;

/* DOS errors fail instead of opening volume requesters that block the main task. */
void *amiga_requesters_off(void);
void amiga_requesters_restore(void *previous);

/* Returns a sorted snapshot. The caller frees *entries. Empty folders succeed. */
int amiga_folder_read(const char *path, DockFolderEntry **entries, size_t *count);

#endif
