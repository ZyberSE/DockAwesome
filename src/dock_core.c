#include "dock_core.h"
#include "dock_geometry.h"
#include "dock_pixel.h"
#include <stdlib.h>
#include <string.h>

DockSettings dock_settings_default(void)
{
    DockSettings s = {32, 64, 33, 5, 8, 16UL * 1024UL * 1024UL, 0, 0};
    return s;
}

int dock_settings_valid(const DockSettings *s)
{
    return s && s->base_size >= DOCK_MIN_ICON_SIZE && s->max_size >= s->base_size &&
        s->max_size <= DOCK_MAX_ICON_SIZE && s->levels >= 2 && s->levels <= DOCK_MAX_LEVELS &&
        s->gap >= 0 && s->gap <= 32 && s->padding >= 0 && s->padding <= 64 &&
        s->end_gap >= 0 && s->end_gap <= 32 && s->edge_padding >= 0 && s->edge_padding <= 32;
}

int dock_row_length(const DockSettings *s, int count)
{
    if (!count) return 0;
    return count*s->base_size+(count-1)*s->gap+(count > 1 ? s->end_gap : 0);
}

static int extra_gap(const DockSettings *s, int count, int left)
{
    return left >= 0 && left == count-2 ? s->end_gap : 0;
}

int dock_scaled_size(int size, int scale_percent)
{
    if (size < DOCK_MIN_ICON_SIZE || size > DOCK_MAX_ICON_SIZE ||
        scale_percent < DOCK_SCALE_ONE || scale_percent > DOCK_MAX_SCALE) return 0;
    return (size*scale_percent+DOCK_SCALE_ONE/2)/DOCK_SCALE_ONE;
}

static int level_size(const DockSettings *s, int level)
{
    if (s->levels < 2) return s->base_size;
    return s->base_size + (s->max_size - s->base_size) * level / (s->levels - 1);
}

/* Resting spacing is unchanged. Compact each enlarged pair by at most two
   pixels, using actual sizes so constrained layouts and animation agree. */
static int pair_gap(const DockSettings *s, int left_size, int right_size)
{
    int range = s->max_size-s->base_size;
    int reduction = s->gap < 2 ? s->gap : 2;
    int enlarged = left_size+right_size-2*s->base_size;
    if (!range || !reduction) return s->gap;
    return s->gap-(reduction*enlarged+range)/(2*range);
}

size_t dock_cache_bytes(const DockSettings *s)
{
    size_t bytes = 0;
    int i;
    if (!dock_settings_valid(s)) return 0;
    for (i = 0; i < s->levels; ++i) {
        size_t n = (size_t)level_size(s, i);
        bytes += n * n * 4;
    }
    return bytes;
}

int dock_items_fit(const DockSettings *s, int count, int viewport_length)
{
    return dock_settings_valid(s) && count >= 0 && count <= DOCK_MAX_ITEMS &&
        viewport_length >= 0 && dock_row_length(s,count) <= viewport_length-2*s->padding &&
        (!count || dock_cache_bytes(s) <= s->cache_limit/(size_t)count);
}

/* The Workbench worker task builds caches too. One addq cannot be split by
   a task switch, so the counter never goes back: two overlapping builds may
   share a stamp, but a build that reuses freed pixels starts after the
   owner's stamp was taken and so takes a later one. */
static unsigned long stamp_counter;

void dock_cache_restamp(DockCache *cache)
{
    unsigned long stamp;
    if (!cache) return;
    do {
#if defined(__GNUC__) && defined(__m68k__)
        __asm__ volatile ("addq.l #1,%0" : "+m"(stamp_counter));
        stamp = *(volatile unsigned long *)&stamp_counter;
#else
        stamp = ++stamp_counter;
#endif
    } while (!stamp);
    cache->stamp = stamp;
}

void dock_cache_free(DockCache *cache)
{
    int i;
    if (!cache) return;
    for (i = 0; i < DOCK_MAX_LEVELS; ++i) { free(cache->image[i].pixels); free(cache->runs[i]); }
    memset(cache, 0, sizeof(*cache));
}

/* Runs of nonzero alpha per row, split where opacity changes; see DockCache.
   Run indices and lengths fit 16 bits for images up to 255 px. */
static uint16_t *build_runs(const DockImage *im)
{
    uint16_t *runs = NULL;
    size_t count = 0;
    int pass, x, y, w = im->width, h = im->height;
    if (w < 1 || h < 1 || w > 255 || h > 255) return NULL;
    for (pass = 0; pass < 2; ++pass) {
        size_t n = 0;
        if (pass && !(runs = (uint16_t *)malloc(((size_t)h+1+count*2)*sizeof(uint16_t)))) return NULL;
        for (y = 0; y < h; ++y) {
            const unsigned char *row = im->pixels+(size_t)y*w*4;
            if (pass) runs[y] = (uint16_t)n;
            for (x = 0; x < w;) {
                int start = x, opaque = row[x*4+3] == 255;
                if (!row[x*4+3]) { ++x; continue; }
                while (x < w && row[x*4+3] && (row[x*4+3] == 255) == opaque) ++x;
                if (pass) {
                    runs[h+1+n*2] = (uint16_t)start;
                    runs[h+1+n*2+1] = (uint16_t)((x-start)|(opaque ? RUN_OPAQUE : 0));
                }
                ++n;
            }
        }
        if (pass) runs[h] = (uint16_t)n;
        count = n;
    }
    return runs;
}

/* Bicubic resampling (Keys, a = -0.75) in Q12. It keeps edges sharper than
   bilinear filtering and is exact at 1:1. Reductions widen the kernel by half
   the size ratio: enough to average fine detail without softening edges.
   On real 48 px icons this measured 6% (rest) and 10% (zoom) higher edge
   contrast than the smoothstep bilinear filter. */
enum { TAP_ONE = 4096, TAP_SHIFT = 12 };

/* 32-bit only: 64-bit math is a library call on the 68k. */
static int32_t div_round(int32_t value, int32_t divisor)
{
    return value >= 0 ? (value+divisor/2)/divisor : -((-value+divisor/2)/divisor);
}

/* Horner form in Q12; every intermediate stays below 2^31. Exact at the
   knots t = 0, 1 and 2, so 1:1 sampling is an identity. */
static int32_t bicubic(int32_t t)
{
    const int32_t one = TAP_ONE;
    int32_t v;
    if (t < 0) t = -t;
    if (t >= 2*one) return 0;
    /* Scaled by 4: (a+2)t^3-(a+3)t^2+1 and a(t^3-5t^2+8t-4) with a = -3/4. */
    if (t < one) v = (((5*t-9*one)*t>>TAP_SHIFT)*t>>TAP_SHIFT)+4*one;
    else v = ((((15*one-3*t)*t>>TAP_SHIFT)-24*one)*t>>TAP_SHIFT)+12*one;
    return div_round(v,4);
}

typedef struct {
    int cap, *count, *index;
    int32_t *weight;
} Taps;

static void taps_free(Taps *taps)
{
    free(taps->count); free(taps->index); free(taps->weight);
    memset(taps,0,sizeof(*taps));
}

/* Map `out` samples onto `in` pixels with the same centers as before:
   sample x reads source position (x+0.5)*in/out-0.5, clamped to the edges.
   Each index is stored premultiplied by `stride` as an element offset. */
static int taps_build(Taps *taps, int out, int in, int stride)
{
    int32_t m = out >= in ? out : out+(in-out)/2, limit = (in-1)*2*out;
    int x;
    taps->cap = 4*m/out+5;
    taps->count = (int *)malloc((size_t)out*sizeof(int));
    taps->index = (int *)malloc((size_t)out*taps->cap*sizeof(int));
    taps->weight = (int32_t *)malloc((size_t)out*taps->cap*sizeof(int32_t));
    if (!taps->count || !taps->index || !taps->weight) { taps_free(taps); return 0; }
    for (x = 0; x < out; ++x) {
        int32_t center = (2*x+1)*in-out, i, lo, hi;
        int *index = taps->index+(size_t)x*taps->cap;
        int32_t *weight = taps->weight+(size_t)x*taps->cap, sum = 0, total = 0;
        int k = 0, j, peak = 0;
        if (center < 0) center = 0;
        if (center > limit) center = limit;
        lo = (center-4*m)/(2*out)-1;
        hi = (center+4*m)/(2*out)+1;
        if (lo < 0) lo = 0;
        if (hi > in-1) hi = in-1;
        /* Taps outside the artwork are dropped and the rest renormalized,
           so wide reductions do not pile weight onto the edge pixel. */
        for (i = lo; i <= hi && k < taps->cap; ++i) {
            int32_t w = bicubic(div_round((2*out*i-center)*TAP_ONE,2*m));
            if (!w) continue;
            index[k] = (int)i;
            weight[k] = w;
            if (w > weight[peak]) peak = k;
            sum += w; ++k;
        }
        if (sum <= 0) { taps_free(taps); return 0; }
        for (j = 0; j < k; ++j) {
            weight[j] = div_round(weight[j]*TAP_ONE,sum);
            total += weight[j];
            index[j] *= stride;
        }
        weight[peak] += TAP_ONE-total;
        taps->count[x] = k;
    }
    return 1;
}

/* Q12 to integer, rounding half up. Right shifts of negative values are
   arithmetic on every compiler this project supports. */
#define TAP_ROUND(value) (((value)+TAP_ONE/2)>>TAP_SHIFT)

/* Premultiplied sums use 255-scaled alpha, so 1:1 output is exact. */
static void store_pixel(unsigned char *dst, int32_t r, int32_t g, int32_t b, int32_t a)
{
    int32_t alpha = TAP_ROUND(a), v[3];
    int c;
    v[0] = TAP_ROUND(r); v[1] = TAP_ROUND(g); v[2] = TAP_ROUND(b);
    if (alpha < 128) { memset(dst,0,4); return; }
    if (alpha >= 255*255) {
        /* Opaque: a constant divisor compiles to a multiply. */
        dst[3] = 255;
        for (c = 0; c < 3; ++c) {
            int32_t x = v[c] <= 0 ? 0 : (v[c]+127)/255;
            dst[c] = (unsigned char)(x > 255 ? 255 : x);
        }
        return;
    }
    dst[3] = (unsigned char)((alpha+127)/255);
    for (c = 0; c < 3; ++c) {
        int32_t x = v[c] <= 0 ? 0 : (v[c]*255+alpha/2)/alpha;
        dst[c] = (unsigned char)(x > 255 ? 255 : x);
    }
}

/* Resample premultiplied artwork into a w x h area of dst. For each output
   row, sum the weighted source rows into one int32 row, then filter across
   it. The unrolled four-tap sum (every enlarging row) is measurably faster
   on Vampire; a generic register loop was slower than streaming, and
   transparent-pixel skips and a reciprocal divide measured no gain. */

static void resample(unsigned char *dst, int stride, int w, int h,
    const uint16_t *art, int cw, const Taps *xt, const Taps *yt, int32_t *column)
{
    int x, y, k, elements = cw*4;
    for (y = 0; y < h; ++y) {
        const int *rows = yt->index+(size_t)y*yt->cap;
        const int32_t *rw = yt->weight+(size_t)y*yt->cap;
        unsigned char *out = dst+(size_t)y*stride;
        int count = yt->count[y];
        if (count == 4) {
            const uint16_t *a = art+rows[0], *b = art+rows[1], *c = art+rows[2], *d = art+rows[3];
            int32_t wa = rw[0], wb = rw[1], wc = rw[2], wd = rw[3];
            for (x = 0; x < elements; ++x)
                column[x] = TAP_ROUND(wa*a[x]+wb*b[x]+wc*c[x]+wd*d[x]);
        } else {
            memset(column,0,(size_t)elements*sizeof(int32_t));
            for (k = 0; k < count; ++k) {
                const uint16_t *src = art+rows[k];
                int32_t weight = rw[k], *acc = column, *end = column+elements;
                while (acc < end) *acc++ += weight*(int32_t)*src++;
            }
            for (x = 0; x < elements; ++x) column[x] = TAP_ROUND(column[x]);
        }
        for (x = 0; x < w; ++x) {
            const int *cols = xt->index+(size_t)x*xt->cap;
            const int32_t *cwt = xt->weight+(size_t)x*xt->cap;
            int32_t r = 0, g = 0, b = 0, a = 0;
            for (k = 0; k < xt->count[x]; ++k) {
                const int32_t *p = column+cols[k];
                int32_t weight = cwt[k];
                r += weight*p[0]; g += weight*p[1]; b += weight*p[2]; a += weight*p[3];
            }
            store_pixel(out+x*4,r,g,b,a);
        }
    }
}

enum { SOLID_ALPHA = 160, DARK_CHANNEL = 32 };

static int near_solid_pixel(const DockImage *image, int x, int y)
{
    int row, col;
    int left = x > 0 ? x-1 : 0;
    int top = y > 0 ? y-1 : 0;

    for (row = top; row <= y+1 && row < image->height; ++row) {
        const unsigned char *pixels = image->pixels+(size_t)row*image->width*4;
        for (col = left; col <= x+1 && col < image->width; ++col)
            if (pixels[col*4+3] >= SOLID_ALPHA) return 1;
    }
    return 0;
}

enum { SHADOW_BLOCK, SHADOW_OPEN, SHADOW_CANDIDATE, SHADOW_REACHED = 4 };

/* Clear dark translucent pixels away from solid artwork that are open to the
   outside: a detached drop shadow is cleared, while dark glass enclosed by
   solid pixels is kept. Runs once on the source so every cache level gets
   the same result. Returns 0 only when scratch memory is unavailable. */
static int clear_faint_shadows(DockImage *image)
{
    int w = image->width, h = image->height, x, y;
    size_t count = (size_t)w*h, i, head = 0, tail = 0;
    unsigned char *state;
    uint32_t *queue;

    for (i = 0; i < count && image->pixels[i*4+3] < SOLID_ALPHA; ++i) {}
    if (i == count) return 1;
    state = (unsigned char *)malloc(count);
    queue = (uint32_t *)malloc(count*sizeof(uint32_t));
    if (!state || !queue) { free(state); free(queue); return 0; }
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x) {
            const unsigned char *p = image->pixels+((size_t)y*w+x)*4;
            unsigned char kind = SHADOW_BLOCK;
            if (!p[3]) kind = SHADOW_OPEN;
            else if (p[3] < SOLID_ALPHA && p[0] <= DARK_CHANNEL && p[1] <= DARK_CHANNEL &&
                p[2] <= DARK_CHANNEL && !near_solid_pixel(image,x,y)) kind = SHADOW_CANDIDATE;
            state[(size_t)y*w+x] = kind;
            if (kind != SHADOW_BLOCK && (!x || !y || x == w-1 || y == h-1)) {
                state[(size_t)y*w+x] |= SHADOW_REACHED;
                queue[tail++] = (uint32_t)((size_t)y*w+x);
            }
        }
    while (head < tail) {
        uint32_t at = queue[head++];
        int px = (int)(at%(uint32_t)w), py = (int)(at/(uint32_t)w), k;
        static const int dx[] = {1, -1, 0, 0}, dy[] = {0, 0, 1, -1};
        for (k = 0; k < 4; ++k) {
            int nx = px+dx[k], ny = py+dy[k];
            size_t next;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            next = (size_t)ny*w+nx;
            if (state[next] == SHADOW_OPEN || state[next] == SHADOW_CANDIDATE) {
                state[next] |= SHADOW_REACHED;
                queue[tail++] = (uint32_t)next;
            }
        }
    }
    for (i = 0; i < count; ++i)
        if (state[i] == (SHADOW_CANDIDATE|SHADOW_REACHED)) memset(image->pixels+i*4,0,4);
    free(state); free(queue);
    return 1;
}

/* Bounds of the pixels with nonzero alpha; right < left when none. */
static void alpha_bounds(const DockImage *image, int *left, int *top, int *right, int *bottom)
{
    int x, y;
    *left = image->width; *top = image->height; *right = *bottom = -1;
    for (y = 0; y < image->height; ++y) {
        const unsigned char *p = image->pixels+(size_t)y*image->width*4+3;
        for (x = 0; x < image->width; ++x, p += 4) {
            if (!*p) continue;
            if (x < *left) *left = x;
            if (x > *right) *right = x;
            if (y < *top) *top = y;
            if (y > *bottom) *bottom = y;
        }
    }
}

static int cache_build(DockCache *out, const DockImage *source, const DockSettings *s, int fill);

int dock_cache_build_scaled(DockCache *out, const DockImage *source, const DockSettings *s, int fill)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (fill < 1 || fill > 256 || !dock_settings_valid(s) || dock_cache_bytes(s) > s->cache_limit)
        return 0;
    return cache_build(out, source, s, fill);
}

int dock_cache_build(DockCache *out, const DockImage *source, const DockSettings *s)
{
    return dock_cache_build_scaled(out, source, s, 256);
}

int dock_icon_build(DockImage *out, const DockImage *source, int size)
{
    DockSettings s = dock_settings_default();
    DockCache cache;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (size < DOCK_MIN_ICON_SIZE || size > DOCK_MAX_ICON_SIZE) return 0;
    s.base_size = s.max_size = size;
    s.levels = 1;
    memset(&cache, 0, sizeof(cache));
    if (!cache_build(&cache, source, &s, 256)) return 0;
    *out = cache.image[0];
    free(cache.runs[0]);
    return 1;
}

/* Settings are validated by the callers; one level is allowed here. */
static int cache_build(DockCache *out, const DockImage *source, const DockSettings *s, int fill)
{
    DockImage art = {0, 0, NULL};
    uint16_t *premultiplied = NULL;
    int32_t *column = NULL;
    Taps xt = {0, NULL, NULL, NULL}, yt = {0, NULL, NULL, NULL};
    int left, right, top, bottom, x, y, l, ok = 0, cw = 0, ch = 0;
    int previous_w = 0, previous_h = 0;
    if (!source || !source->pixels || source->width < 1 || source->height < 1 ||
        source->width > 4096 || source->height > 4096) return 0;
    alpha_bounds(source,&left,&top,&right,&bottom);
    if (right >= left) {
        art.width = right-left+1; art.height = bottom-top+1;
        art.pixels = (unsigned char *)malloc((size_t)art.width*art.height*4);
        if (!art.pixels) goto done;
        for (y = 0; y < art.height; ++y)
            memcpy(art.pixels+(size_t)y*art.width*4,
                source->pixels+((size_t)(y+top)*source->width+left)*4,(size_t)art.width*4);
        if (!clear_faint_shadows(&art)) goto done;
        /* Normalize the visible artwork: cleared shadows must not offset it. */
        alpha_bounds(&art,&left,&top,&right,&bottom);
        if (right >= left) {
            cw = right-left+1; ch = bottom-top+1;
            premultiplied = (uint16_t *)malloc((size_t)cw*ch*4*sizeof(uint16_t));
            column = (int32_t *)malloc((size_t)cw*4*sizeof(int32_t));
            if (!premultiplied || !column) goto done;
            for (y = 0; y < ch; ++y)
                for (x = 0; x < cw; ++x) {
                    const unsigned char *p = art.pixels+((size_t)(y+top)*art.width+x+left)*4;
                    uint16_t *q = premultiplied+((size_t)y*cw+x)*4;
                    q[0] = (uint16_t)(p[0]*p[3]);
                    q[1] = (uint16_t)(p[1]*p[3]);
                    q[2] = (uint16_t)(p[2]*p[3]);
                    q[3] = (uint16_t)(255*p[3]);
                }
        }
        free(art.pixels); art.pixels = NULL;
    }
    for (l = 0; l < s->levels; ++l) {
        DockImage *dst = &out->image[l];
        int n = level_size(s, l), w, h, ox, oy;
        dst->width = dst->height = n;
        dst->pixels = (unsigned char *)calloc((size_t)n * n, 4);
        if (!dst->pixels) goto done;
        out->bytes += (size_t)n * n * 4;
        out->levels = l + 1;
        if (!premultiplied) continue;
        /* Scale the normalized artwork and its padding together. A fixed
          2px inset at every level would turn28px into60px for32->64 boxes. */
        w = h = (((n*(s->base_size-2*DOCK_ICON_INSET))*fill>>8)+s->base_size/2)/s->base_size;
        if (cw > ch) h = (ch * w + cw / 2) / cw;
        else w = (cw * h + ch / 2) / ch;
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        ox = (n - w) / 2; oy = (n - h) / 2;
        if (w == previous_w && h == previous_h) {
            const DockImage *previous = &out->image[l-1];
            int px = (previous->width-w)/2, py = (previous->height-h)/2;
            for (y = 0; y < h; ++y)
                memcpy(dst->pixels+((size_t)(y+oy)*n+ox)*4,
                    previous->pixels+((size_t)(y+py)*previous->width+px)*4,(size_t)w*4);
            continue;
        }
        previous_w = w; previous_h = h;
        taps_free(&xt); taps_free(&yt);
        if (!taps_build(&xt,w,cw,4) || !taps_build(&yt,h,ch,cw*4)) goto done;
        resample(dst->pixels+((size_t)oy*n+ox)*4,n*4,w,h,premultiplied,cw,&xt,&yt,column);
    }
    for (l = 0; l < out->levels; ++l)
        if (!(out->runs[l] = build_runs(&out->image[l]))) goto done;
    /* After every allocation: a later build that reuses these addresses
       takes a later stamp. */
    dock_cache_restamp(out);
    ok = 1;
done:
    taps_free(&xt); taps_free(&yt);
    free(art.pixels); free(premultiplied); free(column);
    if (!ok) dock_cache_free(out);
    return ok;
}

/* Continuous slot geometry is separate from rounded cached image widths.
   Only the final box position is rounded. A neighboring cache-level change
   must not displace every icon further along the row. */
static void logical_positions(const DockSettings *s, int count, int width, int mx,
                               int start, const int *sizes, DockPlacement *out)
{
    int gaps[DOCK_MAX_ITEMS], positions[DOCK_MAX_ITEMS];
    int i, total = 0, range = s->max_size-s->base_size;
    int reduction = s->gap < 2 ? s->gap : 2;
    int64_t origin, mapped, relative = (int64_t)mx-start;
    int64_t focus = relative*256-s->base_size*128;
    int pitch = (s->base_size+s->gap)*256, shift = 0;
    int last_offset = dock_row_length(s,count)-s->base_size;
    for (i = 0; i < count; ++i) {
        total += sizes[i];
        gaps[i] = 0;
        if (i+1 < count) {
            gaps[i] = (s->gap+extra_gap(s,count,i))*256-
                reduction*(sizes[i]+sizes[i+1]-s->base_size*512)/(2*range);
            total += gaps[i];
        }
    }
    if (focus < 0) mapped = relative < 0 ? relative*256 : relative*sizes[0]/s->base_size;
    else if (focus > (int64_t)last_offset*256) {
        relative -= last_offset;
        mapped = total-sizes[count-1]+(relative < s->base_size ?
            relative*sizes[count-1]/s->base_size : sizes[count-1]+(relative-s->base_size)*256);
    } else {
        mapped = sizes[0]/2;
        for (i = 0; i+1 < count; ++i) {
            int span = (sizes[i]+sizes[i+1])/2+gaps[i];
            int rest_pitch = pitch+extra_gap(s,count,i)*256;
            if (focus <= rest_pitch) { mapped += focus*span/rest_pitch; break; }
            mapped += span; focus -= rest_pitch;
        }
    }
    origin = (int64_t)mx*256-mapped;
    if (origin < s->padding*256) origin = s->padding*256;
    if (origin > (int64_t)(width-s->padding)*256-total)
        origin = (int64_t)(width-s->padding)*256-total;
    for (i = 0; i < count; ++i) {
        positions[i] = (int)((origin+(sizes[i]-out[i].size*256)/2+128)/256);
        /* Zero/tiny custom gaps can leave overlapping rounded boxes. Repair
           those locally; normal gaps never need cumulative raster packing. */
        if (i && positions[i] < positions[i-1]+out[i-1].size)
            positions[i] = positions[i-1]+out[i-1].size;
        origin += sizes[i]+gaps[i];
    }
    /* Cache rounding may make an otherwise valid logical span one pixel too
       wide. Preserve the already bounded integer layout in that tight case. */
    if (positions[count-1]+out[count-1].size-positions[0] > width-s->padding*2) return;
    if (positions[0] < s->padding) shift = s->padding-positions[0];
    if (positions[count-1]+out[count-1].size+shift > width-s->padding)
        shift = width-s->padding-positions[count-1]-out[count-1].size;
    for (i = 0; i < count; ++i) out[i].x = positions[i]+shift;
}

/* Share of the full zoom (Q24) for an icon distance pixels from the
   pointer. The macOS Dock, measured at 2x, grows the icons 1, 2 and 3
   pitches away by 0.86, 0.48 and 0.04 of the zoom and the fourth not at all.
   A Catmull-Rom curve through those samples is smooth, and its weights sum
   to one at every phase, so the row's total growth never changes while the
   pointer moves and icons outside the wave stay still. It reaches 5 pitches
   and dips below zero there by at most 0.008 of the zoom. offset is the
   pointer's distance in pitches, Q16. */
static int32_t wave_weight(int32_t offset)
{
    static const int32_t sample[7] = {256, 220, 123, 10, 0, 0, 0}; /* Q8, by pitch */
    int32_t q = offset < 0 ? -offset : offset, f = (q & 65535) >> 4, f2, f3;
    int j = q >> 16;
    if (j > 4) return 0;
    /* Catmull-Rom basis for samples j-1 .. j+2, Q12 phase, Q24 weights. */
    f2 = f*f; f3 = (f2 >> 12)*f;
    return sample[j ? j-1 : 1]*((-f3+2*f2-(f<<12))/2/256)+
        sample[j]*((3*f3-5*f2+(INT32_C(2)<<24))/2/256)+
        sample[j+1]*((-3*f3+4*f2+(f<<12))/2/256)+sample[j+2]*((f3-f2)/2/256);
}

/* Item i's size plus the gaps to its neighbors: its share of the row's total. */
static int slot_span(const DockSettings *s, const DockPlacement *p, int count, int i)
{
    int span = p[i].size;
    if (i) span += pair_gap(s, p[i-1].size, p[i].size);
    if (i+1 < count) span += pair_gap(s, p[i].size, p[i+1].size);
    return span;
}

int dock_layout(const DockSettings *s, int count, int width, int height,
                int mx, int my, int active, DockPlacement *out)
{
    int i, total, rest, start, pitch, x, panel_top, panel_bottom, baseline, constrained = 0;
    int logical_size[DOCK_MAX_ITEMS];
    int32_t slot;
    if (!dock_settings_valid(s) || !out || count < 0 || count > DOCK_MAX_ITEMS ||
        width < 1 || height < s->max_size + s->padding * 2) return 0;
    if (!count) return 1;
    pitch = s->base_size + s->gap;
    rest = dock_row_length(s,count);
    if (rest > width - s->padding * 2) return 0;
    start = (width - rest) / 2;
    dock_panel_cross_bounds(s,height,&panel_top,&panel_bottom);
    /* Center the resting box between the panel edges, then keep its baseline at every size. */
    baseline = panel_top+(panel_bottom-panel_top+s->base_size)/2;
    if (baseline < s->max_size) baseline = s->max_size;
    if (!active || mx < 0 || mx >= width || my < 0 || my >= height) {
        int y = baseline-s->base_size;
        for (i = 0, x = start; i < count; ++i, x += pitch)
            out[i] = (DockPlacement){x+extra_gap(s,count,i-1), y, s->base_size, 0};
        return 1;
    }
    total = count > 1 ? s->end_gap : 0;
    {
        /* The pointer in resting slots (Q16), with the wider end-gap pitch
           counting as one slot, floored once: every icon sees the same phase,
           as logical_positions maps the pointer between the same centers. */
        int focus = mx-(start+s->base_size/2), reach = 6*pitch;
        if (focus < -reach) focus = -reach;
        slot = 0;
        for (i = 0; focus >= 0 && i+1 < count; ++i) {
            int span = pitch+extra_gap(s,count,i);
            if (focus < span) break;
            focus -= span; slot += 65536;
        }
        if (focus > reach) focus = reach;
        slot += focus >= 0 ? focus*65536/(pitch+(i+1 < count ? extra_gap(s,count,i) : 0)) :
            -((-focus*65536+pitch-1)/pitch);
    }
    for (i = 0; i < count; ++i) {
        int level = 0;
        int32_t weight = 0;
        if (s->max_size > s->base_size) weight = wave_weight(slot-i*65536);
        if (weight > 0)
            level = (int)(((uint32_t)(s->levels-1)*(uint32_t)weight + (UINT32_C(1)<<23)) >> 24);
        out[i].level = level;
        out[i].size = level_size(s, level);
        /* The slightly negative tail stays in the logical size, so the row's
           total growth is the same at every pointer position. */
        logical_size[i] = s->base_size*256+
            (int)(((s->max_size-s->base_size)*(weight/16)+2048)/4096);
        total += out[i].size;
    }
    for (i = 0; i+1 < count; ++i)
        total += pair_gap(s, out[i].size, out[i+1].size);
    /* Bound expansion to the window without changing resting icon dimensions. */
    while (total > width - s->padding * 2) {
        int largest = -1;
        constrained = 1;
        for (i = 0; i < count; ++i)
            if (out[i].level && (largest < 0 || out[i].level > out[largest].level)) largest = i;
        if (largest < 0) return 0;
        total -= slot_span(s, out, count, largest);
        --out[largest].level;
        out[largest].size = level_size(s, out[largest].level);
        total += slot_span(s, out, count, largest);
    }
    {
        int relative = mx-start, mapped = 0;
        if (count > 1 && relative >= (s->base_size+1)/2 &&
            relative <= rest-s->base_size+s->base_size/2) {
            int focus2 = relative*2-s->base_size;
            int mapped2 = out[0].size;
            /* Map one continuous center-to-center pitch. Mapping a growing
               icon and a shrinking gap separately reverses row motion at
               every gap. Doubled centers retain odd-size half pixels until
               the final rounding, without changing cached artwork sizes. */
            for (i = 0; i+1 < count; ++i) {
                int extra = extra_gap(s,count,i);
                int pitch2 = 2*(pitch+extra);
                int span2 = out[i].size+out[i+1].size+
                    2*(pair_gap(s, out[i].size, out[i+1].size)+extra);
                if (focus2 <= pitch2) {
                    mapped2 += focus2*span2/pitch2;
                    break;
                }
                mapped2 += span2; focus2 -= pitch2;
            }
            mapped = (mapped2+1)/2;
        } else {
            /* Preserve the existing outer-half and outside-row behavior,
               including the normalized anchor at either end of the Dock. */
            for (i = 0; i < count; ++i) {
                if (relative < s->base_size) {
                    mapped += relative < 0 ? relative : relative*out[i].size/s->base_size;
                    break;
                }
                relative -= s->base_size; mapped += out[i].size;
                if (i == count-1) { mapped += relative; break; }
                relative -= s->gap+extra_gap(s,count,i);
                mapped += pair_gap(s, out[i].size, out[i+1].size)+extra_gap(s,count,i);
            }
        }
        x = mx-mapped;
        if (x < s->padding) x = s->padding;
        if (x > width-s->padding-total) x = width-s->padding-total;
    }
    for (i = 0; i < count; ++i) {
        out[i].x = x;
        out[i].y = baseline-out[i].size;
        x += out[i].size;
        if (i+1 < count) x += pair_gap(s, out[i].size, out[i+1].size)+extra_gap(s,count,i);
    }
    if (!constrained && s->max_size > s->base_size)
        logical_positions(s, count, width, mx, start, logical_size, out);
    return 1;
}

static int ease(int from, int to)
{
    int delta = to - from;
    if (delta > 0) return from + (delta + 2) / 3;
    if (delta < 0) return from - (-delta + 2) / 3;
    return from;
}

static int64_t ease_center(int64_t from, int64_t to)
{
    int64_t delta = to-from, distance = delta < 0 ? -delta : delta;
    int64_t step = ((distance+767)/768)*256;
    /* Retain the old one-pixel minimum correction. A 1/256px minimum would
       add a long visible tail without providing subpixel raster rendering. */
    if (step > distance) step = distance;
    return delta < 0 ? from-step : from+step;
}

void dock_animation_reset(DockAnimation *animation)
{
    if (animation) memset(animation, 0, sizeof(*animation));
}

/* Both the previous row and the target row lie inside the surface, so keep
   the new row within their span. Eased centers can move the first icon
   before the start when its size jumps, and the overlap push can carry the
   last one past the end: shift the row, then pack from the violated end. */
static void keep_row_inside(DockAnimation *animation, DockPlacement *current,
    int count, int start, int end)
{
    int last = count-1, i, shift, over, under;
    under = start-current[0].x;
    if (under > 0) {
        shift = end-(current[last].x+current[last].size);
        if (shift > under) shift = under;
        if (shift < 0) shift = 0;
        for (i = 0; i < count; ++i) current[i].x += shift;
        for (i = 0; i < count; ++i) {
            int limit = i ? current[i-1].x+current[i-1].size : start;
            if (current[i].x < limit) current[i].x = limit;
        }
    }
    over = current[last].x+current[last].size-end;
    if (over > 0) {
        shift = current[0].x-start;
        if (shift > over) shift = over;
        if (shift < 0) shift = 0;
        for (i = 0; i < count; ++i) current[i].x -= shift;
        for (i = last; i >= 0; --i) {
            int limit = i == last ? end : current[i+1].x;
            if (current[i].x+current[i].size > limit) {
                current[i].x = limit-current[i].size;
                if (current[i].x < start) current[i].x = start;
            }
        }
    }
    if (under <= 0 && over <= 0) return;
    for (i = 0; i < count; ++i)
        animation->center[i] = (int64_t)current[i].x*256+current[i].size*128;
}

int dock_animate_step(const DockSettings *s, DockAnimation *animation, DockPlacement *current,
                      const DockPlacement *target, int count)
{
    enum { TRANSITION_TICKS = 8 };
    int i, active = 0, envelope, start, end, target_end;
    int levels[DOCK_MAX_ITEMS], weight = 512;
    DockPlacement before[DOCK_MAX_ITEMS];
    if (!dock_settings_valid(s) || !animation || !current || !target || count < 1 || count > DOCK_MAX_ITEMS) return 0;
    for (i = 0; i < count; ++i) {
        if (target[i].level < 0 || target[i].level >= s->levels ||
            current[i].level < 0 || current[i].level >= s->levels) return 0;
        if (target[i].level) active = 1;
    }
    if (animation->count != count || animation->active != active) {
        if (animation->count != count) for (i = 0; i < count; ++i)
            animation->center[i] = (int64_t)current[i].x*256+current[i].size*128;
        for (i = 0; i < count; ++i) animation->origin_level[i] = current[i].level;
        memcpy(animation->origin_center, animation->center, (size_t)count*sizeof(*animation->center));
        animation->tick = 0;
        animation->active = active;
        animation->count = count;
    }
    envelope = animation->tick < TRANSITION_TICKS;
    if (envelope) {
        int t = ++animation->tick, rest = TRANSITION_TICKS - t;
        /* Cubic ease-out, 1-(1-t/8)^3 over 8^3 = 512: a third of the change
           on the first tick and most of it by the fourth, like the macOS
           Dock; a smoothstep start moved one pixel and felt sluggish. */
        weight = 512 - rest * rest * rest;
        for (i = 0; i < count; ++i) {
            int origin = animation->origin_level[i];
            levels[i] = (origin * (512 - weight) + target[i].level * weight + 256) / 512;
        }
    } else {
        /* Lateral retargeting stays responsive; it never restarts an ease-in. */
        for (i = 0; i < count; ++i) levels[i] = ease(current[i].level, target[i].level);
    }
    memcpy(before, current, (size_t)count*sizeof(*current));
    /* The span of the previous and target rows. */
    start = before[0].x < target[0].x ? before[0].x : target[0].x;
    end = before[count-1].x+before[count-1].size; target_end = target[count-1].x+target[count-1].size;
    if (target_end > end) end = target_end;
    {
        /* Rounded intermediate levels can make the row wider than that span,
           which keep_row_inside cannot pack without overlap. Shrink the icons
           furthest above their target first; the target row fits, so this
           ends at or above it. */
        int64_t width = 0;
        for (i = 0; i < count; ++i) width += level_size(s, levels[i]);
        while (width > end-start) {
            int over = -1;
            for (i = 0; i < count; ++i)
                if (levels[i] > target[i].level &&
                    (over < 0 || levels[i]-target[i].level > levels[over]-target[over].level)) over = i;
            if (over < 0) break;
            width -= level_size(s, levels[over]) - level_size(s, levels[over]-1);
            --levels[over];
        }
    }
    for (i = 0; i < count; ++i) {
        DockPlacement next;
        int64_t target_center = (int64_t)target[i].x*256+target[i].size*128;
        int64_t center = envelope ?
            (animation->origin_center[i]*(512-weight)+target_center*weight+256)/512 :
            ease_center(animation->center[i], target_center);
        animation->center[i] = center;
        next.level = levels[i];
        next.size = level_size(s, next.level);
        next.x = (int)((center-next.size*128+128)/256);
        if (i && next.x < current[i-1].x+current[i-1].size) {
            next.x = current[i-1].x+current[i-1].size;
            animation->center[i] = (int64_t)next.x*256+next.size*128;
        }
        next.y = target[i].y + target[i].size - next.size;
        current[i] = next;
    }
    keep_row_inside(animation, current, count, start, end);
    /* The ease-out reaches the target before its last ticks. End it there,
       so a later retarget eases instead of jumping on a near-full weight. */
    if (envelope && !memcmp(current, target, (size_t)count*sizeof(*current)))
        animation->tick = TRANSITION_TICKS;
    return memcmp(before, current, (size_t)count*sizeof(*current)) != 0;
}

int dock_hit_test(const DockPlacement *p, int count, int x, int y)
{
    int i;
    if (!p || count < 0 || count > DOCK_MAX_ITEMS) return -1;
    for (i = count - 1; i >= 0; --i)
        if (p[i].size > 0 && x >= p[i].x && y >= p[i].y &&
            (unsigned int)x-(unsigned int)p[i].x < (unsigned int)p[i].size &&
            (unsigned int)y-(unsigned int)p[i].y < (unsigned int)p[i].size) return i;
    return -1;
}

int dock_image_opaque_at(const DockImage *im, int x, int y)
{
    return im && im->pixels && x >= 0 && y >= 0 && x < im->width && y < im->height &&
        im->pixels[((size_t)y*im->width+x)*4+3];
}

int dock_hover_contains(const DockSettings *s, const DockPlacement *p, int count,
                        int width, int height, int x, int y)
{
    DockRect r;
    int i;
    /* x lies inside [0,width), so the plate's clipping to it changes nothing. */
    if (x < 0 || y < 0 || x >= width || y >= height ||
        !dock_edge_panel_rect(s, DOCK_EDGE_BOTTOM, p, count, width, height, &r)) return 0;
    /* Once enlarged, retain one continuous hover band across the row. Fast
       lateral movement can reach a smaller neighbor before it has grown;
       making that empty area a leave event causes unwanted recentering. At
       rest the shelf still defines entry, excluding maximum-zoom headroom. */
    for (i = 0; i < count; ++i) if (p[i].y < r.top) r.top = p[i].y;
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return 1;
    return dock_hit_test(p, count, x, y) >= 0;
}

int dock_reorder(void *items, size_t size, int count, int from, int to)
{
    size_t b;
    unsigned char *p = (unsigned char *)items;
    if (!p || !size || count < 0 || count > DOCK_MAX_ITEMS ||
        from < 0 || from >= count || to < 0 || to >= count ||
        (count && size > (size_t)-1 / (size_t)count)) return 0;
    /* Byte-wise rotation avoids a heap allocation or variable stack buffer. */
    for (b = 0; b < size; ++b) {
        unsigned char saved = p[(size_t)from * size + b];
        int i;
        if (from < to)
            for (i = from; i < to; ++i) p[(size_t)i * size + b] = p[(size_t)(i + 1) * size + b];
        else
            for (i = from; i > to; --i) p[(size_t)i * size + b] = p[(size_t)(i - 1) * size + b];
        p[(size_t)to * size + b] = saved;
    }
    return 1;
}

#if !defined(__BYTE_ORDER__)
#error "The RGB24 kernel needs __BYTE_ORDER__ to pick its word layout."
#endif
/* DOCK_EMULATE_BIG_ENDIAN runs the big-endian path on a little-endian host:
   words are byte-swapped on load and store, so the 68k formulas are tested. */
#if defined(DOCK_EMULATE_BIG_ENDIAN) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#define DOCK_LITTLE_ENDIAN 0
#else
#define DOCK_LITTLE_ENDIAN 1
#endif

void dock_rgba_to_rgb24_c(unsigned char *dst, const unsigned char *src, unsigned long pixels)
{
    /* Raw-pointer kernel avoids per-byte aliasing against display structures.
       memcpy keeps unaligned buffers legal; constant4-byte copies inline. */
    while (pixels >= 4) {
        uint32_t a, b, c, d, o0, o1, o2;
        a = load32(src); b = load32(src + 4); c = load32(src + 8); d = load32(src + 12);
#if DOCK_LITTLE_ENDIAN
        o0 = (a & UINT32_C(0x00ffffff)) | (b << 24);
        o1 = ((b >> 8) & UINT32_C(0xffff)) | (c << 16);
        o2 = ((c >> 16) & UINT32_C(0xff)) | (d << 8);
#else
        o0 = (a & UINT32_C(0xffffff00)) | (b >> 24);
        o1 = ((b & UINT32_C(0x00ffff00)) << 8) | (c >> 16);
        o2 = ((c & UINT32_C(0x0000ff00)) << 16) | (d >> 8);
#endif
        store32(dst, o0); store32(dst + 4, o1); store32(dst + 8, o2);
        src += 16; dst += 12; pixels -= 4;
    }
    while (pixels--) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
        src += 4; dst += 3;
    }
}

void dock_rgb24_to_rgba_c(unsigned char *dst, const unsigned char *src, unsigned long pixels)
{
    while (pixels--) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
        src += 3; dst += 4;
    }
}

void dock_rgb565_to_rgba_c(unsigned char *dst, const unsigned char *src, unsigned long pixels)
{
    while (pixels--) {
        unsigned int v = ((unsigned int)src[0] << 8) | src[1];
        unsigned int r = v >> 11, g = (v >> 5) & 63, b = v & 31;
        dst[0] = (unsigned char)((r << 3) | (r >> 2));
        dst[1] = (unsigned char)((g << 2) | (g >> 4));
        dst[2] = (unsigned char)((b << 3) | (b >> 2)); dst[3] = 255;
        src += 2; dst += 4;
    }
}

void dock_rgba_to_rgb565_c(unsigned char *dst, const unsigned char *src, unsigned long pixels)
{
    while (pixels--) {
        unsigned int v = ((unsigned int)(src[0] >> 3) << 11) |
                         ((unsigned int)(src[1] >> 2) << 5) | (src[2] >> 3);
        dst[0] = (unsigned char)(v >> 8); dst[1] = (unsigned char)v;
        src += 4; dst += 2;
    }
}

void dock_rgb565_key_c(unsigned char *dst, const unsigned char *coverage, unsigned long pixels)
{
    while (pixels--) {
        unsigned int value = ((unsigned int)dst[0] << 8) | dst[1];
        if (!*coverage++) value = 0xf81f;
        else if (value == 0xf81f) value = 0xf81e;
        dst[0] = (unsigned char)(value >> 8); dst[1] = (unsigned char)value;
        dst += 2;
    }
}

int dock_bytes_equal_c(const unsigned char *a, const unsigned char *b, unsigned long bytes)
{
    return !bytes || !memcmp(a, b, (size_t)bytes);
}

void dock_mix_half_c(unsigned char *dst, const unsigned char *src, unsigned long pixels)
{
    /* Per-byte (a+b+1)>>1, four bytes per 32-bit word. Byte order does not
       matter: each byte is averaged on its own. */
    unsigned long i;
    for (i = 0; i < pixels; ++i) {
        uint32_t a = load32(dst+i*4), b = load32(src+i*4);
        store32(dst+i*4, (a|b)-(((a^b)>>1)&UINT32_C(0x7f7f7f7f)));
    }
}
