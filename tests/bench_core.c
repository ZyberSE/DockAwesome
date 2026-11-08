#include "dock_core.h"
#include "dock_geometry.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(void)
{
    enum { WIDTH=1280, HEIGHT=128, COUNT=16, FRAMES=2000 };
    unsigned char source_pixels[64*64*4];
    DockImage source = {64,64,source_pixels};
    DockImage frame = {WIDTH,HEIGHT,NULL}, back = {WIDTH,HEIGHT,NULL};
    DockCache cache;
    const DockCache *icons[COUNT];
    DockPlacement p[COUNT];
    DockSettings s = dock_settings_default();
    clock_t begin, end;
    volatile unsigned long checksum = 0;
    int i, j;
    for (i=0;i<64*64;++i) {
        source_pixels[i*4]=40; source_pixels[i*4+1]=120; source_pixels[i*4+2]=220;
        source_pixels[i*4+3]=(i%64<4 || i%64>59 || i/64<4 || i/64>59)?0:200;
    }
    begin=clock();
    if (!dock_cache_build(&cache,&source,&s)) return 1;
    end=clock();
    printf("HOST ONLY (not Vampire): cache build %.3f ms/icon, %lu bytes/icon\n",
        1000.0*(double)(end-begin)/CLOCKS_PER_SEC,(unsigned long)cache.bytes);
    frame.pixels=(unsigned char *)malloc(WIDTH*HEIGHT*4);
    back.pixels=(unsigned char *)malloc(WIDTH*HEIGHT*4);
    if (!frame.pixels || !back.pixels) return 1;
    for(i=0;i<WIDTH*HEIGHT;++i) {
        back.pixels[i*4]=80; back.pixels[i*4+1]=100; back.pixels[i*4+2]=130; back.pixels[i*4+3]=255;
    }
    for(i=0;i<COUNT;++i) icons[i]=&cache;
    begin=clock();
    for(j=0;j<FRAMES;++j) {
        if(!dock_layout(&s,COUNT,WIDTH,HEIGHT,j%WIDTH,90,1,p)) return 1;
        dock_render_oriented_mask(&frame,&back,icons,p,COUNT,&s, NULL, NULL, NULL, 0, DOCK_EDGE_BOTTOM, 0, NULL);
        checksum+=frame.pixels[(j%(WIDTH*HEIGHT))*4];
    }
    end=clock();
    printf("HOST ONLY: %d icons, %dx%d, %d frames: %.3f ms/frame, checksum %lu\n",
        COUNT,WIDTH,HEIGHT,FRAMES,1000.0*(double)(end-begin)/CLOCKS_PER_SEC/FRAMES,checksum);
    printf("Window RGBA buffers: %lu bytes; distinct 16-icon cache: %lu bytes\n",
        2UL*WIDTH*HEIGHT*4,COUNT*(unsigned long)cache.bytes);
    dock_cache_free(&cache); free(frame.pixels); free(back.pixels);
    return 0;
}
