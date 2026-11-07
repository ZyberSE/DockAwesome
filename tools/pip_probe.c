/* Explicit bounded PiP2 hardware test. Own screen; no Workbench framebuffer
   writes. The documented write-only slot must be reserved before invocation. */
#include "amiga_pip.h"
#include <exec/types.h>
#include <devices/timer.h>
#include <graphics/gfxbase.h>
#include <graphics/view.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>
#include <proto/timer.h>
#include <cybergraphx/cybergraphics.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
struct Device *TimerBase;
long __stack=32768;
static ULONG ticks(void) { struct EClockVal c; ReadEClock(&c); return c.ev_lo; }
static int color_matches(const unsigned char *rgb, ULONG color)
{
    /* Allow only the precision loss of the source RGB565 screen. */
    int r=(color>>16)&255,g=(color>>8)&255,b=color&255;
    return abs((int)rgb[0]-r)<=7 && abs((int)rgb[1]-g)<=3 && abs((int)rgb[2]-b)<=7;
}

int main(int argc,char **argv)
{
    enum { W=352,H=100,FRAMES=200 };
    struct Screen *pub=NULL,*screen=NULL;
    struct Window *window=NULL;
    struct MsgPort *port=NULL;
    struct timerequest *io=NULL;
    AmigaPip *pip=NULL;
    unsigned char *rgb=NULL;
    int width=0,height=0,x=0,y=0,i,px,py,n=0,status=20,timer_open=0,source_changes=0;
    ULONG mode=0,hz=0,start=0,work=0,begin;
    unsigned char before[3],after[3],previous[3]={0};
    char error[192];
    if (argc!=2 || strcmp(argv[1],"--reserved-pip2")) {
        puts("Usage: DockAwesomePipProbe080 --reserved-pip2\n"
             "Only run with exclusive PiP2 reservation. Registers are write-only;\n"
             "another direct PiP2 user cannot be detected or restored.\n"
             "An own screen displays2sec RGB bars then2sec live preblended panel."); return 20;
    }
    IntuitionBase=(struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library",39);
    GfxBase=(struct GfxBase *)OpenLibrary((STRPTR)"graphics.library",39);
    CyberGfxBase=OpenLibrary((STRPTR)"cybergraphics.library",40);
    if (!IntuitionBase || !GfxBase || !CyberGfxBase) { puts("FAIL: OS/RTG libraries"); goto done; }
    pub=LockPubScreen(NULL);
    if (!pub) { puts("FAIL: public screen lock"); goto done; }
    width=pub->Width; height=pub->Height; mode=GetVPModeID(&pub->ViewPort);
    if (GetBitMapAttr(pub->RastPort.BitMap,BMA_DEPTH)!=16 || width<640 || height<400) {
        puts("FAIL: needs 16-bit RTG"); goto done;
    }
    UnlockPubScreen(NULL,pub); pub=NULL;
    port=CreateMsgPort(); if (port) io=(struct timerequest *)CreateIORequest(port,sizeof(*io));
    if (!io || OpenDevice((STRPTR)TIMERNAME,UNIT_MICROHZ,(struct IORequest *)io,0)) { puts("FAIL: timer"); goto done; }
    timer_open=1; TimerBase=io->tr_node.io_Device;
    { struct EClockVal c; hz=ReadEClock(&c); }
    if (!hz) goto done;
    screen=OpenScreenTags(NULL,SA_DisplayID,mode,SA_Width,width,SA_Height,height,SA_Depth,16,
        SA_Type,CUSTOMSCREEN,SA_Behind,TRUE,SA_Quiet,TRUE,
        SA_Title,(ULONG)"PiP2 probe: RGB bars then live translucent panel",TAG_DONE);
    if (!screen) { puts("FAIL: owned screen"); goto done; }
    FillPixelArray(&screen->RastPort,0,0,width,height,0xff203040UL);
    x=(width-W)/2;y=height-140;
    window=OpenWindowTags(NULL,WA_CustomScreen,(ULONG)screen,WA_Left,width/2-200,
        WA_Top,height-230,WA_Width,400,WA_Height,210,WA_DragBar,TRUE,
        WA_SimpleRefresh,TRUE,WA_IDCMP,IDCMP_REFRESHWINDOW,
        WA_Title,(ULONG)"Source window behind PiP2",TAG_DONE);
    if (!window) { puts("FAIL: owned window"); goto done; }
    pip=amiga_pip_open(screen,x,y,W,H,0,error,sizeof(error));
    if (!pip) { printf("FAIL: %s\n",error); goto done; }
    rgb=malloc(W*H*3);
    if (!rgb) { puts("FAIL: RGB scratch"); goto done; }
    printf("Core%u, reservedPiP2, logical(%d,%d)%dx%d, stride%d bytes\n",
        amiga_pip_core_revision(pip),x,y,W,H,amiga_pip_stride(pip)); fflush(stdout);
    ScreenToFront(screen);
    /* Intuition queues ScreenToFront: never present before it takes effect. */
    for(i=0;i<100 && IntuitionBase->FirstScreen!=screen;++i) {
        if(SetSignal(0,SIGBREAKF_CTRL_C)&SIGBREAKF_CTRL_C) { puts("STOP: Ctrl-C"); goto done; }
        Delay(1);
    }
    if(IntuitionBase->FirstScreen!=screen) {
        puts("FAIL: private probe screen did not become front within100 ticks; PiP2 not enabled");
        goto done;
    }
    {
        ULONG before_field=*(volatile ULONG *)&GfxBase->VBCounter,after_field;
        WaitTOF();after_field=*(volatile ULONG *)&GfxBase->VBCounter;
        if(after_field==before_field) {
            puts("FAIL: VBlank counter did not advance across WaitTOF; PiP2 not enabled");
            goto done;
        }
        printf("VBlank retirement counter verified: %lu -> %lu\n",
            (unsigned long)before_field,(unsigned long)after_field);fflush(stdout);
    }
    start=ticks();
    for(i=0;i<FRAMES;++i) {
        struct IntuiMessage *m;
        unsigned char *dst=amiga_pip_back_buffer(pip);
        ULONG color=0xff000000UL|(((ULONG)i*7&255)<<16)|0x00004000|((ULONG)i*11&255);
        if ((ULONG)(ticks()-start)>hz*10) { puts("FAIL:10sec budget"); goto done; }
        if(SetSignal(0,SIGBREAKF_CTRL_C)&SIGBREAKF_CTRL_C) { puts("STOP: Ctrl-C"); goto done; }
        while((m=(struct IntuiMessage *)GetMsg(window->UserPort))) {
            ULONG cls=m->Class; ReplyMsg((struct Message *)m);
            if(cls==IDCMP_REFRESHWINDOW) { BeginRefresh(window);EndRefresh(window,TRUE); }
        }
        FillPixelArray(window->RPort,window->BorderLeft,window->BorderTop,
            window->Width-window->BorderLeft-window->BorderRight,
            window->Height-window->BorderTop-window->BorderBottom,color);
        WaitBlit();
        begin=ticks();
        if (ReadPixelArray(before,0,0,3,&screen->RastPort,x+W/2,y+H/2,1,1,RECTFMT_RGB) != 1) {
            puts("FAIL: source pixel readback"); goto done;
        }
        if(!color_matches(before,color)) {
            printf("FAIL: frame%d source paint wanted=%08lx got=%u,%u,%u at%d,%d; screenfront=%d\n",
                i,(unsigned long)color,before[0],before[1],before[2],x+W/2,y+H/2,
                IntuitionBase->FirstScreen==screen);
            goto done;
        }
        if(i && memcmp(before,previous,3)) ++source_changes;
        memcpy(previous,before,3);
        if(i<FRAMES/2) {
            for(py=0;py<H;++py) {
                UWORD *row=(UWORD *)(dst+(size_t)py*amiga_pip_stride(pip));
                for(px=0;px<W;++px) row[px]=px<W/3?0xf800:px<W*2/3?0x07e0:0x001f;
            }
        } else {
            if (ReadPixelArray(rgb,0,0,W*3,&screen->RastPort,x,y,W,H,RECTFMT_RGB) != W*H) {
                puts("FAIL: source ROI readback"); goto done;
            }
            for(py=0;py<H;++py) {
                UWORD *row=(UWORD *)(dst+(size_t)py*amiga_pip_stride(pip));
                for(px=0;px<W;++px) {
                    unsigned char *s=rgb+((size_t)py*W+px)*3;
                    unsigned int r=(s[0]+20+1)>>1,g=(s[1]+24+1)>>1,b=(s[2]+32+1)>>1;
                    row[px]=(UWORD)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));
                }
            }
        }
        if(!amiga_pip_present(pip)) {
            printf("FAIL: PiP2 presentation rejected: screenfront=%d depth=%lu screenoffset=%d,%d logical=%d,%d\n",
                IntuitionBase->FirstScreen==screen,
                (unsigned long)GetBitMapAttr(screen->RastPort.BitMap,BMA_DEPTH),
                screen->LeftEdge,screen->TopEdge,x,y);
            goto done;
        }
        if (ReadPixelArray(after,0,0,3,&screen->RastPort,x+W/2,y+H/2,1,1,RECTFMT_RGB) != 1) {
            puts("FAIL: post-present pixel readback"); goto done;
        }
        if(memcmp(before,after,3)) { puts("FAIL: PiP2 contaminated source framebuffer"); goto done; }
        work+=ticks()-begin;++n;Delay(1);
    }
    if(source_changes!=FRAMES-1) { printf("FAIL: expected199 source updates, got%d\n",source_changes);goto done; }
    printf("PASS: %d PiP2 frames submitted; source updates%d; known RGB565 source colors verified; main framebuffer unchanged\n",n,source_changes);
    printf("mean_work_us=%lu (includes deliberately scalar test generation/readback), output not screenshot-readable\n",
        (unsigned long)((uint64_t)work*1000000/hz/n));
    puts("Physical RGB bars/alignment/preblended appearance require visual confirmation; not inferred from write-only registers.");
    status=0;
done:
    /* Stop PiP DMA before releasing either video buffer or owned screen. */
    amiga_pip_close(pip);
    if(window) { struct Message *m;while((m=GetMsg(window->UserPort))) ReplyMsg(m);CloseWindow(window); }
    if(screen) CloseScreen(screen);
    free(rgb);
    if(timer_open) CloseDevice((struct IORequest *)io);
    if(io) DeleteIORequest((struct IORequest *)io);
    if(port) DeleteMsgPort(port);
    if(pub) UnlockPubScreen(NULL,pub);
    if(CyberGfxBase) CloseLibrary(CyberGfxBase);
    if(GfxBase) CloseLibrary((struct Library *)GfxBase);
    if(IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return status;
}
