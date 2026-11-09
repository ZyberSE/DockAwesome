/* Floating pictures in SAGA's PiP1 overlay. See amiga_float.h.

   Turning PiP1's DMA on or off (opening, closing, reconfiguring it) makes
   the dock in PiP2 show a doubled image for a few frames on Coffin (core
   11900+, 24-bit screen, filmed 2026-11-09). So PiP1 opens once, stays on
   showing the colour key when nothing floats, and only its pointer and
   position registers change afterwards. It reopens only to grow. */
#include "amiga_float.h"
#include "amiga_pip.h"
#include <intuition/screens.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { KEY565 = 0xf81f };

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
};

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
    int cap_w, cap_h, i;
    Area all;
    if (f->pip && w <= f->cap_w && h <= f->cap_h) return 1;
    cap_w = (w > f->cap_w ? w : f->cap_w);
    cap_w = (cap_w+31)&~31;
    cap_h = h > f->cap_h ? h : f->cap_h;
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

/* The overlay's top left, and the picture's offset in it, for the picture
   at f->x, f->y kept on the screen (and one line down, a PiP rule). */
static void layout(const AmigaFloat *f, int *left, int *top, int *ox, int *oy)
{
    int x = dock_clamp(f->x,0,f->screen->Width-f->width);
    int y = dock_clamp(f->y,1,f->screen->Height-f->height);
    *left = x < f->screen->Width-f->cap_w ? x : f->screen->Width-f->cap_w;
    *top = y < f->screen->Height-f->cap_h ? y : f->screen->Height-f->cap_h;
    *ox = x-*left; *oy = y-*top;
}

/* Draw the picture (or nothing) at ox, oy into the back buffer and show it
   there with the overlay at left, top. */
static int render(AmigaFloat *f, int picture, int left, int top, int ox, int oy)
{
    int back = amiga_pip_back_index(f->pip), stride, row;
    unsigned char *buffer = amiga_pip_back_buffer(f->pip);
    stride = amiga_pip_stride(f->pip);
    fill_key(f,buffer,f->drawn[back]);
    f->drawn[back] = (Area){0};
    if (picture) {
        for (row = 0; row < f->height; ++row)
            memcpy(buffer+(size_t)(oy+row)*stride+(size_t)ox*2,
                f->image+(size_t)row*f->width,(size_t)f->width*2);
        f->drawn[back] = (Area){ox,oy,f->width,f->height};
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
    if (width > 0 && height > 0) reserve(f,width,height);
    return f;
}

int amiga_float_show(AmigaFloat *f, const DockImage *image, int x, int y)
{
    int row, col, left, top, ox, oy;
    uint16_t *pixels;
    if (!f || !image || !image->pixels || image->width < 1 || image->height < 1 ||
        !reserve(f,image->width,image->height)) return 0;
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
    return f->shown;
}

int amiga_float_move(AmigaFloat *f, int x, int y)
{
    int left, top, ox, oy;
    if (!f || !f->shown) return 0;
    if (x == f->x && y == f->y) return 1;
    f->x = x; f->y = y;
    layout(f,&left,&top,&ox,&oy);
    /* Away from the screen's edges the picture keeps its offset and only
       the position registers change. */
    if (ox == f->ox && oy == f->oy) return amiga_pip_move(f->pip,left,top);
    f->shown = render(f,1,left,top,ox,oy);
    return f->shown;
}

int amiga_float_shown(const AmigaFloat *f)
{
    return f && f->shown;
}

void amiga_float_hide(AmigaFloat *f)
{
    int left, top, ox, oy;
    if (!f || !f->shown) return;
    f->shown = 0;
    layout(f,&left,&top,&ox,&oy);
    if (!render(f,0,left,top,ox,oy)) amiga_pip_suspend(f->pip);
}

void amiga_float_destroy(AmigaFloat *f)
{
    if (!f) return;
    amiga_pip_close(f->pip);
    free(f->image);
    free(f);
}
