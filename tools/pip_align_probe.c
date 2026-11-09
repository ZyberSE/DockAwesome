/* Physical PiP2 registration: exact native RGB565 copy, no tint or conversion.
   Only the private screen is drawn. PiP register access stays in amiga_pip.c. */
#include "amiga_pip.h"
#include "amiga_text.h"
#include <exec/types.h>
#include <devices/inputevent.h>
#include <graphics/gfxbase.h>
#include <graphics/rastport.h>
#include <graphics/text.h>
#include <graphics/view.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
long __stack=32768;
static struct TextFont *control_font;

/* Each diagnostic write is separately closed, independent of redirected stdio. */
static void stage(const char *message)
{
    BPTR file=Open((STRPTR)"RAM:DockAwesomePiPAlign.stage",MODE_READWRITE);
    if(file) {
        Seek(file,0,OFFSET_END);
        Write(file,(APTR)message,(LONG)strlen(message));Write(file,(APTR)"\n",1);
        Close(file);
    }
}

static int wait_front(struct Screen *screen)
{
    int i;
    for(i=0;i<100 && IntuitionBase->FirstScreen!=screen;++i) Delay(1);
    return IntuitionBase->FirstScreen==screen;
}

static int make_pattern(struct Screen *screen, unsigned char *roi, int x,int y,int w,int h)
{
    APTR base=NULL,lock;
    ULONG stride=0,format=~0UL,bytes=0;
    int px,py;
    lock=LockBitMapTags(screen->RastPort.BitMap,LBMI_BASEADDRESS,(ULONG)&base,
        LBMI_BYTESPERROW,(ULONG)&stride,LBMI_BYTESPERPIX,(ULONG)&bytes,
        LBMI_PIXFMT,(ULONG)&format,TAG_DONE);
    if(!lock) return 0;
    if(!base || bytes!=2 || format!=PIXFMT_RGB16 || stride<(ULONG)screen->Width*2) {
        UnLockBitMap(lock);return 0;
    }
    for(py=0;py<screen->Height;++py) {
        UWORD *row=(UWORD *)((unsigned char *)base+(size_t)py*stride);
        for(px=0;px<screen->Width;++px) {
            /* Coprime spacings and non-repeating tiles make offsets obvious. */
            unsigned int tile=((unsigned int)(px/37)*73U)^((unsigned int)(py/29)*151U);
            tile^=tile>>3;
            UWORD color=(tile&1)?0x3186:0xbdf7;
            if(px%37==0) color=0xf800;
            if(py%29==0) color=0x07e0;
            if((px+py*3)%113<2) color=0xffff;
            /* Window-like outlines cross the ROI boundary on all sides. */
            if((px==x+73 || px==x+w-89) && py>y-50 && py<y+h+50) color=0xffe0;
            if((py==y+43 || py==y+h-37) && px>x-60 && px<x+w+60) color=0xffe0;
            row[px]=color;
        }
    }
    for(py=0;py<h;++py)
        memcpy(roi+(size_t)py*w*2,(unsigned char *)base+(size_t)(y+py)*stride+x*2,(size_t)w*2);
    UnLockBitMap(lock);return 1;
}

static void instructions(struct Window *window,int dx,int dy,int enabled)
{
    static int first=1;
    char line[112];
    const char *lines[]={
        "Exact RGB565 copy: arrows move PiP by 1 pixel.",
        "Shift + arrow: 8 pixels. Esc / Return: finish. Timeout: 120 sec.",
        "Correct alignment: grid lines stay continuous as PiP blinks."};
    int i;
    if(first) stage("instructions: fill begin");
    FillPixelArray(window->RPort,window->BorderLeft,window->BorderTop,
        window->Width-window->BorderLeft-window->BorderRight,
        window->Height-window->BorderTop-window->BorderBottom,0xff182030UL);
    if(first) stage("instructions: fill complete");
    SetAPen(window->RPort,1);SetDrMd(window->RPort,JAM1);SetFont(window->RPort,control_font);
    if(first) stage("instructions: pens/mode/font complete; text begin");
    for(i=0;i<3;++i) { Move(window->RPort,12,30+i*15);amiga_text_draw(window->RPort,lines[i],strlen(lines[i])); }
    if(first) stage("instructions: fixed text complete");
    snprintf(line,sizeof(line),"PiP correction x=%+d y=%+d | plane %s | range -128..128",dx,dy,enabled?"ON":"OFF");
    Move(window->RPort,12,82);amiga_text_draw(window->RPort,line,strlen(line));
    if(first) stage("instructions: status text complete");
    first=0;
}

static int show_copy(AmigaPip *pip,const unsigned char *roi,int w,int h)
{
    unsigned char *dst=amiga_pip_back_buffer(pip);
    int row;
    for(row=0;row<h;++row)
        memcpy(dst+(size_t)row*amiga_pip_stride(pip),roi+(size_t)row*w*2,(size_t)w*2);
    return amiga_pip_present(pip);
}

int main(int argc,char **argv)
{
    enum { W=352,H=160 };
    struct Screen *pub=NULL,*screen=NULL;
    struct Window *window=NULL;
    AmigaPip *pip=NULL;
    unsigned char *roi=NULL;
    ULONG mode,sec,micro,start_sec,start_micro,elapsed,last_blink=0;
    int x,y,dx=16,dy=-1,enabled=1,finish=0,status=20;
    char error[192];
    { BPTR file=Open((STRPTR)"RAM:DockAwesomePiPAlign.stage",MODE_NEWFILE);if(file) Close(file); }
    stage("start");
    if(argc!=2 || strcmp(argv[1],"--reserved-pip2")) {
        puts("Usage: DockAwesomePipAlignProbe080 --reserved-pip2\nStop Dock/video PiP2 users first. Own-screen visual calibration;120-second timeout.");return 20;
    }
    IntuitionBase=(struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library",39);
    GfxBase=(struct GfxBase *)OpenLibrary((STRPTR)"graphics.library",39);
    CyberGfxBase=OpenLibrary((STRPTR)"cybergraphics.library",40);
    if(!IntuitionBase || !GfxBase || !CyberGfxBase) { puts("FAIL: OS/RTG libraries");goto done; }
    stage("libraries open");
    pub=LockPubScreen(NULL);
    if(!pub || pub->Width<640 || pub->Height<480 || GetBitMapAttr(pub->RastPort.BitMap,BMA_DEPTH)!=16) {
        puts("FAIL: needs a 16-bit public screen");goto done;
    }
    mode=GetVPModeID(&pub->ViewPort);
    screen=OpenScreenTags(NULL,SA_DisplayID,mode,SA_Width,pub->Width,SA_Height,pub->Height,
        SA_Depth,16,SA_Type,CUSTOMSCREEN,SA_Behind,TRUE,SA_Quiet,TRUE,
        SA_Title,(ULONG)"PiP2 pixel alignment calibration",TAG_DONE);
    if(!screen) { puts("FAIL: private screen");goto done; }
    stage("private screen open");
    x=(screen->Width-W)/2;y=(screen->Height-H)/2+50;
    roi=malloc(W*H*2);
    if(!roi || !make_pattern(screen,roi,x,y,W,H)) { puts("FAIL: native RGB565 pattern/capture");goto done; }
    stage("pattern copied");
    window=OpenWindowTags(NULL,WA_CustomScreen,(ULONG)screen,WA_Left,20,WA_Top,20,
        WA_Width,screen->Width-40,WA_Height,108,WA_Activate,TRUE,WA_CloseGadget,TRUE,
        WA_SimpleRefresh,TRUE,WA_IDCMP,IDCMP_RAWKEY|IDCMP_CLOSEWINDOW|IDCMP_REFRESHWINDOW,
        WA_Title,(ULONG)"Arrows adjust; Esc saves chosen corrections to stdout",TAG_DONE);
    if(!window) { puts("FAIL: control window");goto done; }
    stage("control window open");
    pip=amiga_pip_open(screen,x,y,W,H,0,error,sizeof(error));
    if(!pip) { printf("FAIL: %s\n",error);goto done; }
    stage("PiP state allocated; hardware not enabled");
    printf("Core%u; exact native ROI(%d,%d)%dx%d; initial corrections x=%d y=%d\n",
        amiga_pip_core_revision(pip),x,y,W,H,dx,dy);fflush(stdout);
    stage("ScreenToFront begin");ScreenToFront(screen);
    if(!wait_front(screen)) { puts("FAIL: private screen front timeout");goto done; }
    WaitTOF();ActivateWindow(window);stage("private screen front");
    {
        struct TextAttr attr={(STRPTR)"topaz.font",8,FS_NORMAL,FPF_ROMFONT};
        control_font=OpenFont(&attr);
        if(!control_font) { puts("FAIL: topaz8 font");goto done; }
    }
    stage("topaz8 open");instructions(window,dx,dy,enabled);stage("instructions complete");
    if(!show_copy(pip,roi,W,H)) { puts("FAIL: initial presentation");goto done; }
    stage("PiP copy presented; event loop begin");
    CurrentTime(&start_sec,&start_micro);
    while(!finish) {
        struct IntuiMessage *m;
        int changed=0;
        CurrentTime(&sec,&micro);elapsed=(sec-start_sec)*1000000UL+micro-start_micro;
        if(elapsed>=120000000UL) { puts("TIMEOUT:120 seconds");break; }
        if(SetSignal(0,SIGBREAKF_CTRL_C)&SIGBREAKF_CTRL_C) break;
        if(IntuitionBase->FirstScreen!=screen) { puts("STOP: calibration screen lost front");break; }
        while((m=(struct IntuiMessage *)GetMsg(window->UserPort))) {
            ULONG cls=m->Class;UWORD code=m->Code,qual=m->Qualifier;
            ReplyMsg((struct Message *)m);
            if(cls==IDCMP_CLOSEWINDOW) finish=1;
            else if(cls==IDCMP_REFRESHWINDOW) { BeginRefresh(window);instructions(window,dx,dy,enabled);EndRefresh(window,TRUE); }
            else if(cls==IDCMP_RAWKEY && !(code&0x80)) {
                int step=(qual&(IEQUALIFIER_LSHIFT|IEQUALIFIER_RSHIFT))?8:1;
                int nx=dx,ny=dy;
                if(code==0x45 || code==0x44) finish=1;
                else if(code==0x4f) nx-=step;
                else if(code==0x4e) nx+=step;
                else if(code==0x4c) ny-=step;
                else if(code==0x4d) ny+=step;
                if(amiga_pip_set_alignment(pip,nx,ny) && (nx!=dx || ny!=dy)) {
                    dx=nx;dy=ny;changed=1;
                    printf("PiP correction x=%+d y=%+d\n",dx,dy);fflush(stdout);
                }
            }
        }
        if(elapsed-last_blink>=500000UL) { enabled=!enabled;last_blink=elapsed;changed=1; }
        if(changed) {
            if(enabled) { if(!show_copy(pip,roi,W,H)) { puts("FAIL: adjusted presentation");goto done; } }
            else amiga_pip_suspend(pip);
            instructions(window,dx,dy,enabled);
        }
        Delay(1);
    }
    printf("Chosen PiP corrections: x=%+d y=%+d (visual calibration; no automatic alignment verdict)\n",dx,dy);
    status=0;
done:
    stage("cleanup begin");
    amiga_pip_close(pip); /* DMA is disabled before source/buffers/screen vanish. */
    if(window) { struct Message *m;while((m=GetMsg(window->UserPort))) ReplyMsg(m);CloseWindow(window); }
    if(control_font) { CloseFont(control_font);control_font=NULL; }
    if(screen) { ScreenToFront(pub);wait_front(pub);CloseScreen(screen); }
    free(roi);
    if(pub) UnlockPubScreen(NULL,pub);
    if(CyberGfxBase) CloseLibrary(CyberGfxBase);
    if(GfxBase) CloseLibrary((struct Library *)GfxBase);
    if(IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    stage("cleanup complete");
    return status;
}
