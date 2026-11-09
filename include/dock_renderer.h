#ifndef DOCK_RENDERER_H
#define DOCK_RENDERER_H
#include "dock_core.h"
#include "dock_geometry.h"
/* Pixel backend owns no OS resources. Add kernels without touching layout or
   Workbench. AMMX selection is explicit: never execute a probe instruction. */
typedef struct { const char *name; DockMixHalf mix_half; DockConvertRGB24 rgba_to_rgb24;
    DockConvertRGB565 decode565, encode565; DockBytesEqual bytes_equal;
    DockKeyRGB565 key565; } DockPixelBackend;

/* Canvas regions that changed: a list of rectangles that never intersect,
   or the whole canvas. */
#define DOCK_DAMAGE_MAX 80
typedef struct { int full, count; DockRect rect[DOCK_DAMAGE_MAX]; } DockDamage;
void dock_damage_clear(DockDamage *d);
void dock_damage_set_full(DockDamage *d);
/* Clip to [0,width)x[0,height); ignore empty; merge with every rect it
   intersects into their bounding box, repeated until no two rects
   intersect; overflow sets full. */
void dock_damage_add(DockDamage *d, DockRect r, int width, int height);
void dock_damage_merge(DockDamage *dst, const DockDamage *src, int width, int height);

/* Converts the damaged regions of frame (RGBA) to keyed big-endian RGB565
   in buffer, or the whole frame when damage is full. Each row span widens
   to multiples of 8 pixels (clamped to the width) so the AMMX kernels stay
   on whole quads and octets; widened or overlapping spans are harmless
   because encode plus key is idempotent per pixel. The coverage span is
   copied to scratch (width bytes), mask may clear it, then key uses it:
   coverage itself is never written. A NULL coverage counts as covered.
   Returns the pixels converted, widening included; 0 when nothing is. */
typedef void (*DockCoverageMask)(void *context, int y, int left, int right,
                                 unsigned char *coverage_span);
unsigned long dock_convert565_damage(unsigned char *buffer, int stride,
    const DockImage *frame, const unsigned char *coverage,
    const DockDamage *damage, unsigned char *scratch,
    DockConvertRGB565 encode, DockKeyRGB565 key,
    DockCoverageMask mask, void *context);

/* An image blended over the frame after the icons, with coverage: running
   dots and the dragged icon. */
typedef struct { const DockImage *image; int x, y; } DockOverlay;
#define DOCK_MAX_OVERLAYS (DOCK_MAX_ITEMS+1)

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

/* The inputs of the last draw, compared field by field to find damage. */
typedef struct {
    int valid;
    const unsigned char *canvas, *backdrop, *coverage; /* backdrop: NULL when not captured. */
    const void *pixels;
    int width, height, edge, center_panel, count, radius, overlay_count;
    DockSettings settings;
    DockRect panel; /* Empty plates are stored as {0,0,0,0}. */
    DockPlacement placement[DOCK_MAX_ITEMS];
    const unsigned char *image[DOCK_MAX_ITEMS]; /* NULL when nothing is drawn. */
    DockRect icon[DOCK_MAX_ITEMS];
    unsigned char pressed[DOCK_MAX_ITEMS];
    DockOverlay overlay[DOCK_MAX_OVERLAYS];
    const unsigned char *overlay_pixels[DOCK_MAX_OVERLAYS];
    DockRect overlay_rect[DOCK_MAX_OVERLAYS];
} DockRenderState;

/* Draws the frame, including the overlays, and redraws only what changed
   since the last draw. damage holds the regions the last draw wrote. */
typedef struct {
    const DockPixelBackend *pixels;
    DockPanelShape panel; /* Zero-initialize the renderer before first draw. */
    unsigned char *coverage; /* Optional caller-owned width*height transport mask. */
    int pressed_item; /* One-based pressed item; zero means none. */
    int center_panel; /* Keep the panel around an empty end slot during a drag. */
    DockEdge edge;
    /* Set before each draw, like pressed_item; the array must stay valid
       until dock_renderer_draw returns. */
    const DockOverlay *overlays;
    int overlay_count;
    /* Plate rows mixed with the backdrop. Call dock_renderer_backdrop_changed
       whenever the backdrop pixels change; free with dock_renderer_release. */
    DockGlass glass;
    DockDamage damage;
    DockRenderState previous;
} DockRenderer;
/* The next draw is full. Call it whenever an icon cache is freed, replaced
   or rebuilt, or the item order or count changes: a rebuilt cache can reuse
   a freed address. The other two calls imply it. */
void dock_renderer_invalidate(DockRenderer *renderer);
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
