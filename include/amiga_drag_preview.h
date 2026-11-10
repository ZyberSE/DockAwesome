#ifndef AMIGA_DRAG_PREVIEW_H
#define AMIGA_DRAG_PREVIEW_H
#include "dock_core.h"
struct Screen;
struct Window;
typedef struct AmigaDragPreview AmigaDragPreview;

/* A transient native window extends the drag beyond the Dock's surface.
   Shaped, it shows the icon's outline over a matte drawn once, with the
   "Remove" label only if opened with remove; without shapes it keeps a
   save-under of the screen and redraws on each move.
   Intuition moves the window and restores what it uncovers. */
AmigaDragPreview *amiga_drag_preview_open(struct Screen *screen, const DockImage *icon,
    int shaped, int remove, int pointer_x, int pointer_y);
/* Show or hide the "Remove" label (without shapes; a shaped preview is
   reopened instead). */
void amiga_drag_preview_set_remove(AmigaDragPreview *preview, int remove);
/* Returns 1 when the window moves (to where icon reports it). */
int amiga_drag_preview_move(AmigaDragPreview *preview, int pointer_x, int pointer_y);
/* The window's private copy of the icon and its top left on the screen
   once the last move lands, for the dock to draw the same picture in step. */
const DockImage *amiga_drag_preview_icon(const AmigaDragPreview *preview, int *x, int *y);
int amiga_drag_preview_owns(const AmigaDragPreview *preview, const struct Window *window);
void amiga_drag_preview_close(AmigaDragPreview *preview);
#endif
