#ifndef AMIGA_DROP_CATCHER_H
#define AMIGA_DROP_CATCHER_H
#include <stdint.h>
struct Screen;
struct Window;
struct Region;
struct BitMap;
typedef struct AmigaDropCatcher AmigaDropCatcher;

enum {
    AMIGA_DROP_STARTED = 1,
    AMIGA_DROP_RESTORED = 2,
    AMIGA_DROP_ERROR = 4,
    AMIGA_DROP_SCANNED = 8 /* Workbench desktop icon query attempted. */
};

/* Main-task API only. The caller owns normal_shape and keeps screen/window
   alive. Coordinates below are logical screen coordinates, NOT PiP offsets.
   This recognizes Workbench client-area drag candidates, not private icon
   state: background/lasso gestures can also qualify. Titlebar/border starts
   are excluded. No original input events are consumed or replayed. */
AmigaDropCatcher *amiga_drop_catcher_init(struct Screen *screen,
    struct Window *window, struct Region *normal_shape, int width, int height);
unsigned int amiga_drop_catcher_update(AmigaDropCatcher *catcher,
    uint32_t external_serial, struct Screen *external_screen,
    int down_x, int down_y, int external_held, int external_dragged,
    int release_known, int release_x, int release_y, uint16_t qualifiers,
    uint64_t now, uint32_t frequency);
/* Restore after replying to an accepted AppMessage, or on explicit cancel.
   The original UP must still be delivered to Workbench. A release inside or
   with ambiguous coordinates retains the catcher until acceptance or a 300ms
   deadline. Keep servicing the existing timer during that grace. */
unsigned int amiga_drop_catcher_accepted(AmigaDropCatcher *catcher);
unsigned int amiga_drop_catcher_cancel(AmigaDropCatcher *catcher);
int amiga_drop_catcher_active(const AmigaDropCatcher *catcher);
/* Immutable desktop pixels sampled before Workbench starts the drag. */
struct BitMap *amiga_drop_catcher_backing(const AmigaDropCatcher *catcher);
/* Call ONLY AFTER CloseWindow: the temporary Region must outlive its Layer,
   even if a shape operation failed. Does not dispose the caller's normal shape. */
void amiga_drop_catcher_destroy(AmigaDropCatcher *catcher);
#endif
