/* See amiga_drag_bob.h. */
#include "amiga_drag_bob.h"
#include "dock_bob.h"
#include <graphics/rastport.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <stdlib.h>
#define CYBERGFX_BASE_NAME b->cyber
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>

struct AmigaDragBob {
    struct Library *cyber;
    struct RastPort rp; /* the screen's bitmap, no layer */
    DockBob bob;
};

static int bob_read(void *context, unsigned char *rgb, size_t stride, int x, int y, int w, int h)
{
    AmigaDragBob *b = context;
    return ReadPixelArray(rgb,0,0,(UWORD)stride,&b->rp,x,y,w,h,RECTFMT_RGB) == (ULONG)w*h;
}

static int bob_write(void *context, const unsigned char *rgb, size_t stride, int x, int y, int w, int h)
{
    AmigaDragBob *b = context;
    return WritePixelArray((APTR)rgb,0,0,(UWORD)stride,&b->rp,x,y,w,h,RECTFMT_RGB) == (ULONG)w*h;
}

AmigaDragBob *amiga_drag_bob_open(struct Screen *screen)
{
    AmigaDragBob *b;
    DockBobScreen s;
    if (!screen || GetBitMapAttr(screen->RastPort.BitMap,BMA_DEPTH) <= 8) return NULL;
    b = calloc(1,sizeof(*b));
    if (!b) return NULL;
    b->cyber = OpenLibrary((STRPTR)"cybergraphics.library",40);
    if (!b->cyber) { free(b); return NULL; }
    InitRastPort(&b->rp);
    b->rp.BitMap = screen->RastPort.BitMap;
    s = (DockBobScreen){bob_read,bob_write,b,screen->Width,screen->Height};
    dock_bob_init(&b->bob,&s);
    return b;
}

int amiga_drag_bob_place(AmigaDragBob *b, const DockImage *picture, int x, int y)
{
    if (!b) return 0;
    if (dock_bob_place(&b->bob,picture,x,y)) return 1;
    dock_bob_hide(&b->bob);
    return 0;
}

void amiga_drag_bob_close(AmigaDragBob *b)
{
    if (!b) return;
    dock_bob_hide(&b->bob);
    dock_bob_free(&b->bob);
    CloseLibrary(b->cyber);
    free(b);
}
