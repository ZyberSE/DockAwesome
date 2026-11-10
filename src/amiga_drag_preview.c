#include "amiga_drag_preview.h"
#include "dock_compose.h"
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
#include <string.h>

#ifndef WA_ShapeRegion
#define WA_ShapeRegion (WA_Dummy + 65)
#endif

struct AmigaDragPreview {
    struct Screen *screen;
    struct Window *window;
    struct Region *shape;
    int width, height, icon_height, left, top;
    int shaped, remove;
    int text_width, label_width, label_top;
    /* Without shapes the window keeps a copy of what lies under it (a
       save-under, as for a BOB) and draws the icon over it. */
    AmigaDisplay *display;
    DockImage icon, background, scratch, frame;
};

static const char label[] = "Remove";

static void position(const AmigaDragPreview *p, int x, int y, int *left, int *top)
{
    *left = dock_clamp(x-p->width/2, 0, p->screen->Width-p->width);
    *top = dock_clamp(y-p->icon_height/2, 0, p->screen->Height-p->height);
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
    return !label_width ||
        (add_rect(p->shape,label_left,top+1,right,p->height-2) &&
        add_rect(p->shape,label_left+1,top,right-1,p->height-1));
}

static int alloc_image(DockImage *image, int width, int height)
{
    image->width = width; image->height = height;
    image->pixels = malloc((size_t)width*height*4);
    return image->pixels != NULL;
}

/* The icon over the backdrop (the matte when shaped), then the label. */
static int render(AmigaDragPreview *p)
{
    static const unsigned char matte[4] = {32, 36, 42, 255};
    struct DrawInfo *info;
    struct RastPort *rp = p->window->RPort;
    int i, label_left = (p->width-p->label_width)/2;
    if (p->shaped) for (i = 0; i < p->width*p->height; ++i) memcpy(p->frame.pixels+i*4,matte,4);
    else memcpy(p->frame.pixels,p->background.pixels,(size_t)p->width*p->height*4);
    dock_blend(&p->frame,&p->icon,(p->width-p->icon.width)/2,0);
    if (!amiga_display_present(p->display,p->window,&p->frame)) return 0;
    if (!p->remove) return 1;
    if (!(info = GetScreenDrawInfo(p->screen))) return 0;
    SetFont(rp,p->screen->RastPort.Font);
    SetAPen(rp,info->dri_Pens[BACKGROUNDPEN]);
    SetDrMd(rp,JAM1);
    RectFill(rp,label_left,p->label_top,label_left+p->label_width-1,p->height-1);
    SetAPen(rp,info->dri_Pens[TEXTPEN]);
    Move(rp,(p->width-p->text_width)/2,p->label_top+3+rp->Font->tf_Baseline);
    amiga_text_draw(rp,label,sizeof(label)-1);
    FreeScreenDrawInfo(p->screen,info);
    return 1;
}

AmigaDragPreview *amiga_drag_preview_open(struct Screen *screen, const DockImage *icon,
    int shaped, int remove, int x, int y)
{
    AmigaDragPreview *p;
    if (!screen || !icon || !icon->pixels || !screen->RastPort.Font) return NULL;
    p = calloc(1,sizeof(*p));
    if (!p) return NULL;
    p->screen = screen;
    p->shaped = shaped;
    p->remove = remove;
    p->text_width = amiga_text_length(&screen->RastPort,label,sizeof(label)-1);
    p->label_width = p->text_width+10;
    p->label_top = icon->height+4;
    p->icon_height = icon->height;
    p->width = icon->width > p->label_width ? icon->width : p->label_width;
    p->height = p->label_top+screen->RastPort.Font->tf_YSize+6;
    /* Shaped without Remove: the icon alone, the same picture the dock
       draws under it on its surface. */
    if (shaped && !remove) { p->width = icon->width; p->height = icon->height; }
    if (p->text_width <= 0 || p->width > screen->Width || p->height > screen->Height) goto fail;
    if (shaped && !build_shape(p,icon,remove ? p->label_width : 0)) goto fail;
    /* A private copy: the item's cache can be rebuilt during the drag. */
    if (!alloc_image(&p->icon,icon->width,icon->height) || !alloc_image(&p->frame,p->width,p->height))
        goto fail;
    memcpy(p->icon.pixels,icon->pixels,(size_t)icon->width*icon->height*4);
    position(p,x,y,&p->left,&p->top);
    if (!(p->display = amiga_display_open(screen,p->width,p->height))) goto fail;
    if (!shaped && (!alloc_image(&p->background,p->width,p->height) ||
        !alloc_image(&p->scratch,p->width,p->height) ||
        !amiga_display_capture(p->display,&p->background,p->left,p->top))) goto fail;
    p->window = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)screen,
        WA_Left, p->left, WA_Top, p->top, WA_Width, p->width, WA_Height, p->height,
        WA_Borderless, TRUE, WA_Activate, FALSE, WA_SmartRefresh, TRUE, WA_NoCareRefresh, TRUE,
        shaped ? WA_ShapeRegion : TAG_IGNORE, (ULONG)p->shape, TAG_DONE);
    if (!p->window || !render(p)) goto fail;
    p->left = p->window->LeftEdge; p->top = p->window->TopEdge;
    return p;
fail:
    amiga_drag_preview_close(p);
    return NULL;
}

/* The save-under for a move: the screen at the new place, except where the
   window still covers it, which the old save-under already holds. */
static int save_under(AmigaDragPreview *p, int left, int top)
{
    int x0 = left > p->left ? left : p->left, y0 = top > p->top ? top : p->top;
    int x1 = left+p->width < p->left+p->width ? left+p->width : p->left+p->width;
    int y1 = top+p->height < p->top+p->height ? top+p->height : p->top+p->height;
    int y;
    DockImage swap;
    if (!amiga_display_capture(p->display,&p->scratch,left,top)) return 0;
    for (y = y0; y < y1 && x0 < x1; ++y)
        memcpy(p->scratch.pixels+((size_t)(y-top)*p->width+(x0-left))*4,
            p->background.pixels+((size_t)(y-p->top)*p->width+(x0-p->left))*4,(size_t)(x1-x0)*4);
    swap = p->background; p->background = p->scratch; p->scratch = swap;
    return 1;
}

void amiga_drag_preview_move(AmigaDragPreview *p, int x, int y)
{
    int left, top;
    if (!p) return;
    /* Intuition queues MoveWindow. Keep at most one move in flight. */
    if (p->window->LeftEdge != p->left || p->window->TopEdge != p->top) return;
    position(p,x,y,&left,&top);
    if (left == p->left && top == p->top) return;
    if (!p->shaped && !save_under(p,left,top)) return;
    MoveWindow(p->window,left-p->left,top-p->top);
    p->left = left; p->top = top;
    /* Smart Refresh carries this drawing along when the move lands. */
    if (!p->shaped) render(p);
}

void amiga_drag_preview_set_remove(AmigaDragPreview *p, int remove)
{
    if (!p || p->remove == !!remove) return;
    p->remove = !!remove;
    if (!p->shaped) render(p);
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
    amiga_display_close(p->display);
    free(p->icon.pixels); free(p->background.pixels); free(p->scratch.pixels); free(p->frame.pixels);
    free(p);
}
