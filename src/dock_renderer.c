#include "dock_renderer.h"
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
    /* dock_render_oriented_mask validates the canvas, settings and row. */
    if (!r || !r->pixels) return;
    dock_render_oriented_mask(canvas, backdrop, icons, p, count, s, r->pixels->mix_half,
                           &r->panel, r->coverage, r->pressed_item, r->edge, r->center_panel);
}
