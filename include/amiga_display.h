#ifndef AMIGA_DISPLAY_H
#define AMIGA_DISPLAY_H
#include "dock_geometry.h"
struct Screen;
struct Window;
struct BitMap;
struct RastPort;
/* OS-owned window uploads are isolated here; rendering never touches SAGA registers. */
typedef struct AmigaDisplay AmigaDisplay;
AmigaDisplay *amiga_display_open(struct Screen *screen, int width, int height);
void amiga_display_close(AmigaDisplay *display);
/* encode565 writes big-endian RGB565, used for direct uploads to RGB16
   bitmaps; NULL keeps the RGB24 upload. */
void amiga_display_set_converters(AmigaDisplay *display, DockConvertRGB24 encode, DockConvertRGB565 decode,
    DockConvertRGB565 encode565);
int amiga_display_capture(AmigaDisplay *display, DockImage *image, int x, int y);
/* Native RGB16 cache: -1 unsupported/changed bitmap, 0 equal, 1 copied.
   A valid check limits comparison in canvas coordinates. Damage still
   refreshes the entire cache. The caller owns width*height*2 bytes. */
int amiga_display_capture565(AmigaDisplay *display, unsigned char *cache,
    int x, int y, int force, const DockRect *check, DockBytesEqual equal);
/* Read the drag baseline from the catcher bitmap. The mask update reads
   native RGB16 through an unlayered RastPort on the supported SAGA screen.
   Its bounds are relative to the Dock canvas. Returns -1 on read failure. */
int amiga_display_drag_reference565(AmigaDisplay *display, struct BitMap *backing,
    unsigned char *reference, int visible_width);
int amiga_display_drag_mask565(AmigaDisplay *display, const unsigned char *reference,
    unsigned char *mask, int x, int y, int left, int top, int right, int bottom,
    DockBytesEqual equal);
int amiga_display_present(AmigaDisplay *display, struct Window *window, const DockImage *image);
/* With no background, the caller initializes the returned back buffer. */
struct RastPort *amiga_display_begin_frame(AmigaDisplay *display, const DockImage *background);
int amiga_display_capture_frame(AmigaDisplay *display, DockImage *image);
void amiga_display_end_frame(AmigaDisplay *display, struct Window *window);
/* Partial uploads are available on RTG. Indexed screens use a full repaint. */
int amiga_display_present_tile(AmigaDisplay *display, struct Window *window,
    const DockImage *image, int x, int y, int width, int height);
typedef struct { uint64_t conversion_ticks, upload_ticks; } AmigaDisplayTiming;
/* Optional stage measurements use a caller-owned monotonic clock; normal uploads do not read it. */
int amiga_display_present_profiled(AmigaDisplay *display, struct Window *window,
    const DockImage *image, uint64_t (*clock)(void), AmigaDisplayTiming *timing);
int amiga_display_depth(const AmigaDisplay *display);
#endif
