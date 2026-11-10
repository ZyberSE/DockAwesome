#include "dock_bob.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A fake RGB screen; every write is also checked to stay inside it. */
enum { W = 97, H = 61 };
static unsigned char screen[H][W][3], original[H][W][3];
static int writes;

static int screen_read(void *context, unsigned char *rgb, size_t stride, int x, int y, int w, int h)
{
    int row;
    (void)context;
    assert(x >= 0 && y >= 0 && x+w <= W && y+h <= H && w > 0 && h > 0);
    for (row = 0; row < h; ++row) memcpy(rgb+(size_t)row*stride,screen[y+row][x],(size_t)w*3);
    return 1;
}

static int screen_write(void *context, const unsigned char *rgb, size_t stride, int x, int y, int w, int h)
{
    int row;
    (void)context;
    assert(x >= 0 && y >= 0 && x+w <= W && y+h <= H && w > 0 && h > 0);
    for (row = 0; row < h; ++row) memcpy(screen[y+row][x],rgb+(size_t)row*stride,(size_t)w*3);
    ++writes;
    return 1;
}

/* The screen equals the original with picture blended at (px, py). */
static void expect(const DockImage *picture, int px, int py, int shown)
{
    int x, y, c;
    for (y = 0; y < H; ++y) for (x = 0; x < W; ++x) for (c = 0; c < 3; ++c) {
        unsigned int want = original[y][x][c];
        if (shown && x >= px && y >= py && x < px+picture->width && y < py+picture->height) {
            const unsigned char *p = picture->pixels+((size_t)(y-py)*picture->width+(x-px))*4;
            unsigned int a = p[3];
            if (a) want = (p[c]*a+want*(255-a)+127)/255;
        }
        if (screen[y][x][c] != want) {
            fprintf(stderr,"pixel %d,%d channel %d: %u, want %u (picture at %d,%d shown %d)\n",
                x,y,c,screen[y][x][c],want,px,py,shown);
            exit(1);
        }
    }
}

static DockImage make_picture(int w, int h, unsigned seed)
{
    DockImage p = {w,h,malloc((size_t)w*h*4)};
    int i;
    assert(p.pixels);
    for (i = 0; i < w*h*4; ++i) {
        seed = seed*1103515245u+12345u;
        p.pixels[i] = (unsigned char)(seed>>16);
    }
    /* Clear and opaque corners as well as partial alpha. */
    p.pixels[3] = 0; p.pixels[(w*h-1)*4+3] = 255;
    return p;
}

int main(void)
{
    DockBobScreen s = {screen_read,screen_write,NULL,W,H};
    DockBob bob;
    DockImage a = make_picture(13,9,7), b = make_picture(17,11,99);
    /* Positions: overlapping steps, a far jump, every edge, off screen. */
    static const int path[][2] = {{20,20},{22,21},{21,24},{60,40},{61,40},{-5,10},{-4,-3},
        {90,55},{88,57},{120,30},{30,-20},{40,30},{41,31},{0,0},{W-13,H-9}};
    unsigned seed = 3;
    int i, x, y, c;
    for (y = 0; y < H; ++y) for (x = 0; x < W; ++x) for (c = 0; c < 3; ++c) {
        seed = seed*1103515245u+12345u;
        screen[y][x][c] = original[y][x][c] = (unsigned char)(seed>>16);
    }
    dock_bob_init(&bob,&s);
    for (i = 0; i < (int)(sizeof(path)/sizeof(path[0])); ++i) {
        const DockImage *picture = i == 8 ? &b : i == 9 ? &a : NULL;
        const DockImage *shown = i < 8 || i >= 9 ? &a : &b;
        if (i == 0) picture = &a;
        assert(dock_bob_place(&bob,picture,path[i][0],path[i][1]));
        expect(shown,path[i][0],path[i][1],1);
    }
    /* A small step is one write of the union. */
    writes = 0;
    assert(dock_bob_place(&bob,NULL,W-14,H-9));
    assert(writes == 1);
    expect(&a,W-14,H-9,1);
    assert(dock_bob_hide(&bob));
    expect(&a,0,0,0);
    assert(dock_bob_hide(&bob));
    dock_bob_free(&bob);
    free(a.pixels); free(b.pixels);
    puts("bob: save-under, union moves, edges and restore PASS");
    return 0;
}
