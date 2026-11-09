#ifndef DOCK_COMPOSE_H
#define DOCK_COMPOSE_H
/* The compositor: plate, glass, divider and icon blending onto a canvas. */
#include "dock_core.h"
#include "dock_geometry.h"

#define DOCK_PANEL_MAX_RADIUS 80
/* Zero-initialize before use. Reuse corner masks while the radius stays fixed. */
typedef struct {
    int radius;
    unsigned char outer[DOCK_PANEL_MAX_RADIUS * DOCK_PANEL_MAX_RADIUS];
    unsigned char inner[DOCK_PANEL_MAX_RADIUS * DOCK_PANEL_MAX_RADIUS];
} DockPanelShape;

/* Blend RGBA into an opaque destination and clip to its bounds. */
void dock_blend(DockImage *dst, const DockImage *src, int x, int y);
void dock_blend_masked(DockImage *dst, const DockImage *src, int x, int y, unsigned char *coverage);

/* Canvas and backdrop use matching dimensions and opaque RGBA. The backdrop,
   mix_half and shape cache are optional. Optional coverage holds one byte
   per canvas pixel: the plate and visible artwork mark 255, all others 0.
   A one-based pressed item darkens its artwork only; zero is normal.
   edge is a DOCK_EDGE_* value; artwork remains upright. glass, when given
   with the backdrop's size, keeps plate rows (the plate color mixed 50% with
   the backdrop) between frames: a row is mixed once, then copied, until the
   caller clears ready[] because the backdrop changed. */
typedef struct { DockImage image; unsigned char *ready; } DockGlass;
void dock_render_oriented_mask(DockImage *canvas, const DockImage *backdrop,
                 const DockCache *const *icons, const DockPlacement *p, int count,
                 const DockSettings *s, DockMixHalf mix_half, DockPanelShape *shape,
                 unsigned char *coverage, int pressed_item, int edge, int center_panel,
                 DockGlass *glass);

/* An image blended over the frame after the icons, with coverage: running
   dots and the dragged icon. */
typedef struct { const DockImage *image; int x, y; } DockOverlay;

/* The plate as drawn: dock_edge_panel_rect with the center_panel mirror,
   and its corner radius. An empty plate is {0,0,0,0} with radius 0.
   Returns 0 when nothing is drawn. */
int dock_render_panel(const DockSettings *s, int edge, const DockPlacement *p, int count,
                      int width, int height, int center_panel, DockRect *rect, int *radius);
/* dock_render_oriented_mask plus overlays, writing canvas and coverage only
   inside clip, with the bytes a full render gives there. Glass rows are
   still mixed whole (into the glass cache only). */
void dock_render_oriented_clip(DockImage *canvas, const DockImage *backdrop,
                 const DockCache *const *icons, const DockPlacement *p, int count,
                 const DockSettings *s, DockMixHalf mix_half, DockPanelShape *shape,
                 unsigned char *coverage, int pressed_item, int edge, int center_panel,
                 DockGlass *glass, const DockOverlay *overlays, int overlay_count,
                 const DockRect *clip);
#endif
