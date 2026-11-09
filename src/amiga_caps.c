/* Capability probes, run at startup and when Workbench reopens. Keep every
   version and LVO check here; the rest of the program reads DockCaps. */
#include "amiga_caps.h"
#include "amiga_font_aa.h"
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <graphics/gfxbase.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/layers.h>
#define CYBERGFX_BASE_NAME cyber
#include <proto/cybergraphics.h>
#include <cybergraphx/cybergraphics.h>
#include <stdio.h>

#ifndef RPTAG_PenMode
#define RPTAG_PenMode 0x80000080UL
#endif
#ifndef RPTAG_FgColor
#define RPTAG_FgColor 0x80000081UL
#endif

#ifndef AFF_68080
/* ApolloOS sets bit 10 for the Apollo 68080 (AFB_68080); the classic NDK
   names it AFF_FPGA. */
#define AFF_68080 (1L<<10)
#endif
#define SAGA_CORE_REVISION 0x00dff3eaUL

int amiga_caps_cpu_080(void)
{
    return !!(SysBase->AttnFlags & AFF_68080);
}

/* ApolloOS Intuition V50 functions past the classic vector table: offset
   is the LVO's negative size. Other V50 Intuitions (MorphOS) have different
   tables, so only lib_Version 50 qualifies. */
static int intuition_extension(ULONG offset)
{
    return IntuitionBase->LibNode.lib_Version == 50 && IntuitionBase->LibNode.lib_NegSize >= offset;
}

static int library_is(const struct Library *library, int version, int revision)
{
    return library && library->lib_Version == version && library->lib_Revision == revision;
}

/* The Shell close dispatch is specific to the audited Apollo releases. The
   handler itself is read from console.device's vector at each close. */
static int apollo_console(void)
{
    enum { CD_INPUT_HANDLER_LVO = 42 };
    const struct Library *input, *console;
    int ok;
    if (!library_is(&IntuitionBase->LibNode,50,7)) return 0;
    Forbid();
    input = (const struct Library *)FindName(&SysBase->DeviceList,(STRPTR)"input.device");
    console = (const struct Library *)FindName(&SysBase->DeviceList,(STRPTR)"console.device");
    ok = library_is(input,41,4) && library_is(console,41,7) && console->lib_NegSize >= CD_INPUT_HANDLER_LVO;
    Permit();
    return ok;
}

static int optional_library(const char *name, ULONG version, UWORD negsize)
{
    struct Library *library = OpenLibrary((STRPTR)name,version);
    int ok = library && library->lib_NegSize >= negsize;
    if (library) CloseLibrary(library);
    return ok;
}

/* Directory Opus 4 also names its port DOPUS.1 but has no lister
   commands; DOpus 5 keeps dopus5.library open while it runs. */
struct MsgPort *amiga_caps_dopus_port(void)
{
    if (!FindName(&SysBase->LibList,(STRPTR)"dopus5.library")) return NULL;
    return FindPort((STRPTR)"DOPUS.1");
}

int amiga_caps_probe(DockCaps *caps, struct Screen *screen)
{
    *caps = (DockCaps){0};
    caps->cpu_080 = amiga_caps_cpu_080();
#ifdef DOCK_AMMX
    /* This build runs only on a 68080, whether or not exec reports one:
       AmigaOS 3.2 need not set AttnFlags bit 10. */
    caps->ammx = 1;
#endif
    /* A custom chip register: read it only on a 68080 (a Vampire). */
    if (caps->ammx || caps->cpu_080) {
        unsigned int core = *(volatile UWORD *)SAGA_CORE_REVISION;
        caps->saga_core = core;
        caps->saga_pip2 = core >= 11900 && core != 0xffff;
    }
    caps->shaped_windows = intuition_extension(858) && LayersBase->lib_Version >= 50;
    caps->window_action = intuition_extension(942);
    caps->status_notify = intuition_extension(930);
    caps->apollo_console = apollo_console();
    caps->screen_notify = optional_library("screennotify.library",1,0);
    /* Pins open through workbench.library, which needs Workbench itself;
       a replacement desktop such as Scalos may not take those calls. */
    Forbid();
    caps->workbench_port = FindPort((STRPTR)"WORKBENCH") != NULL;
    caps->scalos = FindPort((STRPTR)"SCALOS") != NULL;
    caps->dopus = amiga_caps_dopus_port() != NULL;
    Permit();
    amiga_caps_probe_screen(caps,screen);
    return 1;
}

/* The AROS RGB RastPort tags (ApolloOS): draw one pixel with an RGB
   foreground into a bitmap like the screen's and read it back. AmigaOS 3.9
   has SetRPAttrsA but ignores the tags and draws with the pen. */
static int rgb_rastport(struct Screen *screen, int depth)
{
    static const struct TagItem tags[] = {
        {RPTAG_PenMode,FALSE}, {RPTAG_FgColor,0xff2a5c93UL}, {TAG_DONE,0}
    };
    struct Library *cyber;
    struct BitMap *bitmap;
    struct RastPort rp;
    unsigned char pixel[3] = {0,0,0};
    int ok = 0;
    if (!screen || depth <= 8 || GfxBase->LibNode.lib_NegSize < 0x40e) return 0;
    cyber = OpenLibrary((STRPTR)"cybergraphics.library",40);
    if (!cyber) return 0;
    bitmap = AllocBitMap(1,1,depth,BMF_MINPLANES,screen->RastPort.BitMap);
    if (bitmap) {
        InitRastPort(&rp);
        rp.BitMap = bitmap;
        SetRPAttrsA(&rp,(struct TagItem *)tags);
        RectFill(&rp,0,0,0,0);
        ok = ReadPixelArray(pixel,0,0,3,&rp,0,0,1,1,RECTFMT_RGB) == 1 &&
            pixel[0] == 0x2a && pixel[1] == 0x5c && pixel[2] == 0x93;
        WaitBlit();
        FreeBitMap(bitmap);
    }
    CloseLibrary(cyber);
    return ok;
}

/* cybergraphics.library V51 BltTemplateAlpha (ApolloOS): blend one opaque
   template pixel in pen 1 over pen 0 and read it back. A library that
   opens as V51 is not enough: on Coffin (AmigaOS 3.9) menu text drawn
   this way stayed invisible. */
static int alpha_text(struct Screen *screen, int depth)
{
    static const unsigned char opaque[4] = {255,255,255,255};
    struct Library *cyber;
    struct BitMap *bitmap;
    struct RastPort rp;
    unsigned char before[3] = {0,0,0}, after[3] = {0,0,0};
    int ok = 0;
    if (!screen || depth <= 8 || !optional_library("cybergraphics.library",51,0xde)) return 0;
    cyber = OpenLibrary((STRPTR)"cybergraphics.library",51);
    if (!cyber) return 0;
    bitmap = AllocBitMap(1,1,depth,BMF_MINPLANES,screen->RastPort.BitMap);
    if (bitmap) {
        InitRastPort(&rp);
        rp.BitMap = bitmap;
        SetRast(&rp,0);
        if (ReadPixelArray(before,0,0,3,&rp,0,0,1,1,RECTFMT_RGB) == 1) {
            SetAPen(&rp,1);
            SetDrMd(&rp,JAM1);
            amiga_alpha_template_blit(cyber,&rp,opaque,1,1,0,0);
            WaitBlit();
            ok = ReadPixelArray(after,0,0,3,&rp,0,0,1,1,RECTFMT_RGB) == 1 &&
                (before[0] != after[0] || before[1] != after[1] || before[2] != after[2]);
        }
        FreeBitMap(bitmap);
    }
    CloseLibrary(cyber);
    return ok;
}

void amiga_caps_probe_screen(DockCaps *caps, struct Screen *screen)
{
    caps->screen_depth = screen ? (int)GetBitMapAttr(screen->RastPort.BitMap,BMA_DEPTH) : 0;
    caps->rgb_rastport = rgb_rastport(screen,caps->screen_depth);
    caps->alpha_text = alpha_text(screen,caps->screen_depth);
}

static const char *yes_no(int flag)
{
    return flag ? "yes" : "no";
}

static void print_library(const char *name, const struct Library *library)
{
    if (library) printf("  %-22s %d.%d\n",name,library->lib_Version,library->lib_Revision);
    else printf("  %-22s missing\n",name);
}

void amiga_caps_print(const DockCaps *caps, const DockProfile *profile, int requested_backend)
{
    struct Library *cyber = OpenLibrary((STRPTR)"cybergraphics.library",0);
    const struct Library *input, *console;
    printf("DockAwesome capabilities\n");
    printf("  %-22s %s (AttnFlags $%04x)\n","cpu_080",yes_no(caps->cpu_080),(unsigned int)SysBase->AttnFlags);
    printf("  %-22s %s\n","ammx",yes_no(caps->ammx));
    if (caps->saga_core) printf("  %-22s %d\n","saga_core",caps->saga_core);
    else printf("  %-22s not read\n","saga_core");
    printf("  %-22s %s\n","saga_pip2",yes_no(caps->saga_pip2));
    printf("  %-22s %s\n","shaped_windows",yes_no(caps->shaped_windows));
    printf("  %-22s %s\n","rgb_rastport",yes_no(caps->rgb_rastport));
    printf("  %-22s %s\n","alpha_text",yes_no(caps->alpha_text));
    printf("  %-22s %s\n","window_action",yes_no(caps->window_action));
    printf("  %-22s %s\n","status_notify",yes_no(caps->status_notify));
    printf("  %-22s %s\n","apollo_console",yes_no(caps->apollo_console));
    printf("  %-22s %s\n","screen_notify",yes_no(caps->screen_notify));
    printf("  %-22s %d\n","screen_depth",caps->screen_depth);
    printf("  %-22s %s\n","workbench_port",yes_no(caps->workbench_port));
    printf("  %-22s %s\n","scalos",yes_no(caps->scalos));
    printf("  %-22s %s\n","dopus",yes_no(caps->dopus));
    printf("Libraries\n");
    print_library("exec.library",&SysBase->LibNode);
    print_library("intuition.library",&IntuitionBase->LibNode);
    print_library("graphics.library",&GfxBase->LibNode);
    print_library("layers.library",LayersBase);
    print_library("cybergraphics.library",cyber);
    {
        static const char *const desktop[] = { "workbench.library", "icon.library", "scalos.library" };
        const struct Library *dopus5;
        int version = 0, revision = 0;
        unsigned int i;
        for (i = 0; i < sizeof(desktop)/sizeof(desktop[0]); ++i) {
            struct Library *library = OpenLibrary((STRPTR)desktop[i],0);
            print_library(desktop[i],library);
            if (library) CloseLibrary(library);
        }
        /* Only a loaded copy: opening it would leave it in memory and make
           the dopus probe see DOpus 5 where it does not run. */
        Forbid();
        dopus5 = (struct Library *)FindName(&SysBase->LibList,(STRPTR)"dopus5.library");
        if (dopus5) { version = dopus5->lib_Version; revision = dopus5->lib_Revision; }
        Permit();
        if (dopus5) printf("  %-22s %d.%d\n","dopus5.library",version,revision);
        else printf("  %-22s not loaded\n","dopus5.library");
    }
    Forbid();
    input = (const struct Library *)FindName(&SysBase->DeviceList,(STRPTR)"input.device");
    console = (const struct Library *)FindName(&SysBase->DeviceList,(STRPTR)"console.device");
    Permit();
    print_library("input.device",input);
    print_library("console.device",console);
    if (cyber) CloseLibrary(cyber);
    printf("Profile (requested %s)\n",dock_backend_name(requested_backend));
    printf("  %-22s %s\n","backend",dock_backend_name(profile->backend));
    printf("  %-22s %s\n","pixels",profile->pixels == DOCK_PIXELS_AMMX ? "ammx" : "portable");
    printf("  %-22s %s\n","ui_shapes",yes_no(profile->ui_shapes));
    printf("  %-22s %s\n","ui_glass",yes_no(profile->ui_glass));
    printf("  %-22s %s\n","ui_rgb_pens",yes_no(profile->ui_rgb_pens));
    printf("  %-22s %s\n","aa_text",yes_no(profile->aa_text));
    printf("  %-22s %s\n","apollo_pointer",yes_no(profile->apollo_pointer));
    printf("  %-22s %s\n","error",profile->error ? dock_profile_error_text(profile->error) : "none");
}
