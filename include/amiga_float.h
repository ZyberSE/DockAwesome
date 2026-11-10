#ifndef AMIGA_FLOAT_H
#define AMIGA_FLOAT_H
#include "dock_core.h"
struct Screen;
typedef struct AmigaFloat AmigaFloat;

/* One small picture over the screen in SAGA's PiP1 overlay, for what
   floats beside the dock without window shapes (a dragged pin, a label).
   PiP1 is leased and on only while something is shown; width and height
   at create set the least size it opens with, so pictures up to that size
   reuse it while shown.
   Pixels with alpha below one half show what lies under (the colour key);
   the others show their colour, so blend soft edges before showing. */
AmigaFloat *amiga_float_create(struct Screen *screen, int x_offset, int y_offset, int width, int height);
/* Show image with its top left at x, y (screen coordinates; what lies
   past the screen's edges is cut). Returns 0 when PiP1 is not available. */
int amiga_float_show(AmigaFloat *flt, const DockImage *image, int x, int y);
/* Move what is shown without redrawing it. */
int amiga_float_move(AmigaFloat *flt, int x, int y);
int amiga_float_shown(const AmigaFloat *flt);
void amiga_float_hide(AmigaFloat *flt);
void amiga_float_destroy(AmigaFloat *flt);
#endif
