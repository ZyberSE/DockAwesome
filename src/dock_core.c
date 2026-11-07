#include "dock_core.h"
#include "dock_geometry.h"
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

void dock_cache_free(DockCache *cache)
{
    int i;
    if (!cache) return;
    for (i = 0; i < DOCK_MAX_LEVELS; ++i) free(cache->image[i].pixels);
    memset(cache, 0, sizeof(*cache));
}

typedef struct {
    int left, right;
    unsigned int fraction;
} SampleColumn;

static int sample_coordinate(int position, int size, int start, int length)
{
    int low = start*256, high = (start+length-1)*256;
    int value = low+((2*position+1)*length*128)/size-128;
    return value < low ? low : value > high ? high : value;
}

/* Interpolate premultiplied channels, then return straight alpha. When all
   taps share an alpha, it cancels from the color calculation exactly. */
static void sample(const unsigned char *upper, const unsigned char *lower,
    const SampleColumn *column, unsigned int fy, unsigned char *dst)
{
    const unsigned char *p[] = {upper+column->left, upper+column->right,
        lower+column->left, lower+column->right};
    unsigned int fx = column->fraction, diagonal = fx*fy;
    uint32_t weights[] = {65536-(fx<<8)-(fy<<8)+diagonal,
        (fx<<8)-diagonal, (fy<<8)-diagonal, diagonal};
    unsigned int common = p[0][3];
    uint32_t alpha = 0;
    int i, c;
    if (p[1][3] == common && p[2][3] == common && p[3][3] == common) {
        if (!common) { memset(dst,0,4); return; }
        dst[3] = common;
        for (c = 0; c < 3; ++c) {
            uint32_t v = p[0][c]*weights[0]+p[1][c]*weights[1]+
                p[2][c]*weights[2]+p[3][c]*weights[3];
            dst[c] = (unsigned char)(v>>16);
        }
        return;
    }
    for (i = 0; i < 4; ++i) {
        weights[i] *= p[i][3];
        alpha += weights[i];
    }
    if (!alpha) { memset(dst,0,4); return; }
    dst[3] = (unsigned char)((alpha + 32768UL) >> 16);
    for (c = 0; c < 3; ++c) {
        uint32_t v = p[0][c]*weights[0]+p[1][c]*weights[1]+
            p[2][c]*weights[2]+p[3][c]*weights[3];
        dst[c] = (unsigned char)(v/alpha);
    }
}

int dock_cache_build(DockCache *out, const DockImage *source, const DockSettings *s)
{
    SampleColumn columns[DOCK_MAX_ICON_SIZE];
    int left, right = -1, top, bottom = -1, x, y, l;
    int previous_w = 0, previous_h = 0;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!dock_settings_valid(s) || !source || !source->pixels ||
        source->width < 1 || source->height < 1 ||
        source->width > 4096 || source->height > 4096 ||
        dock_cache_bytes(s) > s->cache_limit) return 0;
    left = source->width; top = source->height;
    for (y = 0; y < source->height; ++y) {
        for (x = 0; x < source->width; ++x) {
            if (!source->pixels[((size_t)y * source->width + x) * 4 + 3]) continue;
            if (x < left) left = x;
            if (x > right) right = x;
            if (y < top) top = y;
            if (y > bottom) bottom = y;
        }
    }
    for (l = 0; l < s->levels; ++l) {
        DockImage *dst = &out->image[l];
        int n = level_size(s, l), w, h, ox, oy, cw, ch;
        dst->width = dst->height = n;
        dst->pixels = (unsigned char *)calloc((size_t)n * n, 4);
        if (!dst->pixels) { dock_cache_free(out); return 0; }
        out->bytes += (size_t)n * n * 4;
        out->levels = l + 1;
        if (right < left) continue;
        cw = right - left + 1; ch = bottom - top + 1;
        /* Scale the normalized artwork and its padding together. A fixed
          2px inset at every level would turn28px into60px for32->64 boxes. */
        w = h = (n*(s->base_size-2*DOCK_ICON_INSET)+s->base_size/2)/s->base_size;
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
        for (x = 0; x < w; ++x) {
            int sx = sample_coordinate(x,w,left,cw), x0 = sx>>8;
            int x1 = x0+1 < source->width ? x0+1 : x0;
            columns[x] = (SampleColumn){4*x0,4*x1,(unsigned int)sx&255};
        }
        for (y = 0; y < h; ++y) {
            int sy = sample_coordinate(y,h,top,ch), y0 = sy>>8;
            int y1 = y0+1 < source->height ? y0+1 : y0;
            const unsigned char *upper = source->pixels+(size_t)y0*source->width*4;
            const unsigned char *lower = source->pixels+(size_t)y1*source->width*4;
            unsigned char *row = dst->pixels+((size_t)(y+oy)*n+ox)*4;
            for (x = 0; x < w; ++x)
                sample(upper,lower,&columns[x],(unsigned int)sy&255,row+x*4);
        }
    }
    return 1;
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

int dock_layout(const DockSettings *s, int count, int width, int height,
                int mx, int my, int active, DockPlacement *out)
{
    int i, total, rest, start, pitch, reach, x, panel_top, panel_bottom, baseline, constrained = 0;
    int logical_size[DOCK_MAX_ITEMS];
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
    reach = pitch * 4;
    total = count > 1 ? s->end_gap : 0;
    for (i = 0; i < count; ++i) {
        int distance = mx - (start+i*pitch+s->base_size/2+extra_gap(s,count,i-1));
        int level = 0;
        unsigned long weight = 0;
        if (distance < 0) distance = -distance;
        if (s->max_size > s->base_size && distance < reach) {
            /* Rounded broad wave: smoothstep has zero slope at the peak and
               tail, unlike the old narrow squared cusp. Fixed-point only. */
            unsigned long t = (unsigned long)(reach-distance) * 256 / reach;
            weight = t * t * (768 - 2*t);
            level = (int)(((s->levels-1)*weight + (1UL<<23)) >> 24);
        }
        out[i].level = level;
        out[i].size = level_size(s, level);
        logical_size[i] = s->base_size*256+
            (int)(((unsigned long)(s->max_size-s->base_size)*weight+32768UL)>>16);
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
        total -= out[largest].size;
        if (largest) total -= pair_gap(s, out[largest-1].size, out[largest].size);
        if (largest+1 < count) total -= pair_gap(s, out[largest].size, out[largest+1].size);
        --out[largest].level;
        out[largest].size = level_size(s, out[largest].level);
        total += out[largest].size;
        if (largest) total += pair_gap(s, out[largest-1].size, out[largest].size);
        if (largest+1 < count) total += pair_gap(s, out[largest].size, out[largest+1].size);
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

int dock_animate_step(const DockSettings *s, DockAnimation *animation, DockPlacement *current,
                      const DockPlacement *target, int count)
{
    enum { TRANSITION_TICKS = 8 };
    int i, changed = 0, active = 0, envelope;
    int levels[DOCK_MAX_ITEMS], weight = 512;
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
        int t = ++animation->tick;
        /* smoothstep(t/8): integer numerator, denominator 8^3 = 512. */
        weight = t * t * (3 * TRANSITION_TICKS - 2 * t);
        for (i = 0; i < count; ++i) {
            int origin = animation->origin_level[i];
            levels[i] = (origin * (512 - weight) + target[i].level * weight + 256) / 512;
        }
    } else {
        /* Lateral retargeting stays responsive; it never restarts an ease-in. */
        for (i = 0; i < count; ++i) levels[i] = ease(current[i].level, target[i].level);
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
        if (memcmp(&current[i], &next, sizeof(next))) changed = 1;
        current[i] = next;
    }
    return changed;
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

int dock_hover_contains(const DockSettings *s, const DockPlacement *p, int count,
                        int width, int height, int x, int y)
{
    int64_t left, right;
    int top, bottom, i;
    if (!dock_settings_valid(s) || count < 0 || count > DOCK_MAX_ITEMS ||
        (count && !p) || width < 1 || height < 1 ||
        x < 0 || y < 0 || x >= width || y >= height) return 0;
    left = count ? (int64_t)p[0].x-s->padding : s->padding;
    right = count ? (int64_t)p[count-1].x+p[count-1].size+s->padding : width-s->padding;
    dock_panel_cross_bounds(s,height,&top,&bottom);
    /* Once enlarged, retain one continuous hover band across the row. Fast
       lateral movement can reach a smaller neighbor before it has grown;
       making that empty area a leave event causes unwanted recentering. At
       rest the shelf still defines entry, excluding maximum-zoom headroom. */
    for (i = 0; i < count; ++i) if (p[i].y < top) top = p[i].y;
    if (x >= left && x < right && y >= top && y < bottom) return 1;
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

static void blend_mask(DockImage *dst, const DockImage *src, int x, int y,
                        unsigned char *coverage, int pressed)
{
    int sx, sy, left, top;
    int right, bottom;
    if (!dst || !src || !dst->pixels || !src->pixels ||
        dst->width <= 0 || dst->height <= 0 || src->width <= 0 || src->height <= 0) return;
    if (x >= dst->width || y >= dst->height || x <= -src->width || y <= -src->height) return;
    left = x < 0 ? -x : 0; top = y < 0 ? -y : 0;
    right = src->width; bottom = src->height;
    if ((int64_t)right > (int64_t)dst->width - x) right = (int)((int64_t)dst->width - x);
    if ((int64_t)bottom > (int64_t)dst->height - y) bottom = (int)((int64_t)dst->height - y);
    for (sy = top; sy < bottom; ++sy) {
        const unsigned char *s = src->pixels + ((size_t)sy * src->width + left) * 4;
        unsigned char *d = dst->pixels + ((size_t)(sy + y) * dst->width + left + x) * 4;
        for (sx = left; sx < right; ++sx, s += 4, d += 4) {
            unsigned int a = s[3], inv = 255 - a;
            int c;
            if (!a) continue;
            if (coverage) coverage[(size_t)(sy+y)*dst->width+sx+x] = 255;
            if (a == 255 && !pressed) { memcpy(d, s, 4); continue; }
            for (c = 0; c < 3; ++c) {
                unsigned int color = pressed ? (s[c]*3+2)/4 : s[c];
                d[c] = (unsigned char)((color * a + d[c] * inv + 127) / 255);
            }
            d[3] = 255;
        }
    }
}

void dock_blend(DockImage *dst, const DockImage *src, int x, int y)
{
    blend_mask(dst, src, x, y, NULL, 0);
}

void dock_blend_masked(DockImage *dst, const DockImage *src, int x, int y, unsigned char *coverage)
{
    blend_mask(dst, src, x, y, coverage, 0);
}

void dock_rgba_to_rgb24_c(unsigned char *dst, const unsigned char *src, unsigned long pixels)
{
    /* Raw-pointer kernel avoids per-byte aliasing against display structures.
       memcpy keeps unaligned buffers legal; constant4-byte copies inline. */
    while (pixels >= 4) {
        uint32_t a, b, c, d, o0, o1, o2;
        memcpy(&a, src, 4); memcpy(&b, src + 4, 4);
        memcpy(&c, src + 8, 4); memcpy(&d, src + 12, 4);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        o0 = (a & UINT32_C(0x00ffffff)) | (b << 24);
        o1 = ((b >> 8) & UINT32_C(0xffff)) | (c << 16);
        o2 = ((c >> 16) & UINT32_C(0xff)) | (d << 8);
#else
        o0 = (a & UINT32_C(0xffffff00)) | (b >> 24);
        o1 = ((b & UINT32_C(0x00ffff00)) << 8) | (c >> 16);
        o2 = ((c & UINT32_C(0x0000ff00)) << 16) | (d >> 8);
#endif
        memcpy(dst, &o0, 4); memcpy(dst + 4, &o1, 4); memcpy(dst + 8, &o2, 4);
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
    unsigned long i;
    for (i = 0; i < pixels; ++i) {
        int c;
        for (c = 0; c < 4; ++c) dst[c] = (unsigned char)((dst[c] + src[c] + 1) >> 1);
        dst += 4; src += 4;
    }
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
    const DockPlacement *previous, const DockPlacement *last, int edge, int max_gap)
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
    for (cross = start+inset; cross < end-inset; ++cross) {
        int x = bottom ? (int)line : cross, y = bottom ? cross : (int)line;
        unsigned char *pixel = canvas->pixels+((size_t)y*canvas->width+x)*4;
        panel_edge(pixel,pixel,255,0,88);
    }
}

void dock_render_oriented_mask(DockImage *canvas, const DockImage *backdrop,
                 const DockCache *const *icons, const DockPlacement *p, int count,
                 const DockSettings *s, DockMixHalf mix_half, DockPanelShape *shape,
                 unsigned char *coverage, int pressed_item, int edge, int center_panel)
{
    enum { RIM_STRENGTH = 52 };
    static const unsigned char flat_background[4] = {32, 36, 42, 255};
    static const unsigned char panel_background[4] = {20, 24, 32, 255};
    int x, y, shelf_top, shelf_bottom, left, right, radius, i;
    int captured, width, height;
    DockRect rect;
    unsigned char *pixels;
    if (!canvas || !canvas->pixels || canvas->width < 1 || canvas->height < 1 ||
        !dock_settings_valid(s) || count < 0 || count > DOCK_MAX_ITEMS ||
        (count && (!icons || !p))) return;
    width = canvas->width; height = canvas->height; pixels = canvas->pixels;
    if (!dock_edge_panel_rect(s, (DockEdge)edge, p, count, width, height, &rect)) return;
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
    if (coverage) memset(coverage, 0, (size_t)width * height);
    captured = backdrop && backdrop->pixels && backdrop->width == width && backdrop->height == height;
    if (captured) memcpy(pixels, backdrop->pixels, (size_t)width * height * 4);
    else {
        size_t n = (size_t)width * height, j;
        for (j = 0; j < n; ++j) memcpy(pixels + j * 4, flat_background, 4);
    }
    left = rect.left; right = rect.right;
    shelf_top = rect.top; shelf_bottom = rect.bottom;
    radius = (edge == DOCK_EDGE_BOTTOM ? shelf_bottom-shelf_top : right-left)/4;
    if (radius > (right - left) / 2) radius = (right - left) / 2;
    if (radius > (shelf_bottom-shelf_top)/2) radius = (shelf_bottom-shelf_top)/2;
    if (radius > DOCK_PANEL_MAX_RADIUS) radius = DOCK_PANEL_MAX_RADIUS;
    if (radius < 0) radius = 0;
    panel_shape_prepare(shape, radius);
    if (!mix_half) mix_half = dock_mix_half_c;
    if (right > left && shelf_bottom > shelf_top) for (y = shelf_top; y < shelf_bottom; ++y) {
        int cy = y - shelf_top, row_inset = 0, px, center_left, center_right;
        unsigned int outer = 255, inner = 255;
        unsigned char *row = pixels + (size_t)y * width * 4, *d;
        if (shelf_bottom - 1 - y < cy) cy = shelf_bottom - 1 - y;
        if (cy < radius) {
            while (row_inset < radius) {
                panel_corner(shape,row_inset,cy,radius,&outer,&inner);
                if (outer) break;
                ++row_inset;
            }
        }
        d = row + (left + row_inset) * 4;
        if (coverage) memset(coverage+(size_t)y*width+left+row_inset, 255,
                             (size_t)(right-left-row_inset*2));
        for (x = left + row_inset; x < right - row_inset; ++x, d += 4)
            memcpy(d,panel_background,4);
        if (captured) {
            size_t offset = ((size_t)y * width + left + row_inset) * 4;
            mix_half(pixels + offset, backdrop->pixels + offset, (unsigned long)(right - left - row_inset * 2));
        }
        center_left = left + (cy < radius ? radius : 1);
        center_right = right - (cy < radius ? radius : 1);
        for (px = left + row_inset; px < center_left; ++px) {
            unsigned char *l = row + px * 4;
            unsigned char *r = row + (right - 1 - (px - left)) * 4;
            if (cy < radius) panel_corner(shape,px-left,cy,radius,&outer,&inner);
            else { outer = 255; inner = 0; }
            if (outer == 255 && inner == 255) continue;
            panel_edge(l,captured ? backdrop->pixels+(size_t)(l-pixels) : flat_background,
                outer,inner,RIM_STRENGTH);
            if (r != l) panel_edge(r, captured ? backdrop->pixels + (size_t)(r - pixels) : flat_background,
                outer,inner,RIM_STRENGTH);
        }
        if (!cy) for (px = center_left; px < center_right; ++px) {
            unsigned char *edge_pixel = row+px*4;
            panel_edge(edge_pixel,edge_pixel,255,0,RIM_STRENGTH);
        }
    }
    if (s->end_gap && count > 1)
        draw_divider(canvas,&rect,&p[count-2],&p[count-1],edge,s->gap+s->end_gap);
    for (i = 0; i < count; ++i) {
        int level = p[i].level;
        if (icons[i] && level >= 0 && level < DOCK_MAX_LEVELS && level < icons[i]->levels)
            blend_mask(canvas, &icons[i]->image[level], p[i].x, p[i].y, coverage, pressed_item == i+1);
    }
}

void dock_render_interactive_mask(DockImage *canvas, const DockImage *backdrop,
                 const DockCache *const *icons, const DockPlacement *p, int count,
                 const DockSettings *s, DockMixHalf mix_half, DockPanelShape *shape,
                 unsigned char *coverage, int pressed_item)
{
    dock_render_oriented_mask(canvas, backdrop, icons, p, count, s, mix_half, shape, coverage, pressed_item, DOCK_EDGE_BOTTOM, 0);
}

void dock_render_cached_mask(DockImage *canvas, const DockImage *backdrop,
                 const DockCache *const *icons, const DockPlacement *p, int count,
                 const DockSettings *s, DockMixHalf mix_half, DockPanelShape *shape,
                 unsigned char *coverage)
{
    dock_render_interactive_mask(canvas, backdrop, icons, p, count, s, mix_half, shape, coverage, 0);
}

void dock_render_cached(DockImage *canvas, const DockImage *backdrop,
                 const DockCache *const *icons, const DockPlacement *p, int count,
                 const DockSettings *s, DockMixHalf mix_half, DockPanelShape *shape)
{
    dock_render_cached_mask(canvas, backdrop, icons, p, count, s, mix_half, shape, NULL);
}

void dock_render(DockImage *canvas, const DockImage *backdrop,
                 const DockCache *const *icons, const DockPlacement *p, int count,
                 const DockSettings *s, DockMixHalf mix_half)
{
    dock_render_cached(canvas, backdrop, icons, p, count, s, mix_half, NULL);
}
