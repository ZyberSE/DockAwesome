#ifndef DOCK_CORE_H
#define DOCK_CORE_H
#include <stddef.h>
#include <stdint.h>

#define DOCK_MAX_ITEMS 32
#define DOCK_MAX_LEVELS 33
#define DOCK_MIN_ICON_SIZE 16
#define DOCK_MAX_ICON_SIZE 192
#define DOCK_SCALE_ONE 100
#define DOCK_ICON_INSET 2
#define DOCK_MAX_SCALE (DOCK_MAX_ICON_SIZE * DOCK_SCALE_ONE / DOCK_MIN_ICON_SIZE)
#define DOCK_PANEL_MAX_RADIUS 80
/* Zero-initialize before use. Reuse corner masks while the radius stays fixed. */
typedef struct {
    int radius;
    unsigned char outer[DOCK_PANEL_MAX_RADIUS * DOCK_PANEL_MAX_RADIUS];
    unsigned char inner[DOCK_PANEL_MAX_RADIUS * DOCK_PANEL_MAX_RADIUS];
} DockPanelShape;
/* Straight-alpha RGBA bytes, independent of host byte order. */
typedef struct { int width, height; unsigned char *pixels; } DockImage;
typedef struct {
    int base_size, max_size, levels, gap, padding;
    size_t cache_limit;
    int end_gap; /* Extra space before the last icon in a row of two or more. */
    int edge_padding;
} DockSettings;
typedef struct {
    DockImage image[DOCK_MAX_LEVELS];
    /* Per level: height+1 row starts, then (x, length) pairs for the visible
       runs of each row; bit 15 of length marks an opaque run. Rendering uses
       them to skip transparent pixels and copy opaque ones whole. They take
       about 3% of the pixel bytes on real icons and are not counted in bytes. */
    uint16_t *runs[DOCK_MAX_LEVELS];
    size_t bytes;
    int levels;
    /* Nonzero and new for each built cache (see dock_cache_restamp); the
       renderer compares it per slot, since a rebuilt cache can reuse freed
       pixel addresses. Zero for caches assembled by hand. */
    unsigned long stamp;
} DockCache;
typedef struct { int x, y, size, level; } DockPlacement;
/* Track the zoom transition and each icon's subpixel center. */
typedef struct {
    int origin_level[DOCK_MAX_ITEMS];
    int64_t center[DOCK_MAX_ITEMS], origin_center[DOCK_MAX_ITEMS]; /* Q8 pixels. */
    int tick, active, count;
} DockAnimation;
typedef void (*DockConvertRGB565)(unsigned char *, const unsigned char *, unsigned long);
typedef void (*DockKeyRGB565)(unsigned char *, const unsigned char *, unsigned long);
typedef int (*DockBytesEqual)(const unsigned char *, const unsigned char *, unsigned long);
typedef void (*DockConvertRGB24)(unsigned char *, const unsigned char *, unsigned long);
typedef void (*DockMixHalf)(unsigned char *, const unsigned char *, unsigned long);

DockSettings dock_settings_default(void);
int dock_settings_valid(const DockSettings *s);
int dock_row_length(const DockSettings *s, int count);
/* Convert hundredths of a factor to the nearest pixel. Zero means invalid input. */
int dock_scaled_size(int size, int scale_percent);
size_t dock_cache_bytes(const DockSettings *s);
/* Check whether the full row fits the viewport and cache. */
int dock_items_fit(const DockSettings *s, int count, int viewport_length);
/* Normalizes visible alpha bounds to square boxes, retaining aspect ratio.
   Caller owns source. Failure leaves output empty. */
int dock_cache_build(DockCache *out, const DockImage *source, const DockSettings *s);
/* The same, with the artwork filling fill/256 of an icon's usual extent. */
int dock_cache_build_scaled(DockCache *out, const DockImage *source, const DockSettings *s, int fill);
/* One normalized image of size x size, as level 0 of a cache would be. */
int dock_icon_build(DockImage *out, const DockImage *source, int size);
void dock_cache_free(DockCache *cache);
/* Gives the cache a new stamp from the counter builds use. Call it after
   changing a cache's pixels in place so the renderer redraws its slot. */
void dock_cache_restamp(DockCache *cache);
/* Use window coordinates. An inactive dock returns resting sizes. */
int dock_layout(const DockSettings *s, int count, int width, int height,
                int mouse_x, int mouse_y, int active, DockPlacement *out);
/* Returns 1 when pixels move. Compare current and target to stop the timer.
   A subpixel step may leave the frame unchanged. */
void dock_animation_reset(DockAnimation *animation);
int dock_animate_step(const DockSettings *s, DockAnimation *animation, DockPlacement *current,
                      const DockPlacement *target, int count);
int dock_hit_test(const DockPlacement *p, int count, int x, int y);
/* Nonzero when (x,y) lies inside the image on a pixel with nonzero alpha.
   Pure and allocation-free, so input handlers may call it. */
int dock_image_opaque_at(const DockImage *image, int x, int y);
/* Hover follows the plate and enlarged icons. Extra canvas stays inactive. */
int dock_hover_contains(const DockSettings *s, const DockPlacement *p, int count,
                        int width, int height, int x, int y);
int dock_reorder(void *items, size_t item_size, int count, int from, int to);
/* Blend RGBA into an opaque destination and clip to its bounds. */
void dock_blend(DockImage *dst, const DockImage *src, int x, int y);
void dock_blend_masked(DockImage *dst, const DockImage *src, int x, int y, unsigned char *coverage);
/* Packed RGBA->RGB24, no partial overlap, zero count permits null pointers. */
void dock_rgba_to_rgb24_c(unsigned char *dst, const unsigned char *src, unsigned long pixels);
/* Packed RGB24->opaque RGBA, with the same span/zero-count contract. */
void dock_rgb24_to_rgba_c(unsigned char *dst, const unsigned char *src, unsigned long pixels);
#ifdef DOCK_AMMX
void dock_rgba_to_rgb24_ammx(unsigned char *dst, const unsigned char *src, unsigned long pixels);
#endif
/* RGB565 is big-endian. Decode repeats bits. Encode truncates channels. */
void dock_rgb565_to_rgba_c(unsigned char *dst, const unsigned char *src, unsigned long pixels);
void dock_rgba_to_rgb565_c(unsigned char *dst, const unsigned char *src, unsigned long pixels);
int dock_bytes_equal_c(const unsigned char *a, const unsigned char *b, unsigned long bytes);
/* With a separate coverage byte per pixel, map 0 to F81F and escape covered
   F81F to F81E. A zero count permits null pointers. */
void dock_rgb565_key_c(unsigned char *dst, const unsigned char *coverage, unsigned long pixels);
#ifdef DOCK_AMMX
void dock_rgb565_to_rgba_ammx(unsigned char *dst, const unsigned char *src, unsigned long pixels);
void dock_rgba_to_rgb565_ammx(unsigned char *dst, const unsigned char *src, unsigned long pixels);
int dock_bytes_equal_ammx(const unsigned char *a, const unsigned char *b, unsigned long bytes);
void dock_rgb565_key_ammx(unsigned char *dst, const unsigned char *source, unsigned long pixels);
#endif
void dock_mix_half_c(unsigned char *dst, const unsigned char *src, unsigned long pixels);
#ifdef DOCK_AMMX
void dock_mix_half_ammx(unsigned char *dst, const unsigned char *src, unsigned long pixels);
#endif
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
#endif
