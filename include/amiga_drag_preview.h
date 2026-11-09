#ifndef AMIGA_DRAG_PREVIEW_H
#define AMIGA_DRAG_PREVIEW_H
#include "dock_core.h"
struct Screen;
struct Window;
typedef struct AmigaDragPreview AmigaDragPreview;

/* A transient native window extends the drag beyond the Dock's surface.
   Shaped, it shows the icon's outline over a matte drawn once; without
   shapes it keeps a save-under of the screen and redraws on each move.
   Intuition moves the window and restores what it uncovers. */
AmigaDragPreview *amiga_drag_preview_open(struct Screen *screen, const DockImage *icon,
    int shaped, int pointer_x, int pointer_y);
void amiga_drag_preview_move(AmigaDragPreview *preview, int pointer_x, int pointer_y);
int amiga_drag_preview_owns(const AmigaDragPreview *preview, const struct Window *window);
void amiga_drag_preview_close(AmigaDragPreview *preview);
#endif
