#include "amiga_display.h"
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
    DockConvertRGB565 rgb565_to_rgba;
    int width, height, depth, stride;
    unsigned char *upload, *lookup;
    ULONG palette[256 * 3];
    struct RastPort temp;
    struct BitMap *temp_bitmap;
    struct BitMap *source_bitmap;
    struct RastPort frame;
    struct BitMap *frame_bitmap;
};

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
    int r, g, b, i, n;
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
        d->upload = malloc((size_t)d->stride * height);
    } else {
        /* WritePixelArray8 requires 16-pixel row alignment and a one-row temporary bitmap. */
        d->stride = (width + 15) & ~15;
        d->upload = calloc((size_t)d->stride, height);
        d->lookup = malloc(4096);
        d->temp_bitmap = AllocBitMap(d->stride, 1, 8, BMF_CLEAR, NULL);
        if (d->temp_bitmap) { InitRastPort(&d->temp); d->temp.BitMap = d->temp_bitmap; }
        n = screen->ViewPort.ColorMap->Count;
        if (n > 256) n = 256;
        GetRGB32(screen->ViewPort.ColorMap, 0, n, d->palette);
        if (d->lookup) for (r = 0; r < 16; ++r) for (g = 0; g < 16; ++g) for (b = 0; b < 16; ++b) {
            long best_error = 0x7fffffffL;
            int best = 0;
            for (i = 0; i < n; ++i) {
                long dr = r * 17 - (d->palette[i*3] >> 24);
                long dg = g * 17 - (d->palette[i*3+1] >> 24);
                long db = b * 17 - (d->palette[i*3+2] >> 24);
                long error = 2*dr*dr + 3*dg*dg + db*db;
                if (error < best_error) { best_error = error; best = i; }
            }
            d->lookup[(r<<8)|(g<<4)|b] = best;
        }
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
    free(d->lookup); free(d->upload);
    if (d->cyber) CloseLibrary(d->cyber);
    free(d);
}

void amiga_display_set_converters(AmigaDisplay *d, DockConvertRGB24 encode, DockConvertRGB565 decode)
{
    if (!d) return;
    d->rgba_to_rgb24 = encode ? encode : dock_rgba_to_rgb24_c;
    d->rgb565_to_rgba = decode ? decode : dock_rgb565_to_rgba_c;
}

int amiga_display_depth(const AmigaDisplay *d) { return d ? d->depth : 0; }

int amiga_display_capture565(AmigaDisplay *d, unsigned char *cache,
                            int x, int y, int force, const DockRect *check, DockBytesEqual equal)
{
    APTR lock, base = NULL;
    ULONG stride = 0, format = ~0UL, bytes = 0, width = 0, height = 0;
    const unsigned char *source;
    size_t row_bytes, visible_bytes;
    int row, changed = force, left = 0, top = 0, right, bottom, visible_width;
    if (!d || !cache || !d->cyber || d->depth != 16 ||
        d->screen->RastPort.BitMap != d->source_bitmap || x < 0 || y < 0 ||
        x >= d->screen->Width || y+d->height > d->screen->Height) return -1;
    visible_width = d->screen->Width-x;
    if (visible_width > d->width) visible_width = d->width;
    lock = LockBitMapTags(d->source_bitmap, LBMI_BASEADDRESS, (ULONG)&base,
        LBMI_BYTESPERROW, (ULONG)&stride, LBMI_BYTESPERPIX, (ULONG)&bytes,
        LBMI_PIXFMT, (ULONG)&format, LBMI_WIDTH, (ULONG)&width,
        LBMI_HEIGHT, (ULONG)&height, TAG_DONE);
    if (!lock) return -1;
    if (!base || bytes != 2 || format != PIXFMT_RGB16 ||
        width < (ULONG)(x+visible_width) || height < (ULONG)(y+d->height) ||
        stride < width*2) {
        UnLockBitMap(lock); return -1;
    }
    if (!equal) equal = dock_bytes_equal_c;
    source = (const unsigned char *)base+(size_t)y*stride+x*2;
    row_bytes = (size_t)d->width*2;
    visible_bytes = (size_t)visible_width*2;
    right = visible_width; bottom = d->height;
    if (check && check->left >= 0 && check->right <= right &&
        check->top >= 0 && check->bottom <= bottom &&
        check->left < check->right && check->top < check->bottom) {
        left = check->left; top = check->top; right = check->right; bottom = check->bottom;
    }
    if (!changed) for (row = top; row < bottom; ++row)
        if (!equal(cache+(size_t)row*row_bytes+left*2,
                   source+(size_t)row*stride+left*2,(unsigned long)(right-left)*2)) {
            changed = 1; break;
        }
    if (changed) for (row = 0; row < d->height; ++row) {
        unsigned char *line = cache+(size_t)row*row_bytes;
        memcpy(line,source+(size_t)row*stride,visible_bytes);
        if (visible_bytes < row_bytes) memset(line+visible_bytes,0,row_bytes-visible_bytes);
    }
    UnLockBitMap(lock);
    return changed;
}

int amiga_display_drag_reference565(AmigaDisplay *d, struct BitMap *backing,
    unsigned char *reference, int visible_width)
{
    struct RastPort rp;
    int row;
    if (!backing || visible_width < 1 || visible_width > d->width ||
        GetCyberMapAttr(backing,CYBRMATTR_PIXFMT) != PIXFMT_RGB16) return 0;
    InitRastPort(&rp);
    rp.BitMap = backing;
    if (ReadPixelArray(reference,0,0,d->width*2,&rp,0,0,
        visible_width,d->height,RECTFMT_RAW) != (ULONG)visible_width*d->height) return 0;
    if (visible_width < d->width) for (row = 0; row < d->height; ++row)
        memset(reference+((size_t)row*d->width+visible_width)*2,0,(d->width-visible_width)*2);
    return 1;
}

int amiga_display_drag_mask565(AmigaDisplay *d, const unsigned char *reference,
    unsigned char *mask, int x, int y, int left, int top, int right, int bottom,
    DockBytesEqual equal)
{
    struct RastPort rp;
    int row, col, changed = 0, width = d->width;
    if (d->screen->RastPort.BitMap != d->source_bitmap) return -1;
    /* Workbench can keep Layers locked throughout a drag. This RastPort has
       no Layer and the SAGA native GetImage copies without taking its lock. */
    InitRastPort(&rp);
    rp.BitMap = d->source_bitmap;
    if (ReadPixelArray(d->upload,left,top,width*2,&rp,x+left,y+top,
            right-left,bottom-top,RECTFMT_RAW) != (ULONG)(right-left)*(bottom-top)) return -1;
    for (row = 0; row < d->height; ++row) {
        unsigned char *line = mask+(size_t)row*width;
        const unsigned char *a = reference+(size_t)row*width*2;
        const unsigned char *b = d->upload+(size_t)row*width*2;
        if (row < top || row >= bottom || equal(a+left*2,b+left*2,(right-left)*2)) {
            if (memchr(line,1,width)) { memset(line,0,width); changed = 1; }
            continue;
        }
        for (col = 0; col < width; ++col) {
            int pixel = col >= left && col < right &&
                (a[col*2] != b[col*2] || a[col*2+1] != b[col*2+1]);
            if (line[col] != pixel) { line[col] = pixel; changed = 1; }
        }
    }
    return changed;
}

int amiga_display_capture(AmigaDisplay *d, DockImage *im, int x, int y)
{
    int ix, iy;
    if (!d || !im || im->width != d->width || im->height != d->height) return 0;
    if (d->depth == 16 && amiga_display_capture565(d,d->upload,x,y,1,NULL,NULL) == 1) {
        d->rgb565_to_rgba(im->pixels,d->upload,(unsigned long)d->width*d->height);
        return 1;
    }
    if (d->depth > 8) {
        if (ReadPixelArray(d->upload, 0, 0, d->stride, &d->screen->RastPort,
                x, y, d->width, d->height, RECTFMT_RGB) != (ULONG)d->width*d->height) return 0;
        dock_rgb24_to_rgba_c(im->pixels,d->upload,(unsigned long)d->width*d->height);
    } else {
        if (ReadPixelArray8(&d->screen->RastPort, x, y, x+d->width-1, y+d->height-1,
                           d->upload, &d->temp) != (LONG)d->width*d->height) return 0;
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

static int present_port(AmigaDisplay *d, struct RastPort *port, const DockImage *im,
    uint64_t (*clock)(void), AmigaDisplayTiming *timing)
{
    int x, y, result;
    uint64_t started = clock ? clock() : 0, converted;
    if (timing) memset(timing, 0, sizeof(*timing));
    if (!d || !port || !im || !im->pixels || im->width != d->width || im->height != d->height) return 0;
    if (d->depth > 8) {
        /* RGB24 is supported by SAGA/P96, including drivers whose RGBA path is incomplete. */
        d->rgba_to_rgb24(d->upload, im->pixels, (unsigned long)d->width*d->height);
    } else {
        for (y = 0; y < d->height; ++y) {
            const unsigned char *s = im->pixels + (size_t)y * d->width * 4;
            unsigned char *out = d->upload + (size_t)y * d->stride;
            for (x = 0; x < d->width; ++x, s += 4)
                out[x] = d->lookup[((s[0]>>4)<<8)|((s[1]>>4)<<4)|(s[2]>>4)];
        }
    }
    converted = clock ? clock() : 0;
    if (d->depth > 8) {
        WritePixelArray(d->upload, 0, 0, d->stride, port, 0, 0,
                        d->width, d->height, RECTFMT_RGB);
        /* CGX documentation warns the returned pixel count is unreliable. */
        result = 1;
    } else {
        result = write_indexed_pixels(d,port) >= 0;
    }
    if (clock && timing) {
        timing->conversion_ticks = converted-started;
        timing->upload_ticks = clock()-converted;
    }
    return result;
}

int amiga_display_present_profiled(AmigaDisplay *d, struct Window *window, const DockImage *im,
    uint64_t (*clock)(void), AmigaDisplayTiming *timing)
{
    return present_port(d,window ? window->RPort : NULL,im,clock,timing);
}

int amiga_display_present(AmigaDisplay *d, struct Window *window, const DockImage *im)
{
    return amiga_display_present_profiled(d, window, im, NULL, NULL);
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
    int row, stride = width*3;
    if (!d || !window || !im || !im->pixels || d->depth <= 8 ||
        im->width != d->width || im->height != d->height ||
        x < 0 || y < 0 || width < 1 || height < 1 || width > d->width-x || height > d->height-y)
        return 0;
    for (row = 0; row < height; ++row) {
        const unsigned char *source = im->pixels+((size_t)(y+row)*im->width+x)*4;
        d->rgba_to_rgb24(d->upload+(size_t)row*stride,source,width);
    }
    WritePixelArray(d->upload,0,0,stride,window->RPort,x,y,width,height,RECTFMT_RGB);
    return 1;
}
