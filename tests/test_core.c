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
    /* The macOS Dock profile (measured at 2x, +-0.03 of the zoom): icons 1, 2
       and 3 steps from the pointer grow 0.84-0.87, 0.47-0.49 and ~0.04 of the
       full zoom; the fourth stays at rest. Here the zoom is 32 px. */
    assert(wave[4].size == 64);
    assert(wave[3].size >= 32+26 && wave[3].size <= 32+29);
    assert(wave[2].size >= 32+14 && wave[2].size <= 32+17);
    assert(wave[1].size >= 32 && wave[1].size <= 32+3);
    for(i=0;i<4;++i) assert(wave[i].size == wave[8-i].size);
    assert(wave[0].size == s.base_size);
    /* Shared midpoint makes both adjacent icons near the peak, rather than
       a narrow spike with an almost unscaled neighbor. */
    x = first_center+3*(s.base_size+s.gap)+(s.base_size+s.gap)/2;
    assert(dock_layout(&s,9,700,128,x,100,1,wave));
    assert(wave[3].size >= 62 && wave[4].size >= 62);
    assert(wave[2].size >= 53 && wave[5].size >= 53);
    assert(wave[1].size >= 36 && wave[1].size <= 42 && abs(wave[6].size-wave[1].size) <= 1);
    assert(wave[0].size == s.base_size && wave[7].size == s.base_size);
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
    assert(target[4].size == 64 && target[3].size == 60);
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
    int rest_center[17], outside = 0;
    assert(dock_layout(&s,17,1024,128,0,0,0,current));
    for (i = 0; i < 17; ++i) rest_center[i] = current[i].x+s.base_size/2;
    assert(dock_layout(&s,17,1024,128,380,100,1,previous));
    /* A complete wave, four pitches each way, travels inside this row.
       Icons outside it stay at size 32. Rounded neighboring widths previously
       moved them up to 3px for a single pointer pixel; the wave keeps the
       row's growth constant, so these icons must stay put. */
    for (x = 381; x <= 640; ++x) {
        assert(dock_layout(&s,17,1024,128,x,100,1,target));
        for (i = 0; i < 17; ++i)
            if (abs(x-1-rest_center[i]) >= 4*(s.base_size+s.gap) && abs(x-rest_center[i]) >= 4*(s.base_size+s.gap)) {
                assert(!previous[i].level && !target[i].level);
                assert(previous[i].x == target[i].x);
                ++outside;
            }
        memcpy(previous,target,sizeof(previous));
    }
    assert(outside > 2000);
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


/* Shared render fixture for the goldens and damage tests: three synthetic
   icons with opaque, translucent and transparent runs, a patterned backdrop,
   and the app's running dot at the app's position. */
enum { FIX_COUNT = 5, FIX_SCENARIOS = 3*2*2*2*2*2*4 };
typedef struct {
    DockSettings s;
    DockCache cache[3];
    int edge, width, height;
    DockImage canvas, backdrop;
    unsigned char *coverage;
} RenderFixture;

static unsigned char fixture_dot_pixels[] = {
    175,179,188,32, 175,179,188,210, 175,179,188,32,
    175,179,188,210, 175,179,188,255, 175,179,188,210,
    175,179,188,32, 175,179,188,210, 175,179,188,32
};
static const DockImage fixture_dot = {3,3,fixture_dot_pixels};

static uint32_t fnv1a(uint32_t h, const unsigned char *p, size_t n)
{
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h;
}

static void fixture_sources(RenderFixture *f)
{
    static unsigned char src[3][64*64*4];
    unsigned seed = 99;
    int k, x, y;
    for (k = 0; k < 3; ++k) {
        DockImage source = {64,64,src[k]};
        for (y = 0; y < 64; ++y) for (x = 0; x < 64; ++x) {
            unsigned char *q = src[k]+(y*64+x)*4;
            int dx = x-32, dy = y-32, d = dx*dx+dy*dy;
            seed = seed*1103515245u+12345u;
            q[0] = (unsigned char)(x*4+k*60); q[1] = (unsigned char)(y*4); q[2] = (unsigned char)(seed>>16);
            if (k == 0) q[3] = d < 500 ? 255 : d < 800 ? (unsigned char)(800-d) : 0;
            else if (k == 1) {
                q[3] = (x/6+y/6)%3 ? 255 : (x%7 ? 120 : 0);
                if ((x+y)%11 == 0) { q[0] = 255; q[1] = 0; q[2] = 255; }
                if (x < 4 || y < 4 || x > 59 || y > 59) q[3] = 0;
            } else q[3] = d < 300 ? 255 : d < 900 ? (unsigned char)(seed>>24) : 0;
        }
        memset(&f->cache[k],0,sizeof(f->cache[k]));
        assert(dock_cache_build(&f->cache[k],&source,&f->s));
    }
}

static void fixture_open(RenderFixture *f, int edge)
{
    int cross, length, i;
    f->edge = edge;
    length = dock_viewport_length(&f->s,FIX_COUNT);
    cross = f->s.max_size+2*f->s.padding+12;
    if (cross < dock_panel_thickness(&f->s)+DOCK_DESKTOP_GAP) cross = dock_panel_thickness(&f->s)+DOCK_DESKTOP_GAP;
    f->width = edge == DOCK_EDGE_BOTTOM ? length : cross;
    f->height = edge == DOCK_EDGE_BOTTOM ? cross : length;
    f->canvas = (DockImage){f->width,f->height,malloc((size_t)f->width*f->height*4)};
    f->backdrop = (DockImage){f->width,f->height,malloc((size_t)f->width*f->height*4)};
    f->coverage = malloc((size_t)f->width*f->height);
    assert(f->canvas.pixels && f->backdrop.pixels && f->coverage);
    for (i = 0; i < f->width*f->height; ++i) {
        unsigned char *q = f->backdrop.pixels+(size_t)i*4;
        q[0] = (unsigned char)(i*3); q[1] = (unsigned char)(i/f->width*5); q[2] = (unsigned char)(i*i>>7); q[3] = 255;
    }
}

static void fixture_close(RenderFixture *f)
{
    free(f->canvas.pixels); free(f->backdrop.pixels); free(f->coverage);
    f->canvas.pixels = f->backdrop.pixels = NULL; f->coverage = NULL;
}

/* The pointer on the icon centre line: at rest, at the left (top) end, the
   middle, and the right (bottom) end of the resting row. */
static void fixture_layout(const RenderFixture *f, int pointer, DockPlacement *p)
{
    DockPlacement rest[FIX_COUNT];
    int bottom = f->edge == DOCK_EDGE_BOTTOM, along, centre;
    assert(dock_edge_layout(&f->s,(DockEdge)f->edge,FIX_COUNT,f->width,f->height,0,0,0,rest));
    if (!pointer) { memcpy(p,rest,sizeof(rest)); return; }
    centre = bottom ? rest[0].y+rest[0].size/2 : rest[0].x+rest[0].size/2;
    along = pointer == 1 ? (bottom ? rest[0].x : rest[0].y)+2 :
        pointer == 2 ? (bottom ? f->width : f->height)/2 :
        (bottom ? rest[FIX_COUNT-1].x : rest[FIX_COUNT-1].y)+rest[FIX_COUNT-1].size-3;
    assert(dock_edge_layout(&f->s,(DockEdge)f->edge,FIX_COUNT,f->width,f->height,
        bottom ? along : centre, bottom ? centre : along, 1, p));
}

static void fixture_dot_position(const RenderFixture *f, const DockPlacement *icon, int *x, int *y)
{
    int inset = (f->s.base_size-2*DOCK_ICON_INSET+4)/9;
    *x = icon->x+icon->size/2-fixture_dot.width/2;
    *y = icon->y+icon->size/2-fixture_dot.height/2;
    if (inset < 2) inset = 2;
    inset += DOCK_DESKTOP_GAP+f->s.edge_padding;
    if (f->edge == DOCK_EDGE_BOTTOM) *y = f->height-1-inset-fixture_dot.height/2;
    else if (f->edge == DOCK_EDGE_RIGHT) *x = f->width-1-inset-fixture_dot.width/2;
    else *x = inset-fixture_dot.width/2;
}

static void fixture_icons(const RenderFixture *f, const DockCache **icons)
{
    int i;
    for (i = 0; i < FIX_COUNT; ++i) icons[i] = &f->cache[i%3];
}

static uint32_t fixture_checksum(const RenderFixture *f)
{
    uint32_t h = fnv1a(2166136261u,f->canvas.pixels,(size_t)f->width*f->height*4);
    return fnv1a(h,f->coverage,(size_t)f->width*f->height);
}

/* Running dots on slots 0 and 3, drawn after the frame the old way. */
static const int fixture_running[FIX_COUNT] = {1,0,0,1,0};

/* Pixel-exact checksums of canvas plus coverage, generated with the
   full-frame renderer before damage rendering existed. A change here is a
   rendering regression, never a table update. The hover scenarios also
   depend on dock_layout: they were regenerated once when the zoom wave
   changed to the macOS profile, with the rest scenarios unchanged. */
static const uint32_t render_goldens[FIX_SCENARIOS] = {
    0x1d1a8288, 0xe3a6c983, 0x84b7daf1, 0xbaeb2950, 0x8ccc2510, 0xcca2f0fb,
    0x667dc33b, 0x8bfbc137, 0x0494c9de, 0xe5d3f233, 0x0c206437, 0xb20c6a56,
    0x91e50f12, 0xae3aeeeb, 0x65100913, 0x7096de39, 0x52a4e958, 0xa8cfd5a4,
    0xb9e8109e, 0xbca8bb10, 0x8943428c, 0x376ba5e0, 0xe634ae08, 0x4e9ec7b3,
    0x5081cad2, 0x04727c54, 0x32bbbd4c, 0xe18bec82, 0x2c88ea52, 0x519c8610,
    0xe7b5c010, 0x528720c1, 0x1d1a8288, 0xe3a6c983, 0x84b7daf1, 0xbaeb2950,
    0x8ccc2510, 0xcca2f0fb, 0x667dc33b, 0x8bfbc137, 0x0494c9de, 0xe5d3f233,
    0x0c206437, 0xb20c6a56, 0x91e50f12, 0xae3aeeeb, 0x65100913, 0x7096de39,
    0x52a4e958, 0xa8cfd5a4, 0xb9e8109e, 0xbca8bb10, 0x8943428c, 0x376ba5e0,
    0xe634ae08, 0x4e9ec7b3, 0x5081cad2, 0x04727c54, 0x32bbbd4c, 0xe18bec82,
    0x2c88ea52, 0x519c8610, 0xe7b5c010, 0x528720c1, 0xf6243960, 0x4ffb78f7,
    0x1faf51cf, 0x4dbc7131, 0x65808cc1, 0xc647c92b, 0x50b1b56d, 0x09630a43,
    0x5bc4096a, 0x92fe1fa2, 0xbf0af790, 0x51893a64, 0x40bc840a, 0xdf4d7791,
    0xad31ce01, 0x671a8def, 0x6da0b0ef, 0xf072c23e, 0xff4e1a8e, 0xdfcde28a,
    0x2af548bc, 0x923eca85, 0x05b6449b, 0xd0ea6f18, 0x2f103d81, 0xb0cdf107,
    0x2a07ef21, 0x0e9f515f, 0x15229b6f, 0xe2d7b4af, 0x130f4b67, 0x7394b3f0,
    0xf6243960, 0x4ffb78f7, 0x1faf51cf, 0x4dbc7131, 0x65808cc1, 0xc647c92b,
    0x50b1b56d, 0x09630a43, 0x5bc4096a, 0x92fe1fa2, 0xbf0af790, 0x51893a64,
    0x40bc840a, 0xdf4d7791, 0xad31ce01, 0x671a8def, 0x6da0b0ef, 0xf072c23e,
    0xff4e1a8e, 0xdfcde28a, 0x2af548bc, 0x923eca85, 0x05b6449b, 0xd0ea6f18,
    0x2f103d81, 0xb0cdf107, 0x2a07ef21, 0x0e9f515f, 0x15229b6f, 0xe2d7b4af,
    0x130f4b67, 0x7394b3f0, 0xa09a8da0, 0xd47b6ced, 0xc5ce8c5c, 0x8ed46511,
    0x855f4a7c, 0x6b019cf9, 0xc41e093a, 0x3d76294c, 0xe8f48812, 0x8a099f3d,
    0xa6cdcc9e, 0x1627b0fb, 0xa543c7ee, 0x6474a7c9, 0x5f7e7002, 0x948e51e6,
    0xf3060da0, 0xbe504041, 0x44276c8b, 0x41b04c11, 0xcc3aca7c, 0x83b7b7ad,
    0xf343fed1, 0x64f00b8c, 0x3b600812, 0xa177b211, 0x40e4e1f5, 0xc90397fb,
    0xec1f47ee, 0xac77e5fd, 0xbb8db2b9, 0xc2708b26, 0xa09a8da0, 0xd47b6ced,
    0xc5ce8c5c, 0x8ed46511, 0x855f4a7c, 0x6b019cf9, 0xc41e093a, 0x3d76294c,
    0xe8f48812, 0x8a099f3d, 0xa6cdcc9e, 0x1627b0fb, 0xa543c7ee, 0x6474a7c9,
    0x5f7e7002, 0x948e51e6, 0xf3060da0, 0xbe504041, 0x44276c8b, 0x41b04c11,
    0xcc3aca7c, 0x83b7b7ad, 0xf343fed1, 0x64f00b8c, 0x3b600812, 0xa177b211,
    0x40e4e1f5, 0xc90397fb, 0xec1f47ee, 0xac77e5fd, 0xbb8db2b9, 0xc2708b26,
    0x34f1533f, 0xf9000c98, 0xed44f88f, 0x67a56abd, 0xe5c27c6c, 0x3e8f1cf2,
    0x2d279810, 0x857d5453, 0xc09e7def, 0x25f78b99, 0xed234cfc, 0xa6e09849,
    0x671b8c23, 0x97bcb0eb, 0x3a7c9f98, 0x36ba5969, 0x08498ab7, 0x21ff5f4c,
    0xd38bba85, 0x83844abd, 0x45b411b4, 0xe81275b6, 0x037e0d44, 0x2b7365df,
    0xcf85cf27, 0xfd06c7bd, 0x02cd73a2, 0xc2bf7849, 0x67726a4b, 0x92b6e1bf,
    0x1406eb9c, 0x30bebdf5, 0x34f1533f, 0xf9000c98, 0xed44f88f, 0x67a56abd,
    0xe5c27c6c, 0x3e8f1cf2, 0x2d279810, 0x857d5453, 0xc09e7def, 0x25f78b99,
    0xed234cfc, 0xa6e09849, 0x671b8c23, 0x97bcb0eb, 0x3a7c9f98, 0x36ba5969,
    0x08498ab7, 0x21ff5f4c, 0xd38bba85, 0x83844abd, 0x45b411b4, 0xe81275b6,
    0x037e0d44, 0x2b7365df, 0xcf85cf27, 0xfd06c7bd, 0x02cd73a2, 0xc2bf7849,
    0x67726a4b, 0x92b6e1bf, 0x1406eb9c, 0x30bebdf5, 0xa251c330, 0x05326d1c,
    0x760bb050, 0x99ffcaf8, 0x4171878c, 0xf473a078, 0xd09ecdf2, 0x88121491,
    0xac482ba2, 0xf414c58c, 0x5e274d22, 0xd149bf9a, 0x3882933e, 0xef564ee8,
    0xb6fce41a, 0xcae61283, 0x73bff1b0, 0x71e10b6b, 0x65453aef, 0xc47b59b8,
    0xd828740c, 0x0e42d207, 0xe2804fb9, 0xaca25b91, 0x8cb82222, 0x354f9e9b,
    0xac2a2ee9, 0xac3d4a5a, 0x647c93be, 0x13ae2337, 0x00be3001, 0xef765983,
    0xa251c330, 0x05326d1c, 0x760bb050, 0x99ffcaf8, 0x4171878c, 0xf473a078,
    0xd09ecdf2, 0x88121491, 0xac482ba2, 0xf414c58c, 0x5e274d22, 0xd149bf9a,
    0x3882933e, 0xef564ee8, 0xb6fce41a, 0xcae61283, 0x73bff1b0, 0x71e10b6b,
    0x65453aef, 0xc47b59b8, 0xd828740c, 0x0e42d207, 0xe2804fb9, 0xaca25b91,
    0x8cb82222, 0x354f9e9b, 0xac2a2ee9, 0xac3d4a5a, 0x647c93be, 0x13ae2337,
    0x00be3001, 0xef765983, 0xa46d6f02, 0x1dcdf89c, 0x1eb00470, 0xa33c58ad,
    0xc755d1c0, 0x9ca18a67, 0x55356cf7, 0x4208d6ca, 0x442acffe, 0x7d616f32,
    0x1586f67b, 0xc47b94fc, 0xb5f122a2, 0x0f3a9f49, 0xf0615517, 0x853cf432,
    0x0785f749, 0xdfc6bfe0, 0xb971d18f, 0x8c2e2049, 0xd0a2b297, 0x2dfdd6db,
    0x71145bbf, 0x849a1508, 0x6c542211, 0xc4b62c52, 0x5d3bed00, 0xd11b3af0,
    0x55211ad1, 0xe4758c69, 0xe26d445f, 0x13277ed4, 0xa46d6f02, 0x1dcdf89c,
    0x1eb00470, 0xa33c58ad, 0xc755d1c0, 0x9ca18a67, 0x55356cf7, 0x4208d6ca,
    0x442acffe, 0x7d616f32, 0x1586f67b, 0xc47b94fc, 0xb5f122a2, 0x0f3a9f49,
    0xf0615517, 0x853cf432, 0x0785f749, 0xdfc6bfe0, 0xb971d18f, 0x8c2e2049,
    0xd0a2b297, 0x2dfdd6db, 0x71145bbf, 0x849a1508, 0x6c542211, 0xc4b62c52,
    0x5d3bed00, 0xd11b3af0, 0x55211ad1, 0xe4758c69, 0xe26d445f, 0x13277ed4
};

static int fixture_scenario(int edge, int captured, int glass, int pressed, int center,
                            int end_gap, int pointer)
{
    return ((((((edge*2+captured)*2+glass)*2+pressed)*2+center)*2+end_gap)*4)+pointer;
}

/* as_overlays 0: dots blended after the frame (the generated table);
   1: the same dots passed to the renderer as overlays. */
static void test_render_goldens(int as_overlays)
{
    RenderFixture f;
    uint32_t sums[FIX_SCENARIOS];
    int edge, captured, glass, pressed, center, end_gap, pointer, i, print = getenv("DOCK_PRINT_GOLDENS") != NULL;
    DockPanelShape *shape = calloc(1,sizeof(*shape));
    assert(shape);
    memset(&f,0,sizeof(f));
    for (end_gap = 0; end_gap < 2; ++end_gap) {
        f.s = dock_settings_default();
        f.s.levels = 9; f.s.max_size = 64;
        f.s.end_gap = end_gap ? 6 : 0;
        fixture_sources(&f);
        for (edge = DOCK_EDGE_BOTTOM; edge <= DOCK_EDGE_RIGHT; ++edge) {
            fixture_open(&f,edge);
            for (captured = 0; captured < 2; ++captured)
            for (glass = 0; glass < 2; ++glass)
            for (pressed = 0; pressed < 2; ++pressed)
            for (center = 0; center < 2; ++center)
            for (pointer = 0; pointer < 4; ++pointer) {
                DockPlacement p[FIX_COUNT];
                const DockCache *icons[FIX_COUNT];
                DockOverlay dots[FIX_COUNT];
                int dot_count = 0;
                fixture_layout(&f,pointer,p);
                for (i = 0; i < FIX_COUNT; ++i) if (fixture_running[i]) {
                    dots[dot_count].image = &fixture_dot;
                    fixture_dot_position(&f,&p[i],&dots[dot_count].x,&dots[dot_count].y);
                    ++dot_count;
                }
                fixture_icons(&f,icons);
                memset(f.canvas.pixels,0xa5,(size_t)f.width*f.height*4);
                memset(f.coverage,0x5a,(size_t)f.width*f.height);
                if (glass) {
                    DockRenderer r;
                    memset(&r,0,sizeof(r));
                    r.pixels = dock_backend_portable(); r.coverage = f.coverage;
                    r.pressed_item = pressed ? 2 : 0; r.center_panel = center; r.edge = (DockEdge)edge;
                    if (as_overlays) { r.overlays = dots; r.overlay_count = dot_count; }
                    dock_renderer_draw(&r,&f.canvas,captured ? &f.backdrop : NULL,icons,p,FIX_COUNT,&f.s);
                    dock_renderer_release(&r);
                } else if (as_overlays) {
                    DockRect full = {0,0,f.width,f.height};
                    dock_render_oriented_clip(&f.canvas,captured ? &f.backdrop : NULL,icons,p,FIX_COUNT,
                        &f.s,NULL,shape,f.coverage,pressed ? 2 : 0,edge,center,NULL,dots,dot_count,&full);
                } else dock_render_oriented_mask(&f.canvas,captured ? &f.backdrop : NULL,icons,p,FIX_COUNT,
                    &f.s,NULL,shape,f.coverage,pressed ? 2 : 0,edge,center,NULL);
                if (!as_overlays) for (i = 0; i < dot_count; ++i)
                    dock_blend_masked(&f.canvas,dots[i].image,dots[i].x,dots[i].y,f.coverage);
                sums[fixture_scenario(edge,captured,glass,pressed,center,end_gap,pointer)] = fixture_checksum(&f);
            }
            fixture_close(&f);
        }
        for (i = 0; i < 3; ++i) dock_cache_free(&f.cache[i]);
    }
    free(shape);
    if (print && !as_overlays) {
        for (i = 0; i < FIX_SCENARIOS; ++i) printf("0x%08lx,%s", (unsigned long)sums[i], i%6 == 5 ? "\n" : " ");
        return;
    }
    for (i = 0; i < FIX_SCENARIOS; ++i) {
        if (sums[i] != render_goldens[i]) printf("Golden %d: 0x%08lx, expected 0x%08lx\n",
            i,(unsigned long)sums[i],(unsigned long)render_goldens[i]);
        assert(sums[i] == render_goldens[i]);
    }
}

static int damage_disjoint(const DockDamage *d)
{
    int i, j;
    for (i = 0; i < d->count; ++i) for (j = i+1; j < d->count; ++j) {
        const DockRect *a = &d->rect[i], *b = &d->rect[j];
        if (a->left < b->right && b->left < a->right && a->top < b->bottom && b->top < a->bottom) return 0;
    }
    return 1;
}

static int damage_covers(const DockDamage *d, int x, int y)
{
    int i;
    if (d->full) return 1;
    for (i = 0; i < d->count; ++i)
        if (x >= d->rect[i].left && x < d->rect[i].right && y >= d->rect[i].top && y < d->rect[i].bottom) return 1;
    return 0;
}

static void test_damage_add(void)
{
    enum { W = 64, H = 48 };
    DockDamage d, e;
    static unsigned char added[W*H];
    unsigned seed = 5;
    int i, x, y, round;
    dock_damage_clear(&d);
    dock_damage_add(&d,(DockRect){-5,-5,10,10},8,8);
    assert(d.count == 1 && !d.full && d.rect[0].left == 0 && d.rect[0].top == 0 &&
        d.rect[0].right == 8 && d.rect[0].bottom == 8);
    dock_damage_clear(&d);
    dock_damage_add(&d,(DockRect){3,3,3,9},W,H);
    dock_damage_add(&d,(DockRect){5,9,9,9},W,H);
    dock_damage_add(&d,(DockRect){W,0,W+5,4},W,H);
    dock_damage_add(&d,(DockRect){0,-9,4,0},W,H);
    dock_damage_add(&d,(DockRect){9,9,2,2},W,H);
    assert(!d.count && !d.full);
    /* Edge-adjacent rects stay separate; overlap merges. */
    dock_damage_add(&d,(DockRect){0,0,2,2},W,H);
    dock_damage_add(&d,(DockRect){2,0,4,2},W,H);
    assert(d.count == 2);
    dock_damage_add(&d,(DockRect){1,1,3,3},W,H);
    assert(d.count == 1 && d.rect[0].left == 0 && d.rect[0].right == 4 && d.rect[0].bottom == 3);
    /* Transitive: C meets A only, but their box meets B. */
    dock_damage_clear(&d);
    dock_damage_add(&d,(DockRect){0,0,6,2},W,H);
    dock_damage_add(&d,(DockRect){5,4,7,6},W,H);
    dock_damage_add(&d,(DockRect){0,1,2,5},W,H);
    assert(d.count == 1 && d.rect[0].left == 0 && d.rect[0].top == 0 &&
        d.rect[0].right == 7 && d.rect[0].bottom == 6);
    /* Overflow sets full; full absorbs everything. */
    dock_damage_clear(&d);
    for (i = 0; i < DOCK_DAMAGE_MAX; ++i) dock_damage_add(&d,(DockRect){(i%20)*3,(i/20)*3,(i%20)*3+1,(i/20)*3+1},W,H);
    assert(d.count == DOCK_DAMAGE_MAX && !d.full && damage_disjoint(&d));
    dock_damage_add(&d,(DockRect){62,46,63,47},W,H);
    assert(d.full && !d.count);
    dock_damage_add(&d,(DockRect){0,0,1,1},W,H);
    assert(d.full && !d.count);
    dock_damage_clear(&e);
    dock_damage_add(&e,(DockRect){0,0,1,1},W,H);
    dock_damage_merge(&e,&d,W,H);
    assert(e.full);
    /* Random adds: never intersecting, covering every added pixel, merge
       equals adding each rect. */
    for (round = 0; round < 300; ++round) {
        int adds = 1+round%40;
        DockDamage m;
        dock_damage_clear(&d); dock_damage_clear(&m); dock_damage_clear(&e);
        memset(added,0,sizeof(added));
        for (i = 0; i < adds; ++i) {
            DockRect r;
            seed = seed*1103515245u+12345u; r.left = (int)(seed>>8)%(W+8)-4;
            seed = seed*1103515245u+12345u; r.top = (int)(seed>>8)%(H+8)-4;
            seed = seed*1103515245u+12345u; r.right = r.left+(int)(seed>>8)%9;
            seed = seed*1103515245u+12345u; r.bottom = r.top+(int)(seed>>8)%9;
            dock_damage_add(&d,r,W,H);
            dock_damage_add(i&1 ? &m : &e,r,W,H);
            for (y = r.top < 0 ? 0 : r.top; y < r.bottom && y < H; ++y)
                for (x = r.left < 0 ? 0 : r.left; x < r.right && x < W; ++x) added[y*W+x] = 1;
            assert(damage_disjoint(&d));
        }
        dock_damage_merge(&e,&m,W,H);
        assert(damage_disjoint(&e));
        for (y = 0; y < H; ++y) for (x = 0; x < W; ++x)
            if (added[y*W+x]) assert(damage_covers(&d,x,y) && damage_covers(&e,x,y));
        for (i = 0; i < d.count; ++i) {
            assert(d.rect[i].left >= 0 && d.rect[i].top >= 0 && d.rect[i].right <= W && d.rect[i].bottom <= H);
            assert(d.rect[i].left < d.rect[i].right && d.rect[i].top < d.rect[i].bottom);
        }
    }
}

/* The divider moves with the last two slots. With no artwork there and the
   plate ends far away, only the divider rule repaints its old position. */
static void test_damage_divider(void)
{
    enum { LONG = 600, CROSS = 92 };
    static const int frames[][3] = {{20,100,143},{20,300,343},{20,300,343},{20,120,170},{20,121,171}};
    DockSettings s = dock_settings_default();
    DockRenderer r;
    DockPanelShape *shape = calloc(1,sizeof(*shape));
    const DockCache *icons[3] = {NULL,NULL,NULL};
    unsigned char *pixels = malloc(LONG*CROSS*4), *ref = malloc(LONG*CROSS*4);
    unsigned char *cov = malloc(LONG*CROSS), *ref_cov = malloc(LONG*CROSS);
    int edge, k;
    assert(shape && pixels && ref && cov && ref_cov);
    s.end_gap = 6;
    for (edge = DOCK_EDGE_BOTTOM; edge <= DOCK_EDGE_RIGHT; ++edge) {
        int bottom = edge == DOCK_EDGE_BOTTOM;
        DockImage canvas = {bottom ? LONG : CROSS, bottom ? CROSS : LONG, pixels};
        DockImage reference = {canvas.width, canvas.height, ref};
        memset(&r,0,sizeof(r));
        r.pixels = dock_backend_portable(); r.coverage = cov; r.edge = (DockEdge)edge;
        for (k = 0; k < (int)(sizeof(frames)/sizeof(frames[0])); ++k) {
            DockPlacement p[3];
            int i;
            for (i = 0; i < 3; ++i) {
                int cross = bottom ? CROSS-3-40 : edge == DOCK_EDGE_LEFT ? 3+4 : CROSS-3-40;
                p[i] = bottom ? (DockPlacement){frames[k][i],cross,32,0} : (DockPlacement){cross,frames[k][i],32,0};
            }
            dock_renderer_draw(&r,&canvas,NULL,icons,p,3,&s);
            if (k) assert(!r.damage.full);
            dock_render_oriented_mask(&reference,NULL,icons,p,3,&s,NULL,shape,ref_cov,0,edge,0,NULL);
            assert(!memcmp(pixels,ref,LONG*CROSS*4) && !memcmp(cov,ref_cov,LONG*CROSS));
        }
        dock_renderer_release(&r);
    }
    free(shape); free(pixels); free(ref); free(cov); free(ref_cov);
}

static unsigned fuzz_next(unsigned *seed, unsigned range)
{
    *seed = *seed*1103515245u+12345u;
    return (*seed>>8)%range;
}

/* A constant keyed tail, like the PiP transport padding, plus a band that
   an owned popup or drag reveal can switch on. */
typedef struct { int visible, band, band_left, band_right; } TestMask;

static void test_mask(void *context, int y, int left, int right, unsigned char *span)
{
    const TestMask *m = context;
    int x;
    for (x = left; x < right; ++x)
        if (x >= m->visible || (m->band && y >= 3 && x >= m->band_left && x < m->band_right)) span[x-left] = 0;
}

/* Q4: two buffers with per-buffer pending damage, random skipped and failed
   presents and a switching mask. After every conversion the back buffer
   equals a full conversion of the current frame; coverage is never written. */
static void test_conversion_double_buffer(void)
{
    enum { W = 70, H = 21, STRIDE = W*2+18, FRAMES = 6000 };
    static unsigned char frame_pixels[W*H*4], coverage[W*H], saved[W*H], scratch[W];
    static unsigned char buffer[2][STRIDE*H], reference[STRIDE*H];
    DockImage frame = {W,H,frame_pixels};
    DockDamage pending[2], whole, d;
    TestMask mask = {W-6,0,10,30};
    int masked[2] = {0,0}, front = 1, k, i, y, conversions = 0, partial = 0;
    unsigned seed = 77;
    for (i = 0; i < W*H*4; ++i) frame_pixels[i] = (unsigned char)fuzz_next(&seed,256);
    for (i = 0; i < W*H; ++i) coverage[i] = (unsigned char)(fuzz_next(&seed,3) ? 255 : 0);
    memset(buffer,0x3c,sizeof(buffer));
    dock_damage_set_full(&pending[0]); dock_damage_set_full(&pending[1]); dock_damage_set_full(&whole);
    for (k = 0; k < FRAMES; ++k) {
        int rects = (int)fuzz_next(&seed,4), back = front^1, x;
        dock_damage_clear(&d);
        for (i = 0; i < rects; ++i) {
            DockRect r;
            r.left = (int)fuzz_next(&seed,W); r.top = (int)fuzz_next(&seed,H);
            r.right = r.left+1+(int)fuzz_next(&seed,12); r.bottom = r.top+1+(int)fuzz_next(&seed,5);
            if (r.right > W) r.right = W;
            if (r.bottom > H) r.bottom = H;
            for (y = r.top; y < r.bottom; ++y) for (x = r.left; x < r.right; ++x) {
                unsigned char *q = frame_pixels+(y*W+x)*4;
                q[0] = (unsigned char)fuzz_next(&seed,256); q[1] = (unsigned char)fuzz_next(&seed,256);
                q[2] = (unsigned char)fuzz_next(&seed,256);
                if (!fuzz_next(&seed,8)) { q[0] = 255; q[1] = 0; q[2] = 255; } /* keyed magenta */
                coverage[y*W+x] = (unsigned char)(fuzz_next(&seed,3) ? 255 : fuzz_next(&seed,2) ? 0 : 7);
            }
            dock_damage_add(&d,r,W,H);
        }
        if (!fuzz_next(&seed,40)) mask.band = !mask.band;
        dock_damage_merge(&pending[0],&d,W,H);
        dock_damage_merge(&pending[1],&d,W,H);
        if (!fuzz_next(&seed,6)) continue; /* skipped present: PiP suspended */
        if (mask.band || masked[back]) dock_damage_set_full(&pending[back]);
        if (!pending[back].full) ++partial;
        memcpy(saved,coverage,sizeof(coverage));
        dock_convert565_damage(buffer[back],STRIDE,&frame,coverage,&pending[back],scratch,
            dock_rgba_to_rgb565_c,dock_rgb565_key_c,test_mask,&mask);
        dock_damage_clear(&pending[back]);
        masked[back] = mask.band;
        assert(!memcmp(saved,coverage,sizeof(coverage)));
        dock_convert565_damage(reference,STRIDE,&frame,coverage,&whole,scratch,
            dock_rgba_to_rgb565_c,dock_rgb565_key_c,test_mask,&mask);
        for (y = 0; y < H; ++y) assert(!memcmp(buffer[back]+y*STRIDE,reference+y*STRIDE,W*2));
        ++conversions;
        if (fuzz_next(&seed,8)) front = back; /* else a failed present keeps the same back buffer */
    }
    assert(conversions > FRAMES/2 && partial > conversions/2);
    /* Spans widen to 8-pixel multiples and clamp to the width; padding
       past each row is never written. */
    for (y = 0; y < H; ++y) for (i = W*2; i < STRIDE; ++i) assert(buffer[0][y*STRIDE+i] == 0x3c && buffer[1][y*STRIDE+i] == 0x3c);
    dock_damage_clear(&d);
    dock_damage_add(&d,(DockRect){9,2,10,3},W,H);
    memset(reference,0x3c,sizeof(reference));
    dock_convert565_damage(reference,STRIDE,&frame,NULL,&d,scratch,dock_rgba_to_rgb565_c,dock_rgb565_key_c,NULL,NULL);
    for (i = 0; i < STRIDE*H; ++i) {
        int row = i/STRIDE, x = i%STRIDE/2;
        assert((reference[i] != 0x3c) <= (row == 2 && x >= 8 && x < 16));
    }
}

/* Damage fuzz: an incremental renderer (glass) and a damage-only redraw
   without glass both equal a fresh full render plus overlays, byte for byte,
   on every frame. */
static void test_damage_fuzz(void)
{
    enum { FRAMES_PER_CONFIG = 3400, MAX_COUNT = 6 };
    RenderFixture f;
    DockPanelShape *shape_ref = calloc(1,sizeof(*shape_ref)), *shape_b = calloc(1,sizeof(*shape_b));
    unsigned seed = 2024;
    int edge, captured, total = 0, full_frames = 0;
    uint64_t damaged = 0, pixels_total = 0;
    assert(shape_ref && shape_b);
    memset(&f,0,sizeof(f));
    f.s = dock_settings_default();
    f.s.levels = 9; f.s.max_size = 64;
    fixture_sources(&f);
    for (edge = DOCK_EDGE_BOTTOM; edge <= DOCK_EDGE_RIGHT; ++edge)
    for (captured = 0; captured < 2; ++captured) {
        DockRenderer r;
        DockAnimation animation;
        DockPlacement current[MAX_COUNT], target[MAX_COUNT];
        const DockCache *icons[MAX_COUNT];
        DockOverlay overlays[MAX_COUNT+1];
        DockImage ref, b;
        unsigned char *ref_cov, *b_cov;
        int running[MAX_COUNT] = {1,0,1,0,0,1}, hidden = -1, pressed = 0, center = 0, active = 0;
        int count = FIX_COUNT, along, cross, drag = 0, drag_x = 0, drag_y = 0, drag_level = 0, frame, i;
        int bottom = edge == DOCK_EDGE_BOTTOM, size;
        f.s.end_gap = 0;
        fixture_open(&f,edge);
        size = f.width*f.height;
        ref = (DockImage){f.width,f.height,malloc((size_t)size*4)};
        b = (DockImage){f.width,f.height,malloc((size_t)size*4)};
        ref_cov = malloc((size_t)size); b_cov = malloc((size_t)size);
        assert(ref.pixels && b.pixels && ref_cov && b_cov);
        memset(&r,0,sizeof(r));
        r.pixels = dock_backend_portable(); r.coverage = f.coverage; r.edge = (DockEdge)edge;
        for (i = 0; i < MAX_COUNT; ++i) icons[i] = &f.cache[i%3];
        assert(dock_edge_layout(&f.s,(DockEdge)edge,count,f.width,f.height,0,0,0,current));
        dock_animation_reset(&animation);
        cross = bottom ? current[0].y+current[0].size/2 : current[0].x+current[0].size/2;
        along = (bottom ? f.width : f.height)/2;
        for (frame = 0; frame < FRAMES_PER_CONFIG; ++frame) {
            unsigned event = fuzz_next(&seed,100);
            int overlay_count = 0, long_extent = bottom ? f.width : f.height;
            if (event < 55) along += (int)fuzz_next(&seed,41)*(fuzz_next(&seed,2) ? 1 : -1);
            else if (event < 60) along = (int)fuzz_next(&seed,(unsigned)long_extent);
            else if (event < 66) active = !active;
            else if (event < 70) pressed = (int)fuzz_next(&seed,(unsigned)count+1);
            else if (event < 73) center = !center;
            else if (event < 76) {
                count = 1+(int)fuzz_next(&seed,MAX_COUNT);
                if (pressed > count) pressed = 0;
                assert(dock_edge_layout(&f.s,(DockEdge)edge,count,f.width,f.height,0,0,0,current));
                dock_animation_reset(&animation);
            } else if (event < 81) running[fuzz_next(&seed,MAX_COUNT)] ^= 1;
            else if (event < 88) {
                unsigned what = fuzz_next(&seed,4);
                if (!what) drag = !drag;
                drag_x = (int)fuzz_next(&seed,(unsigned)f.width+40)-20;
                drag_y = (int)fuzz_next(&seed,(unsigned)f.height+40)-20;
                if (what == 1) drag_level = (int)fuzz_next(&seed,(unsigned)f.cache[0].levels);
            } else if (event < 90) {
                /* A backdrop change: new pixels in a band, then the call. */
                int y0 = (int)fuzz_next(&seed,(unsigned)f.height), n = 1+(int)fuzz_next(&seed,8);
                for (i = y0*f.width*4; i < (y0+n)*f.width*4 && i < size*4; ++i)
                    if ((i&3) != 3) f.backdrop.pixels[i] = (unsigned char)(f.backdrop.pixels[i]*7+frame);
                dock_renderer_backdrop_changed(&r);
            } else if (event < 94) {
                int a = (int)fuzz_next(&seed,MAX_COUNT), c = (int)fuzz_next(&seed,MAX_COUNT);
                const DockCache *t = icons[a]; icons[a] = icons[c]; icons[c] = t;
                if (fuzz_next(&seed,2)) dock_renderer_invalidate(&r);
            } else if (event < 96) hidden = hidden >= 0 ? -1 : (int)fuzz_next(&seed,MAX_COUNT);
            else if (event < 97) f.s.end_gap = f.s.end_gap ? 0 : 6;
            /* else: an unchanged frame */
            {
                int mx = bottom ? along : cross, my = bottom ? cross : along;
                const DockCache *row[MAX_COUNT];
                assert(dock_edge_layout(&f.s,(DockEdge)edge,count,f.width,f.height,mx,my,active,target));
                /* Snap the row at once, as animation_finish and a drop do. */
                if (event >= 97 && event < 99) memcpy(current,target,sizeof(*current)*(size_t)count);
                else dock_edge_animate(&f.s,(DockEdge)edge,&animation,current,target,count);
                for (i = 0; i < count; ++i) row[i] = i == hidden ? NULL : icons[i];
                for (i = 0; i < count; ++i) if (row[i] && running[i]) {
                    overlays[overlay_count].image = &fixture_dot;
                    fixture_dot_position(&f,&current[i],&overlays[overlay_count].x,&overlays[overlay_count].y);
                    ++overlay_count;
                }
                if (drag) overlays[overlay_count++] = (DockOverlay){&f.cache[1].image[drag_level],drag_x,drag_y};
                r.pressed_item = pressed; r.center_panel = center;
                r.overlays = overlays; r.overlay_count = overlay_count;
                dock_renderer_draw(&r,&f.canvas,captured ? &f.backdrop : NULL,row,current,count,&f.s);
                if (r.damage.full) { ++full_frames; damaged += (uint64_t)size; }
                else for (i = 0; i < r.damage.count; ++i) {
                    const DockRect *d = &r.damage.rect[i];
                    damaged += (uint64_t)(d->right-d->left)*(uint64_t)(d->bottom-d->top);
                }
                assert(r.damage.full || damage_disjoint(&r.damage));
                pixels_total += (uint64_t)size;
                {
                    DockRect whole = {0,0,f.width,f.height};
                    int n = r.damage.full ? 1 : r.damage.count;
                    for (i = 0; i < n; ++i)
                        dock_render_oriented_clip(&b,captured ? &f.backdrop : NULL,row,current,count,&f.s,NULL,
                            shape_b,b_cov,pressed,edge,center,NULL,overlays,overlay_count,
                            r.damage.full ? &whole : &r.damage.rect[i]);
                }
                memset(ref.pixels,0xa5,(size_t)size*4); memset(ref_cov,0x5a,(size_t)size);
                dock_render_oriented_mask(&ref,captured ? &f.backdrop : NULL,row,current,count,&f.s,NULL,
                    shape_ref,ref_cov,pressed,edge,center,NULL);
                for (i = 0; i < overlay_count; ++i)
                    dock_blend_masked(&ref,overlays[i].image,overlays[i].x,overlays[i].y,ref_cov);
                if (memcmp(ref.pixels,f.canvas.pixels,(size_t)size*4) || memcmp(ref_cov,f.coverage,(size_t)size))
                    printf("Damage fuzz mismatch: edge %d captured %d frame %d event %u\n",edge,captured,frame,event);
                assert(!memcmp(ref.pixels,f.canvas.pixels,(size_t)size*4));
                assert(!memcmp(ref_cov,f.coverage,(size_t)size));
                assert(!memcmp(ref.pixels,b.pixels,(size_t)size*4));
                assert(!memcmp(ref_cov,b_cov,(size_t)size));
            }
            ++total;
        }
        dock_renderer_release(&r);
        free(ref.pixels); free(b.pixels); free(ref_cov); free(b_cov);
        fixture_close(&f);
    }
    for (edge = 0; edge < 3; ++edge) dock_cache_free(&f.cache[edge]);
    free(shape_ref); free(shape_b);
    /* The fuzz must exercise partial damage, not only full frames. */
    assert(total >= 20000 && full_frames < total/4 && damaged*2 < pixels_total);
    printf("Damage fuzz: %d frames, %d full, %lu%% of pixels redrawn.\n",
        total,full_frames,(unsigned long)(damaged*100/pixels_total));
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
    test_render_goldens(0); test_render_goldens(1); test_damage_add(); test_damage_divider(); test_damage_fuzz(); test_conversion_double_buffer();
    printf("All core tests passed: normalization, alpha, layout bounds, clipping, 65536 blends, reorder, renderer.\n");
    printf("Default cache: %lu bytes/icon; 20 icons: %lu bytes.\n",
        (unsigned long)dock_cache_bytes(&defaults),
        20UL * (unsigned long)dock_cache_bytes(&defaults));
    return 0;
}
