/* Write only the reserved PiP2 registers. See the ApolloCrossDev SAGAPiP example. */
#include "amiga_pip.h"
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/semaphores.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define PIP1_BASE 0x00dff3d0UL
#define PIP2_BASE 0x00dff3b0UL
#define CORE_REVISION 0x00dff3eaUL
#define PIP1_LEASE "DockAwesome.PiP1"
#define PIP2_LEASE "DockAwesome.PiP2"

struct AmigaPip {
    struct Screen *screen;
    ULONG base; /* the unit's registers: PiP1 or PiP2 */
    struct SignalSemaphore lease;
    int leased, x, y, width, height, stride, front, configured, touched, right_padding;
    int physical_x, physical_y;
    int x_offset, y_offset;
    int keying, configuration_dirty;
    int retirement_pending;
    ULONG last_present_field;
    volatile ULONG *flip_slot; /* amiga_pip_flip_at_vblank */
    unsigned int core;
    APTR allocation[2];
    unsigned char *pixels[2];
};

static void failure(char *error, size_t capacity, const char *text)
{
    if (error && capacity) snprintf(error, capacity, "%s", text);
}

#ifdef DOCK_AMMX
static void reg16(const AmigaPip *p, unsigned int offset, UWORD value)
{
    *(volatile UWORD *)(p->base + offset) = value;
}
static void reg32(const AmigaPip *p, unsigned int offset, ULONG value)
{
    *(volatile ULONG *)(p->base + offset) = value;
}
static ULONG field_counter(void)
{
    /* ApolloOS graphics TOF_VBlank increments this before waking WaitTOF. */
    return *(volatile ULONG *)&GfxBase->VBCounter;
}
#endif

AmigaPip *amiga_pip_open(struct Screen *screen, int x, int y, int width, int height, int right_padding,
                       char *error, size_t error_capacity)
{
    return amiga_pip_open_unit(screen,2,x,y,width,height,right_padding,error,error_capacity);
}

AmigaPip *amiga_pip_open_unit(struct Screen *screen, int unit, int x, int y, int width, int height,
                       int right_padding, char *error, size_t error_capacity)
{
    const char *lease_name = unit == 1 ? PIP1_LEASE : PIP2_LEASE;
#ifndef DOCK_AMMX
    (void)screen; (void)unit; (void)x; (void)y; (void)width; (void)height; (void)right_padding;
    failure(error,error_capacity,"PiP needs DockAwesome080.");
    return NULL;
#else
    AmigaPip *p;
    size_t bytes;
    unsigned int core;
    int i, conflict;
    failure(error,error_capacity,"");
    if (!screen || width < 1 || height < 1 ||
        width > AMIGA_PIP_MAX_WIDTH || height > AMIGA_PIP_MAX_HEIGHT ||
        right_padding < 0 || right_padding >= width || x < 0 || y < 1 ||
        x > screen->Width-width+right_padding || y > screen->Height-height ||
        GetBitMapAttr(screen->RastPort.BitMap,BMA_DEPTH) < 15) {
        failure(error,error_capacity,"PiP2 needs a valid 16-, 24- or 32-bit screen region."); return NULL;
    }
    /* On core 12515, 336 px showed stripes and 352 px worked with zero modulo. */
    if (width&31) {
        failure(error,error_capacity,"PiP2 width must be a multiple of 32 pixels."); return NULL;
    }
    /* Read the core revision after explicit selection of the 080 build. */
    core=*(volatile UWORD *)CORE_REVISION;
    if (core < 11900 || core == 0xffff) {
        failure(error,error_capacity,"PiP2 needs Apollo core 11900 or newer."); return NULL;
    }
    p=calloc(1,sizeof(*p));
    if (!p) { failure(error,error_capacity,"Not enough memory for PiP2 state."); return NULL; }
    p->screen=screen; p->x=x; p->y=y; p->width=width; p->height=height;
    p->base = unit == 1 ? PIP1_BASE : PIP2_BASE;
    p->right_padding = right_padding;
    p->stride=width*2; p->front=1; p->core=core;
    p->x_offset=16; p->y_offset=-1;
    InitSemaphore(&p->lease); p->lease.ss_Link.ln_Name=(char *)lease_name;
    Forbid();
    conflict=FindSemaphore((STRPTR)lease_name)!=NULL;
    if (!conflict) { AddSemaphore(&p->lease); p->leased=1; }
    Permit();
    if (conflict) {
        failure(error,error_capacity,unit == 1 ? "PiP1 is already in use." : "PiP2 is already in use.");
        free(p); return NULL;
    }
    bytes=(size_t)p->stride*height;
    for (i=0;i<2;++i) {
        /* ApolloLib uses MEMF_ANY. We additionally align both buffer bases
           and all rows to64bytes for the conservatively supported transport. */
        p->allocation[i]=AllocVec(bytes+63,MEMF_ANY|MEMF_CLEAR);
        if (!p->allocation[i]) {
            failure(error,error_capacity,"Not enough contiguous memory for PiP2 buffers.");
            amiga_pip_close(p); return NULL;
        }
        p->pixels[i]=(unsigned char *)(((uintptr_t)p->allocation[i]+63)&~(uintptr_t)63);
    }
    /* No hardware writes until a complete first back buffer is presented.
       Write-only registers cannot identify or preserve an unknown PiP user. */
    return p;
#endif
}

void amiga_pip_flip_at_vblank(AmigaPip *p, volatile ULONG *register_slot, volatile ULONG *buffer_slot)
{
    if (!p) return;
    if (p->flip_slot) *p->flip_slot = 0;
    p->flip_slot = NULL;
    if (!register_slot || !buffer_slot) return;
    *buffer_slot = 0;
    *register_slot = p->base+0x08;
    p->flip_slot = buffer_slot;
}

void amiga_pip_suspend(AmigaPip *p)
{
    if (p && p->flip_slot) *p->flip_slot = 0;
#ifdef DOCK_AMMX
    if (p && p->touched && p->configured) reg16(p,0x12,0); /* DMA row bytes0 disables */
#endif
    if (p) p->configured=0;
}

static int physical_position(const AmigaPip *p, int x_offset, int y_offset, int *x, int *y)
{
    *x=p->x+p->screen->LeftEdge+x_offset;
    *y=p->y+p->screen->TopEdge+y_offset;
    return *x >= 0 && *y >= 0 && *x+p->width <= 65535 && *y+p->height <= 65535;
}

/* The screen region checks of open apply. A configured overlay moves at
   once, without a buffer flip. */
int amiga_pip_move(AmigaPip *p, int x, int y)
{
    if (!p || x < 0 || y < 1 || x > p->screen->Width-p->width+p->right_padding ||
        y > p->screen->Height-p->height) return 0;
    p->x = x; p->y = y;
#ifdef DOCK_AMMX
    if (p->configured) {
        int px, py;
        if (!physical_position(p,p->x_offset,p->y_offset,&px,&py)) return 0;
        reg16(p,0x00,(UWORD)px); reg16(p,0x02,(UWORD)py);
        reg16(p,0x04,(UWORD)(px+p->width)); reg16(p,0x06,(UWORD)(py+p->height));
        p->physical_x = px; p->physical_y = py;
    }
#endif
    return 1;
}

int amiga_pip_set_alignment(AmigaPip *p, int x_offset, int y_offset)
{
    int x,y;
    if (!p || x_offset < -128 || x_offset > 128 || y_offset < -128 || y_offset > 128 ||
        !physical_position(p,x_offset,y_offset,&x,&y)) return 0;
    p->x_offset=x_offset; p->y_offset=y_offset;
    return 1;
}

int amiga_pip_set_keying(AmigaPip *p, int enable)
{
    if (!p) return 0;
    enable=enable!=0;
    if (p->keying!=enable) { p->keying=enable; p->configuration_dirty=1; }
    return 1;
}

void amiga_pip_close(AmigaPip *p)
{
    int i;
    if (!p) return;
    if (p->flip_slot) *p->flip_slot = 0;
#ifdef DOCK_AMMX
    if (p->touched) {
        /* Stop DMA before freeing either possibly scanned buffer. */
        reg16(p,0x12,0);
        WaitTOF();
        reg32(p,0x08,0);
    }
#endif
    for (i=0;i<2;++i) if (p->allocation[i]) FreeVec(p->allocation[i]);
    if (p->leased) { Forbid(); RemSemaphore(&p->lease); Permit(); }
    free(p);
}

unsigned char *amiga_pip_back_buffer(AmigaPip *p)
{
    if (!p) return NULL;
#ifdef DOCK_AMMX
    /* A pointer write can leave the old front scanned until the next field.
       Normal20ms pacing already supplies this interval; rapid callers sleep
       on the OS VBlank signal, without polling or reusing that buffer early. */
    if (p->retirement_pending) {
        if (field_counter()==p->last_present_field) WaitTOF();
        p->retirement_pending=0;
    }
#endif
    return p->pixels[p->front^1];
}
int amiga_pip_back_index(const AmigaPip *p) { return p?p->front^1:0; }
int amiga_pip_stride(const AmigaPip *p) { return p?p->stride:0; }
unsigned int amiga_pip_core_revision(const AmigaPip *p) { return p?p->core:0; }

int amiga_pip_present(AmigaPip *p)
{
#ifndef DOCK_AMMX
    (void)p; return 0;
#else
    int x,y,next;
    if (!p) return 0;
    if (IntuitionBase->FirstScreen != p->screen ||
        GetBitMapAttr(p->screen->RastPort.BitMap,BMA_DEPTH) < 15) {
        amiga_pip_suspend(p); return 0;
    }
    if (!physical_position(p,p->x_offset,p->y_offset,&x,&y)) {
        amiga_pip_suspend(p); return 0;
    }
    next=p->front^1;
    if (!p->configured || p->configuration_dirty || x!=p->physical_x || y!=p->physical_y) {
        if (p->flip_slot) *p->flip_slot = 0;
        reg16(p,0x12,0); p->touched=1;
        reg16(p,0x00,(UWORD)x); reg16(p,0x02,(UWORD)y);
        reg16(p,0x04,(UWORD)(x+p->width)); reg16(p,0x06,(UWORD)(y+p->height));
        reg16(p,0x0c,(UWORD)(2|(p->keying?0x0100:0))); /* RGB565 + optional fixed0xf81f source key */
        reg16(p,0x0e,0); /* supported widths have no inter-row padding/modulo */
        reg16(p,0x10,0);  /* chromakey disabled: front overlay */
        reg32(p,0x08,(ULONG)p->pixels[next]);
        reg16(p,0x12,(UWORD)p->stride);
        p->physical_x=x; p->physical_y=y; p->configured=1; p->configuration_dirty=0;
    } else if (p->flip_slot) *p->flip_slot = (ULONG)p->pixels[next];
    else reg32(p,0x08,(ULONG)p->pixels[next]);
    /* Read after the write: a racing VBlank then causes a conservative extra
       wait, never premature reuse of the previous front. Without a VBlank
       slot the pointer flips at once, so mid-field tearing is possible. */
    p->last_present_field=field_counter(); p->retirement_pending=1;
    p->front=next;
    return 1;
#endif
}
