/* Save-under drawing of one picture onto a screen. See dock_bob.h. */
#include "dock_bob.h"
#include <stdlib.h>
#include <string.h>

static int empty(DockRect r)
{
    return r.right <= r.left || r.bottom <= r.top;
}

static size_t area(DockRect r)
{
    return empty(r) ? 0 : (size_t)(r.right-r.left)*(size_t)(r.bottom-r.top);
}

static int grow(unsigned char **buffer, size_t *cap, size_t bytes)
{
    unsigned char *next;
    if (bytes <= *cap) return 1;
    next = realloc(*buffer,bytes);
    if (!next) return 0;
    *buffer = next; *cap = bytes;
    return 1;
}

/* Copy the rect r of an RGB image laid out over from into one over to. */
static void copy_rect(unsigned char *to, DockRect to_box, const unsigned char *from, DockRect from_box,
    DockRect r)
{
    size_t to_stride = (size_t)(to_box.right-to_box.left)*3;
    size_t from_stride = (size_t)(from_box.right-from_box.left)*3;
    size_t bytes = (size_t)(r.right-r.left)*3;
    int y;
    for (y = r.top; y < r.bottom; ++y)
        memcpy(to+(size_t)(y-to_box.top)*to_stride+(size_t)(r.left-to_box.left)*3,
            from+(size_t)(y-from_box.top)*from_stride+(size_t)(r.left-from_box.left)*3,bytes);
}

/* Blend the picture, its top left at (x, y), over the RGB image laid out
   over box, within clip and outside exclude. */
static void blend(unsigned char *rgb, DockRect box, const DockImage *picture, int x, int y, DockRect clip,
    DockRect exclude)
{
    size_t stride = (size_t)(box.right-box.left)*3;
    int row, col;
    for (row = clip.top; row < clip.bottom; ++row) {
        const unsigned char *in = picture->pixels+((size_t)(row-y)*picture->width+(clip.left-x))*4;
        unsigned char *out = rgb+(size_t)(row-box.top)*stride+(size_t)(clip.left-box.left)*3;
        int skip = row >= exclude.top && row < exclude.bottom;
        for (col = clip.left; col < clip.right; ++col, in += 4, out += 3) {
            unsigned int a = in[3], keep = 255-a;
            if (!a || (skip && col >= exclude.left && col < exclude.right)) continue;
            out[0] = (unsigned char)((in[0]*a+out[0]*keep+127)/255);
            out[1] = (unsigned char)((in[1]*a+out[1]*keep+127)/255);
            out[2] = (unsigned char)((in[2]*a+out[2]*keep+127)/255);
        }
    }
}

void dock_bob_init(DockBob *bob, const DockBobScreen *screen)
{
    memset(bob,0,sizeof(*bob));
    bob->screen = *screen;
}

static int set_picture(DockBob *bob, const DockImage *picture)
{
    size_t bytes = (size_t)picture->width*picture->height*4;
    unsigned char *copy;
    if (picture->width < 1 || picture->height < 1 || !picture->pixels) return 0;
    if (picture->pixels == bob->picture.pixels) return 1;
    copy = malloc(bytes);
    if (!copy) return 0;
    memcpy(copy,picture->pixels,bytes);
    free(bob->picture.pixels);
    bob->picture = (DockImage){picture->width,picture->height,copy};
    return 1;
}

int dock_bob_place(DockBob *bob, const DockImage *picture, int x, int y)
{
    DockRect next, old = bob->drawn, box;
    const DockBobScreen *s = &bob->screen;
    if (picture && !set_picture(bob,picture)) return 0;
    if (!bob->picture.pixels) return 0;
    next = (DockRect){x,y,x+bob->picture.width,y+bob->picture.height};
    if (next.left < 0) next.left = 0;
    if (next.top < 0) next.top = 0;
    if (next.right > s->width) next.right = s->width;
    if (next.bottom > s->height) next.bottom = s->height;
    if (empty(next)) return dock_bob_hide(bob);
    /* One write of the union of the old and new places unless they lie
       apart; then the old place is restored first, as two writes. */
    box = next;
    if (!empty(old)) {
        DockRect u = {old.left < next.left ? old.left : next.left, old.top < next.top ? old.top : next.top,
            old.right > next.right ? old.right : next.right, old.bottom > next.bottom ? old.bottom : next.bottom};
        if (area(u) <= 2*(area(old)+area(next))) box = u;
        else if (!dock_bob_hide(bob)) return 0;
        else old = bob->drawn;
    }
    if (!grow(&bob->work,&bob->work_cap,area(box)*3) || !grow(&bob->saved,&bob->saved_cap,area(next)*3))
        return 0;
    if (!s->read(s->context,bob->work,(size_t)(box.right-box.left)*3,box.left,box.top,
            box.right-box.left,box.bottom-box.top)) return 0;
    /* The screen there still shows the old picture: put back what it hid. */
    if (!empty(old)) copy_rect(bob->work,box,bob->saved,old,old);
    copy_rect(bob->saved,next,bob->work,box,next);
    blend(bob->work,box,&bob->picture,x,y,next,bob->exclude);
    if (!s->write(s->context,bob->work,(size_t)(box.right-box.left)*3,box.left,box.top,
            box.right-box.left,box.bottom-box.top)) {
        bob->drawn = (DockRect){0,0,0,0};
        return 0;
    }
    bob->drawn = next;
    return 1;
}

void dock_bob_set_exclude(DockBob *bob, DockRect rect)
{
    bob->exclude = rect;
}

int dock_bob_hide(DockBob *bob)
{
    DockRect r = bob->drawn;
    const DockBobScreen *s = &bob->screen;
    if (empty(r)) return 1;
    bob->drawn = (DockRect){0,0,0,0};
    return s->write(s->context,bob->saved,(size_t)(r.right-r.left)*3,r.left,r.top,
        r.right-r.left,r.bottom-r.top);
}

void dock_bob_free(DockBob *bob)
{
    free(bob->picture.pixels);
    free(bob->saved);
    free(bob->work);
    memset(bob,0,sizeof(*bob));
}
