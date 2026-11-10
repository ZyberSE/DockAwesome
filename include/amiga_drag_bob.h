#ifndef AMIGA_DRAG_BOB_H
#define AMIGA_DRAG_BOB_H
#include "dock_core.h"
struct Screen;
typedef struct AmigaDragBob AmigaDragBob;

/* A dragged pin drawn straight onto the screen's bitmap with a save-under
   (dock_bob.h), for RTG screens without window shapes. A window preview
   showed the old place's background until Intuition's queued MoveWindow
   landed; this draws and restores in one write at once. It takes no layer
   lock, so nothing can wait on it: a window that redraws under it is
   overwritten from the save-under until it redraws again. */
AmigaDragBob *amiga_drag_bob_open(struct Screen *screen);
/* Draw picture (NULL keeps the last) with its top left at x, y. Returns 0
   on failure: the screen is restored and the caller closes it. */
int amiga_drag_bob_place(AmigaDragBob *bob, const DockImage *picture, int x, int y);
/* Restore the screen and free. */
void amiga_drag_bob_close(AmigaDragBob *bob);
#endif
