#include "amiga_drag_preview.h"
#include "amiga_display.h"
#include "amiga_text.h"
#include <graphics/gfxbase.h>
#include <graphics/regions.h>
#include <graphics/text.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <stdlib.h>

#ifndef WA_ShapeRegion
#define WA_ShapeRegion (WA_Dummy + 65)
#endif

struct AmigaDragPreview {
    struct Screen *screen;
    struct Window *window;
    struct Region *shape;
    int width, height, icon_height, left, top;
};

static const char label[] = "Remove";

static int clamp(int value, int high)
{
    return value < 0 ? 0 : value > high ? high : value;
}

static void position(const AmigaDragPreview *p, int x, int y, int *left, int *top)
{
    *left = clamp(x-p->width/2, p->screen->Width-p->width);
    *top = clamp(y-p->icon_height/2, p->screen->Height-p->height);
}

static int add_rect(struct Region *shape, int left, int top, int right, int bottom)
{
    struct Rectangle rect = {left, top, right, bottom};
    return OrRectRegion(shape, &rect);
}

static int build_shape(AmigaDragPreview *p, const DockImage *icon, int label_width)
{
    int x, y, icon_left = (p->width-icon->width)/2, label_left = (p->width-label_width)/2;
    int top = icon->height+4, right = label_left+label_width-1;
    p->shape = NewRegion();
    if (!p->shape) return 0;
    for (y = 0; y < icon->height; ++y) {
        const unsigned char *row = icon->pixels+(size_t)y*icon->width*4;
        int first = icon->width, last = -1;
        for (x = 0; x < icon->width; ++x) if (row[x*4+3] >= 24) {
            if (first == icon->width) first = x;
            last = x;
        }
        if (last >= first && !add_rect(p->shape,icon_left+first,y,icon_left+last,y)) return 0;
    }
    return add_rect(p->shape,label_left,top+1,right,p->height-2) &&
        add_rect(p->shape,label_left+1,top,right-1,p->height-1);
}

AmigaDragPreview *amiga_drag_preview_open(struct Screen *screen, const DockImage *icon,
    int shaped, int x, int y)
{
    AmigaDragPreview *p;
    AmigaDisplay *display = NULL;
    struct DrawInfo *info;
    struct RastPort *rp;
    DockImage frame = {0};
    int i, text_width, label_width, label_left, label_top, baseline, ok = 0;
    static const unsigned char matte[4] = {32, 36, 42, 255};
    if (!screen || !icon || !icon->pixels || !screen->RastPort.Font) return NULL;
    p = calloc(1,sizeof(*p));
    if (!p) return NULL;
    info = GetScreenDrawInfo(screen);
    if (!info) goto done;
    text_width = amiga_text_length(&screen->RastPort,label,sizeof(label)-1);
    label_width = text_width+10;
    label_top = icon->height+4;
    p->screen = screen;
    p->icon_height = icon->height;
    p->width = icon->width > label_width ? icon->width : label_width;
    p->height = label_top+screen->RastPort.Font->tf_YSize+6;
    if (text_width <= 0 || p->width > screen->Width || p->height > screen->Height) goto done;
    if (shaped && !build_shape(p,icon,label_width)) goto done;
    position(p,x,y,&p->left,&p->top);
    frame.width = p->width; frame.height = p->height;
    frame.pixels = malloc((size_t)p->width*p->height*4);
    display = amiga_display_open(screen,p->width,p->height);
    if (!display || !frame.pixels) goto done;
    for (i = 0; i < p->width*p->height; ++i) {
        unsigned char *pixel = frame.pixels+i*4;
        pixel[0] = matte[0]; pixel[1] = matte[1]; pixel[2] = matte[2]; pixel[3] = matte[3];
    }
    dock_blend(&frame,icon,(p->width-icon->width)/2,0);
    p->window = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)screen,
        WA_Left, p->left, WA_Top, p->top, WA_Width, p->width, WA_Height, p->height,
        WA_Borderless, TRUE, WA_Activate, FALSE, WA_SmartRefresh, TRUE, WA_NoCareRefresh, TRUE,
        shaped ? WA_ShapeRegion : TAG_IGNORE, (ULONG)p->shape, TAG_DONE);
    if (!p->window || !amiga_display_present(display,p->window,&frame)) goto done;
    rp = p->window->RPort;
    SetFont(rp,screen->RastPort.Font);
    label_left = (p->width-label_width)/2;
    baseline = label_top+3+rp->Font->tf_Baseline;
    SetAPen(rp,info->dri_Pens[BACKGROUNDPEN]);
    SetDrMd(rp,JAM1);
    RectFill(rp,label_left,label_top,label_left+label_width-1,p->height-1);
    SetAPen(rp,info->dri_Pens[TEXTPEN]);
    Move(rp,(p->width-text_width)/2,baseline);
    amiga_text_draw(rp,label,sizeof(label)-1);
    p->left = p->window->LeftEdge; p->top = p->window->TopEdge;
    ok = 1;
done:
    if (info) FreeScreenDrawInfo(screen,info);
    amiga_display_close(display);
    free(frame.pixels);
    if (!ok) { amiga_drag_preview_close(p); return NULL; }
    return p;
}

void amiga_drag_preview_move(AmigaDragPreview *p, int x, int y)
{
    int left, top;
    if (!p) return;
    /* Intuition queues MoveWindow. Keep at most one move in flight. */
    if (p->window->LeftEdge != p->left || p->window->TopEdge != p->top) return;
    position(p,x,y,&left,&top);
    if (left == p->left && top == p->top) return;
    MoveWindow(p->window,left-p->left,top-p->top);
    p->left = left; p->top = top;
}

int amiga_drag_preview_owns(const AmigaDragPreview *p, const struct Window *window)
{
    return p && p->window == window;
}

void amiga_drag_preview_close(AmigaDragPreview *p)
{
    if (!p) return;
    if (p->window) CloseWindow(p->window);
    if (p->shape) DisposeRegion(p->shape);
    free(p);
}
