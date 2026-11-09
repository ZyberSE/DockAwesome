#include "dock_compose.h"
#include "dock_pixel.h"
#include <string.h>

/* dock_settings_valid only reads *s. Telling GCC so keeps the canvas fields
   dock_render_oriented_clip loaded before the call in registers, as when
   both lived in dock_core.c; without it the compositor's code changes. */
#ifdef __GNUC__
int dock_settings_valid(const DockSettings *s) __attribute__((pure));
#endif

/* Blend one translucent pixel exactly as blend_mask does. */
static void blend_pixel(unsigned char *d, const unsigned char *s, int pressed)
{
    uint32_t a = s[3], inv = 255 - a, t;
    if (!pressed) {
        /* Two channels per multiply in 16-bit lanes, each (v+127)/255
           without a divide. The alpha lane is reset below. */
        uint32_t sw = load32(s), dw = load32(d), lo, hi;
        lo = (sw & UINT32_C(0x00ff00ff))*a + (dw & UINT32_C(0x00ff00ff))*inv + UINT32_C(0x00800080);
        hi = ((sw>>8) & UINT32_C(0x00ff00ff))*a + ((dw>>8) & UINT32_C(0x00ff00ff))*inv + UINT32_C(0x00800080);
        lo = ((lo + ((lo>>8) & UINT32_C(0x00ff00ff))) >> 8) & UINT32_C(0x00ff00ff);
        hi = (hi + ((hi>>8) & UINT32_C(0x00ff00ff))) & UINT32_C(0xff00ff00);
        store32(d, lo | hi);
    } else {
        int c;
        for (c = 0; c < 3; ++c) {
            /* (v+127)/255 without a divide; exact for v <= 65025. */
            t = (s[c]*3u+2)/4 * a + d[c] * inv + 128;
            d[c] = (unsigned char)((t + (t >> 8)) >> 8);
        }
    }
    d[3] = 255;
}

/* The source rows and columns [left,right)x[top,bottom) that land inside
   clip, itself bounded by the destination. Returns 0 when nothing does. */
static int clip_source(const DockImage *dst, const DockImage *src, int x, int y,
                       const DockRect *clip, int *left, int *top, int *right, int *bottom)
{
    int64_t cl = clip->left > 0 ? clip->left : 0, ct = clip->top > 0 ? clip->top : 0;
    int64_t cr = clip->right < dst->width ? clip->right : dst->width;
    int64_t cb = clip->bottom < dst->height ? clip->bottom : dst->height;
    int64_t l = cl-x, t = ct-y, r = cr-x, b = cb-y;
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > src->width) r = src->width;
    if (b > src->height) b = src->height;
    if (l >= r || t >= b) return 0;
    *left = (int)l; *top = (int)t; *right = (int)r; *bottom = (int)b;
    return 1;
}

static void blend_mask(DockImage *dst, const DockImage *src, int x, int y,
                        unsigned char *coverage, int pressed, const DockRect *clip)
{
    int sx, sy, left, top;
    int right, bottom;
    if (!dst || !src || !dst->pixels || !src->pixels ||
        dst->width <= 0 || dst->height <= 0 || src->width <= 0 || src->height <= 0) return;
    if (!clip_source(dst,src,x,y,clip,&left,&top,&right,&bottom)) return;
    for (sy = top; sy < bottom; ++sy) {
        const unsigned char *s = src->pixels + ((size_t)sy * src->width + left) * 4;
        unsigned char *d = dst->pixels + ((size_t)(sy + y) * dst->width + left + x) * 4;
        unsigned char *m = coverage ? coverage + (size_t)(sy + y) * dst->width + left + x : NULL;
        sx = left;
        while (sx < right) {
            int run = sx;
            /* Transparent and opaque pixels come in runs: skip or copy them whole. */
            while (run < right && !s[3]) { ++run; s += 4; }
            d += (size_t)(run-sx)*4; if (m) m += run-sx;
            sx = run;
            if (!pressed) {
                while (run < right && s[(run-sx)*4+3] == 255) ++run;
                if (run > sx) {
                    size_t n = (size_t)(run-sx);
                    memcpy(d, s, n*4);
                    if (m) { memset(m, 255, n); m += n; }
                    s += n*4; d += n*4; sx = run;
                    continue;
                }
            }
            if (sx < right) {
                blend_pixel(d, s, pressed);
                if (m) *m++ = 255;
                s += 4; d += 4; ++sx;
            }
        }
    }
}

/* blend_mask with the image's precomputed runs: the same pixels, without
   scanning transparent ones. */
static void blend_runs(DockImage *dst, const DockImage *src, const uint16_t *runs,
                       int x, int y, unsigned char *coverage, int pressed, const DockRect *clip)
{
    int sy, left, top, right, bottom, h = src->height;
    if (!dst->pixels || dst->width <= 0 || dst->height <= 0) return;
    if (!clip_source(dst,src,x,y,clip,&left,&top,&right,&bottom)) return;
    for (sy = top; sy < bottom; ++sy) {
        const uint16_t *r = runs+h+1+(size_t)runs[sy]*2, *end = runs+h+1+(size_t)runs[sy+1]*2;
        const unsigned char *srow = src->pixels+(size_t)sy*src->width*4;
        size_t row = (size_t)(sy+y)*dst->width;
        for (; r < end; r += 2) {
            int a = r[0], b = a+(r[1] & ~RUN_OPAQUE), n;
            const unsigned char *s;
            unsigned char *d;
            if (a < left) a = left;
            if (b > right) b = right;
            if (a >= b) continue;
            /* Clipped, so a+x is inside the destination row. */
            n = b-a; s = srow+a*4; d = dst->pixels+(row+(size_t)(a+x))*4;
            if (coverage) memset(coverage+row+(size_t)(a+x),255,(size_t)n);
            if ((r[1] & RUN_OPAQUE) && !pressed)
                for (; n; --n, s += 4, d += 4) memcpy(d,s,4);
            else
                for (; n; --n, s += 4, d += 4) blend_pixel(d,s,pressed);
        }
    }
}

void dock_blend(DockImage *dst, const DockImage *src, int x, int y)
{
    DockRect full = {0,0,dst ? dst->width : 0,dst ? dst->height : 0};
    blend_mask(dst, src, x, y, NULL, 0, &full);
}

void dock_blend_masked(DockImage *dst, const DockImage *src, int x, int y, unsigned char *coverage)
{
    DockRect full = {0,0,dst ? dst->width : 0,dst ? dst->height : 0};
    blend_mask(dst, src, x, y, coverage, 0, &full);
}

/* Area coverage of a circular corner cell. Supersampling is confined to the
   boundary and cached by DockRenderer. All coordinates use integer sixteenths. */
static unsigned char corner_coverage(int x, int y, int radius, int inset)
{
    int r = radius - inset, dx = radius - x, dy = radius - y;
    int near = (dx - 1) * (dx - 1) + (dy - 1) * (dy - 1);
    int far = dx * dx + dy * dy;
    int sx, sy, covered = 0, rr = r * 16;
    if (r <= 0 || near >= r * r) return 0;
    if (far <= r * r) return 255;
    for (sy = 0; sy < 8; ++sy) for (sx = 0; sx < 8; ++sx) {
        int px = dx * 16 - (sx * 2 + 1);
        int py = dy * 16 - (sy * 2 + 1);
        if (px * px + py * py <= rr * rr) ++covered;
    }
    return (unsigned char)((covered * 255 + 32) / 64);
}

static void panel_shape_prepare(DockPanelShape *shape, int radius)
{
    int x, y;
    if (!shape || shape->radius == radius) return;
    for (y = 0; y < radius; ++y) for (x = 0; x < radius; ++x) {
        int index = y * DOCK_PANEL_MAX_RADIUS + x;
        shape->outer[index] = corner_coverage(x, y, radius, 0);
        shape->inner[index] = corner_coverage(x, y, radius, 1);
    }
    shape->radius = radius;
}

static void panel_corner(const DockPanelShape *shape, int x, int y, int radius,
                         unsigned int *outer, unsigned int *inner)
{
    if (shape) {
        int index = y * DOCK_PANEL_MAX_RADIUS + x;
        *outer = shape->outer[index]; *inner = shape->inner[index];
    } else {
        *outer = corner_coverage(x, y, radius, 0);
        *inner = corner_coverage(x, y, radius, 1);
    }
}

static void panel_edge(unsigned char *pixel, const unsigned char *background,
                       unsigned int coverage, unsigned int inside, unsigned int strength)
{
    static const unsigned char rim[3] = {238, 242, 248};
    unsigned int alpha = ((coverage-inside)*strength+127)/255;
    int c;
    if (coverage < 255) for (c = 0; c < 3; ++c)
        pixel[c] = (unsigned char)((pixel[c] * coverage + background[c] * (255 - coverage) + 127) / 255);
    if (alpha) for (c = 0; c < 3; ++c)
        pixel[c] = (unsigned char)((rim[c] * alpha + pixel[c] * (255 - alpha) + 127) / 255);
}

static void draw_divider(DockImage *canvas, const DockRect *panel,
    const DockPlacement *previous, const DockPlacement *last, int edge, int max_gap,
    const DockRect *clip)
{
    int bottom = edge == DOCK_EDGE_BOTTOM;
    int64_t gap_start = (int64_t)(bottom ? previous->x : previous->y)+previous->size;
    int64_t gap_end = bottom ? last->x : last->y;
    int start = bottom ? panel->top : panel->left;
    int end = bottom ? panel->bottom : panel->right;
    int inset = (end-start)/5;
    int cross;
    int64_t line;
    /* Leave a vacant drop slot on the ordinary-icon side of the divider. */
    if (gap_start < gap_end-max_gap) gap_start = gap_end-max_gap;
    line = gap_start+(gap_end-gap_start)/2;
    if (gap_end <= gap_start || line < (bottom ? panel->left : panel->top) ||
        line >= (bottom ? panel->right : panel->bottom)) return;
    if (line < (bottom ? clip->left : clip->top) || line >= (bottom ? clip->right : clip->bottom)) return;
    start += inset; end -= inset;
    if (start < (bottom ? clip->top : clip->left)) start = bottom ? clip->top : clip->left;
    if (end > (bottom ? clip->bottom : clip->right)) end = bottom ? clip->bottom : clip->right;
    for (cross = start; cross < end; ++cross) {
        int x = bottom ? (int)line : cross, y = bottom ? cross : (int)line;
        unsigned char *pixel = canvas->pixels+((size_t)y*canvas->width+x)*4;
        panel_edge(pixel,pixel,255,0,88);
    }
}

/* Pixels [first, first+count) of the canvas from the backdrop, or the flat fill. */
static void background_span(unsigned char *pixels, const unsigned char *backdrop,
                            const unsigned char *flat, size_t first, size_t count)
{
    if (!count) return;
    if (backdrop) memcpy(pixels+first*4, backdrop+first*4, count*4);
    else {
        size_t i, done;
        memcpy(pixels+first*4, flat, 4);
        /* Doubling copies: a few memcpy calls instead of one per pixel. */
        for (done = 1; done < count; done += i) {
            i = done < count-done ? done : count-done;
            memcpy(pixels+(first+done)*4, pixels+first*4, i*4);
        }
    }
}

static const unsigned char dock_panel_background[4] = {20, 24, 32, 255};

int dock_render_panel(const DockSettings *s, int edge, const DockPlacement *p, int count,
                      int width, int height, int center_panel, DockRect *out, int *out_radius)
{
    DockRect rect;
    int radius;
    if (!dock_edge_panel_rect(s, (DockEdge)edge, p, count, width, height, &rect)) return 0;
    if (center_panel) {
        /* End slots have no artwork to extend that side of the panel.
           Mirror the farther edge of the animated row about its center. */
        if (edge == DOCK_EDGE_BOTTOM) {
            if (rect.left > width-rect.right) rect.left = width-rect.right;
            else rect.right = width-rect.left;
        } else {
            if (rect.top > height-rect.bottom) rect.top = height-rect.bottom;
            else rect.bottom = height-rect.top;
        }
    }
    if (rect.right <= rect.left || rect.bottom <= rect.top) rect = (DockRect){0,0,0,0};
    radius = (edge == DOCK_EDGE_BOTTOM ? rect.bottom-rect.top : rect.right-rect.left)/4;
    if (radius > (rect.right - rect.left) / 2) radius = (rect.right - rect.left) / 2;
    if (radius > (rect.bottom-rect.top)/2) radius = (rect.bottom-rect.top)/2;
    if (radius > DOCK_PANEL_MAX_RADIUS) radius = DOCK_PANEL_MAX_RADIUS;
    if (radius < 0) radius = 0;
    *out = rect; *out_radius = radius;
    return 1;
}

/* Background (backdrop or flat fill) and zero coverage for columns [a,b) of row y. */
static void background_row(unsigned char *pixels, unsigned char *coverage, const unsigned char *backdrop,
                           const unsigned char *flat, int width, int y, int a, int b)
{
    size_t start = (size_t)y*width+a;
    if (a >= b) return;
    background_span(pixels, backdrop, flat, start, (size_t)(b-a));
    if (coverage) memset(coverage+start, 0, (size_t)(b-a));
}

/* Rows [top,bottom) outside the plate: one span when the clip spans the width. */
static void background_rows(unsigned char *pixels, unsigned char *coverage, const unsigned char *backdrop,
                            const unsigned char *flat, int width, int top, int bottom, int left, int right)
{
    int y;
    if (top >= bottom) return;
    if (!left && right == width) {
        size_t start = (size_t)width*top, n = (size_t)width*(bottom-top);
        background_span(pixels, backdrop, flat, start, n);
        if (coverage) memset(coverage+start, 0, n);
    } else for (y = top; y < bottom; ++y)
        background_row(pixels, coverage, backdrop, flat, width, y, left, right);
}

void dock_render_oriented_clip(DockImage *canvas, const DockImage *backdrop,
                 const DockCache *const *icons, const DockPlacement *p, int count,
                 const DockSettings *s, DockMixHalf mix_half, DockPanelShape *shape,
                 unsigned char *coverage, int pressed_item, int edge, int center_panel,
                 DockGlass *glass, const DockOverlay *overlays, int overlay_count,
                 const DockRect *clip)
{
    enum { RIM_STRENGTH = 52 };
    static const unsigned char flat_background[4] = {32, 36, 42, 255};
    int x, y, shelf_top, shelf_bottom, left, right, radius, i;
    int captured, width, height, first, last;
    DockRect rect, c;
    unsigned char *pixels;
    const unsigned char *back;
    if (!canvas || !canvas->pixels || canvas->width < 1 || canvas->height < 1 ||
        !dock_settings_valid(s) || count < 0 || count > DOCK_MAX_ITEMS ||
        (count && (!icons || !p)) || !clip) return;
    if (overlay_count < 0 || !overlays) overlay_count = 0;
    width = canvas->width; height = canvas->height; pixels = canvas->pixels;
    /* Every canvas and coverage write below stays inside c. */
    c.left = clip->left > 0 ? clip->left : 0; c.top = clip->top > 0 ? clip->top : 0;
    c.right = clip->right < width ? clip->right : width;
    c.bottom = clip->bottom < height ? clip->bottom : height;
    if (c.left >= c.right || c.top >= c.bottom) return;
    if (!dock_render_panel(s, edge, p, count, width, height, center_panel, &rect, &radius)) return;
    captured = backdrop && backdrop->pixels && backdrop->width == width && backdrop->height == height;
    if (!captured || !glass || !glass->image.pixels || !glass->ready ||
        glass->image.width != width || glass->image.height != height) glass = NULL;
    back = captured ? backdrop->pixels : NULL;
    left = rect.left; right = rect.right;
    shelf_top = rect.top; shelf_bottom = rect.bottom;
    /* The plate rows draw their own background around the plate; every
       other row gets the backdrop (or flat fill) and no coverage here. */
    background_rows(pixels, coverage, back, flat_background, width,
                    c.top, shelf_top < c.bottom ? shelf_top : c.bottom, c.left, c.right);
    background_rows(pixels, coverage, back, flat_background, width,
                    shelf_bottom > c.top ? shelf_bottom : c.top, c.bottom, c.left, c.right);
    panel_shape_prepare(shape, radius);
    if (!mix_half) mix_half = dock_mix_half_c;
    first = shelf_top > c.top ? shelf_top : c.top;
    last = shelf_bottom < c.bottom ? shelf_bottom : c.bottom;
    if (right > left && shelf_bottom > shelf_top) for (y = first; y < last; ++y) {
        int cy = y - shelf_top, row_inset = 0, px, center_left, center_right, a, b;
        unsigned int outer = 255, inner = 255;
        unsigned char *row = pixels + (size_t)y * width * 4;
        size_t start = (size_t)y*width;
        if (shelf_bottom - 1 - y < cy) cy = shelf_bottom - 1 - y;
        if (cy < radius) {
            while (row_inset < radius) {
                panel_corner(shape,row_inset,cy,radius,&outer,&inner);
                if (outer) break;
                ++row_inset;
            }
        }
        /* Outside the plate interior: background, no coverage. */
        background_row(pixels, coverage, back, flat_background, width, y,
                       c.left, left+row_inset < c.right ? left+row_inset : c.right);
        background_row(pixels, coverage, back, flat_background, width, y,
                       right-row_inset > c.left ? right-row_inset : c.left, c.right);
        a = left+row_inset > c.left ? left+row_inset : c.left;
        b = right-row_inset < c.right ? right-row_inset : c.right;
        if (coverage && a < b) memset(coverage+start+a, 255, (size_t)(b-a));
        if (glass) {
            /* The same bytes as the fill and mix below, mixed once per backdrop row. */
            unsigned char *g = glass->image.pixels+start*4;
            if (!glass->ready[y]) {
                for (x = 0; x < width; ++x) memcpy(g+x*4,dock_panel_background,4);
                mix_half(g,backdrop->pixels+start*4,(unsigned long)width);
                glass->ready[y] = 1;
            }
            if (a < b) memcpy(row+a*4, g+a*4, (size_t)(b-a)*4);
        } else if (a < b) {
            for (x = a; x < b; ++x) memcpy(row+x*4,dock_panel_background,4);
            if (captured) mix_half(row+a*4, backdrop->pixels+(start+a)*4, (unsigned long)(b-a));
        }
        center_left = left + (cy < radius ? radius : 1);
        center_right = right - (cy < radius ? radius : 1);
        for (px = left + row_inset; px < center_left; ++px) {
            int mirror = right - 1 - (px - left);
            int draw_left = px >= c.left && px < c.right;
            int draw_right = mirror != px && mirror >= c.left && mirror < c.right;
            if (!draw_left && !draw_right) continue;
            if (cy < radius) panel_corner(shape,px-left,cy,radius,&outer,&inner);
            else { outer = 255; inner = 0; }
            if (outer == 255 && inner == 255) continue;
            if (draw_left) panel_edge(row+px*4, captured ? back+(start+px)*4 : flat_background,
                outer,inner,RIM_STRENGTH);
            if (draw_right) panel_edge(row+mirror*4, captured ? back+(start+mirror)*4 : flat_background,
                outer,inner,RIM_STRENGTH);
        }
        if (!cy) {
            a = center_left > c.left ? center_left : c.left;
            b = center_right < c.right ? center_right : c.right;
            for (px = a; px < b; ++px) {
                unsigned char *edge_pixel = row+px*4;
                panel_edge(edge_pixel,edge_pixel,255,0,RIM_STRENGTH);
            }
        }
    }
    if (s->end_gap && count > 1)
        draw_divider(canvas,&rect,&p[count-2],&p[count-1],edge,s->gap+s->end_gap,&c);
    for (i = 0; i < count; ++i) {
        const DockImage *image = level_image(icons[i], p[i].level);
        if (!image) continue;
        if (icons[i]->runs[p[i].level])
            blend_runs(canvas, image, icons[i]->runs[p[i].level], p[i].x, p[i].y,
                       coverage, pressed_item == i+1, &c);
        else blend_mask(canvas, image, p[i].x, p[i].y, coverage, pressed_item == i+1, &c);
    }
    for (i = 0; i < overlay_count; ++i)
        if (overlays[i].image)
            blend_mask(canvas, overlays[i].image, overlays[i].x, overlays[i].y, coverage, 0, &c);
}

void dock_render_oriented_mask(DockImage *canvas, const DockImage *backdrop,
                 const DockCache *const *icons, const DockPlacement *p, int count,
                 const DockSettings *s, DockMixHalf mix_half, DockPanelShape *shape,
                 unsigned char *coverage, int pressed_item, int edge, int center_panel,
                 DockGlass *glass)
{
    DockRect full;
    if (!canvas) return;
    full = (DockRect){0, 0, canvas->width, canvas->height};
    dock_render_oriented_clip(canvas, backdrop, icons, p, count, s, mix_half, shape, coverage,
                              pressed_item, edge, center_panel, glass, NULL, 0, &full);
}
