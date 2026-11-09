/* Rare, temporary AppWindow ownership for external Workbench drag candidates.
   Shape changes and ownership checks run in the application task, never from
   the input handler. No per-drag allocation, timer, patch or input replay. */
#include "amiga_drop_catcher.h"
#include "amiga_abi.h"
#include <exec/ports.h>
#include <devices/inputevent.h>
#include <proto/input.h>
#include <graphics/clip.h>
#include <graphics/gfxbase.h>
#include <graphics/layers.h>
#include <graphics/regions.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <inline/macros.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/layers.h>
#include <proto/wb.h>
#include <utility/hooks.h>
#include <workbench/workbench.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum { DESKTOP_ICON_CAP = 128 };

struct AmigaDropCatcher {
    struct Screen *screen;
    struct Window *window;
    struct Region *normal, *catch_region;
    struct BitMap *backing;
    struct Hook backfill;
    int left, top, width, height;
    int shaped; /* catch by shape (ApolloOS) or by moving the window */
    uint32_t serial;
    uint64_t deadline, next_icons, icons_not_before;
    int have_serial, active, waiting, failed;
    volatile int backing_valid;
    const struct Window *desktop;
    int icon_count, icons_incomplete;
    struct Rectangle icons[DESKTOP_ICON_CAP];
    struct Rectangle labels[DESKTOP_ICON_CAP];
};

extern ULONG dock_backfill_entry(void);
extern ULONG dock_workbench_hit_entry(void);

ULONG dock_workbench_hit(struct Hook *hook, const struct IconSelectMsg *icon)
{
    AmigaDropCatcher *c = hook->h_Data;
    int left, top, right, bottom, label_half, center, index;
    if (!icon || icon->ism_Length < offsetof(struct IconSelectMsg,ism_Height)+sizeof(icon->ism_Height) ||
        !icon->ism_ParentWindow || c->icon_count == DESKTOP_ICON_CAP ||
        (c->desktop && c->desktop != icon->ism_ParentWindow)) {
        c->icons_incomplete = 1;
        return ISMACTION_Stop;
    }
    c->desktop = icon->ism_ParentWindow;
    index = c->icon_count++;
    left = icon->ism_Left; top = icon->ism_Top;
    right = left+icon->ism_Width; bottom = top+icon->ism_Height;
    c->icons[index] = (struct Rectangle){left-2, top-2, right+1, bottom+1};
    /* Allow for centered labels. Their font can differ from the window. */
    label_half = icon->ism_Name ? (int)strlen((const char *)icon->ism_Name)*6+2 : 0;
    center = left+icon->ism_Width/2;
    c->labels[index] = (struct Rectangle){center-label_half, bottom-2, center+label_half, bottom+19};
    return ISMACTION_Ignore;
}

static void invalidate_desktop_icons(AmigaDropCatcher *c)
{
    c->desktop = NULL;
    c->next_icons = 0;
}

static unsigned int refresh_desktop_icons(AmigaDropCatcher *c, uint64_t now, uint32_t frequency)
{
    struct Hook hook = {0};
    if (WorkbenchBase->lib_Version < 45 || WorkbenchBase->lib_NegSize < 126 ||
        now < c->next_icons || now < c->icons_not_before ||
        (PeekQualifier() & (IEQUALIFIER_LEFTBUTTON|IEQUALIFIER_RBUTTON|IEQUALIFIER_MIDBUTTON)))
        return 0;
    c->next_icons = now+(uint64_t)frequency*4;
    c->icons_not_before = now+frequency/4;
    c->desktop = NULL; c->icon_count = c->icons_incomplete = 0;
    hook.h_Entry = dock_workbench_hit_entry;
    hook.h_Data = c;
    if (!ChangeWorkbenchSelectionA(NULL, &hook, NULL) || c->icons_incomplete) c->desktop = NULL;
    return AMIGA_DROP_SCANNED;
}

static int rect_contains(const struct Rectangle *r, int x, int y)
{
    return x >= r->MinX && x <= r->MaxX && y >= r->MinY && y <= r->MaxY;
}

/* The first two fields are the public backfill message prefix. Bounds are
   bitmap coordinates and already clipped by Layers. Don't lock a layer or
   use a layer-aware RastPort operation from its own callback. */
struct DockBackfillMessage { struct Layer *layer; struct Rectangle bounds; };

ULONG dock_backfill_paint(struct Hook *hook, struct RastPort *port,
                         const struct DockBackfillMessage *msg)
{
    AmigaDropCatcher *c = hook->h_Data;
    int x = msg->bounds.MinX, y = msg->bounds.MinY;
    int sx = x-c->left, sy = y-c->top;
    int w = msg->bounds.MaxX-x+1, h = msg->bounds.MaxY-y+1;
    if (!c->backing_valid || port->BitMap != c->screen->RastPort.BitMap ||
        sx < 0 || sy < 0 || sx+w > c->width || sy+h > c->height || w <= 0 || h <= 0) return 0;
    BltBitMap(c->backing,sx,sy,port->BitMap,x,y,w,h,0xc0,0xff,NULL);
    return 0;
}

static int screen_is_front(struct Screen *screen)
{
    ULONG key = LockIBase(0);
    int front = IntuitionBase->FirstScreen == screen;
    UnlockIBase(key);
    return front;
}

static APTR signal_port_owner(const struct MsgPort *port)
{
    return port && (port->mp_Flags & PF_ACTION) == PA_SIGNAL ? port->mp_SigTask : NULL;
}

/* Public port ownership is stronger than title matching and has been verified
   on the target: WORKBENCH and the desktop UserPort signal the same task. */
static int workbench_client_start(const AmigaDropCatcher *c, int x, int y)
{
    static const char *const names[] = {"WORKBENCH", "WANDERER", "WANDERER.1"};
    enum { OWNER_COUNT = sizeof(names)/sizeof(names[0]) };
    APTR owners[OWNER_COUNT];
    struct Layer *layer;
    struct Window *window;
    int i, result = 0;
    Forbid();
    for (i = 0; i < OWNER_COUNT; ++i) owners[i] = signal_port_owner(FindPort((STRPTR)names[i]));
    Permit();
    /* A competing native drag may already own Layers. Retry from the main
       loop instead of blocking input while trying to inspect its source. */
    if (!AttemptSemaphore(&c->screen->LayerInfo.Lock)) return -1;
    layer = WhichLayer(&c->screen->LayerInfo, x, y);
    window = layer ? layer->Window : NULL;
    if (window && window != c->window && window->WScreen == c->screen &&
        x >= window->LeftEdge + window->BorderLeft &&
        x < window->LeftEdge + window->Width - window->BorderRight &&
        y >= window->TopEdge + window->BorderTop &&
        y < window->TopEdge + window->Height - window->BorderBottom) {
        APTR owner = signal_port_owner(window->UserPort);
        for (i = 0; i < OWNER_COUNT; ++i) if (owner && owner == owners[i]) {
            result = 1;
            break;
        }
        if (result && window == c->desktop) {
            x -= window->LeftEdge; y -= window->TopEdge;
            result = 0;
            for (i = 0; i < c->icon_count; ++i) {
                if (rect_contains(&c->icons[i], x, y) || rect_contains(&c->labels[i], x, y)) {
                    result = 1;
                    break;
                }
            }
        }
    }
    ReleaseSemaphore(&c->screen->LayerInfo.Lock);
    return result;
}

static int inside(const AmigaDropCatcher *c, int x, int y)
{
    return x >= c->left && x < c->left + c->width &&
           y >= c->top && y < c->top + c->height;
}

/* Cover the dock's box (catching) or not. Shapes report the region they
   replace, which must be the expected one; ChangeWindowBox and
   WindowToFront are carried out by Intuition shortly after. */
static int set_catching(AmigaDropCatcher *c, int catching)
{
    if (c->shaped) {
        struct Region *expected = catching ? c->normal : c->catch_region;
        struct Region *previous = amiga_change_window_shape(c->window, catching ? c->catch_region : c->normal);
        return previous == expected || (!catching && previous == c->normal);
    }
    if (catching) {
        ChangeWindowBox(c->window, c->left, c->top, c->width, c->height);
        WindowToFront(c->window);
    } else ChangeWindowBox(c->window, 0, 0, 1, 1);
    return 1;
}

AmigaDropCatcher *amiga_drop_catcher_init(struct Screen *screen,
    struct Window *window, struct Region *normal_shape, int left, int top, int width, int height)
{
    AmigaDropCatcher *c;
    struct Rectangle rect;
    if (!screen || !window || width < 1 || height < 1 ||
        width > 32767 || height > 32767) return NULL;
    c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->catch_region = NewRegion();
    rect.MinX = rect.MinY = 0; rect.MaxX = width - 1; rect.MaxY = height - 1;
    if (!c->catch_region || !OrRectRegion(c->catch_region, &rect)) {
        if (c->catch_region) DisposeRegion(c->catch_region);
        free(c); return NULL;
    }
    c->screen = screen; c->window = window; c->normal = normal_shape;
    c->shaped = normal_shape != NULL;
    c->left = left; c->top = top; c->width = width; c->height = height;
    c->backing = AllocBitMap(width,height,GetBitMapAttr(screen->RastPort.BitMap,BMA_DEPTH),
        BMF_MINPLANES,screen->RastPort.BitMap);
    if (!c->backing) { DisposeRegion(c->catch_region); free(c); return NULL; }
    c->backfill.h_Entry = dock_backfill_entry;
    c->backfill.h_Data = c;
    InstallLayerHook(window->WLayer,&c->backfill);
    return c;
}

unsigned int amiga_drop_catcher_cancel(AmigaDropCatcher *c)
{
    if (!c) return 0;
    c->waiting = 0;
    if (!c->active) return c->failed ? AMIGA_DROP_ERROR : 0;
    if (!set_catching(c, 0)) {
        c->failed = 1;
        return AMIGA_DROP_ERROR;
    }
    c->active = 0;
    c->backing_valid = 0;
    return AMIGA_DROP_RESTORED | (c->failed ? AMIGA_DROP_ERROR : 0);
}

unsigned int amiga_drop_catcher_accepted(AmigaDropCatcher *c)
{
    return amiga_drop_catcher_cancel(c);
}

unsigned int amiga_drop_catcher_update(AmigaDropCatcher *c,
    uint32_t external_serial, struct Screen *external_screen,
    int down_x, int down_y, int external_held, int external_dragged,
    int release_known, int release_x, int release_y, uint16_t qualifiers,
    uint64_t now, uint32_t frequency)
{
    unsigned int flags = 0;
    if (!c) return 0;
    if (c->failed) return AMIGA_DROP_ERROR;
    if (!screen_is_front(c->screen) || (qualifiers & IEQUALIFIER_RBUTTON)) {
        /* Consume the generation logically so the same held gesture cannot
           rearm after a menu or screen switch. Original events pass through. */
        c->serial = external_serial; c->have_serial = 1;
        invalidate_desktop_icons(c);
        return amiga_drop_catcher_cancel(c);
    }
    /* Workbench can wait for the button to be released inside the
       selection API. Never call it while servicing a press or drag. */
    if (external_dragged && (external_held || c->active || !c->have_serial || external_serial != c->serial))
        invalidate_desktop_icons(c);
    if (!external_held && !c->active) flags |= refresh_desktop_icons(c, now, frequency);
    if (now >= c->next_icons) c->desktop = NULL;
    if (!c->have_serial || external_serial != c->serial) {
        int client = 0;
        flags |= amiga_drop_catcher_cancel(c);
        if (flags & AMIGA_DROP_ERROR) return flags;
        if (external_held && external_screen == c->screen) {
            client = workbench_client_start(c, down_x, down_y);
            if (client < 0) return flags;
        }
        c->serial = external_serial; c->have_serial = 1;
        if (client) {
            BltBitMap(c->screen->RastPort.BitMap,c->left,c->top,
                c->backing,0,0,c->width,c->height,0xc0,0xff,NULL);
            WaitBlit();
            c->backing_valid = 1;
            c->active = 1;
            if (!set_catching(c, 1)) { c->failed = 1; return flags | AMIGA_DROP_ERROR; }
            flags |= AMIGA_DROP_STARTED;
        }
    }
    if (c->waiting) {
        if (now >= c->deadline) flags |= amiga_drop_catcher_cancel(c);
        return flags;
    }
    if (!c->active || external_held) return flags;
    if (release_known && (!inside(c, release_x, release_y) ||
        (!external_dragged && release_x == down_x && release_y == down_y)))
        return flags | amiga_drop_catcher_cancel(c);
    c->waiting = 1;
    {
        uint64_t grace = (uint64_t)frequency * 3 / 10;
        c->deadline = now + (grace ? grace : 1);
    }
    return flags;
}

int amiga_drop_catcher_active(const AmigaDropCatcher *c)
{
    return c && c->active;
}

struct BitMap *amiga_drop_catcher_backing(const AmigaDropCatcher *c)
{
    return c && c->backing_valid ? c->backing : NULL;
}

void amiga_drop_catcher_destroy(AmigaDropCatcher *c)
{
    if (!c) return;
    WaitBlit();
    FreeBitMap(c->backing);
    DisposeRegion(c->catch_region);
    free(c);
}
