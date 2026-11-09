/* Floating pictures in SAGA's PiP1 overlay. See amiga_float.h.

   PiP1 is on only while something floats: kept on (keyed clear) it stayed
   over the dock's rows after a label, and with the dock 992 px wide at the
   bottom of a 24-bit Coffin screen the whole dock came out scrambled
   (filmed 2026-11-10). While shown, a new picture reuses the open overlay
   when it fits (sized for the largest icon with the Remove pill), so a
   drag changes only pointer and position registers.

   PiP1 must not share display rows with a wide PiP2: with the dock 992 px
   wide, a label's overlay (96 rows, reaching over the dock's) scrambled
   the dock; beside a 96 px side dock it showed fine. Past SHARE_WIDTH the
   overlay is cut to the picture and kept off the dock's rows (the band);
   a picture on them is not shown (-1), so the caller draws it otherwise. */
#include "amiga_float.h"
#include "amiga_pip.h"
#include <intuition/screens.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { KEY565 = 0xf81f };
/* PiP1 and PiP2 widths that shared rows on Coffin: 128 + 96 showed fine,
   128 + 992 did not. */
enum { SHARE_WIDTH = 256 };
/* Rows kept free between PiP1 and a wide PiP2: ending right on the row
   above the dock still scrambled it (the next row is fetched ahead). */
enum { BAND_GAP = 4 };

typedef struct { int x, y, w, h; } Area;

struct AmigaFloat {
    struct Screen *screen;
    AmigaPip *pip;
    int x_offset, y_offset;
    int cap_w, cap_h;      /* the overlay; cap_w is a multiple of 32 */
    uint16_t *image;       /* the picture in RGB565, KEY565 where clear */
    int width, height;     /* its size */
    int x, y;              /* its top left as asked */
    int ox, oy;            /* where the front buffer holds it in the overlay */
    int shown;
    Area drawn[2];         /* the picture's pixels in each buffer */
    int min_w, min_h;      /* the size asked at create */
    int band_top, band_bottom, band_width; /* PiP2's display rows and width */
    int tight;             /* the overlay is the picture, off the band */
};

/* A picture of height h at y in tight mode: 1 clear of the band, 2 when
   it reaches down onto the band (the overlay then ends at the band's top
   and the rest is cut), -1 when its top lies on the band or there is no
   room above it. */
static int band_fit(const AmigaFloat *f, int y, int h)
{
    int top = dock_clamp(y,1,f->screen->Height-h), limit = f->band_top-BAND_GAP;
    if (top+h <= limit || top >= f->band_bottom+BAND_GAP) return 1;
    if (top >= limit || limit-h < 1) return -1;
    return 2;
}

static void fill_key(AmigaFloat *f, unsigned char *buffer, Area a)
{
    int stride = amiga_pip_stride(f->pip), row, col;
    for (row = 0; row < a.h; ++row) {
        uint16_t *out = (uint16_t *)(buffer+(size_t)(a.y+row)*stride)+a.x;
        for (col = 0; col < a.w; ++col) out[col] = KEY565;
    }
}

/* Open PiP1 at least w by h, keyed clear and on. Only a larger picture
   than any before reopens it. */
static int reserve(AmigaFloat *f, int w, int h)
{
    int cap_w, cap_h, i, pw = (w+31)&~31;
    Area all;
    if (f->pip && (f->tight ? pw == f->cap_w && h == f->cap_h : w <= f->cap_w && h <= f->cap_h))
        return 1;
    if (f->tight) { cap_w = pw; cap_h = h; }
    else {
        /* At least the size asked at create, or the one open now. */
        int base_w = f->pip ? f->cap_w : f->min_w, base_h = f->pip ? f->cap_h : f->min_h;
        cap_w = ((w > base_w ? w : base_w)+31)&~31;
        cap_h = h > base_h ? h : base_h;
    }
    if (cap_w > f->screen->Width || cap_h >= f->screen->Height) return 0;
    if (f->pip) { amiga_pip_close(f->pip); f->pip = NULL; }
    f->shown = 0;
    f->pip = amiga_pip_open_unit(f->screen,1,0,1,cap_w,cap_h,0,NULL,0);
    if (!f->pip) return 0;
    if (!amiga_pip_set_alignment(f->pip,f->x_offset,f->y_offset) || !amiga_pip_set_keying(f->pip,1)) {
        amiga_pip_close(f->pip); f->pip = NULL; return 0;
    }
    f->cap_w = cap_w; f->cap_h = cap_h;
    all = (Area){0,0,cap_w,cap_h};
    /* Both buffers keyed clear; the presents turn PiP1 on (or wait for
       the screen to come to the front). */
    for (i = 0; i < 2; ++i) {
        fill_key(f,amiga_pip_back_buffer(f->pip),all);
        f->drawn[i] = (Area){0};
        amiga_pip_present(f->pip);
    }
    return 1;
}

/* The overlay's top left (on the screen and one line down, a PiP rule)
   and the picture's offset in it. The picture follows f->x, f->y past the
   screen's edges and is cut there, as the dock's own copy of it is. */
static void layout(const AmigaFloat *f, int *left, int *top, int *ox, int *oy)
{
    *left = dock_clamp(f->x,0,f->screen->Width-f->cap_w);
    *top = dock_clamp(f->y,1,f->screen->Height-f->cap_h);
    /* Reaching onto the band: the overlay ends at the band's top. */
    if (f->tight && *top < f->band_top && *top+f->cap_h > f->band_top-BAND_GAP)
        *top = f->band_top-BAND_GAP-f->cap_h;
    *ox = f->x-*left; *oy = f->y-*top;
}

/* Draw the picture (or nothing) at ox, oy, cut to the overlay, into the
   back buffer and show it there with the overlay at left, top. */
static int render(AmigaFloat *f, int picture, int left, int top, int ox, int oy)
{
    int back = amiga_pip_back_index(f->pip), stride, row;
    unsigned char *buffer = amiga_pip_back_buffer(f->pip);
    stride = amiga_pip_stride(f->pip);
    fill_key(f,buffer,f->drawn[back]);
    f->drawn[back] = (Area){0};
    if (picture) {
        int col0 = ox < 0 ? -ox : 0, row0 = oy < 0 ? -oy : 0;
        int col1 = f->cap_w-ox < f->width ? f->cap_w-ox : f->width;
        int row1 = f->cap_h-oy < f->height ? f->cap_h-oy : f->height;
        if (col1 > col0 && row1 > row0) {
            for (row = row0; row < row1; ++row)
                memcpy(buffer+(size_t)(oy+row)*stride+(size_t)(ox+col0)*2,
                    f->image+(size_t)row*f->width+col0,(size_t)(col1-col0)*2);
            f->drawn[back] = (Area){ox+col0,oy+row0,col1-col0,row1-row0};
        }
    }
    if (!amiga_pip_move(f->pip,left,top) || !amiga_pip_present(f->pip)) return 0;
    f->ox = ox; f->oy = oy;
    return 1;
}

AmigaFloat *amiga_float_create(struct Screen *screen, int x_offset, int y_offset, int width, int height)
{
    AmigaFloat *f;
    if (!screen) return NULL;
    f = calloc(1,sizeof(*f));
    if (!f) return NULL;
    f->screen = screen; f->x_offset = x_offset; f->y_offset = y_offset;
    if (width > 0 && height > 0) { f->min_w = (width+31)&~31; f->min_h = height; }
    return f;
}

void amiga_float_set_band(AmigaFloat *f, int top, int bottom, int width)
{
    if (!f) return;
    f->band_top = top; f->band_bottom = bottom; f->band_width = width;
}

int amiga_float_show(AmigaFloat *f, const DockImage *image, int x, int y)
{
    int row, col, left, top, ox, oy, tight, fit = 1;
    uint16_t *pixels;
    if (!f || !image || !image->pixels || image->width < 1 || image->height < 1) return 0;
    tight = f->band_bottom > f->band_top &&
        ((image->width > f->min_w ? image->width : f->min_w)+31)/32*32+f->band_width > SHARE_WIDTH;
    if (tight && (fit = band_fit(f,y,image->height)) < 0) {
        amiga_float_hide(f);
        return -1;
    }
    if (tight != f->tight) { amiga_float_hide(f); f->tight = tight; }
    if (!reserve(f,image->width,image->height)) return 0;
    pixels = malloc((size_t)image->width*image->height*2);
    if (!pixels) return 0;
    for (row = 0; row < image->height; ++row) {
        const unsigned char *in = image->pixels+(size_t)row*image->width*4;
        uint16_t *out = pixels+(size_t)row*image->width;
        for (col = 0; col < image->width; ++col, in += 4) {
            unsigned int v = KEY565;
            if (in[3] >= 128) {
                v = ((unsigned int)(in[0]&0xf8)<<8)|((unsigned int)(in[1]&0xfc)<<3)|((unsigned int)in[2]>>3);
                if (v == KEY565) v = KEY565^0x20; /* opaque magenta must not key */
            }
            out[col] = (uint16_t)v;
        }
    }
    free(f->image);
    f->image = pixels; f->width = image->width; f->height = image->height;
    f->x = x; f->y = y;
    layout(f,&left,&top,&ox,&oy);
    f->shown = render(f,1,left,top,ox,oy);
    return f->shown ? fit : 0;
}

int amiga_float_move(AmigaFloat *f, int x, int y)
{
    int left, top, ox, oy, fit = 1;
    if (!f || !f->shown) return 0;
    if (f->tight && (fit = band_fit(f,y,f->height)) < 0) {
        amiga_float_hide(f);
        return -1;
    }
    if (x == f->x && y == f->y) return fit;
    f->x = x; f->y = y;
    layout(f,&left,&top,&ox,&oy);
    /* Away from the screen's edges and the band the picture keeps its
       offset and only the position registers change. */
    if (ox == f->ox && oy == f->oy) return amiga_pip_move(f->pip,left,top) ? fit : 0;
    f->shown = render(f,1,left,top,ox,oy);
    return f->shown ? fit : 0;
}

int amiga_float_shown(const AmigaFloat *f)
{
    return f && f->shown;
}

void amiga_float_hide(AmigaFloat *f)
{
    if (!f || !f->pip) return;
    f->shown = 0;
    amiga_pip_close(f->pip);
    f->pip = NULL;
}

void amiga_float_destroy(AmigaFloat *f)
{
    if (!f) return;
    amiga_pip_close(f->pip);
    free(f->image);
    free(f);
}
