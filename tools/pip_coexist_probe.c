/* Run while DockAwesome080 --pip owns the public-screen PiP2 lease.
   This helper never presents PiP pixels; its private screen tests Dock pause. */
#include "amiga_pip.h"
#include <exec/types.h>
#include <graphics/gfxbase.h>
#include <graphics/view.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>
#include <stdio.h>
#include <string.h>

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
long __stack=32768;

static int wait_front(struct Screen *screen)
{
    int i;
    for(i=0;i<100 && IntuitionBase->FirstScreen!=screen;++i) Delay(1);
    return IntuitionBase->FirstScreen==screen;
}

int main(void)
{
    struct Screen *pub=NULL,*private_screen=NULL;
    AmigaPip *unexpected=NULL;
    ULONG mode;
    int i,status=20,restore=1;
    char error[192];
    IntuitionBase=(struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library",39);
    GfxBase=(struct GfxBase *)OpenLibrary((STRPTR)"graphics.library",39);
    CyberGfxBase=OpenLibrary((STRPTR)"cybergraphics.library",40);
    if(!IntuitionBase || !GfxBase || !CyberGfxBase) { puts("FAIL: OS/RTG libraries");goto done; }
    pub=LockPubScreen(NULL);
    if(!pub || IntuitionBase->FirstScreen!=pub ||
       GetBitMapAttr(pub->RastPort.BitMap,BMA_DEPTH)!=16) {
        puts("FAIL: start on the front 16-bit public screen with DockAwesome --pip running");goto done;
    }
    /* Open refuses a cooperating lease before buffers/hardware are touched.
       Even unexpected success is closed without ever calling present. */
    unexpected=amiga_pip_open(pub,0,1,32,16,0,error,sizeof(error));
    if(unexpected) { puts("FAIL: PiP2 had no existing Dock lease");goto done; }
    if(!strstr(error,"already reserved")) { printf("FAIL: unexpected lease error: %s\n",error);goto done; }
    puts("PASS: second PiP2 open refused existing Dock lease; no presentation requested");fflush(stdout);
    mode=GetVPModeID(&pub->ViewPort);
    private_screen=OpenScreenTags(NULL,SA_DisplayID,mode,SA_Width,pub->Width,
        SA_Height,pub->Height,SA_Depth,16,SA_Type,CUSTOMSCREEN,SA_Behind,TRUE,
        SA_Quiet,TRUE,SA_Title,(ULONG)"Dock coexistence: private screen for two seconds",TAG_DONE);
    if(!private_screen) { puts("FAIL: private screen allocation");goto done; }
    FillPixelArray(&private_screen->RastPort,0,0,pub->Width,pub->Height,0xff18283cUL);
    ScreenToFront(private_screen);
    if(!wait_front(private_screen)) { puts("FAIL: private screen front timeout");goto done; }
    WaitTOF();puts("PRIVATE FRONT: Dock must suspend its overlay on this screen");fflush(stdout);
    for(i=0;i<100;++i) {
        if(SetSignal(0,SIGBREAKF_CTRL_C)&SIGBREAKF_CTRL_C) { puts("STOP: Ctrl-C");goto done; }
        Delay(1);
    }
    status=0;
done:
    amiga_pip_close(unexpected);
    if(private_screen) {
        ScreenToFront(pub);restore=wait_front(pub);
        CloseScreen(private_screen);
        if(!restore) { puts("FAIL: public screen restore timeout");status=20; }
        else { puts("PUBLIC RESTORED: Dock can resume its overlay");fflush(stdout); }
    }
    if(pub) UnlockPubScreen(NULL,pub);
    if(CyberGfxBase) CloseLibrary(CyberGfxBase);
    if(GfxBase) CloseLibrary((struct Library *)GfxBase);
    if(IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if(!status) puts("PASS: lease conflict and private/public screen transitions completed; Dock behavior needs observation");
    return status;
}
