#ifndef AMIGA_FLOAT_H
#define AMIGA_FLOAT_H
#include "dock_core.h"
struct Screen;
typedef struct AmigaFloat AmigaFloat;

/* One small picture over the screen in SAGA's PiP1 overlay, for what
   floats beside the dock without window shapes (a dragged pin, a label).
   The overlay is leased only while something is shown. Pixels with alpha
   below one half show the screen through (the colour key); the others show
   their colour, so blend soft edges over the screen before showing. */
AmigaFloat *amiga_float_create(struct Screen *screen, int x_offset, int y_offset);
/* Show image with its top left at x, y (screen coordinates, clamped onto
   the screen). Returns 0 when PiP1 is not available. */
int amiga_float_show(AmigaFloat *flt, const DockImage *image, int x, int y);
/* Move what is shown without redrawing it. */
int amiga_float_move(AmigaFloat *flt, int x, int y);
int amiga_float_shown(const AmigaFloat *flt);
void amiga_float_hide(AmigaFloat *flt);
void amiga_float_destroy(AmigaFloat *flt);
#endif
