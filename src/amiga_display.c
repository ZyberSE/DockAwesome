#include "amiga_display.h"
#include "dock_renderer.h"
#include <exec/types.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <intuition/screens.h>
#include <intuition/intuition.h>
#include <inline/macros.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#define CYBERGFX_BASE_NAME d->cyber
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>
#include <stdlib.h>
#include <string.h>

#ifndef RECTFMT_RAW
#define RECTFMT_RAW 5UL
#endif
struct AmigaDisplay {
    struct Screen *screen;
    struct Library *cyber;
    DockConvertRGB24 rgba_to_rgb24;
    DockConvertRGB565 rgb565_to_rgba, rgba_to_rgb565;
    int width, height, depth, stride;
    unsigned char *upload, *lookup;
    size_t upload_capacity; /* RTG upload bytes, so amiga_display_resize can reuse them */
    int shared_lookup;
    ULONG palette[256 * 3];
    struct RastPort temp;
    struct BitMap *temp_bitmap;
    struct BitMap *source_bitmap;
    struct RastPort frame;
    struct BitMap *frame_bitmap;
    /* PiP on a 24- or 32-bit screen: the last capture in the screen's own
       format, compared as is; only changed rows convert to RGB565. */
    unsigned char *raw;
    size_t raw_capacity;
    int raw_valid, raw_bytes;
};

/* One palette lookup serves every display on the same palette. Building it
   costs about one million distance tests, so transient surfaces reuse it. */
static struct {
    int refs, count;
    ULONG palette[256 * 3];
    unsigned char table[4096];
} *lookup_cache;

static unsigned char *build_lookup(AmigaDisplay *d, int n)
{
    unsigned char *lookup;
    int r, g, b, i;
    if (lookup_cache && lookup_cache->count == n &&
        !memcmp(lookup_cache->palette, d->palette, sizeof(ULONG) * 3 * n)) {
        ++lookup_cache->refs;
        d->shared_lookup = 1;
        return lookup_cache->table;
    }
    if (!lookup_cache) {
        lookup_cache = malloc(sizeof(*lookup_cache));
        if (lookup_cache) {
            lookup_cache->refs = 1;
            lookup_cache->count = n;
            memcpy(lookup_cache->palette, d->palette, sizeof(ULONG) * 3 * n);
            lookup = lookup_cache->table;
            d->shared_lookup = 1;
        } else lookup = NULL;
    } else lookup = malloc(4096);
    if (lookup) for (r = 0; r < 16; ++r) for (g = 0; g < 16; ++g) for (b = 0; b < 16; ++b) {
        long best_error = 0x7fffffffL;
        int best = 0;
        for (i = 0; i < n; ++i) {
            long dr = r * 17 - (d->palette[i*3] >> 24);
            long dg = g * 17 - (d->palette[i*3+1] >> 24);
            long db = b * 17 - (d->palette[i*3+2] >> 24);
            long error = 2*dr*dr + 3*dg*dg + db*db;
            if (error < best_error) { best_error = error; best = i; }
        }
        lookup[(r<<8)|(g<<4)|b] = best;
    }
    return lookup;
}

static void free_lookup(AmigaDisplay *d)
{
    if (!d->lookup) return;
    if (!d->shared_lookup) free(d->lookup);
    else if (!--lookup_cache->refs) { free(lookup_cache); lookup_cache = NULL; }
    d->lookup = NULL;
}

/* ApolloOS reads full D0–D3 coordinates. The NDK inline loads only their low words. */
static LONG read_indexed_pixels(AmigaDisplay *d, int x, int y)
{
    return LP7(0x30c, LONG, DockReadPixelArray8, struct RastPort *, &d->screen->RastPort, a0,
        ULONG, (ULONG)x, d0, ULONG, (ULONG)y, d1, ULONG, (ULONG)(x + d->width - 1), d2,
        ULONG, (ULONG)(y + d->height - 1), d3, UBYTE *, d->upload, a2,
        struct RastPort *, &d->temp, a1, , GfxBase);
}

static LONG write_indexed_pixels(AmigaDisplay *d, struct RastPort *port)
{
    /* ApolloOS reads full D0–D3 coordinates. Classic graphics reads their low words. */
    return LP7(0x312, LONG, DockWritePixelArray8, struct RastPort *, port, a0,
        ULONG, 0, d0, ULONG, 0, d1, ULONG, (ULONG)(d->width - 1), d2,
        ULONG, (ULONG)(d->height - 1), d3, UBYTE *, d->upload, a2,
        struct RastPort *, &d->temp, a1, , GfxBase);
}

AmigaDisplay *amiga_display_open(struct Screen *screen, int width, int height)
{
    AmigaDisplay *d;
    int n;
    d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->screen = screen; d->width = width; d->height = height;
    d->source_bitmap = screen->RastPort.BitMap;
    d->rgba_to_rgb24 = dock_rgba_to_rgb24_c;
    d->rgb565_to_rgba = dock_rgb565_to_rgba_c;
    d->depth = GetBitMapAttr(screen->RastPort.BitMap, BMA_DEPTH);
    if (d->depth < 8) { amiga_display_close(d); return NULL; }
    if (d->depth > 8) {
        d->cyber = OpenLibrary((STRPTR)"cybergraphics.library", 40);
        if (!d->cyber || !GetCyberMapAttr(screen->RastPort.BitMap, CYBRMATTR_ISCYBERGFX)) {
            amiga_display_close(d); return NULL;
        }
        d->stride = width * 3;
        d->upload_capacity = (size_t)d->stride * height;
        d->upload = malloc(d->upload_capacity);
    } else {
        /* WritePixelArray8 requires 16-pixel row alignment and a one-row temporary bitmap. */
        d->stride = (width + 15) & ~15;
        d->upload = calloc((size_t)d->stride, height);
        d->temp_bitmap = AllocBitMap(d->stride, 1, 8, BMF_CLEAR, NULL);
        if (d->temp_bitmap) { InitRastPort(&d->temp); d->temp.BitMap = d->temp_bitmap; }
        n = screen->ViewPort.ColorMap->Count;
        if (n > 256) n = 256;
        GetRGB32(screen->ViewPort.ColorMap, 0, n, d->palette);
        d->lookup = build_lookup(d, n);
    }
    if (!d->upload || (d->depth == 8 && (!d->lookup || !d->temp_bitmap))) {
        amiga_display_close(d); return NULL;
    }
    return d;
}

void amiga_display_close(AmigaDisplay *d)
{
    if (!d) return;
    if (d->temp_bitmap) { WaitBlit(); FreeBitMap(d->temp_bitmap); }
    if (d->frame_bitmap) { WaitBlit(); FreeBitMap(d->frame_bitmap); }
    free_lookup(d); free(d->upload); free(d->raw);
    if (d->cyber) CloseLibrary(d->cyber);
    free(d);
}

int amiga_display_resize(AmigaDisplay *d, int width, int height)
{
    size_t bytes;
    if (!d || d->depth <= 8 || width < 1 || height < 1 ||
        d->screen->RastPort.BitMap != d->source_bitmap) return 0;
    if (width == d->width && height == d->height) return 1;
    bytes = (size_t)width*3*height;
    if (bytes > d->upload_capacity) {
        unsigned char *upload = malloc(bytes);
        if (!upload) return 0;
        free(d->upload);
        d->upload = upload;
        d->upload_capacity = bytes;
    }
    if (d->frame_bitmap) { WaitBlit(); FreeBitMap(d->frame_bitmap); d->frame_bitmap = NULL; }
    d->width = width; d->height = height; d->stride = width*3;
    d->raw_valid = 0;
    return 1;
}

void amiga_display_set_converters(AmigaDisplay *d, DockConvertRGB24 encode, DockConvertRGB565 decode,
    DockConvertRGB565 encode565)
{
    if (!d) return;
    d->rgba_to_rgb24 = encode ? encode : dock_rgba_to_rgb24_c;
    d->rgb565_to_rgba = decode ? decode : dock_rgb565_to_rgba_c;
    d->rgba_to_rgb565 = encode565;
}

int amiga_display_depth(const AmigaDisplay *d) { return d ? d->depth : 0; }

/* Pixel formats the PiP capture reads; others fail the capture. */
static int rgb565_source(ULONG format, ULONG bytes)
{
    switch (format) {
    case PIXFMT_RGB16: case PIXFMT_RGB16PC: return bytes == 2;
    case PIXFMT_RGB24: case PIXFMT_BGR24: return bytes == 3;
    case PIXFMT_ARGB32: case PIXFMT_BGRA32: case PIXFMT_RGBA32: return bytes == 4;
    default: return 0;
    }
}

/* One row of n source pixels as big-endian RGB565, the PiP overlay's
   format: the dock's own pixels are 16-bit whatever the screen. */
static void row_to_rgb565(unsigned char *out, const unsigned char *in, int n, ULONG format)
{
    int r, g, b;
    unsigned int v;
    if (format == PIXFMT_RGB16) { memcpy(out,in,(size_t)n*2); return; }
    for (; n > 0; --n, out += 2) {
        switch (format) {
        case PIXFMT_RGB16PC: out[0] = in[1]; out[1] = in[0]; in += 2; continue;
        case PIXFMT_RGB24: r = in[0]; g = in[1]; b = in[2]; in += 3; break;
        case PIXFMT_BGR24: b = in[0]; g = in[1]; r = in[2]; in += 3; break;
        case PIXFMT_ARGB32: r = in[1]; g = in[2]; b = in[3]; in += 4; break;
        case PIXFMT_BGRA32: b = in[0]; g = in[1]; r = in[2]; in += 4; break;
        default: /* PIXFMT_RGBA32 */ r = in[0]; g = in[1]; b = in[2]; in += 4; break;
        }
        v = ((unsigned int)(r&0xf8)<<8)|((unsigned int)(g&0xfc)<<3)|((unsigned int)b>>3);
        out[0] = (unsigned char)(v>>8); out[1] = (unsigned char)v;
    }
}

/* A rectangle of a 24- or 32-bit bitmap as RGB565 rows of out_stride
   bytes. Reads memory under LockBitMap, which takes no layer lock. */
static int read_rect565(AmigaDisplay *d, struct BitMap *bitmap, int x, int y, int w, int h,
    unsigned char *out, size_t out_stride)
{
    APTR lock, base = NULL;
    ULONG stride = 0, format = ~0UL, bytes = 0, width = 0, height = 0;
    int row;
    lock = LockBitMapTags(bitmap, LBMI_BASEADDRESS, (ULONG)&base,
        LBMI_BYTESPERROW, (ULONG)&stride, LBMI_BYTESPERPIX, (ULONG)&bytes,
        LBMI_PIXFMT, (ULONG)&format, LBMI_WIDTH, (ULONG)&width,
        LBMI_HEIGHT, (ULONG)&height, TAG_DONE);
    if (!lock) return 0;
    if (!base || !rgb565_source(format,bytes) || x < 0 || y < 0 ||
        width < (ULONG)(x+w) || height < (ULONG)(y+h)) { UnLockBitMap(lock); return 0; }
    for (row = 0; row < h; ++row)
        row_to_rgb565(out+(size_t)row*out_stride,
            (const unsigned char *)base+(size_t)(y+row)*stride+(size_t)x*bytes,w,format);
    UnLockBitMap(lock);
    return 1;
}

/* The capture of a 24- or 32-bit screen: compare rows in the screen's own
   format against the raw copy, at rest only the check box, and convert just
   the changed rows into the RGB565 cache. Returns as capture565 does. */
static int capture_raw(AmigaDisplay *d, unsigned char *cache, const unsigned char *source,
    ULONG stride, ULONG bytes, ULONG format, int visible_width, int force,
    const DockRect *check, DockBytesEqual equal, DockRect *box)
{
    size_t raw_row = (size_t)d->width*bytes, visible_raw = (size_t)visible_width*bytes;
    size_t row565 = (size_t)d->width*2, needed = raw_row*d->height;
    int row, top = -1, bottom = 0, left = visible_width, right = 0;
    if (needed > d->raw_capacity) {
        unsigned char *raw = malloc(needed);
        if (!raw) return -1;
        free(d->raw); d->raw = raw; d->raw_capacity = needed; d->raw_valid = 0;
    }
    if (!d->raw_valid || d->raw_bytes != (int)bytes) force = 1;
    if (check && (check->left < 0 || check->right > visible_width || check->top < 0 ||
        check->bottom > d->height || check->left >= check->right || check->top >= check->bottom))
        check = NULL;
    if (box) *box = (DockRect){0,0,0,0};
    if (!force && check) {
        for (row = check->top; row < check->bottom; ++row)
            if (!equal(d->raw+(size_t)row*raw_row+(size_t)check->left*bytes,
                       source+(size_t)row*stride+(size_t)check->left*bytes,
                       (unsigned long)(check->right-check->left)*bytes)) break;
        if (row == check->bottom) return 0;
    }
    for (row = 0; row < d->height; ++row) {
        unsigned char *copy = d->raw+(size_t)row*raw_row;
        const unsigned char *line = source+(size_t)row*stride;
        int l = 0, r = visible_width;
        if (!force) {
            size_t a = 0, b = visible_raw;
            if (equal(copy,line,(unsigned long)visible_raw)) continue;
            while (a < b && copy[a] == line[a]) ++a;
            while (b > a && copy[b-1] == line[b-1]) --b;
            l = (int)(a/bytes); r = (int)((b+bytes-1)/bytes);
        }
        memcpy(copy,line,visible_raw);
        row_to_rgb565(cache+row*row565,line,visible_width,format);
        if (visible_width < d->width) memset(cache+row*row565+(size_t)visible_width*2,0,row565-(size_t)visible_width*2);
        if (top < 0) top = row;
        bottom = row+1;
        if (l < left) left = l;
        if (r > right) right = r;
    }
    d->raw_valid = 1; d->raw_bytes = (int)bytes;
    if (force) { if (box) *box = (DockRect){0,0,d->width,d->height}; return 1; }
    if (top < 0) return 0;
    if (box) *box = (DockRect){left,top,right,bottom};
    return 1;
}

int amiga_display_capture565(AmigaDisplay *d, unsigned char *cache,
                            int x, int y, int force, const DockRect *check, DockBytesEqual equal,
                            DockRect *box)
{
    APTR lock, base = NULL;
    ULONG stride = 0, format = ~0UL, bytes = 0, width = 0, height = 0;
    const unsigned char *source;
    size_t row_bytes, visible_bytes;
    int row, changed = force, left = 0, top = 0, right, bottom, visible_width;
    if (!d || !cache || !d->cyber || d->depth < 15 ||
        d->screen->RastPort.BitMap != d->source_bitmap || x < 0 || y < 0 ||
        x >= d->screen->Width || y+d->height > d->screen->Height) return -1;
    visible_width = d->screen->Width-x;
    if (visible_width > d->width) visible_width = d->width;
    lock = LockBitMapTags(d->source_bitmap, LBMI_BASEADDRESS, (ULONG)&base,
        LBMI_BYTESPERROW, (ULONG)&stride, LBMI_BYTESPERPIX, (ULONG)&bytes,
        LBMI_PIXFMT, (ULONG)&format, LBMI_WIDTH, (ULONG)&width,
        LBMI_HEIGHT, (ULONG)&height, TAG_DONE);
    if (!lock) return -1;
    if (!base || !rgb565_source(format,bytes) ||
        width < (ULONG)(x+visible_width) || height < (ULONG)(y+d->height) ||
        stride < width*bytes) {
        UnLockBitMap(lock); return -1;
    }
    if (!equal) equal = dock_bytes_equal_c;
    row_bytes = (size_t)d->width*2;
    visible_bytes = (size_t)visible_width*2;
    if (format != PIXFMT_RGB16) {
        int result = capture_raw(d,cache,(const unsigned char *)base+(size_t)y*stride+(size_t)x*bytes,
            stride,bytes,format,visible_width,force,check,equal,box);
        UnLockBitMap(lock);
        return result;
    }
    source = (const unsigned char *)base+(size_t)y*stride+x*2;
    right = visible_width; bottom = d->height;
    if (check && check->left >= 0 && check->right <= right &&
        check->top >= 0 && check->bottom <= bottom &&
        check->left < check->right && check->top < check->bottom) {
        left = check->left; top = check->top; right = check->right; bottom = check->bottom;
    }
    if (box) *box = (DockRect){0,0,0,0};
    /* Without a check the box scan below is the comparison. */
    if (!changed && (!box || check)) for (row = top; row < bottom; ++row)
        if (!equal(cache+(size_t)row*row_bytes+left*2,
                   source+(size_t)row*stride+left*2,(unsigned long)(right-left)*2)) {
            changed = 1; break;
        }
    if (box && !force && (changed || !check)) {
        /* Find the change over the whole canvas, not only the check, so
           every stale pixel is refreshed as by a whole copy. */
        *box = dock_rgb565_refresh(cache,source,stride,d->width,visible_width,d->height,equal);
        changed = box->left < box->right;
    } else if (changed) for (row = 0; row < d->height; ++row) {
        unsigned char *line = cache+(size_t)row*row_bytes;
        memcpy(line,source+(size_t)row*stride,visible_bytes);
        if (visible_bytes < row_bytes) memset(line+visible_bytes,0,row_bytes-visible_bytes);
    }
    if (box && force) *box = (DockRect){0,0,d->width,d->height};
    UnLockBitMap(lock);
    return changed;
}

int amiga_display_drag_reference565(AmigaDisplay *d, struct BitMap *backing,
    unsigned char *reference, int visible_width)
{
    struct RastPort rp;
    int row;
    if (!d || !d->cyber || d->depth < 15 ||
        !backing || visible_width < 1 || visible_width > d->width) return 0;
    /* reference is the capture's RGB565 cache: the raw copy no longer matches. */
    d->raw_valid = 0;
    if (GetCyberMapAttr(backing,CYBRMATTR_PIXFMT) != PIXFMT_RGB16) {
        if (!read_rect565(d,backing,0,0,visible_width,d->height,reference,(size_t)d->width*2)) return 0;
    } else {
    InitRastPort(&rp);
    rp.BitMap = backing;
    if (ReadPixelArray(reference,0,0,d->width*2,&rp,0,0,
        visible_width,d->height,RECTFMT_RAW) != (ULONG)visible_width*d->height) return 0;
    }
    if (visible_width < d->width) for (row = 0; row < d->height; ++row)
        memset(reference+((size_t)row*d->width+visible_width)*2,0,(d->width-visible_width)*2);
    return 1;
}

int amiga_display_drag_mask565(AmigaDisplay *d, const unsigned char *reference,
    unsigned char *mask, DockRect *marked, int x, int y, int left, int top, int right, int bottom,
    DockBytesEqual equal)
{
    struct RastPort rp;
    int row, col, changed = 0, width, scan_left = left, scan_top = top, scan_right = right, scan_bottom = bottom;
    int marked_top = -1, marked_bottom = 0;
    if (!d) return -1;
    width = d->width;
    if (!d->cyber || d->depth < 15 || !reference || !mask ||
        d->screen->RastPort.BitMap != d->source_bitmap ||
        left < 0 || top < 0 || right > width || bottom > d->height ||
        left >= right || top >= bottom || x < 0 || y < 0 ||
        x+right > d->screen->Width || y+bottom > d->screen->Height) return -1;
    if (!equal) equal = dock_bytes_equal_c;
    /* Stale 1s can only lie inside the previous box: scan its union with
       the pointer box. Without a box, scan the whole mask. */
    if (!marked) { scan_left = scan_top = 0; scan_right = width; scan_bottom = d->height; }
    else if (marked->left < marked->right && marked->top < marked->bottom) {
        if (marked->left < scan_left) scan_left = marked->left > 0 ? marked->left : 0;
        if (marked->top < scan_top) scan_top = marked->top > 0 ? marked->top : 0;
        if (marked->right > scan_right) scan_right = marked->right < width ? marked->right : width;
        if (marked->bottom > scan_bottom) scan_bottom = marked->bottom < d->height ? marked->bottom : d->height;
    }
    /* Workbench can keep Layers locked throughout a drag. This RastPort has
       no Layer and the SAGA native GetImage copies without taking its lock. */
    if (GetCyberMapAttr(d->source_bitmap,CYBRMATTR_PIXFMT) != PIXFMT_RGB16) {
        if (!read_rect565(d,d->source_bitmap,x+left,y+top,right-left,bottom-top,
                d->upload+((size_t)top*width+left)*2,(size_t)width*2)) return -1;
    } else {
    InitRastPort(&rp);
    rp.BitMap = d->source_bitmap;
    if (ReadPixelArray(d->upload,left,top,width*2,&rp,x+left,y+top,
            right-left,bottom-top,RECTFMT_RAW) != (ULONG)(right-left)*(bottom-top)) return -1;
    }
    for (row = scan_top; row < scan_bottom; ++row) {
        unsigned char *line = mask+(size_t)row*width;
        const unsigned char *a = reference+(size_t)row*width*2;
        const unsigned char *b = d->upload+(size_t)row*width*2;
        int any = 0;
        if (row < top || row >= bottom || equal(a+left*2,b+left*2,(right-left)*2)) {
            if (memchr(line+scan_left,1,scan_right-scan_left)) {
                memset(line+scan_left,0,scan_right-scan_left); changed = 1;
            }
            continue;
        }
        if (left > scan_left && memchr(line+scan_left,1,left-scan_left)) {
            memset(line+scan_left,0,left-scan_left); changed = 1;
        }
        if (right < scan_right && memchr(line+right,1,scan_right-right)) {
            memset(line+right,0,scan_right-right); changed = 1;
        }
        for (col = left; col < right; ++col) {
            int pixel = a[col*2] != b[col*2] || a[col*2+1] != b[col*2+1];
            any |= pixel;
            if (line[col] != pixel) { line[col] = pixel; changed = 1; }
        }
        if (any) { if (marked_top < 0) marked_top = row; marked_bottom = row+1; }
    }
    /* Every 1 now lies in the pointer box, on the rows found. */
    if (marked) *marked = marked_top < 0 ? (DockRect){0,0,0,0} :
        (DockRect){left,marked_top,right,marked_bottom};
    return changed;
}

int amiga_display_capture(AmigaDisplay *d, DockImage *im, int x, int y)
{
    int ix, iy;
    if (!d || !im || im->width != d->width || im->height != d->height) return 0;
    if (d->depth == 16 && amiga_display_capture565(d,d->upload,x,y,1,NULL,NULL,NULL) == 1) {
        d->rgb565_to_rgba(im->pixels,d->upload,(unsigned long)d->width*d->height);
        return 1;
    }
    if (d->depth > 8) {
        if (ReadPixelArray(d->upload, 0, 0, d->stride, &d->screen->RastPort,
                x, y, d->width, d->height, RECTFMT_RGB) != (ULONG)d->width*d->height) return 0;
        dock_rgb24_to_rgba_c(im->pixels,d->upload,(unsigned long)d->width*d->height);
    } else {
        if (read_indexed_pixels(d, x, y) != (LONG)d->width*d->height) return 0;
        for (iy = 0; iy < d->height; ++iy) for (ix = 0; ix < d->width; ++ix) {
            unsigned int pen = d->upload[(size_t)iy*d->stride+ix];
            unsigned char *out = im->pixels + ((size_t)iy * d->width + ix) * 4;
            out[0] = d->palette[pen*3] >> 24;
            out[1] = d->palette[pen*3+1] >> 24;
            out[2] = d->palette[pen*3+2] >> 24;
            out[3] = 255;
        }
    }
    return 1;
}

/* RTG only. Native big-endian RGB565 takes one encode and a raw copy on
   RGB16 bitmaps (the RAW format capture uses), instead of RGB24 that the
   OS converts again. RGB24 is supported by SAGA/P96, including drivers
   whose RGBA path is incomplete. */
static int native565(const AmigaDisplay *d, struct RastPort *port)
{
    return d->depth == 16 && d->rgba_to_rgb565 && port->BitMap &&
        GetCyberMapAttr(port->BitMap,CYBRMATTR_PIXFMT) == PIXFMT_RGB16;
}

/* Converts one rectangle of im, packed at its own width in the upload
   buffer, and writes it at the same place in port. Adds its stage times
   and pixels to timing. */
static void upload_rect(AmigaDisplay *d, struct RastPort *port, int rgb565, const DockImage *im,
    const DockRect *r, uint64_t (*clock)(void), AmigaDisplayTiming *timing)
{
    int row, width = r->right-r->left, height = r->bottom-r->top;
    size_t stride = (size_t)width*(rgb565 ? 2 : 3);
    const unsigned char *source = im->pixels+((size_t)r->top*im->width+r->left)*4;
    uint64_t started = clock ? clock() : 0, converted;
    /* Whole rows are contiguous in both buffers: one call converts them. */
    if (width == im->width) {
        if (rgb565) d->rgba_to_rgb565(d->upload,source,(unsigned long)width*height);
        else d->rgba_to_rgb24(d->upload,source,(unsigned long)width*height);
    } else for (row = 0; row < height; ++row) {
        const unsigned char *line = source+(size_t)row*im->width*4;
        if (rgb565) d->rgba_to_rgb565(d->upload+(size_t)row*stride,line,width);
        else d->rgba_to_rgb24(d->upload+(size_t)row*stride,line,width);
    }
    converted = clock ? clock() : 0;
    /* CGX documentation warns the returned pixel count is unreliable. */
    WritePixelArray(d->upload,0,0,stride,port,r->left,r->top,width,height,
        rgb565 ? RECTFMT_RAW : RECTFMT_RGB);
    if (!timing) return;
    timing->pixels += (unsigned long)width*height;
    if (clock) {
        timing->conversion_ticks += converted-started;
        timing->upload_ticks += clock()-converted;
    }
}

static int present_port(AmigaDisplay *d, struct RastPort *port, const DockImage *im,
    uint64_t (*clock)(void), AmigaDisplayTiming *timing)
{
    int x, y, result;
    uint64_t started = clock ? clock() : 0, converted;
    if (timing) memset(timing, 0, sizeof(*timing));
    if (!d || !port || !im || !im->pixels || im->width != d->width || im->height != d->height) return 0;
    if (d->depth > 8) {
        DockRect whole = {0,0,d->width,d->height};
        upload_rect(d,port,native565(d,port),im,&whole,clock,timing);
        return 1;
    }
    for (y = 0; y < d->height; ++y) {
        const unsigned char *s = im->pixels + (size_t)y * d->width * 4;
        unsigned char *out = d->upload + (size_t)y * d->stride;
        for (x = 0; x < d->width; ++x, s += 4)
            out[x] = d->lookup[((s[0]>>4)<<8)|((s[1]>>4)<<4)|(s[2]>>4)];
    }
    converted = clock ? clock() : 0;
    result = write_indexed_pixels(d,port) >= 0;
    if (timing) {
        timing->pixels = (unsigned long)d->width*d->height;
        if (clock) {
            timing->conversion_ticks = converted-started;
            timing->upload_ticks = clock()-converted;
        }
    }
    return result;
}

int amiga_display_present_damage(AmigaDisplay *d, struct Window *window, const DockImage *im,
    const DockDamage *damage, uint64_t (*clock)(void), AmigaDisplayTiming *timing)
{
    DockRect rect[AMIGA_DISPLAY_TILES];
    struct RastPort *port = window ? window->RPort : NULL;
    int i, n, rgb565;
    if (!damage || damage->full || !d || d->depth <= 8) return present_port(d,port,im,clock,timing);
    if (timing) memset(timing,0,sizeof(*timing));
    if (!port || !im || !im->pixels || im->width != d->width || im->height != d->height) return 0;
    n = dock_damage_upload_rects(damage,d->width,d->height,AMIGA_DISPLAY_TILES,rect);
    if (!n) return 1;
    rgb565 = native565(d,port);
    for (i = 0; i < n; ++i) upload_rect(d,port,rgb565,im,&rect[i],clock,timing);
    return 1;
}

int amiga_display_write_rgba(AmigaDisplay *d, struct RastPort *port, const DockImage *im)
{
    if (!d || !d->cyber || !port || !im || !im->pixels) return 0;
    return WritePixelArray(im->pixels,0,0,im->width*4,port,0,0,im->width,im->height,RECTFMT_RGBA) ==
        (ULONG)im->width*im->height;
}

int amiga_display_read_rgba(AmigaDisplay *d, struct RastPort *port, DockImage *im)
{
    if (!d || !d->cyber || !port || !im || !im->pixels) return 0;
    return ReadPixelArray(im->pixels,0,0,im->width*4,port,0,0,im->width,im->height,RECTFMT_RGBA) ==
        (ULONG)im->width*im->height;
}

int amiga_display_present(AmigaDisplay *d, struct Window *window, const DockImage *im)
{
    return present_port(d,window ? window->RPort : NULL,im,NULL,NULL);
}

struct RastPort *amiga_display_begin_frame(AmigaDisplay *d, const DockImage *background)
{
    if (!d || d->depth <= 8) return NULL;
    if (!d->frame_bitmap) {
        d->frame_bitmap = AllocBitMap(d->width,d->height,d->depth,BMF_MINPLANES,d->source_bitmap);
        if (!d->frame_bitmap) return NULL;
        InitRastPort(&d->frame);
        d->frame.BitMap = d->frame_bitmap;
    }
    return !background || present_port(d,&d->frame,background,NULL,NULL) ? &d->frame : NULL;
}

int amiga_display_capture_frame(AmigaDisplay *d, DockImage *im)
{
    unsigned long count;
    if (!d || !d->frame_bitmap || !im || !im->pixels ||
        im->width != d->width || im->height != d->height) return 0;
    count = (unsigned long)d->width*d->height;
    if (d->depth == 16 && GetCyberMapAttr(d->frame_bitmap,CYBRMATTR_PIXFMT) == PIXFMT_RGB16 &&
        ReadPixelArray(d->upload,0,0,d->width*2,&d->frame,0,0,d->width,d->height,RECTFMT_RAW) == count) {
        d->rgb565_to_rgba(im->pixels,d->upload,count);
        return 1;
    }
    if (ReadPixelArray(d->upload,0,0,d->stride,&d->frame,0,0,d->width,d->height,RECTFMT_RGB) != count)
        return 0;
    dock_rgb24_to_rgba_c(im->pixels,d->upload,count);
    return 1;
}

void amiga_display_end_frame(AmigaDisplay *d, struct Window *window)
{
    LP9NR(0x25e, DockBltBitMapRastPort, const struct BitMap *, d->frame_bitmap, a0,
        ULONG, 0, d0, ULONG, 0, d1, struct RastPort *, window->RPort, a1,
        ULONG, 0, d2, ULONG, 0, d3, ULONG, d->width, d4, ULONG, d->height, d5,
        ULONG, 0xc0, d6, , GfxBase);
}

int amiga_display_present_tile(AmigaDisplay *d, struct Window *window, const DockImage *im,
    int x, int y, int width, int height)
{
    DockRect r;
    if (!d || !window || !im || !im->pixels || d->depth <= 8 ||
        im->width != d->width || im->height != d->height ||
        x < 0 || y < 0 || width < 1 || height < 1 || width > d->width-x || height > d->height-y)
        return 0;
    r = (DockRect){x,y,x+width,y+height};
    upload_rect(d,window->RPort,native565(d,window->RPort),im,&r,NULL,NULL);
    return 1;
}
