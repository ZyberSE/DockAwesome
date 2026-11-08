#include "dock_core.h"
#include "dock_renderer.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_normalize(void)
{
    unsigned char data[12 * 8 * 4] = {0};
    DockImage source = {12, 8, data};
    DockSettings s = dock_settings_default();
    DockCache cache;
    int x, y, l, base_extent = 0, peak_extent = 0, previous_extent = 0;
    /* Transparent colored padding must not reduce the visible icon footprint. */
    memset(data, 220, sizeof(data));
    for (y = 0; y < 8; ++y) for (x = 0; x < 12; ++x) {
        unsigned char *p = data + (y * 12 + x) * 4;
        p[3] = 0;
        if (x >= 3 && x < 9 && y >= 3 && y < 5) {
            p[0] = 10; p[1] = 70; p[2] = 150; p[3] = 255;
        }
    }
    assert(dock_cache_build(&cache, &source, &s));
    assert(cache.bytes == dock_cache_bytes(&s));
    assert(cache.levels == 33 && cache.image[0].width == 32 && cache.image[32].width == 64);
    for (l = 0; l < s.levels; ++l) {
        DockImage *im = &cache.image[l];
        assert(im->width == s.base_size + l);
        int left = im->width, right = -1, top = im->height, bottom = -1;
        for (y = 0; y < im->height; ++y) for (x = 0; x < im->width; ++x) {
            unsigned char *p = im->pixels + (y * im->width + x) * 4;
            if (p[3]) {
                if (x < left) left = x;
                if (x > right) right = x;
                if (y < top) top = y;
                if (y > bottom) bottom = y;
                assert(p[0] == 10 && p[1] == 70 && p[2] == 150 && p[3] == 255);
            }
        }
        {
            int visible = right - left + 1;
            int expected = (im->width * (s.base_size - 4) + s.base_size / 2) / s.base_size;
            assert(visible == expected && visible >= previous_extent);
            assert(abs(visible - 3 * (bottom - top + 1)) <= 1);
            assert(left == (im->width - expected) / 2);
            if (!l) base_extent = visible;
            if (l == s.levels - 1) peak_extent = visible;
            previous_extent = visible;
        }
    }
    assert(base_extent == 28 && peak_extent == 56 && peak_extent == 2 * base_extent);
    dock_cache_free(&cache);
    assert(cache.bytes == 0 && cache.image[0].pixels == NULL);
    s.cache_limit = dock_cache_bytes(&s) - 1;
    assert(!dock_cache_build(&cache, &source, &s));
    s = dock_settings_default();
    memset(data, 0, sizeof(data));
    assert(dock_cache_build(&cache, &source, &s));
    for (l = 0; l < s.levels; ++l)
        for (x = 0; x < cache.image[l].width * cache.image[l].height * 4; ++x)
            assert(cache.image[l].pixels[x] == 0);
    dock_cache_free(&cache);
    source.width = 0;
    assert(!dock_cache_build(&cache, &source, &s));
}

static void test_reference_wave_and_hover(void)
{
    DockSettings s = dock_settings_default();
    DockPlacement rest[9], wave[9];
    int x, i, first_center;
    assert(s.padding == 8 && s.gap == 5);
    assert(dock_layout(&s,9,700,128,0,0,0,rest));
    first_center = rest[0].x+s.base_size/2;
    x = first_center+4*(s.base_size+s.gap);
    assert(dock_layout(&s,9,700,128,x,100,1,wave));
    assert(wave[4].size == 64 && wave[3].size >= 58 && wave[2].size >= 47 && wave[1].size >= 36);
    for(i=0;i<4;++i) assert(wave[i].size == wave[8-i].size);
    assert(wave[0].size == s.base_size);
    /* Shared midpoint makes both adjacent icons near the peak, rather than
       the old narrow spike with an almost unscaled neighbor. */
    x = first_center+3*(s.base_size+s.gap)+(s.base_size+s.gap)/2;
    assert(dock_layout(&s,9,700,128,x,100,1,wave));
    assert(wave[3].size >= 62 && wave[4].size >= 62);
    assert(wave[2].size >= 53 && wave[5].size >= 53);
    assert(wave[1].size >= 41 && wave[6].size >= 41);
    /* Empty maximum-zoom headroom does not activate; the shelf does.
       An actually enlarged icon keeps hover when the pointer moves inward. */
    assert(!dock_hover_contains(&s,rest,9,700,128,350,30));
    assert(!dock_hover_contains(&s,rest,9,700,128,0,110));
    assert(dock_hover_contains(&s,rest,9,700,128,350,100));
    assert(dock_layout(&s,9,700,128,350,100,1,wave));
    assert(dock_hover_contains(&s,wave,9,700,128,wave[4].x+10,wave[4].y+2));
    {
        int gap_x = wave[3].x+wave[3].size+s.gap/2;
        int gap_y = (wave[3].y < wave[4].y ? wave[3].y : wave[4].y)+2;
        assert(dock_hit_test(wave,9,gap_x,gap_y) == -1);
        assert(dock_hover_contains(&s,wave,9,700,128,gap_x,gap_y));
        assert(!dock_hover_contains(&s,rest,9,700,128,gap_x,gap_y));
        /* A fast lateral move can outrun the neighboring icon's growth.
           Retain hover across the row instead of resetting the envelope. */
        assert(dock_hit_test(wave,9,wave[0].x+10,gap_y) == -1);
        assert(dock_hover_contains(&s,wave,9,700,128,wave[0].x+10,gap_y));
    }
    assert(!dock_hover_contains(&s,rest,9,700,128,wave[4].x+10,wave[4].y+2));
    assert(!dock_hover_contains(&s,wave,9,700,128,INT_MIN,INT_MAX));
}

static void test_zoom_gap_compaction(void)
{
    DockSettings s = dock_settings_default();
    DockPlacement rest[9], target[9], current[9];
    DockAnimation animation;
    int i, tick, gap;
    assert(dock_layout(&s,9,700,128,0,0,0,rest));
    assert(dock_layout(&s,9,700,128,350,100,1,target));
    for(i=0;i<8;++i) {
        assert(rest[i+1].x-rest[i].x-rest[i].size == 5);
    }
    assert(target[4].size == 64 && target[3].size == 59);
    assert(target[4].x-target[3].x-target[3].size == 3);
    assert(target[1].x-target[0].x-target[0].size == 5);
    memcpy(current,rest,sizeof(rest)); dock_animation_reset(&animation);
    for(tick=0;tick<8;++tick) {
        dock_animate_step(&s,&animation,current,target,9);
        for(i=0;i<8;++i) {
            gap = current[i+1].x-current[i].x-current[i].size;
            /* Continuous centers and integer cached boxes may differ by a
               pixel. Do not force raster widths back into cumulative packing. */
            assert(gap >= 2 && gap <= 6);
            if(!tick) assert(abs(gap-5) <= 1);
        }
    }
    assert(!memcmp(current,target,sizeof(current)));
    for(tick=0;tick<8;++tick) {
        dock_animate_step(&s,&animation,current,rest,9);
        for(i=0;i<8;++i) {
            gap = current[i+1].x-current[i].x-current[i].size;
            assert(gap >= 2 && gap <= 6);
        }
    }
    assert(!memcmp(current,rest,sizeof(current)));
    for(gap=0;gap<=1;++gap) {
        s.gap = gap;
        assert(dock_layout(&s,9,700,128,350,100,1,target));
        for(i=0;i<8;++i) {
            int actual = target[i+1].x-target[i].x-target[i].size;
            assert(actual >= 0 && actual <= gap+1);
        }
    }
    s = dock_settings_default(); s.max_size = s.base_size;
    assert(dock_layout(&s,9,700,128,350,100,1,target));
    for(i=0;i<8;++i) assert(target[i+1].x-target[i].x-target[i].size == 5);
    /* Small configured zoom ranges may reclaim more gap pixels than an icon
       gains; the constrained layout must still terminate and stay in bounds. */
    s.max_size = s.base_size+1;
    assert(dock_layout(&s,9,344,128,172,100,1,target));
    assert(target[0].x >= s.padding && target[8].x+target[8].size <= 344-s.padding);
}

static void test_inter_icon_anchor_continuity(void)
{
    DockSettings s = dock_settings_default();
    DockPlacement rest[5], p[5];
    int gap, x, minimum, maximum, i;
    static const int begins[2] = {171,208};
    static const int ends[2] = {176,213};
    /* Real 384px five-pin regression: the old separate canvas/gap anchor
       reversed its left edge by 5px and 7px over these gap crossings.
       Keep only the one-pixel cache/center rounding wobble, without freezing
       the panel or changing the broad magnification wave. */
    for (gap = 0; gap < 2; ++gap) {
        minimum = INT_MAX; maximum = 0;
        for (x = begins[gap]; x <= ends[gap]; ++x) {
            int left;
            assert(dock_layout(&s,5,384,92,x,62,1,p));
            left = p[0].x-s.padding;
            if (left < minimum) minimum = left;
            if (left-minimum > maximum) maximum = left-minimum;
            for (i = 1; i < 5; ++i)
                assert(p[i].x-p[i-1].x-p[i-1].size >= 2);
        }
        assert(maximum <= 1); /* no backward excursion; normal forward travel is allowed */
    }
    assert(dock_layout(&s,5,384,92,0,0,0,rest));
    for (i = 0; i < 5; ++i) {
        int center = rest[i].x+s.base_size/2;
        assert(dock_layout(&s,5,384,92,center,62,1,p));
        assert(abs(2*p[i].x+p[i].size-2*center) <= 1);
        assert(p[i].size == s.max_size);
    }
    s.max_size = s.base_size;
    for (x = 0; x < 384; ++x) {
        assert(dock_layout(&s,5,384,92,x,62,1,p));
        for (i = 0; i < 5; ++i) {
            assert(p[i].x == rest[i].x);
            assert(p[i].size == rest[i].size);
            assert(p[i].level == 0); /* disabled magnification must not start a timer */
        }
    }
}

static void test_alpha_fringe(void)
{
    unsigned char data[] = {255, 0, 0, 255, 0, 255, 0, 0, 255, 0, 0, 255};
    DockImage source = {3, 1, data};
    DockSettings s = dock_settings_default();
    DockCache cache;
    int i;
    assert(dock_cache_build(&cache, &source, &s));
    for (i = 0; i < cache.image[0].width * cache.image[0].height; ++i) {
        unsigned char *p = cache.image[0].pixels + i * 4;
        if (p[3]) assert(p[0] == 255 && p[1] == 0 && p[2] == 0);
    }
    dock_cache_free(&cache);
}

static void test_faint_shadows(void)
{
    enum { WIDTH = 16, BOX = 20, INSET = 2 };
    unsigned char pixels[WIDTH*WIDTH*4] = {0};
    DockImage source = {WIDTH,WIDTH,pixels};
    DockSettings settings = dock_settings_default();
    DockCache cache;
    unsigned char *image;

    settings.base_size = settings.max_size = BOX;
    pixels[3] = pixels[(WIDTH*WIDTH-1)*4+3] = 255;
    pixels[(1*WIDTH+1)*4+3] = 80;
    pixels[(9*WIDTH+9)*4+3] = 80;
    pixels[(9*WIDTH+10)*4] = 150;
    pixels[(9*WIDTH+10)*4+3] = 80;
    pixels[(12*WIDTH+12)*4+3] = 200;
    assert(dock_cache_build(&cache,&source,&settings));
    image = cache.image[0].pixels+(INSET*BOX+INSET)*4;
    assert(image[(1*BOX+1)*4+3] == 80);
    assert(image[(9*BOX+9)*4+3] == 0);
    assert(image[(9*BOX+10)*4+3] == 80);
    assert(image[(12*BOX+12)*4+3] == 200);
    dock_cache_free(&cache);

    pixels[3] = pixels[(WIDTH*WIDTH-1)*4+3] = 100;
    pixels[(12*WIDTH+12)*4+3] = 100;
    assert(dock_cache_build(&cache,&source,&settings));
    image = cache.image[0].pixels+(INSET*BOX+INSET)*4;
    assert(image[(9*BOX+9)*4+3] == 80);
    dock_cache_free(&cache);
}

/* Shadow clearing runs at source resolution. A soft edge one source pixel
   from solid artwork must survive enlargement, and a detached shadow must
   clear at every level even when downscaling thins the solid strokes. */
static void test_scaled_shadows(void)
{
    enum { EDGE = 8, LINE = 32 };
    unsigned char edge[EDGE*EDGE*4] = {0};
    unsigned char line[LINE*LINE*4] = {0};
    DockImage source = {EDGE,EDGE,edge};
    DockSettings s = dock_settings_default();
    DockCache cache;
    int i, x, y;

    s.base_size = 16; s.max_size = 48; s.levels = 5;
    for (y = 2; y < 4; ++y) {
        edge[(y*EDGE+2)*4+3] = edge[(y*EDGE+3)*4+3] = 255;
        edge[(y*EDGE+4)*4+3] = 80;
    }
    assert(dock_cache_build(&cache,&source,&s));
    for (i = 0; i < cache.levels; ++i) {
        const DockImage *image = &cache.image[i];
        int n = image->width, w = (n*(s.base_size-2*DOCK_ICON_INSET)+s.base_size/2)/s.base_size;
        int right = (n-w)/2+w-1, mid = n/2;
        /* The rightmost artwork column samples the faint source column. */
        assert(image->pixels[((size_t)mid*n+right)*4+3] == 80);
    }
    dock_cache_free(&cache);

    for (y = 0; y < LINE; ++y) {
        unsigned char *p = line+((size_t)y*LINE+4)*4;
        p[0] = p[1] = p[2] = 255; p[3] = 255;
        if (y >= 10 && y < 18)
            for (x = 20; x < 28; ++x) line[((size_t)y*LINE+x)*4+3] = 100;
    }
    source = (DockImage){LINE,LINE,line};
    assert(dock_cache_build(&cache,&source,&s));
    for (i = 0; i < cache.levels; ++i) {
        const DockImage *image = &cache.image[i];
        for (x = 0; x < image->width*image->height; ++x) {
            const unsigned char *p = image->pixels+(size_t)x*4;
            if (p[3]) assert(p[0] == 255 && p[1] == 255 && p[2] == 255);
        }
    }
    dock_cache_free(&cache);
}

/* Dark glass enclosed by a solid frame is artwork, not a shadow, and a
   cleared detached shadow must not offset or shrink the visible artwork. */
static void test_enclosed_glass_and_centering(void)
{
    enum { N = 24 };
    unsigned char pixels[N*N*4];
    DockImage source = {N,N,pixels};
    DockSettings s = dock_settings_default();
    DockCache cache;
    const DockImage *image;
    int x, y, left = N, right = -1, top = N, bottom = -1;

    memset(pixels,0,sizeof(pixels));
    for (y = 0; y < N; ++y)
        for (x = 0; x < N; ++x) {
            unsigned char *p = pixels+(y*N+x)*4;
            int frame = x < 2 || y < 2 || x >= N-2 || y >= N-2;
            if (frame) { p[0] = p[1] = p[2] = 200; p[3] = 255; }
            else { p[0] = p[1] = p[2] = 20; p[3] = 128; }
        }
    s.base_size = s.max_size = N+2*DOCK_ICON_INSET;
    assert(dock_cache_build(&cache,&source,&s));
    image = &cache.image[0];
    for (y = 4; y < N-4; ++y)
        for (x = 4; x < N-4; ++x)
            assert(image->pixels[((size_t)(y+DOCK_ICON_INSET)*image->width+x+DOCK_ICON_INSET)*4+3] == 128);
    dock_cache_free(&cache);

    memset(pixels,0,sizeof(pixels));
    for (y = 0; y < 16; ++y)
        for (x = 0; x < 16; ++x) {
            unsigned char *p = pixels+(y*N+x)*4;
            p[0] = 220; p[1] = 120; p[2] = 40; p[3] = 255;
        }
    for (y = 20; y < N; ++y)
        for (x = 20; x < N; ++x) pixels[(y*N+x)*4+3] = 60;
    s.base_size = 32; s.max_size = 64; s.levels = 5;
    assert(dock_cache_build(&cache,&source,&s));
    image = &cache.image[0];
    for (y = 0; y < image->height; ++y)
        for (x = 0; x < image->width; ++x)
            if (image->pixels[((size_t)y*image->width+x)*4+3]) {
                if (x < left) left = x;
                if (x > right) right = x;
                if (y < top) top = y;
                if (y > bottom) bottom = y;
            }
    assert(left == DOCK_ICON_INSET && top == DOCK_ICON_INSET);
    assert(right == image->width-1-DOCK_ICON_INSET && bottom == image->height-1-DOCK_ICON_INSET);
    dock_cache_free(&cache);
}

/* levels=2 with a wide zoom range: the overlap push must not carry the
   animated row off the surface (it reached 65 px past it before). */
static void test_animation_stays_inside(void)
{
    DockSettings s = dock_settings_default();
    int count, length = 1220;
    s.base_size = 54; s.max_size = 169; s.levels = 2;
    for (count = 2; count <= 20; ++count) {
        DockPlacement current[DOCK_MAX_ITEMS], target[DOCK_MAX_ITEMS];
        DockAnimation animation;
        int mx, tick, i;
        if (!dock_layout(&s,count,length,200,0,0,0,current)) continue;
        dock_animation_reset(&animation);
        for (mx = 0; mx < length; mx += 7) {
            if (!dock_layout(&s,count,length,200,mx,100,1,target)) break;
            for (tick = 0; tick < 3; ++tick) {
                dock_animate_step(&s,&animation,current,target,count);
                for (i = 0; i < count; ++i)
                    assert(current[i].x >= 0 && current[i].x+current[i].size <= length);
            }
        }
    }
}

/* A single icon equals level 0 of a cache with the same resting size. */
static void test_icon_build(void)
{
    unsigned char pixels[20*14*4];
    DockImage source = {20,14,pixels}, icon;
    DockSettings s = dock_settings_default();
    DockCache cache;
    int i;
    for (i = 0; i < 20*14*4; ++i) pixels[i] = (unsigned char)(i*37+11);
    s.base_size = 40; s.max_size = 80;
    assert(dock_cache_build(&cache,&source,&s));
    assert(dock_icon_build(&icon,&source,40));
    assert(icon.width == 40 && icon.height == 40);
    assert(!memcmp(icon.pixels,cache.image[0].pixels,40*40*4));
    free(icon.pixels);
    dock_cache_free(&cache);
    assert(!dock_icon_build(&icon,&source,DOCK_MIN_ICON_SIZE-1) && !icon.pixels);
}

/* Bicubic resampling: exact at 1:1, flat areas stay flat when
   enlarged, and reductions average fine stripes instead of aliasing. */
static void test_resampling(void)
{
    enum { W = 13, H = 11, FLAT = 8, STRIPES = 64 };
    unsigned char exact[W*H*4], flat[FLAT*FLAT*4], stripes[STRIPES*STRIPES*4];
    DockSettings s = dock_settings_default();
    DockImage source = {W,H,exact};
    DockCache cache;
    unsigned int seed = 7;
    int i, x, y;

    for (i = 0; i < W*H*4; ++i) {
        seed = seed*1103515245U+12345U;
        exact[i] = (unsigned char)(64+(seed>>16)%192);
    }
    s.base_size = s.max_size = W+2*DOCK_ICON_INSET;
    assert(dock_cache_build(&cache,&source,&s));
    for (y = 0; y < H; ++y)
        for (x = 0; x < W; ++x) {
            const unsigned char *p = cache.image[0].pixels+
                ((size_t)(y+(s.base_size-H)/2)*s.base_size+x+DOCK_ICON_INSET)*4;
            assert(!memcmp(p,exact+(y*W+x)*4,4));
        }
    dock_cache_free(&cache);

    for (i = 0; i < FLAT*FLAT; ++i) {
        flat[i*4] = 100; flat[i*4+1] = 150; flat[i*4+2] = 200; flat[i*4+3] = 180;
    }
    source = (DockImage){FLAT,FLAT,flat};
    s.base_size = 16; s.max_size = 64; s.levels = 5;
    assert(dock_cache_build(&cache,&source,&s));
    for (i = 0; i < cache.levels; ++i) {
        const DockImage *image = &cache.image[i];
        for (x = 0; x < image->width*image->height; ++x) {
            const unsigned char *p = image->pixels+(size_t)x*4;
            if (p[3]) assert(p[0] == 100 && p[1] == 150 && p[2] == 200 && p[3] == 180);
        }
    }
    dock_cache_free(&cache);

    for (y = 0; y < STRIPES; ++y)
        for (x = 0; x < STRIPES; ++x) {
            unsigned char *p = stripes+((size_t)y*STRIPES+x)*4;
            p[0] = p[1] = p[2] = (unsigned char)(x&1 ? 255 : 0); p[3] = 255;
        }
    source = (DockImage){STRIPES,STRIPES,stripes};
    s.base_size = s.max_size = 16; s.levels = 2;
    assert(dock_cache_build(&cache,&source,&s));
    for (x = 0; x < 16*16; ++x) {
        const unsigned char *p = cache.image[0].pixels+(size_t)x*4;
        if (p[3]) assert(p[0] >= 116 && p[0] <= 140);
    }
    dock_cache_free(&cache);
}

static void test_distant_icon_motion(void)
{
    DockSettings s = dock_settings_default();
    DockPlacement previous[17], target[17], current[17];
    DockAnimation animation;
    int x, i, tick, far_x;
    assert(dock_layout(&s,17,1024,128,380,100,1,previous));
    /* A complete wave travels inside this row. Icons outside it stay at
       size32. Rounded neighboring widths previously moved them up to3px
       for a single pointer pixel; these logical centers must stay put. */
    for (x = 381; x <= 640; ++x) {
        assert(dock_layout(&s,17,1024,128,x,100,1,target));
        for (i = 0; i < 17; ++i) if (!previous[i].level && !target[i].level)
            assert(previous[i].x == target[i].x);
        memcpy(previous,target,sizeof(previous));
    }
    assert(dock_layout(&s,17,1024,128,0,0,0,current));
    assert(dock_layout(&s,17,1024,128,380,100,1,target));
    dock_animation_reset(&animation);
    for (tick = 0; tick < 8; ++tick) dock_animate_step(&s,&animation,current,target,17);
    assert(!memcmp(current,target,sizeof(current)));
    far_x = current[10].x;
    assert(current[10].size == 32);
    assert(dock_layout(&s,17,1024,128,413,100,1,target));
    assert(target[10].x == far_x && target[10].size == 32);
    for (tick = 0; tick < 48; ++tick) {
        dock_animate_step(&s,&animation,current,target,17);
        /* Previously made a2px excursion despite identical endpoints. */
        assert(current[10].x == far_x && current[10].size == 32);
    }
    assert(!memcmp(current,target,sizeof(current)));
    assert(!dock_animate_step(&s,&animation,current,target,17));
    /* No visible change on one tick does not mean the subpixel motion is
       complete. Native scheduling compares raster target/current separately. */
    current[0] = (DockPlacement){100,0,32,0};
    target[0] = (DockPlacement){101,0,32,0};
    dock_animation_reset(&animation);
    assert(!dock_animate_step(&s,&animation,current,target,1));
    assert(memcmp(current,target,sizeof(*current)));
    for (tick = 1; tick < 8; ++tick) dock_animate_step(&s,&animation,current,target,1);
    assert(!memcmp(current,target,sizeof(*current)));
}

static void test_layout(void)
{
    DockSettings s = dock_settings_default();
    DockPlacement p[DOCK_MAX_ITEMS], rest[DOCK_MAX_ITEMS];
    int count, mx, i;
    for (count = 1; count <= DOCK_MAX_ITEMS; ++count) {
        int width = count * (s.base_size + s.gap) - s.gap + 2 * s.padding;
        assert(dock_layout(&s, count, width, 128, 0, 0, 0, rest));
        for (mx = -20; mx < width + 20; mx += 7) {
            assert(dock_layout(&s, count, width, 128, mx, 90, 1, p));
            for (i = 0; i < count; ++i) {
                assert(p[i].x >= s.padding);
                assert(p[i].x + p[i].size <= width - s.padding);
                assert(p[i].y >= 0 && p[i].y+p[i].size == rest[i].y+rest[i].size);
                assert(p[i].level >= 0 && p[i].level < s.levels);
                if (i) assert(p[i].x >= p[i-1].x + p[i-1].size);
                assert(dock_hit_test(p, count, p[i].x, p[i].y) == i);
            }
        }
    }
    assert(dock_layout(&s, 5, 700, 128, 350, 90, 1, p));
    assert(p[2].size == s.max_size);
    assert(p[0].size < p[2].size && p[4].size < p[2].size);
    assert(dock_layout(&s, 5, 700, 128, 350, 90, 0, p));
    for (i = 0; i < 5; ++i) assert(p[i].size == s.base_size);
    assert(!dock_layout(&s, 32, 100, 128, 0, 0, 0, p));
    assert(!dock_layout(&s, 33, 2000, 128, 0, 0, 0, p));
    assert(dock_hit_test(p, 0, 0, 0) == -1);
    assert(dock_layout(&s, 5, 700, 128, INT_MIN, INT_MAX, 1, p));
    {
        DockPlacement rest[2];
        assert(dock_layout(&s, 2, INT_MAX, 100, INT_MAX-1, 50, 0, rest));
        assert(dock_layout(&s, 2, INT_MAX, 100, INT_MAX-1, 50, 1, p));
        assert(!memcmp(p, rest, sizeof(rest)));
    }
}

static void test_animation_and_pointer_anchor(void)
{
    DockSettings s = dock_settings_default();
    DockPlacement rest[5], left[5], right[5], current[5];
    DockAnimation animation;
    dock_animation_reset(&animation);
    int width = 700, height = 128, ticks = 0, i;
    int rest_width = 5 * s.base_size + 4 * s.gap;
    int rest_left = (width - rest_width) / 2;
    int center = rest_left + rest_width / 2;
    assert(dock_layout(&s, 5, width, height, 0, 0, 0, rest));
    assert(dock_layout(&s, 5, width, height, rest[0].x + s.base_size / 2, 100, 1, left));
    assert(dock_layout(&s, 5, width, height, rest[4].x + s.base_size / 2, 100, 1, right));
    /* End hover preserves the pointer point, so the row midpoint moves. */
    assert(left[0].x < rest_left);
    assert(left[4].x + left[4].size > rest[4].x + rest[4].size);
    assert(right[0].x < rest_left);
    assert(left[0].x + left[0].size/2 == rest[0].x + s.base_size/2);
    assert(left[0].x + left[4].x + left[4].size > 2*center);
    assert(right[4].x + right[4].size/2 == rest[4].x + s.base_size/2);
    assert(right[0].x + right[4].x + right[4].size < 2*center);
    assert(right[4].x + right[4].size > rest[4].x + rest[4].size);
    memcpy(current, rest, sizeof(rest));
    while (memcmp(current,left,sizeof(left))) {
        dock_animate_step(&s,&animation,current,left,5);
        assert(++ticks < 30);
        assert(current[0].x >= 0 && current[4].x+current[4].size <= width);
        for (i = 0; i < 5; ++i) {
            assert(current[i].size >= rest[i].size && current[i].size <= left[i].size);
            assert(current[i].y+current[i].size == rest[i].y+rest[i].size);
            if (i) {
                int gap = current[i].x-current[i-1].x-current[i-1].size;
                assert(gap >= 2 && gap <= s.gap+1);
            }
        }
    }
    assert(ticks > 1 && !memcmp(current, left, sizeof(left)));
    ticks = 0;
    while (memcmp(current,right,sizeof(right))) {
        dock_animate_step(&s,&animation,current,right,5);
        assert(++ticks < 30);
    }
    assert(!memcmp(current, right, sizeof(right)));
    ticks = 0;
    while (memcmp(current,rest,sizeof(rest))) {
        dock_animate_step(&s,&animation,current,rest,5);
        assert(++ticks < 30);
        assert(current[0].x >= 0 && current[4].x+current[4].size <= width);
    }
    assert(!memcmp(current, rest, sizeof(rest)));
    assert(!dock_animate_step(&s, &animation, current, rest, 5));
    /* Preserve a quarter-point, not just the center of the first icon. */
    assert(dock_layout(&s,5,width,height,rest[0].x+s.base_size/4,100,1,left));
    assert(abs((left[0].x-rest[0].x)*4+left[0].size-s.base_size) <= 4);
    memcpy(current,rest,sizeof(rest)); dock_animation_reset(&animation);
    for(i=0;i<8;++i) {
        dock_animate_step(&s,&animation,current,left,5);
        assert(abs((current[0].x-rest[0].x)*4+current[0].size-s.base_size) <= 4);
    }
    /* Denser gaps remain intact after changing the continuous row anchor. */
    {
        int focus = rest[1].x+s.base_size+2;
        assert(dock_layout(&s,5,width,height,focus,100,1,left));
        assert(abs(left[2].x-left[1].x-left[1].size-3) <= 1);
    }
}

static void test_reference_timing_and_panel(void)
{
    DockSettings s = dock_settings_default();
    DockPlacement rest[5], left[5], right[5], current[5];
    DockAnimation animation;
    unsigned char pixels[320 * 100 * 4], backdrop[320 * 100 * 4];
    unsigned char center[100 * 4];
    DockImage frame = {320, 100, pixels}, back = {320, 100, backdrop};
    const DockCache *icons[5] = {NULL, NULL, NULL, NULL, NULL};
    int tick, y;
    static const int entry_levels[8] = {1, 5, 10, 16, 22, 27, 31, 32};
    assert(dock_layout(&s, 5, 320, 100, 0, 0, 0, rest));
    assert(dock_layout(&s, 5, 320, 100, 80, 60, 1, left));
    assert(dock_layout(&s, 5, 320, 100, 240, 60, 1, right));
    memcpy(current, rest, sizeof(rest));
    dock_animation_reset(&animation);
    assert(dock_animate_step(&s, &animation, current, left, 5));
    assert(current[0].level == entry_levels[0]); /* gentle one-pixel first step */
    assert(animation.tick == 1);
    for (tick = 2; tick <= 8; ++tick) {
        dock_animate_step(&s, &animation, current, left, 5);
        assert(current[0].level == entry_levels[tick-1]);
    }
    assert(current[0].size == 64 && current[0].size == 2 * s.base_size);
    memcpy(current, rest, sizeof(rest));
    dock_animation_reset(&animation);
    for (tick = 1; tick <= 8; ++tick) {
        const DockPlacement *target = tick % 2 ? left : right;
        dock_animate_step(&s, &animation, current, target, 5);
        assert(animation.tick == tick); /* moving the pointer does not restart */
    }
    assert(!memcmp(current, right, sizeof(right)));
    assert(!dock_animate_step(&s, &animation, current, right, 5));
    for (tick = 1; tick <= 8; ++tick) {
        dock_animate_step(&s, &animation, current, rest, 5);
        assert(animation.tick == tick);
    }
    assert(!memcmp(current, rest, sizeof(rest)));
    memset(backdrop, 200, sizeof(backdrop));
    for (y = 0; y < 320 * 100; ++y) backdrop[y*4+3] = 255;
    dock_render_oriented_mask(&frame, &back, icons, rest, 5, &s, NULL, NULL, NULL, 0, DOCK_EDGE_BOTTOM, 0, NULL);
    for (y = 0; y < 100; ++y) memcpy(center+y*4, pixels+(y*320+160)*4, 4);
    dock_render_oriented_mask(&frame, &back, icons, left, 5, &s, NULL, NULL, NULL, 0, DOCK_EDGE_BOTTOM, 0, NULL);
    for (y = 0; y < 100; ++y) {
        assert(!memcmp(center+y*4, pixels+(y*320+160)*4, 4));
        if (y < 100-DOCK_DESKTOP_GAP-dock_panel_thickness(&s))
            assert(!memcmp(pixels+y*320*4, backdrop+y*320*4, 320*4));
    }
    /* Panel thickness stays fixed; the enlarged icon itself rises above it. */
    assert(left[0].y < 100-DOCK_DESKTOP_GAP-dock_panel_thickness(&s));
    assert(rest[0].y >= 100-DOCK_DESKTOP_GAP-dock_panel_thickness(&s));
}

static void test_rounded_panel(void)
{
    enum { W = 320, H = 100, LEFT = 62, RIGHT = 258, BOTTOM = H-DOCK_DESKTOP_GAP, TOP = BOTTOM-46 };
    DockSettings s = dock_settings_default();
    DockPlacement p[5];
    const DockCache *icons[5] = {NULL, NULL, NULL, NULL, NULL};
    DockPanelShape *shape = (DockPanelShape *)calloc(1, sizeof(*shape));
    unsigned char pixels[W * H * 4], reference[W * H * 4], back[W * H * 4];
    DockImage frame = {W, H, pixels}, backdrop = {W, H, back};
    int x, y, fractional = 0;
    assert(shape && dock_layout(&s, 5, W, H, 0, 0, 0, p));
    for (x = 0; x < W * H; ++x) {
        back[x*4] = 120; back[x*4+1] = 100; back[x*4+2] = 80; back[x*4+3] = 255;
    }
    dock_render_oriented_mask(&frame, &backdrop, icons, p, 5, &s, NULL, shape, NULL, 0, DOCK_EDGE_BOTTOM, 0, NULL);
    assert(shape->radius == 11);
    memcpy(reference, pixels, sizeof(pixels));
    dock_render_oriented_mask(&frame, &backdrop, icons, p, 5, &s, NULL, NULL, NULL, 0, DOCK_EDGE_BOTTOM, 0, NULL);
    assert(!memcmp(reference, pixels, sizeof(pixels))); /* cached and analytic masks agree */
    for (y = TOP; y < BOTTOM; ++y) for (x = LEFT; x < RIGHT; ++x) {
        int mx = LEFT + RIGHT - 1 - x, my = TOP + BOTTOM - 1 - y;
        assert(!memcmp(pixels+(y*W+x)*4,pixels+(y*W+mx)*4,4));
        assert(!memcmp(pixels+(y*W+x)*4,pixels+(my*W+x)*4,4));
    }
    assert(!memcmp(pixels+(TOP*W+LEFT)*4, back+(TOP*W+LEFT)*4, 4));
    assert(!memcmp(pixels+((BOTTOM-1)*W+LEFT)*4, back+((BOTTOM-1)*W+LEFT)*4, 4));
    assert(!memcmp(pixels+(TOP*W+RIGHT-1)*4, back+(TOP*W+RIGHT-1)*4, 4));
    assert(!memcmp(pixels+((BOTTOM-1)*W+RIGHT-1)*4, back+((BOTTOM-1)*W+RIGHT-1)*4, 4));
    assert(!memcmp(pixels+BOTTOM*W*4, back+BOTTOM*W*4, (H-BOTTOM)*W*4));
    /* All four straight perimeter segments have a continuous subtle rim. */
    assert(pixels[(TOP*W+160)*4] > pixels[((TOP+1)*W+160)*4]);
    assert(pixels[((BOTTOM-1)*W+160)*4] > pixels[((BOTTOM-2)*W+160)*4]);
    assert(pixels[(72*W+LEFT)*4] > pixels[(72*W+LEFT+1)*4]);
    assert(pixels[(72*W+RIGHT-1)*4] > pixels[(72*W+RIGHT-2)*4]);
    for (y = 0; y < shape->radius; ++y) for (x = 0; x < shape->radius; ++x) {
        unsigned int a = shape->outer[y*DOCK_PANEL_MAX_RADIUS+x];
        unsigned int b = shape->inner[y*DOCK_PANEL_MAX_RADIUS+x];
        assert(a >= b);
        if (a > 0 && a < 255) ++fractional;
    }
    assert(fractional > 10); /* coverage antialiasing, not binary stair steps */
    assert(dock_layout(&s, 5, W, H, 80, 60, 1, p));
    dock_render_oriented_mask(&frame, &backdrop, icons, p, 5, &s, NULL, shape, NULL, 0, DOCK_EDGE_BOTTOM, 0, NULL);
    assert(shape->radius == 11); /* width animation reuses radius-only masks */
    assert(!memcmp(pixels+BOTTOM*W*4, back+BOTTOM*W*4, (H-BOTTOM)*W*4));
    frame.width = frame.height = 1;
    dock_render_oriented_mask(&frame, NULL, NULL, NULL, 0, &s, NULL, shape, NULL, 0, DOCK_EDGE_BOTTOM, 0, NULL);
    assert(pixels[0] == 32 && pixels[3] == 255);
    free(shape);
}

static void test_blend(void)
{
    unsigned char target[3 * 3 * 4], source[] = {200, 100, 40, 128};
    unsigned char before[sizeof(target)];
    DockImage dst = {3, 3, target}, src = {1, 1, source};
    memset(target, 0, sizeof(target));
    target[(1 * 3 + 1) * 4 + 3] = 255;
    dock_blend(&dst, &src, 1, 1);
    assert(target[16] == 100 && target[17] == 50 && target[18] == 20 && target[19] == 255);
    memcpy(before, target, sizeof(target));
    dock_blend(&dst, &src, -1, -1);
    dock_blend(&dst, &src, 3, 3);
    dock_blend(&dst, &src, INT_MIN, INT_MAX);
    dock_blend(&dst, &src, INT_MAX, INT_MIN);
    assert(memcmp(before, target, sizeof(target)) == 0);
    source[3] = 0;
    dock_blend(&dst, &src, 1, 1);
    assert(memcmp(before, target, sizeof(target)) == 0);
    source[3] = 255;
    dock_blend(&dst, &src, 0, 0);
    assert(memcmp(target, source, 4) == 0);
}

static void test_convert(void)
{
    unsigned char src[160], before[160], out[160];
    int offset, count, i;
    for (i = 0; i < 160; ++i) src[i] = (unsigned char)(i * 137 + 17);
    memcpy(before, src, sizeof(src));
    for (offset = 0; offset < 8; ++offset) for (count = 0; count <= 31; ++count) {
        memset(out, 0xa5, sizeof(out));
        dock_rgba_to_rgb24_c(out + 8 + offset, src + 8 + offset, (unsigned long)count);
        for (i = 0; i < 160; ++i) {
            int n = i - 8 - offset;
            unsigned char expected = 0xa5;
            if (n >= 0 && n < count * 3) expected = src[8 + offset + (n / 3) * 4 + n % 3];
            assert(out[i] == expected);
        }
        assert(!memcmp(src, before, sizeof(src)));
    }
    dock_rgba_to_rgb24_c(NULL, NULL, 0);
}

static void test_native565(void)
{
    unsigned int value;
    unsigned char source[2], decoded[4], packed[2];
    int count, offset, i, n;
    unsigned char input[256], expected[256], output[256];
    for (value = 0; value < 65536; ++value) {
        source[0] = (unsigned char)(value >> 8); source[1] = (unsigned char)value;
        dock_rgb565_to_rgba_c(decoded, source, 1);
        assert(decoded[3] == 255);
        if (value == 0) assert(decoded[0] == 0 && decoded[1] == 0 && decoded[2] == 0);
        if (value == 65535) assert(decoded[0] == 255 && decoded[1] == 255 && decoded[2] == 255);
        if (value == 0xf800) assert(decoded[0] == 255 && decoded[1] == 0 && decoded[2] == 0);
        if (value == 0x07e0) assert(decoded[0] == 0 && decoded[1] == 255 && decoded[2] == 0);
        if (value == 0x001f) assert(decoded[0] == 0 && decoded[1] == 0 && decoded[2] == 255);
        dock_rgba_to_rgb565_c(packed, decoded, 1);
        assert(!memcmp(source, packed, 2));
    }
    for (i = 0; i < 256; ++i) input[i] = (unsigned char)(i * 137 + 17);
    for (count = 0; count <= 31; ++count) for (offset = 0; offset < 8; ++offset) {
        memset(output, 0xa5, sizeof(output)); memset(expected, 0xa5, sizeof(expected));
        for (i = 0; i < count; ++i) dock_rgb565_to_rgba_c(expected+8+offset+i*4, input+8+offset+i*2, 1);
        dock_rgb565_to_rgba_c(output+8+offset, input+8+offset, (unsigned long)count);
        assert(!memcmp(expected, output, sizeof(output)));
        memset(output, 0xa5, sizeof(output)); memset(expected, 0xa5, sizeof(expected));
        for (i = 0; i < count; ++i) dock_rgba_to_rgb565_c(expected+8+offset+i*2, input+8+offset+i*4, 1);
        dock_rgba_to_rgb565_c(output+8+offset, input+8+offset, (unsigned long)count);
        assert(!memcmp(expected, output, sizeof(output)));
    }
    for (n = 0; n <= 63; ++n) for (offset = 0; offset < 8; ++offset) {
        memcpy(output, input, sizeof(input));
        assert(dock_bytes_equal_c(input+offset, output+offset, (unsigned long)n));
        for (i = 0; i < n; ++i) {
            output[offset+i] ^= 1;
            assert(!dock_bytes_equal_c(input+offset, output+offset, (unsigned long)n));
            output[offset+i] ^= 1;
        }
    }
    dock_rgb565_to_rgba_c(NULL, NULL, 0); dock_rgba_to_rgb565_c(NULL, NULL, 0);
    assert(dock_bytes_equal_c(NULL, NULL, 0));
}

/* Coverage, not matching RGB, determines which pixels pass through PiP. */
static void test_key565(void)
{
    unsigned int value, mode;
    unsigned char dst[2], mask;
    int count, align, src_align, i;
    for (value = 0; value < 65536; ++value) for (mode = 0; mode < 3; ++mode) {
        unsigned int expected;
        mask = mode == 0 ? 0 : mode == 1 ? 1 : 255;
        expected = !mask ? 0xf81f : value == 0xf81f ? 0xf81e : value;
        dst[0] = (unsigned char)(value >> 8); dst[1] = (unsigned char)value;
        dock_rgb565_key_c(dst, &mask, 1);
        assert(dst[0] == (unsigned char)(expected >> 8) && dst[1] == (unsigned char)expected);
        assert(mask == (mode == 0 ? 0 : mode == 1 ? 1 : 255));
    }
    for (count = 0; count <= 31; ++count) for (align = 0; align < 8; ++align)
        for (src_align = 0; src_align < 8; ++src_align) {
            unsigned char input[96], saved[96], output[96], expected[96];
            memset(input, 0x5a, sizeof(input)); memset(output, 0xa5, sizeof(output));
            memset(expected, 0xa5, sizeof(expected));
            for (i = 0; i < count; ++i) {
                unsigned int v = i % 3 == 0 ? 0xf81f : (unsigned int)i * 2203;
                unsigned char m = i % 3 == 0 ? 255 : i % 3 == 1 ? 1 : 0;
                unsigned int e = !m ? 0xf81f : v == 0xf81f ? 0xf81e : v;
                output[8+align+i*2] = (unsigned char)(v >> 8);
                output[9+align+i*2] = (unsigned char)v;
                input[8+src_align+i] = m;
                expected[8+align+i*2] = (unsigned char)(e >> 8);
                expected[9+align+i*2] = (unsigned char)e;
            }
            memcpy(saved, input, sizeof(saved));
            dock_rgb565_key_c(output+8+align, input+8+src_align, (unsigned long)count);
            assert(!memcmp(output, expected, sizeof(output)) && !memcmp(input, saved, sizeof(input)));
        }
    dock_rgb565_key_c(NULL, NULL, 0);
}

static void test_overlay_coverage(void)
{
    DockSettings settings = dock_settings_default();
    unsigned char icon_pixels[4*4*4] = {255,0,255,255, 255,0,255,128};
    DockCache icon = {0};
    const DockCache *icons[] = {&icon};
    DockPlacement place = {48,8,4,0};
    DockImage backdrop = {100,100,malloc(100*100*4)};
    DockImage frame = {100,100,malloc(100*100*4)};
    unsigned char *mask = malloc(100*100), *encoded = malloc(100*100*2), *plain = malloc(100*100*4);
    int i;
    assert(backdrop.pixels && frame.pixels && mask && encoded && plain);
    for (i = 0; i < 100*100; ++i) {
        backdrop.pixels[i*4] = 255; backdrop.pixels[i*4+1] = 0;
        backdrop.pixels[i*4+2] = 255; backdrop.pixels[i*4+3] = 255;
    }
    icon.levels = 1; icon.image[0].width = icon.image[0].height = 4; icon.image[0].pixels = icon_pixels;
    dock_render_oriented_mask(&frame, &backdrop, icons, &place, 1, &settings, NULL, NULL, NULL, 0, DOCK_EDGE_BOTTOM, 0, NULL);
    memcpy(plain, frame.pixels, 100*100*4);
    memset(mask, 0xa5, 100*100);
    dock_render_oriented_mask(&frame, &backdrop, icons, &place, 1, &settings, NULL, NULL, mask, 0, DOCK_EDGE_BOTTOM, 0, NULL);
    assert(!memcmp(plain, frame.pixels, 100*100*4));
    assert(mask[8*100+48] == 255 && mask[8*100+49] == 255 && mask[8*100+50] == 0);
    assert(mask[70*100+50] == 255 && mask[0] == 0 && mask[99*100+50] == 0);
    dock_rgba_to_rgb565_c(encoded, frame.pixels, 100*100);
    dock_rgb565_key_c(encoded, mask, 100*100);
    /* Opaque and fractional foreground equal to magenta background stay owned;
       only zero-coverage pixels become the hardware's transparent code. */
    assert(encoded[(8*100+48)*2+1] == 0x1e && encoded[(8*100+49)*2+1] == 0x1e);
    assert(encoded[(8*100+50)*2] == 0xf8 && encoded[(8*100+50)*2+1] == 0x1f);
    free(plain); free(encoded); free(mask); free(frame.pixels); free(backdrop.pixels);
}

static void test_mix(void)
{
    unsigned int a, b;
    for (a = 0; a < 256; ++a) for (b = 0; b < 256; ++b) {
        unsigned char dst[4], src[4];
        memset(dst, (int)a, 4); memset(src, (int)b, 4);
        dock_mix_half_c(dst, src, 1);
        assert(dst[0] == (a + b + 1) / 2 && dst[3] == dst[0]);
    }
    dock_mix_half_c(NULL, NULL, 0);
}

static void test_pressed_artwork(void)
{
    DockSettings s = dock_settings_default();
    unsigned char art[4*4*4] = {200,100,40,255, 200,100,40,128, 90,30,250,0};
    unsigned char saved[sizeof(art)], normal[100*100*4], pixels[100*100*4];
    unsigned char back[100*100*4], normal_mask[100*100], mask[100*100];
    DockImage backdrop = {100,100,back}, frame = {100,100,pixels};
    DockCache cache = {0};
    const DockCache *icons[2] = {&cache,&cache};
    DockPlacement p[2] = {{35,8,4,0},{60,8,4,0}}, saved_places[2];
    DockRenderer renderer = {0};
    int i, x, y, c;
    for (i = 0; i < 100*100; ++i) {
        back[i*4] = 80; back[i*4+1] = 120; back[i*4+2] = 160; back[i*4+3] = 255;
    }
    memcpy(saved,art,sizeof(art)); memcpy(saved_places,p,sizeof(p));
    cache.levels = 1; cache.image[0] = (DockImage){4,4,art};
    renderer.pixels = dock_backend_portable(); renderer.coverage = normal_mask;
    dock_renderer_draw(&renderer,&frame,&backdrop,icons,p,2,&s);
    memcpy(normal,pixels,sizeof(pixels));
    renderer.pressed_item = 1; renderer.coverage = mask;
    dock_renderer_draw(&renderer,&frame,&backdrop,icons,p,2,&s);
    for (y = 0; y < 100; ++y) for (x = 0; x < 100; ++x) {
        int index = (y*100+x)*4;
        if (y == 8 && (x == 35 || x == 36)) {
            int alpha = x == 35 ? 255 : 128;
            for (c = 0; c < 3; ++c) {
                int value = (art[(x-35)*4+c]*3+2)/4;
                assert(pixels[index+c] == (value*alpha+back[index+c]*(255-alpha)+127)/255);
            }
            assert(pixels[index+3] == 255);
        } else assert(!memcmp(pixels+index,normal+index,4));
    }
    assert(!memcmp(mask,normal_mask,sizeof(mask)));
    assert(!memcmp(saved,art,sizeof(art)) && !memcmp(saved_places,p,sizeof(p)));
    renderer.pressed_item = 0;
    dock_renderer_draw(&renderer,&frame,&backdrop,icons,p,2,&s);
    assert(!memcmp(normal,pixels,sizeof(pixels)));
    /* Kept glass rows give the same frame as mixing the plate every frame,
       including after the backdrop changes and the caller says so. */
    assert(renderer.glass.ready);
    dock_render_oriented_mask(&frame,&backdrop,icons,p,2,&s,NULL,NULL,NULL,0,DOCK_EDGE_BOTTOM,0,NULL);
    assert(!memcmp(normal,pixels,sizeof(pixels)));
    for (i = 0; i < 100*100*4; ++i) back[i] = (unsigned char)(i*7+i/400);
    dock_renderer_backdrop_changed(&renderer);
    dock_renderer_draw(&renderer,&frame,&backdrop,icons,p,2,&s);
    memcpy(normal,pixels,sizeof(pixels));
    dock_render_oriented_mask(&frame,&backdrop,icons,p,2,&s,NULL,NULL,NULL,0,DOCK_EDGE_BOTTOM,0,NULL);
    assert(!memcmp(normal,pixels,sizeof(pixels)));
    dock_renderer_release(&renderer);
    assert(!renderer.glass.ready && !renderer.glass.image.pixels);
}

static void test_side_panel_coverage(void)
{
    enum { LONG = 384, CROSS = 96, N = LONG*CROSS };
    DockSettings s = dock_settings_default();
    DockPlacement bottom[5], side[5];
    const DockCache *icons[5] = {NULL,NULL,NULL,NULL,NULL};
    unsigned char *pixels = malloc(N*4), *left_pixels = malloc(N*4);
    unsigned char *bottom_mask = malloc(N), *side_mask = malloc(N);
    DockImage frame = {LONG,CROSS,pixels};
    DockPanelShape *shape = calloc(1,sizeof(*shape));
    int edge,x,y;
    assert(pixels && left_pixels && bottom_mask && side_mask && shape);
    assert(dock_edge_layout(&s,DOCK_EDGE_BOTTOM,5,LONG,CROSS,LONG/2,72,1,bottom));
    dock_render_oriented_mask(&frame,NULL,icons,bottom,5,&s,NULL,shape,bottom_mask,0,DOCK_EDGE_BOTTOM,0,NULL);
    assert(shape->radius == 11);
    frame.width = CROSS; frame.height = LONG;
    for (edge = DOCK_EDGE_LEFT; edge <= DOCK_EDGE_RIGHT; ++edge) {
        int mouse_x = edge == DOCK_EDGE_LEFT ? CROSS-1-72 : 72;
        assert(dock_edge_layout(&s,(DockEdge)edge,5,CROSS,LONG,mouse_x,LONG/2,1,side));
        dock_render_oriented_mask(&frame,NULL,icons,side,5,&s,NULL,shape,side_mask,0,edge,0,NULL);
        assert(shape->radius == 11);
        if (edge == DOCK_EDGE_LEFT) memcpy(left_pixels,pixels,N*4);
        for (y = 0; y < LONG; ++y) for (x = 0; x < CROSS; ++x) {
            int virtual_y = edge == DOCK_EDGE_LEFT ? CROSS-1-x : x;
            assert(side_mask[y*CROSS+x] == bottom_mask[virtual_y*LONG+y]);
            if (edge == DOCK_EDGE_RIGHT)
                assert(!memcmp(pixels+4*(y*CROSS+x),left_pixels+4*(y*CROSS+CROSS-1-x),4));
        }
    }
    free(shape); free(side_mask); free(bottom_mask); free(left_pixels); free(pixels);
}

static void test_reorder(void)
{
    struct Item { int id; char name[4]; } items[] = {{1, "one"}, {2, "two"}, {3, "tri"}};
    assert(dock_reorder(items, sizeof(items[0]), 3, 0, 2));
    assert(items[0].id == 2 && items[2].id == 1 && !strcmp(items[2].name, "one"));
    assert(dock_reorder(items, sizeof(items[0]), 3, 2, 0));
    assert(items[0].id == 1 && items[2].id == 3);
    assert(!dock_reorder(items, sizeof(items[0]), 3, -1, 1));
}

static void test_clipped_divider(void)
{
    unsigned char pixels[32*32*4+16];
    DockImage canvas = {32,32,pixels+8};
    DockSettings s = dock_settings_default();
    const DockCache *icons[2] = {NULL,NULL};
    static const int positions[][2] = {{INT_MAX-4,INT_MAX},{INT_MIN,INT_MAX},
        {INT_MIN,16},{-16,INT_MAX},{INT_MAX,INT_MIN}};
    size_t i;
    int edge, j;
    s.end_gap = 12;
    for (edge = DOCK_EDGE_BOTTOM; edge <= DOCK_EDGE_RIGHT; ++edge)
        for (i = 0; i < sizeof(positions)/sizeof(positions[0]); ++i) {
            DockPlacement p[2] = {{positions[i][0],positions[i][0],32,0},
                {positions[i][1],positions[i][1],32,0}};
            memset(pixels,0xa5,sizeof(pixels));
            dock_render_oriented_mask(&canvas,NULL,icons,p,2,&s,NULL,NULL,NULL,0,edge,0,NULL);
            for (j = 0; j < 8; ++j) {
                assert(pixels[j] == 0xa5);
                assert(pixels[sizeof(pixels)-1-j] == 0xa5);
            }
        }
}

static void test_decode_rgb24(void)
{
    unsigned char source[128], original[128], actual[160], expected[160];
    int source_offset, dest_offset, count, i;
    for (i = 0; i < (int)sizeof(source); ++i) source[i] = (unsigned char)(i*131);
    memcpy(original,source,sizeof(source));
    for (source_offset = 0; source_offset < 8; ++source_offset)
        for (dest_offset = 0; dest_offset < 8; ++dest_offset)
            for (count = 0; count < 32; ++count) {
                memset(actual,0xa5,sizeof(actual));
                memset(expected,0xa5,sizeof(expected));
                for (i = 0; i < count; ++i) {
                    memcpy(expected+dest_offset+i*4,source+source_offset+i*3,3);
                    expected[dest_offset+i*4+3] = 255;
                }
                dock_rgb24_to_rgba_c(actual+dest_offset,source+source_offset,(unsigned long)count);
                assert(!memcmp(actual,expected,sizeof(actual)));
                assert(!memcmp(source,original,sizeof(source)));
            }
    dock_rgb24_to_rgba_c(NULL,NULL,0);
}

static void test_items_fit(void)
{
    DockSettings s = dock_settings_default();
    size_t bytes = dock_cache_bytes(&s);
    int width = dock_row_length(&s,6)+2*s.padding;
    assert(dock_items_fit(&s,6,width));
    assert(!dock_items_fit(&s,6,width-1));
    s.cache_limit = bytes*6;
    assert(dock_items_fit(&s,6,width));
    --s.cache_limit;
    assert(!dock_items_fit(&s,6,width));
    assert(dock_items_fit(&s,5,width));
    s.cache_limit = 0;
    assert(dock_items_fit(&s,0,2*s.padding));
    assert(!dock_items_fit(&s,1,320));
    assert(!dock_items_fit(&s,-1,320));
    assert(!dock_items_fit(&s,DOCK_MAX_ITEMS+1,INT_MAX));
    assert(!dock_items_fit(&s,0,INT_MIN));
    assert(!dock_items_fit(NULL,0,320));
    s = dock_settings_default();
    s.end_gap = 12;
    assert(!dock_items_fit(&s,6,width));
    assert(dock_items_fit(&s,6,width+12));
    s.base_size = 96; s.max_size = 192;
    assert(!dock_items_fit(&s,6,1280));
    assert(dock_items_fit(&s,5,1280));
}

static void test_renderer(void)
{
    DockSettings s = dock_settings_default();
    unsigned char *pixels = (unsigned char *)malloc(320 * 128 * 4);
    unsigned char *back = (unsigned char *)malloc(320 * 128 * 4);
    DockImage frame = {320, 128, pixels}, backdrop = {320, 128, back};
    int i;
    DockRenderer r = {.pixels = dock_backend_portable()};
    assert(pixels && back);
    for (i = 0; i < 320 * 128; ++i) {
        back[i*4] = 200; back[i*4+1] = 100; back[i*4+2] = 50; back[i*4+3] = 255;
    }
    dock_renderer_draw(&r, &frame, &backdrop, NULL, NULL, 0, &s);
    assert(!memcmp(pixels, back, 320 * 4));
    assert(pixels[(100 * 320 + 160) * 4] == 110);
    dock_renderer_draw(&r, &frame, NULL, NULL, NULL, 0, &s);
    assert(pixels[0] == 32 && pixels[3] == 255);
    free(pixels); free(back);
}

/* Precomputed runs draw exactly what scanning every pixel draws, at every
   level, clipped at all four sides, pressed or not, with and without coverage. */
static void test_icon_runs(void)
{
    enum { SRC = 64, W = 160, H = 120 };
    static unsigned char src[SRC*SRC*4], a[W*H*4], b[W*H*4], ma[W*H], mb[W*H];
    DockImage source = {SRC,SRC,src}, fa = {W,H,a}, fb = {W,H,b};
    DockSettings s = dock_settings_default();
    DockCache cache, scanned;
    const DockCache *with[1] = {&cache}, *without[1] = {&scanned};
    unsigned seed = 7;
    int i, l, x, y, k;
    for (y = 0; y < SRC; ++y) for (x = 0; x < SRC; ++x) {
        int dx = x-SRC/2, dy = y-SRC/2, d = dx*dx+dy*dy;
        unsigned char *q = src+(y*SRC+x)*4;
        seed = seed*1103515245u+12345u;
        q[0] = (unsigned char)(seed>>8); q[1] = (unsigned char)(seed>>16); q[2] = (unsigned char)(x*4);
        q[3] = d < 400 ? 255 : d < 700 ? (unsigned char)(seed>>24) : d < 900 ? (unsigned char)((seed>>24)&1 ? 255 : 0) : 0;
    }
    s.base_size = 48; s.max_size = 96; s.levels = 5;
    memset(&cache,0,sizeof(cache));
    assert(dock_cache_build(&cache,&source,&s));
    scanned = cache;
    for (l = 0; l < cache.levels; ++l) { assert(cache.runs[l]); scanned.runs[l] = NULL; }
    for (k = 0; k < 400; ++k) {
        DockPlacement p;
        int pressed = k&1;
        unsigned char *mask_a = k&2 ? ma : NULL, *mask_b = k&2 ? mb : NULL;
        seed = seed*1103515245u+12345u;
        l = (int)(seed>>8)%cache.levels;
        p.size = cache.image[l].width; p.level = l;
        p.x = (int)((seed>>12)%(W+2*p.size))-p.size; p.y = (int)((seed>>20)%(H+2*p.size))-p.size;
        for (i = 0; i < W*H*4; ++i) a[i] = b[i] = (unsigned char)(i*13+k);
        dock_render_oriented_mask(&fa,&fa,with,&p,1,&s,NULL,NULL,mask_a,pressed,DOCK_EDGE_BOTTOM,0,NULL);
        dock_render_oriented_mask(&fb,&fb,without,&p,1,&s,NULL,NULL,mask_b,pressed,DOCK_EDGE_BOTTOM,0,NULL);
        assert(!memcmp(a,b,sizeof(a)));
        if (mask_a) assert(!memcmp(ma,mb,sizeof(ma)));
    }
    dock_cache_free(&cache);
}

int main(void)
{
    DockSettings defaults = dock_settings_default();
    test_normalize(); test_reference_wave_and_hover(); test_zoom_gap_compaction(); test_inter_icon_anchor_continuity(); test_alpha_fringe(); test_distant_icon_motion(); test_layout(); test_blend();
    test_mix(); test_pressed_artwork(); test_side_panel_coverage(); test_reorder(); test_renderer();
    test_faint_shadows(); test_scaled_shadows(); test_resampling(); test_enclosed_glass_and_centering(); test_animation_stays_inside(); test_icon_build(); test_icon_runs();
    test_clipped_divider(); test_decode_rgb24(); test_items_fit();
    test_animation_and_pointer_anchor(); test_reference_timing_and_panel(); test_rounded_panel();
    test_convert(); test_native565(); test_key565(); test_overlay_coverage();
    printf("All core tests passed: normalization, alpha, layout bounds, clipping, 65536 blends, reorder, renderer.\n");
    printf("Default cache: %lu bytes/icon; 20 icons: %lu bytes.\n",
        (unsigned long)dock_cache_bytes(&defaults),
        20UL * (unsigned long)dock_cache_bytes(&defaults));
    return 0;
}
