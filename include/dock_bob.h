#ifndef DOCK_BOB_H
#define DOCK_BOB_H
#include "dock_core.h"
#include "dock_geometry.h"
#include <stddef.h>

/* A picture drawn straight onto a screen with a save-under, as a blitter
   object: each move restores what lay under it and draws it at the new
   place in one write of their union, so there is no frame with the old
   place's background at the new one. The screen is reached through
   read/write callbacks of packed RGB rows (3 bytes a pixel). */
typedef struct {
    int (*read)(void *context, unsigned char *rgb, size_t stride, int x, int y, int w, int h);
    int (*write)(void *context, const unsigned char *rgb, size_t stride, int x, int y, int w, int h);
    void *context;
    int width, height;
} DockBobScreen;

typedef struct {
    DockBobScreen screen;
    DockImage picture;     /* RGBA, a private copy */
    unsigned char *saved;  /* RGB: the screen under drawn */
    unsigned char *work;   /* RGB scratch for the union */
    size_t saved_cap, work_cap;
    DockRect drawn;        /* on the screen; empty while hidden */
    DockRect exclude;      /* never drawn into (empty: none) */
} DockBob;

void dock_bob_init(DockBob *bob, const DockBobScreen *screen);
/* Draws picture (NULL keeps the current one) with its top left at x, y,
   cut at the screen's edges. Returns 0 on failure; the screen is then as
   it was with nothing drawn. */
int dock_bob_place(DockBob *bob, const DockImage *picture, int x, int y);
/* Leave rect alone: the picture is not drawn there (where something
   else shows it). Takes effect with the next place. */
void dock_bob_set_exclude(DockBob *bob, DockRect rect);
/* Restores the screen under the picture. */
int dock_bob_hide(DockBob *bob);
void dock_bob_free(DockBob *bob);

#endif
