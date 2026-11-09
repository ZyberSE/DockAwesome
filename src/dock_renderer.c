#include "dock_renderer.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
static const DockPixelBackend portable = { "68k portable", dock_mix_half_c, dock_rgba_to_rgb24_c, dock_rgb565_to_rgba_c, dock_rgba_to_rgb565_c, dock_bytes_equal_c, dock_rgb565_key_c };
const DockPixelBackend *dock_backend_portable(void) { return &portable; }
#ifdef DOCK_AMMX
static const DockPixelBackend ammx = { "68080 AMMX", dock_mix_half_ammx, dock_rgba_to_rgb24_ammx, dock_rgb565_to_rgba_ammx, dock_rgba_to_rgb565_ammx, dock_bytes_equal_ammx, dock_rgb565_key_ammx };
const DockPixelBackend *dock_backend_ammx(void) { return &ammx; }
#endif

void dock_damage_clear(DockDamage *d)
{
    if (d) d->full = d->count = 0;
}

void dock_damage_set_full(DockDamage *d)
{
    if (d) { d->full = 1; d->count = 0; }
}

static int rects_intersect(const DockRect *a, const DockRect *b)
{
    return a->left < b->right && b->left < a->right && a->top < b->bottom && b->top < a->bottom;
}

void dock_damage_add(DockDamage *d, DockRect r, int width, int height)
{
    int i;
    if (!d || d->full) return;
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > width) r.right = width;
    if (r.bottom > height) r.bottom = height;
    if (r.left >= r.right || r.top >= r.bottom) return;
    /* The list never intersects itself, so only r can: absorb every rect it
       touches and rescan, since the grown r can reach earlier ones. */
    for (i = 0; i < d->count;) {
        const DockRect *o = &d->rect[i];
        if (!rects_intersect(&r,o)) { ++i; continue; }
        if (o->left < r.left) r.left = o->left;
        if (o->top < r.top) r.top = o->top;
        if (o->right > r.right) r.right = o->right;
        if (o->bottom > r.bottom) r.bottom = o->bottom;
        d->rect[i] = d->rect[--d->count];
        i = 0;
    }
    if (d->count >= DOCK_DAMAGE_MAX) { dock_damage_set_full(d); return; }
    d->rect[d->count++] = r;
}

void dock_damage_merge(DockDamage *dst, const DockDamage *src, int width, int height)
{
    int i;
    if (!dst || !src || dst->full) return;
    if (src->full) { dock_damage_set_full(dst); return; }
    for (i = 0; i < src->count && !dst->full; ++i) dock_damage_add(dst, src->rect[i], width, height);
}

unsigned long dock_convert565_damage(unsigned char *buffer, int stride,
    const DockImage *frame, const unsigned char *coverage,
    const DockDamage *damage, unsigned char *scratch,
    DockConvertRGB565 encode, DockKeyRGB565 key,
    DockCoverageMask mask, void *context)
{
    DockRect whole;
    unsigned long pixels = 0;
    int i, n, y, width;
    if (!buffer || !frame || !frame->pixels || frame->width < 1 || frame->height < 1 ||
        !damage || !scratch || !encode || !key) return 0;
    width = frame->width;
    whole = (DockRect){0, 0, width, frame->height};
    n = damage->full ? 1 : damage->count;
    for (i = 0; i < n; ++i) {
        const DockRect *r = damage->full ? &whole : &damage->rect[i];
        int left = r->left > 0 ? r->left & ~7 : 0;
        int right = r->right < width ? r->right : width;
        int top = r->top > 0 ? r->top : 0, bottom = r->bottom < frame->height ? r->bottom : frame->height;
        right = right+7 < width ? (right+7) & ~7 : width;
        if (left >= right) continue;
        for (y = top; y < bottom; ++y) {
            unsigned char *out = buffer+(size_t)y*stride+(size_t)left*2;
            unsigned long count = (unsigned long)(right-left);
            if (coverage) memcpy(scratch, coverage+(size_t)y*width+left, count);
            else memset(scratch, 255, count);
            if (mask) mask(context, y, left, right, scratch);
            encode(out, frame->pixels+((size_t)y*width+left)*4, count);
            key(out, scratch, count);
            pixels += count;
        }
    }
    return pixels;
}

static int clamp_int(int64_t v)
{
    return v < INT_MIN ? INT_MIN : v > INT_MAX ? INT_MAX : (int)v;
}

static DockRect image_rect(const DockImage *image, int x, int y)
{
    DockRect r = {x, y, clamp_int((int64_t)x+image->width), clamp_int((int64_t)y+image->height)};
    return r;
}

static int same_settings(const DockSettings *a, const DockSettings *b)
{
    return a->base_size == b->base_size && a->max_size == b->max_size && a->levels == b->levels &&
        a->gap == b->gap && a->padding == b->padding && a->cache_limit == b->cache_limit &&
        a->end_gap == b->end_gap && a->edge_padding == b->edge_padding;
}

/* The image a placement draws, or NULL when its level has none. */
static const DockImage *level_image(const DockCache *icon, int level)
{
    return icon && level >= 0 && level < DOCK_MAX_LEVELS && level < icon->levels ? &icon->image[level] : NULL;
}

/* Long-axis span [a,b) across the plate's cross extent. */
static void add_span(DockDamage *d, int bottom, const DockRect *panel, int64_t a, int64_t b,
                     int width, int height)
{
    DockRect r;
    if (bottom) r = (DockRect){clamp_int(a), panel->top, clamp_int(b), panel->bottom};
    else r = (DockRect){panel->left, clamp_int(a), panel->right, clamp_int(b)};
    dock_damage_add(d, r, width, height);
}

/* The long-axis extent of the last two slots, which bounds the divider. */
static void add_divider(DockDamage *d, int bottom, const DockRect *panel,
                        const DockPlacement *a, const DockPlacement *b, int width, int height)
{
    int64_t a0 = bottom ? a->x : a->y, b0 = bottom ? b->x : b->y;
    int64_t a1 = a0+a->size, b1 = b0+b->size;
    add_span(d, bottom, panel, a0 < b0 ? a0 : b0, a1 > b1 ? a1 : b1, width, height);
}

void dock_renderer_draw(DockRenderer *r, DockImage *canvas,
    const DockImage *backdrop, const DockCache *const *icons,
    const DockPlacement *p, int count, const DockSettings *s)
{
    DockGlass *glass = NULL;
    DockRenderState *o;
    DockDamage *d;
    DockRect panel, full;
    const DockOverlay *overlays;
    const unsigned char *back;
    int radius, width, height, bottom, overlay_count, i, n;
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
    o = &r->previous; d = &r->damage;
    dock_damage_clear(d);
    overlays = r->overlays; overlay_count = r->overlay_count;
    if (overlay_count < 0 || !overlays) overlay_count = 0;
    /* The same checks as dock_render_oriented_clip: when it draws nothing,
       the canvas is unchanged and the next draw starts over. */
    if (!canvas || !canvas->pixels || canvas->width < 1 || canvas->height < 1 ||
        !dock_settings_valid(s) || count < 0 || count > DOCK_MAX_ITEMS || (count && (!icons || !p)) ||
        !dock_render_panel(s, r->edge, p, count, canvas->width, canvas->height, r->center_panel,
            &panel, &radius)) { o->valid = 0; return; }
    width = canvas->width; height = canvas->height;
    bottom = r->edge == DOCK_EDGE_BOTTOM;
    back = backdrop && backdrop->pixels && backdrop->width == width && backdrop->height == height ?
        backdrop->pixels : NULL;
    if (!o->valid || o->canvas != canvas->pixels || o->width != width || o->height != height ||
        o->backdrop != back ||
        o->coverage != r->coverage || o->pixels != (const void *)r->pixels ||
        o->edge != (int)r->edge || o->center_panel != r->center_panel || o->count != count ||
        !same_settings(&o->settings, s) || o->radius != radius ||
        (bottom ? o->panel.top != panel.top || o->panel.bottom != panel.bottom :
                  o->panel.left != panel.left || o->panel.right != panel.right) ||
        overlay_count > DOCK_MAX_OVERLAYS) dock_damage_set_full(d);
    else {
        int old_low = bottom ? o->panel.left : o->panel.top, low = bottom ? panel.left : panel.top;
        int old_high = bottom ? o->panel.right : o->panel.bottom, high = bottom ? panel.right : panel.bottom;
        int panel_moved = old_low != low || old_high != high;
        /* A moved end changes its corners and rim up to radius pixels inward. */
        if (old_low != low)
            add_span(d, bottom, &panel, old_low < low ? old_low : low,
                     (int64_t)(old_low > low ? old_low : low)+radius+1, width, height);
        if (old_high != high)
            add_span(d, bottom, &panel, (int64_t)(old_high < high ? old_high : high)-radius-1,
                     old_high > high ? old_high : high, width, height);
        if (s->end_gap && count > 1 && (panel_moved ||
                memcmp(&o->placement[count-2], &p[count-2], sizeof(*p)) ||
                memcmp(&o->placement[count-1], &p[count-1], sizeof(*p)))) {
            add_divider(d, bottom, &panel, &o->placement[count-2], &o->placement[count-1], width, height);
            add_divider(d, bottom, &panel, &p[count-2], &p[count-1], width, height);
        }
        for (i = 0; i < count; ++i) {
            const DockImage *image = level_image(icons[i], p[i].level);
            if (memcmp(&o->placement[i], &p[i], sizeof(*p)) ||
                o->image[i] != (image ? image->pixels : NULL) ||
                o->stamp[i] != (image ? icons[i]->stamp : 0) ||
                o->pressed[i] != (r->pressed_item == i+1)) {
                dock_damage_add(d, o->icon[i], width, height);
                if (image) dock_damage_add(d, image_rect(image, p[i].x, p[i].y), width, height);
            }
        }
        n = overlay_count > o->overlay_count ? overlay_count : o->overlay_count;
        for (i = 0; i < n; ++i) {
            const DockOverlay *v = i < overlay_count ? &overlays[i] : NULL;
            if (v && i < o->overlay_count && v->image == o->overlay[i].image &&
                (v->image ? v->image->pixels : NULL) == o->overlay_pixels[i] &&
                v->x == o->overlay[i].x && v->y == o->overlay[i].y) continue;
            if (i < o->overlay_count) dock_damage_add(d, o->overlay_rect[i], width, height);
            if (v && v->image) dock_damage_add(d, image_rect(v->image, v->x, v->y), width, height);
        }
    }
    full = (DockRect){0, 0, width, height};
    n = d->full ? 1 : d->count;
    for (i = 0; i < n; ++i)
        dock_render_oriented_clip(canvas, backdrop, icons, p, count, s, r->pixels->mix_half,
            &r->panel, r->coverage, r->pressed_item, r->edge, r->center_panel, glass,
            overlays, overlay_count, d->full ? &full : &d->rect[i]);
    /* Keep this frame's inputs for the next comparison. */
    o->valid = overlay_count <= DOCK_MAX_OVERLAYS;
    o->canvas = canvas->pixels; o->width = width; o->height = height;
    o->backdrop = back;
    o->coverage = r->coverage; o->pixels = r->pixels;
    o->edge = r->edge; o->center_panel = r->center_panel; o->count = count;
    o->settings = *s; o->panel = panel; o->radius = radius;
    for (i = 0; i < count; ++i) {
        const DockImage *image = level_image(icons[i], p[i].level);
        o->placement[i] = p[i];
        o->image[i] = image ? image->pixels : NULL;
        o->stamp[i] = image ? icons[i]->stamp : 0;
        o->icon[i] = image ? image_rect(image, p[i].x, p[i].y) : (DockRect){0,0,0,0};
        o->pressed[i] = r->pressed_item == i+1;
    }
    o->overlay_count = o->valid ? overlay_count : 0;
    for (i = 0; i < o->overlay_count; ++i) {
        const DockImage *image = overlays[i].image;
        o->overlay[i] = overlays[i];
        o->overlay_pixels[i] = image ? image->pixels : NULL;
        o->overlay_rect[i] = image ? image_rect(image, overlays[i].x, overlays[i].y) : (DockRect){0,0,0,0};
    }
}

void dock_renderer_invalidate(DockRenderer *r)
{
    if (r) r->previous.valid = 0;
}

void dock_renderer_backdrop_changed(DockRenderer *r)
{
    if (!r) return;
    dock_renderer_invalidate(r);
    if (r->glass.ready) memset(r->glass.ready,0,(size_t)r->glass.image.height);
}

void dock_renderer_release(DockRenderer *r)
{
    if (!r) return;
    dock_renderer_invalidate(r);
    free(r->glass.image.pixels); free(r->glass.ready);
    memset(&r->glass,0,sizeof(r->glass));
}
