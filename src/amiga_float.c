/* Floating pictures in SAGA's PiP1 overlay. See amiga_float.h. */
#include "amiga_float.h"
#include "amiga_pip.h"
#include <intuition/screens.h>
#include <stdlib.h>
#include <string.h>

enum { KEY565 = 0xf81f };

struct AmigaFloat {
    struct Screen *screen;
    AmigaPip *pip;
    int x_offset, y_offset;
    int width, height, padded; /* the open overlay's image and its 32-px width */
    int x, y;                  /* the image's top left as asked */
};

AmigaFloat *amiga_float_create(struct Screen *screen, int x_offset, int y_offset)
{
    AmigaFloat *f;
    if (!screen) return NULL;
    f = calloc(1,sizeof(*f));
    if (!f) return NULL;
    f->screen = screen; f->x_offset = x_offset; f->y_offset = y_offset;
    return f;
}

/* The overlay's top left for an image at x, y: on the screen, and at
   least one line down (PiP2 rule, kept for PiP1). Keyed padding may lie
   past the right edge. */
static void place(const AmigaFloat *f, int x, int y, int *left, int *top)
{
    *left = dock_clamp(x,0,f->screen->Width-f->width);
    *top = dock_clamp(y,1,f->screen->Height-f->height);
}

int amiga_float_show(AmigaFloat *f, const DockImage *image, int x, int y)
{
    int padded, row, col, left, top, stride;
    unsigned char *buffer;
    if (!f || !image || !image->pixels || image->width < 1 || image->height < 1 ||
        image->width > f->screen->Width || image->height >= f->screen->Height) return 0;
    padded = (image->width+31)&~31;
    if (f->pip && (f->width != image->width || f->height != image->height)) amiga_float_hide(f);
    f->width = image->width; f->height = image->height; f->padded = padded;
    place(f,x,y,&left,&top);
    if (!f->pip) {
        f->pip = amiga_pip_open_unit(f->screen,1,left,top,padded,image->height,padded-image->width,NULL,0);
        if (!f->pip) return 0;
        if (!amiga_pip_set_alignment(f->pip,f->x_offset,f->y_offset) || !amiga_pip_set_keying(f->pip,1)) {
            amiga_float_hide(f); return 0;
        }
    } else if (!amiga_pip_move(f->pip,left,top)) return 0;
    buffer = amiga_pip_back_buffer(f->pip);
    stride = amiga_pip_stride(f->pip);
    for (row = 0; row < image->height; ++row) {
        const unsigned char *in = image->pixels+(size_t)row*image->width*4;
        unsigned char *out = buffer+(size_t)row*stride;
        for (col = 0; col < padded; ++col, out += 2) {
            unsigned int v = KEY565;
            if (col < image->width && in[col*4+3] >= 128) {
                const unsigned char *px = in+col*4;
                v = ((unsigned int)(px[0]&0xf8)<<8)|((unsigned int)(px[1]&0xfc)<<3)|((unsigned int)px[2]>>3);
                if (v == KEY565) v = KEY565^0x20; /* opaque magenta must not key */
            }
            out[0] = (unsigned char)(v>>8); out[1] = (unsigned char)v;
        }
    }
    f->x = x; f->y = y;
    if (!amiga_pip_present(f->pip)) { amiga_float_hide(f); return 0; }
    return 1;
}

int amiga_float_move(AmigaFloat *f, int x, int y)
{
    int left, top;
    if (!f || !f->pip) return 0;
    if (x == f->x && y == f->y) return 1;
    place(f,x,y,&left,&top);
    f->x = x; f->y = y;
    return amiga_pip_move(f->pip,left,top);
}

int amiga_float_shown(const AmigaFloat *f)
{
    return f && f->pip;
}

void amiga_float_hide(AmigaFloat *f)
{
    if (!f || !f->pip) return;
    amiga_pip_close(f->pip);
    f->pip = NULL;
}

void amiga_float_destroy(AmigaFloat *f)
{
    if (!f) return;
    amiga_float_hide(f);
    free(f);
}
