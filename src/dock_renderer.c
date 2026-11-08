#include "dock_renderer.h"
#include <stdlib.h>
#include <string.h>
static const DockPixelBackend portable = { "68k portable", dock_mix_half_c, dock_rgba_to_rgb24_c, dock_rgb565_to_rgba_c, dock_rgba_to_rgb565_c, dock_bytes_equal_c, dock_rgb565_key_c };
const DockPixelBackend *dock_backend_portable(void) { return &portable; }
#ifdef DOCK_AMMX
static const DockPixelBackend ammx = { "68080 AMMX", dock_mix_half_ammx, dock_rgba_to_rgb24_ammx, dock_rgb565_to_rgba_ammx, dock_rgba_to_rgb565_ammx, dock_bytes_equal_ammx, dock_rgb565_key_ammx };
const DockPixelBackend *dock_backend_ammx(void) { return &ammx; }
#endif
void dock_renderer_draw(DockRenderer *r, DockImage *canvas,
    const DockImage *backdrop, const DockCache *const *icons,
    const DockPlacement *p, int count, const DockSettings *s)
{
    DockGlass *glass = NULL;
    /* dock_render_oriented_mask validates the canvas, settings and row. */
    if (!r || !r->pixels) return;
    if (backdrop && backdrop->pixels && backdrop->width > 0 && backdrop->height > 0) {
        if (r->glass.image.width != backdrop->width || r->glass.image.height != backdrop->height) {
            dock_renderer_release(r);
            r->glass.image.pixels = malloc((size_t)backdrop->width*backdrop->height*4);
            r->glass.ready = calloc((size_t)backdrop->height,1);
            if (!r->glass.image.pixels || !r->glass.ready) dock_renderer_release(r);
            else { r->glass.image.width = backdrop->width; r->glass.image.height = backdrop->height; }
        }
        if (r->glass.ready) glass = &r->glass;
    }
    dock_render_oriented_mask(canvas, backdrop, icons, p, count, s, r->pixels->mix_half,
                           &r->panel, r->coverage, r->pressed_item, r->edge, r->center_panel, glass);
}

void dock_renderer_backdrop_changed(DockRenderer *r)
{
    if (r && r->glass.ready) memset(r->glass.ready,0,(size_t)r->glass.image.height);
}

void dock_renderer_release(DockRenderer *r)
{
    if (!r) return;
    free(r->glass.image.pixels); free(r->glass.ready);
    memset(&r->glass,0,sizeof(r->glass));
}
