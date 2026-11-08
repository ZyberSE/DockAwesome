#ifndef DOCK_RENDERER_H
#define DOCK_RENDERER_H
#include "dock_core.h"
#include "dock_geometry.h"
/* Pixel backend owns no OS resources. Add kernels without touching layout or
   Workbench. AMMX selection is explicit: never execute a probe instruction. */
typedef struct { const char *name; DockMixHalf mix_half; DockConvertRGB24 rgba_to_rgb24;
    DockConvertRGB565 decode565, encode565; DockBytesEqual bytes_equal;
    DockKeyRGB565 key565; } DockPixelBackend;
/* Draws the frame only. The caller adds overlays (running dots, the dragged
   icon) and presents it afterwards. */
typedef struct {
    const DockPixelBackend *pixels;
    DockPanelShape panel; /* Zero-initialize the renderer before first draw. */
    unsigned char *coverage; /* Optional caller-owned width*height transport mask. */
    int pressed_item; /* One-based pressed item; zero means none. */
    int center_panel; /* Keep the panel around an empty end slot during a drag. */
    DockEdge edge;
    /* Plate rows mixed with the backdrop. Call dock_renderer_backdrop_changed
       whenever the backdrop pixels change; free with dock_renderer_release. */
    DockGlass glass;
} DockRenderer;
void dock_renderer_backdrop_changed(DockRenderer *renderer);
void dock_renderer_release(DockRenderer *renderer);
const DockPixelBackend *dock_backend_portable(void);
#ifdef DOCK_AMMX
const DockPixelBackend *dock_backend_ammx(void);
#endif
void dock_renderer_draw(DockRenderer *renderer, DockImage *canvas,
    const DockImage *backdrop, const DockCache *const *icons,
    const DockPlacement *placements, int count, const DockSettings *settings);
#endif
