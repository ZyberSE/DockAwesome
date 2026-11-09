/* Input handlers must not render or allocate. */
#include "dock_core.h"
#include "dock_renderer.h"
#include "dock_interaction.h"
#include "dock_prefs.h"
#include "amiga_display.h"
#include "amiga_platform.h"
#include "amiga_abi.h"
#include "amiga_pip.h"
#include "amiga_drop_catcher.h"
#include "amiga_drag_preview.h"
#include "amiga_text.h"
#include "amiga_ui.h"
#include "amiga_version.h"
#include "amiga_caps.h"
#include <exec/execbase.h>
#include <exec/interrupts.h>
#include <hardware/intbits.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <devices/timer.h>
#include <graphics/gfxbase.h>
#include <graphics/layers.h>
#include <graphics/regions.h>
#include <inline/macros.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <workbench/workbench.h>
#include <workbench/icon.h>
#include <workbench/startup.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/icon.h>
#include <proto/wb.h>
#include <proto/layers.h>
#include <proto/utility.h>
#include <proto/input.h>
#include <proto/timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stddef.h>

#ifndef ICONCTRLA_GetARGBImageData1
/* AROS icon.library extension absent from the classic NDK. */
#define ICONCTRLA_GetARGBImageData1 (ICONA_Dummy+301)
#endif

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *IconBase, *WorkbenchBase, *LayersBase, *UtilityBase;
struct Device *InputBase, *TimerBase;
/* Paired with -Wl,-u,___stkinit: libnix swaps small CLI/Workbench stacks before main. */
long __stack = 32768;
extern void dock_input_entry(void);
typedef void (*InputHandlerCode)(void);
extern void dock_call_console_input(InputHandlerCode code, struct InputEvent *event, APTR data);

#define PATH_CAP DOCK_PREFS_PATH_CAP
#define CONFIG_CAP 768
#define DEFAULT_CONFIG "ENVARC:DockAwesome.json"
#define INPUT_BUTTONS 64
#define IDCMP_FLAGS (IDCMP_MOUSEMOVE|IDCMP_MOUSEBUTTONS|IDCMP_REFRESHWINDOW|IDCMP_RAWKEY|IDCMP_INACTIVEWINDOW)

#ifndef WA_ShapeRegion
#define WA_ShapeRegion (WA_Dummy + 65)
#endif
enum { PIN_PROGRAM = 1, PIN_DRAWER = 2, PIN_SHELL = 4, PIN_DOCUMENT = 8 };
enum { RUNNING_WORKBENCH = 1, RUNNING_WINDOW = 2 };
typedef struct {
    char path[PATH_CAP], title_path[PATH_CAP]; DockCache cache;
    uint32_t pin_id;
    int running, kind, temporary, stack;
    const struct Task *owner; /* A task showing this item's window; compare only. */
} Item;
typedef struct {
    struct Screen *screen;
    int left, top, count;
    ULONG generation;
    DockPlacement placement[DOCK_MAX_ITEMS];
    const DockImage *image[DOCK_MAX_ITEMS];
    uint32_t pin_id[DOCK_MAX_ITEMS];
    DockRect panel;
    int panel_radius;
} InputSnapshot;
typedef struct {
    UWORD code;
    int x, y;
    ULONG generation;
    uint32_t token, pin_id, hit_id;
    unsigned int flags;
} InputButton;
typedef struct {
    ULONG number;
    struct Process *process;
    struct MsgPort *port;
    struct Window *window;
    int cooked;
} ShellTarget;
typedef struct {
    ShellTarget target;
    const struct Interrupt *observer;
    InputHandlerCode code;
    volatile int sent;
} ShellClose;
typedef struct {
    struct Task *task;
    ULONG signal;
    InputSnapshot snapshots[2];
    InputSnapshot *volatile active;
    InputButton buttons[INPUT_BUTTONS];
    volatile UWORD head, tail;
    volatile int held;
    int held_left, held_top, held_down_x, held_down_y;
    volatile int held_dragged, held_cancelled;
    struct Screen *held_screen;
    ULONG held_generation;
    uint32_t held_pin_id, held_token, next_token;
    struct Screen *dock_screen, *external_screen;
    int apollo_pointer;
    int external_x, external_y, external_up_x, external_up_y, external_up_known;
    volatile ULONG external_serial;
    volatile int external_held;
    volatile int external_dragged, external_cancelled;
    ULONG external_seconds, external_micros;
    volatile int right_held, menu_cancelled, ui_busy, popup_open, popup_dismiss;
    /* The Folder Stack's close button in screen coordinates, while it is
       shown: a press there closes the stack and the pair is consumed. */
    volatile int close_valid, close_held;
    volatile int close_left, close_top, close_right, close_bottom;
    volatile ULONG menu_serial;
    uint32_t menu_pin_id;
    int menu_x, menu_y;
    struct Screen *menu_screen;
    volatile ULONG downs, ups, right_icons, ambiguous, overflow;
    ShellClose *volatile shell_close;
} InputContext;
struct App;
typedef struct {
    struct MsgPort *handler;
    BPTR volume;
    LONG key;
} DirectoryIdentity;
enum { DOS_NAME_CAP = 256 };
typedef struct {
    char name[DOS_NAME_CAP], current_dir[DOS_NAME_CAP], task_name[DOS_NAME_CAP];
    DirectoryIdentity directory;
    const struct Process *process; /* Compare only: the owner may exit. */
    int visible; /* Owns a window a user can see; background tasks never do. */
} WindowProgram;
enum { LAUNCH_SLOTS = 4, LAUNCH_SECONDS = 10 };
typedef struct { uint32_t pin_id; uint64_t at; } DockLaunch;
typedef struct {
    struct Message message;
    int stop, count, query, valid, unavailable;
    const struct App *app;
    const struct Task *dock_task;
    DockSettings settings;
    int allow_new;
    char new_path[PATH_CAP];
    DockCache new_cache;
    struct {
        char path[PATH_CAP], title_path[PATH_CAP];
        uint32_t pin_id;
        int running, kind, temporary;
        int attributed;            /* Output: a launched task still shows a window. */
        const struct Task *owner;  /* Output: a task showing this pin's window. */
    } pins[DOCK_MAX_ITEMS];
    /* Dock launches: new apps appearing soon after belong to that pin. */
    DockLaunch launches[LAUNCH_SLOTS];
    int launch_count;
    uint64_t now, launch_ticks;
    /* Console windows resolved by the worker, so the main task never waits on a
       console handler. Probed only on request (probe_consoles): asking a console
       for its window can open an AUTO window or wait on a busy handler. */
    ShellTarget shells[DOCK_MAX_ITEMS];
    int shell_count, probe_consoles, probed;
    /* Worker-only scratch. These tables stay off the worker's stack. */
    WindowProgram owners[DOCK_MAX_ITEMS];
    /* Worker-only state kept between polls: apps already seen, and apps
       attributed to the pin whose launch started them. */
    const struct Task *seen[DOCK_MAX_ITEMS];
    int seen_count;
    struct { const struct Task *task; uint32_t pin_id; } attributions[DOCK_MAX_ITEMS];
    int attribution_count;
    char aliases[DOCK_MAX_ITEMS][PATH_CAP];
    size_t lengths[DOCK_MAX_ITEMS];
} ProgramPoll;
/* mn_Length is a UWORD. The worker scratch is not part of the message. */
_Static_assert(offsetof(ProgramPoll,owners) <= 0xffff, "ProgramPoll message exceeds mn_Length");
/* Shared with the vertical-blank server (amiga_input.s; offsets fixed):
   it counts fields and, while armed, signals the main task. Animation ticks
   then follow the display instead of a 20 ms timer that drifts against
   its 50 Hz, which shows a frame twice or never now and then. */
typedef struct { struct Task *task; ULONG mask; volatile ULONG armed, fields; } VBlankState;
void dock_vblank_entry(void);
/* --profile counters. Only the profile report reads them. */
typedef struct {
    ULONG wakes;
    uint64_t render_ticks, min_ticks, max_ticks;
    uint64_t present_ticks, conversion_ticks, upload_ticks;
    uint64_t program_poll_ticks, program_poll_peak;
    ULONG program_polls, program_changes, menu_suspends;
    ULONG program_checks, drawer_checks, document_checks, status_events;
    ULONG source_polls, source_changes, front_requests, shape_updates, shape_rects, shape_rects_peak;
    ULONG drop_messages, drop_arguments, drop_added, drop_reordered, drop_bad_paths;
    ULONG desktop_scans;
    ULONG drag_reveal_frames, drag_reveal_peak, popup_reveal_frames, dialog_reveal_frames;
    uint64_t capture_ticks, decode_ticks, source_pixels;
    uint64_t settle_ticks, settle_peak;
    ULONG settles, settle_ms[16];
    int settle_samples;
    /* Damage rendering: draws, full draws, and pixel sums for the share of
       the canvas composed and converted per frame. */
    ULONG damage_frames, damage_full, conversion_frames;
    uint64_t compose_pixels, conversion_pixels;
    /* --damage-check: draws compared, and frames that differ from a full
       composition or a full conversion. */
    ULONG check_frames, compose_mismatch, convert_mismatch;
    /* Display fields between consecutive animation frames: one is even
       motion, zero a frame never shown, two or more a stall. */
    ULONG pace_one, pace_same, pace_stall, pace_field;
} ProfileStats;
typedef struct App {
    DockSettings settings;
    int scale_percent;
    DockEdge edge;
    DockUI *ui;
    Item items[DOCK_MAX_ITEMS];
    int requested_backend;
    int count, snapshot, use_ammx, profile, timer_open, timer_pending, timer_failed;
    int incomplete_pins;
    int timer_label_only;
    struct MsgPort *timer_port;
    struct timerequest *timer_io;
    ULONG clock_hz, frames;
    uint64_t started;
    int pip_mode, live_open, live_pending, live_fast, live_failed, pip_visible, source_valid, failed;
    int screen_left, screen_top;
    DockRect dialog_clip;
    int pip_x_offset, pip_y_offset;
    int front_pending;
    struct MsgPort *live_port;
    struct timerequest *live_io;
    struct Process *program_worker;
    struct MsgPort *program_replies;
    ProgramPoll program_poll;
    int program_poll_pending, status_unavailable;
    APTR status_notify;
    LONG status_notify_bit;
    uint32_t status_window_stamp;
    int status_recheck;
    uint64_t status_event_at, status_watch_at, document_poll_at, discovery_at;
    uint64_t program_poll_at, program_poll_started;
    uint32_t menu_pin_id;
    ShellTarget menu_shell;
    ShellTarget shells[DOCK_MAX_ITEMS]; /* Last worker probe; validate before use. */
    int shell_count, shell_probe_wanted;
    uint64_t shells_at, shell_probe_at; /* Probe time, and earliest next hover probe. */
    char notice[PATH_CAP+100]; /* Shown after a Workbench message is replied. */
    AmigaPip *pip;
    AmigaDropCatcher *drop_catcher;
    struct Region *shape, *shape_pending;
    unsigned char *source565, *coverage, *drag_mask;
    int drag_reference, drag_mask_live;
    DockRect drag_box; /* bounds every nonzero drag_mask pixel */
    uint64_t front_requested;
    uint64_t settle_started;
    int settling;
    ProfileStats stats; /* --profile counters, reported at exit. */
    DockLaunch launches[LAUNCH_SLOTS]; /* Recent dock launches, newest last. */
    int launch_count;
    char surface_error[256];

    char config[CONFIG_CAP], shell_launcher_path[PATH_CAP];
    DockPrefs prefs;
    char pip_dump[CONFIG_CAP];
    struct Screen *screen;
    struct Window *window;
    struct MsgPort *drop_port, *input_port;
    /* screennotify.library Workbench client: close before Workbench closes
       (a resolution change) and reopen after it opens again. */
    struct Library *screen_notify;
    struct MsgPort *workbench_port;
    APTR workbench_client;
    int workbench_closed;
    DockCaps caps; /* Probed at startup; screen fields on each reopen. */
    DockProfile features; /* Chosen from caps when the backend is selected. */
    int present_whole; /* The next native present uploads the whole canvas. */
    int held_drops; /* Leading queued drops already in screen coordinates. */
    struct AppWindow *appwindow;
    struct IOStdReq *input_io;
    struct Interrupt input_handler;
    InputContext input_context;
    LONG input_bit;
    int input_open, handler_added;
    ULONG input_generation;
    AmigaDisplay *display;
    DockRenderer renderer;
    DockImage canvas, backdrop;
    DockPlacement placement[DOCK_MAX_ITEMS], target[DOCK_MAX_ITEMS];
    DockPlacement moving_placement[DOCK_MAX_ITEMS];
    DockPlacement held_icon, held_target;
    DockAnimation animation, held_animation;
    DockGesture gesture;
    int moving_item; /* One-based. */
    AmigaDragPreview *drag_preview;
    uint32_t preview_attempt;
    int drop_preview_slot, displayed_drop_slot, snap_wait_motion, snap_x, snap_y;
    uint32_t next_pin_id, flash_pin_id;
    uint64_t flash_deadline;
    uint32_t label_pin_id;
    uint64_t label_at;
    int label_visible;
    int gesture_redraw;
    ULONG native_external_serial;
    int saved_priority, priority_changed;
    ULONG menu_seen;
    int left, top, width, height, active, hovered;
    /* --bench-sweep: a synthetic pointer replaces the real one while active. */
    int bench_speed, bench_active, bench_x, bench_y;
    /* PiP: damage not yet converted into each buffer, whether a coverage
       mask was active in that buffer's last conversion, and a row of
       coverage that masks are applied to (app->coverage stays the
       renderer's output). */
    DockDamage pip_pending[2];
    int pip_masked[2];
    unsigned char *scratch;
    /* --damage-check: a full reference frame and conversion per draw. */
    int damage_check;
    DockRenderer check_renderer;
    DockImage check_canvas;
    unsigned char *check_coverage, *check565;
    /* Vertical-blank animation ticks (see VBlankState). */
    VBlankState vblank;
    struct Interrupt vblank_interrupt;
    LONG vblank_bit;
    ULONG vblank_field, vblank_check_field;
    uint64_t vblank_check_at, vblank_armed_at;
    int vblank_open, vblank_rate_ok, no_vsync;
    /* The close button that replaces the Folder Stack pin while it is
       open, built with the surface for its settings and edge, and kept
       while those stay as it was built for (button_settings, button_edge). */
    DockCache stack_button;
    DockSettings button_settings;
    DockEdge button_edge;
    int stack_button_shown;
    /* The pin morphs between the stack's icon (0) and the close button
       (256) over eight animation ticks, in one buffer. The blend is redone
       and the cache restamped only when its inputs (morph_key) change. */
    int morph;
    DockCache morph_cache;
    unsigned char *morph_pixels;
    struct { int morph, level; unsigned long icon, button; } morph_key;
    int stack_close_press;
} App;

static void animation_cancel(App *app);
static void live_cancel(App *app);
static uint64_t profile_clock(void);
static int viewport_limit(const App *app, DockEdge edge);
static void input_invalidate(App *app);
static void sync_input_ui(App *app);
static void stack_button_build(App *app);

static void item_free(Item *item)
{
    dock_cache_free(&item->cache);
}

static int pin_index(const App *app, uint32_t pin_id)
{
    int i;
    if (pin_id) for (i = 0; i < app->count; ++i) if (app->items[i].pin_id == pin_id) return i;
    return -1;
}

static uint32_t new_pin_id(App *app)
{
    do { if (!++app->next_pin_id) ++app->next_pin_id; }
    while (pin_index(app, app->next_pin_id) >= 0);
    return app->next_pin_id;
}

static int item_path_index(const App *app, const char *path)
{
    int i;
    for (i = 0; i < app->count; ++i)
        if (!app->items[i].stack && !Stricmp((STRPTR)app->items[i].path,(STRPTR)path)) return i;
    return -1;
}

static int stack_index(const App *app)
{
    return app->count && app->items[app->count-1].stack ? app->count-1 : app->count;
}

enum { STACK_DIVIDER_GAP = 12 };

static Item *append_item(App *app, const char *path, int kind, DockCache *cache)
{
    int index = stack_index(app);
    Item *item = &app->items[index];
    uint32_t pin_id = new_pin_id(app);
    if (index < app->count) {
        input_invalidate(app);
        app->items[index+1] = *item;
        app->placement[index+1] = app->placement[index];
    }
    memset(item,0,sizeof(*item));
    strcpy(item->path,path);
    item->pin_id = pin_id;
    item->kind = kind;
    item->cache = *cache;
    memset(cache,0,sizeof(*cache));
    ++app->count;
    return item;
}

/* Transfer cache ownership both ways so settings can roll back on failure. */
static void exchange_stack(App *app, int index, Item *replacement)
{
    Item previous = {0};
    if (index < app->count && app->items[index].stack) {
        previous = app->items[index];
        memmove(app->items+index,app->items+index+1,(app->count-index-1)*sizeof(Item));
        --app->count;
    }
    if (replacement->stack) {
        memmove(app->items+index+1,app->items+index,(app->count-index)*sizeof(Item));
        app->items[index] = *replacement;
        ++app->count;
    }
    *replacement = previous;
}

static ULONG region_rect_count(const struct Region *region)
{
    const struct RegionRectangle *rect;
    ULONG count = 0;
    if (region) for (rect = region->RegionRectangle; rect; rect = rect->Next) ++count;
    return count;
}

static int row_count(const App *app)
{
    return app->count-!!app->moving_item;
}

static int row_item(const App *app, int slot)
{
    return slot+(app->moving_item && slot >= app->moving_item-1);
}

static const DockPlacement *row_placement(const App *app)
{
    return app->moving_item ? app->moving_placement : app->placement;
}

static void finish_move(App *app, int slot, const DockGestureInput *release)
{
    if (!app->moving_item) return;
    /* Preserve the animated neighbors. Only the held icon snaps into place. */
    dock_edge_layout(&app->settings, app->edge, app->count, app->width, app->height,
        release ? release->x : 0, release ? release->y : 0, release != NULL, app->target);
    memcpy(app->placement, app->moving_placement, slot*sizeof(*app->placement));
    app->placement[slot] = app->target[slot];
    memcpy(app->placement+slot+1, app->moving_placement+slot,
        (app->count-slot-1)*sizeof(*app->placement));
    app->moving_item = 0;
    app->renderer.center_panel = 0;
    dock_animation_reset(&app->animation);
}

static int input_hit_test(const App *app, int x, int y)
{
    const DockPlacement *row = row_placement(app);
    int i;
    for (i = row_count(app)-1; i >= 0; --i) {
        int item = row_item(app,i);
        const DockPlacement *p = &row[i];
        if (dock_image_opaque_at(&app->items[item].cache.image[p->level],x-p->x,y-p->y)) return item;
    }
    return -1;
}

static int snapshot_hit(const InputSnapshot *s, int x, int y)
{
    int i;
    for (i = s->count-1; i >= 0; --i)
        if (dock_image_opaque_at(s->image[i],x-s->placement[i].x,y-s->placement[i].y)) return i;
    return -1;
}

static int rounded_background(const DockRect *r, int radius, int x, int y)
{
    int dx = 0, dy = 0;
    if (x < r->left || x >= r->right || y < r->top || y >= r->bottom) return 0;
    if (x < r->left+radius) dx = 2*(r->left+radius-x)-1;
    else if (x >= r->right-radius) dx = 2*(x-r->right+radius)+1;
    if (y < r->top+radius) dy = 2*(r->top+radius-y)-1;
    else if (y >= r->bottom-radius) dy = 2*(y-r->bottom+radius)+1;
    return !dx || !dy || dx*dx+dy*dy <= 4*radius*radius;
}

static int dock_background(App *app, int x, int y)
{
    DockRect rect;
    return input_hit_test(app,x,y) < 0 &&
        dock_edge_panel_rect(&app->settings,app->edge,app->placement,app->count,
            app->width,app->height,&rect) &&
        rounded_background(&rect,app->renderer.panel.radius,x,y);
}

/* Called with LayerInfo locked. Ignore movement outside the PiP surface. */
static DockRect dialog_clip(const App *app)
{
    DockRect clip;
    if (!dock_ui_dialog_bounds(app->ui,&clip)) return (DockRect){0};
    clip.left -= app->left; clip.right -= app->left;
    clip.top -= app->top; clip.bottom -= app->top;
    if (clip.right <= 0 || clip.bottom <= 0 || clip.left >= app->width ||
        clip.top >= app->height) return (DockRect){0};
    return clip;
}

static int native_menu_held(const App *app)
{
    return (PeekQualifier() & IEQUALIFIER_RBUTTON) && !app->input_context.right_held;
}

#define ANY_BUTTON (IEQUALIFIER_LEFTBUTTON|IEQUALIFIER_RBUTTON|IEQUALIFIER_MIDBUTTON)

/* A button the dock tracks, or one of mask, is down. */
static int buttons_engaged(const App *app, UWORD mask)
{
    return app->input_context.held || app->input_context.external_held ||
        app->input_context.right_held || (PeekQualifier() & mask);
}

/* Cache lifetimes and slot order change only after the handler loses access.
   A swallowed DOWN still owns its UP, including across a surface rebuild. */
static void input_invalidate(App *app)
{
    Disable();
    app->input_context.active = NULL;
    ++app->input_generation;
    Enable();
}

static void erase_item(App *app, int index)
{
    Item *item = &app->items[index];
    DockPlacement *placement = &app->placement[index];
    int tail = app->count-index-1;
    input_invalidate(app);
    item_free(item);
    memmove(item,item+1,(size_t)tail*sizeof(*item));
    memmove(placement,placement+1,(size_t)tail*sizeof(*placement));
    memset(&app->items[--app->count],0,sizeof(*item));
}

static void interaction_cancel_for(App *app, uint32_t token)
{
    DockGestureInput in = {0};
    DockGestureEffects effect;
    Disable();
    if (app->input_context.held && (!token || app->input_context.held_token == token))
        app->input_context.held_cancelled = 1;
    if (!token && app->input_context.external_held) {
        app->input_context.external_cancelled = 1;
        app->input_context.external_held = 0;
    }
    Enable();
    in.kind = DOCK_GESTURE_CANCEL;
    in.token = token;
    effect = dock_gesture_update(&app->gesture, &in);
    if (effect.flags & DOCK_EFFECT_REDRAW) {
        if (app->moving_item) finish_move(app, app->moving_item-1, NULL);
        app->gesture_redraw = 1;
    }
    if (app->flash_pin_id) { app->flash_pin_id = 0; app->gesture_redraw = 1; }
}

static void interaction_cancel(App *app) { interaction_cancel_for(app, 0); }

static void input_publish(App *app)
{
    InputContext *c = &app->input_context;
    InputSnapshot *s;
    const DockPlacement *row = row_placement(app);
    int i;
    if (!app->pip_mode || !app->pip_visible || dock_ui_busy(app->ui) ||
        IntuitionBase->FirstScreen != app->screen || native_menu_held(app)) {
        Disable(); c->active = NULL; Enable(); return;
    }
    s = c->active == &c->snapshots[0] ? &c->snapshots[1] : &c->snapshots[0];
    s->screen = app->screen; s->left = app->left; s->top = app->top;
    s->count = row_count(app); s->generation = app->input_generation;
    dock_edge_panel_rect(&app->settings,app->edge,row,s->count,
        app->width,app->height,&s->panel);
    s->panel_radius = app->renderer.panel.radius;
    for (i = 0; i < s->count; ++i) {
        int item = row_item(app,i);
        s->placement[i] = row[i];
        s->image[i] = &app->items[item].cache.image[row[i].level];
        s->pin_id[i] = app->items[item].pin_id;
    }
    Disable();
    __asm volatile ("" ::: "memory");
    c->active = s;
    Enable();
}

static int input_pop(App *app, InputButton *button)
{
    InputContext *c = &app->input_context;
    int result = 0;
    Disable();
    if (c->tail != c->head) {
        *button = c->buttons[c->tail];
        c->tail = (c->tail+1)&(INPUT_BUTTONS-1);
        result = 1;
    }
    Enable();
    return result;
}

static int shell_target_live(const ShellTarget *target)
{
    struct Process *process = FindCliProc(target->number);
    return process && process == target->process && process->pr_CIS &&
        process->pr_ConsoleTask == target->port;
}

/* Apollo's console handler routes CLOSEWINDOW through ActiveWindow.
   input.device owns the handler list and Intuition's activation dispatch.
   Call the verified handler directly so queued activations cannot run
   between checking the selected window and delivering the event. */
static void dispatch_shell_close(ShellClose *close, const struct InputEvent *marker)
{
    const struct Node *node;
    const struct Interrupt *handler = NULL;
    struct Window *window = close->target.window;
    struct InputEvent event = *marker;
    for (node = close->observer->is_Node.ln_Succ; node && node->ln_Succ; node = node->ln_Succ) {
        const struct Interrupt *candidate = (const struct Interrupt *)node;
        if (candidate->is_Code == close->code &&
            candidate->is_Data) { handler = candidate; break; }
    }
    if (!handler) return;
    /* prepare_shell_close validated and activated the window. Take no lock
       here: only an active window is safe to read and close. */
    if (IntuitionBase->ActiveWindow != window || window->FirstRequest ||
        !(window->Flags & WFLG_CLOSEGADGET)) return;
    event.ie_NextEvent = NULL;
    event.ie_Class = IECLASS_CLOSEWINDOW;
    event.ie_EventAddress = window;
    dock_call_console_input(close->code,&event,handler->is_Data);
    close->sent = 1;
}

static void input_push(InputContext *c, UWORD code, int x, int y,
                       uint32_t token, uint32_t pin_id, uint32_t hit_id,
                       ULONG generation, unsigned int flags)
{
    InputButton *button = &c->buttons[c->head];
    button->code = code;
    button->x = x; button->y = y; button->generation = generation;
    button->token = token; button->pin_id = pin_id; button->hit_id = hit_id; button->flags = flags;
    __asm volatile ("" ::: "memory");
    c->head = (c->head+1)&(INPUT_BUTTONS-1);
}

struct InputEvent *dock_input_observe(struct InputEvent *events, void *context)
{
    struct InputEvent *e;
    InputContext *c = context;
    int moved = 0, notify = 0;
    /* A held, external or right-button state can end on another screen. */
    int engaged = c->held || c->external_held || c->right_held || c->close_held;
    for (e = events; e; e = e->ie_NextEvent) {
        if (e->ie_Class == IECLASS_NULL && c->shell_close &&
            e->ie_EventAddress == c->shell_close) {
            dispatch_shell_close(c->shell_close,e);
            e->ie_EventAddress = NULL;
            continue;
        }
        InputSnapshot *s = c->active;
        int external_was_held = c->external_held;
        int right_press = e->ie_Class == IECLASS_RAWMOUSE && e->ie_Code == MENUDOWN;
        if (c->right_held && IntuitionBase->FirstScreen != c->menu_screen) c->menu_cancelled = 1;
        if (c->external_held) {
            int x = c->external_screen->MouseX, y = c->external_screen->MouseY;
            if (x-c->external_x > DOCK_DRAG_THRESHOLD || c->external_x-x > DOCK_DRAG_THRESHOLD ||
                y-c->external_y > DOCK_DRAG_THRESHOLD || c->external_y-y > DOCK_DRAG_THRESHOLD)
                c->external_dragged = 1;
            if (IntuitionBase->FirstScreen != c->external_screen || right_press ||
                (e->ie_Qualifier & IEQUALIFIER_RBUTTON)) {
                c->external_cancelled = 1; c->external_held = 0;
            }
        }
        if (c->held && !c->held_screen) {
            /* Its screen closed: the press stays cancelled until its UP. */
            c->held_cancelled = 1;
        } else if (c->held) {
            int x = c->held_screen->MouseX-c->held_left;
            int y = c->held_screen->MouseY-c->held_top;
            if (IntuitionBase->FirstScreen != c->held_screen || right_press ||
                (e->ie_Qualifier & IEQUALIFIER_RBUTTON))
                c->held_cancelled = 1;
            /* Observe the last processed pointer position on every callback.
               This retains away-and-back excursions even if the main task lags. */
            if (x-c->held_down_x > DOCK_DRAG_THRESHOLD || c->held_down_x-x > DOCK_DRAG_THRESHOLD ||
                y-c->held_down_y > DOCK_DRAG_THRESHOLD || c->held_down_y-y > DOCK_DRAG_THRESHOLD) c->held_dragged = 1;
        }
        if (e->ie_Class == IECLASS_POINTERPOS || e->ie_Class == IECLASS_NEWPOINTERPOS) {
            moved = notify = 1;
        } else if (e->ie_Class == IECLASS_RAWMOUSE) {
            /* Apollo Intuition takes relative left-button coordinates from
               Screen.MouseX/Y. Only its motion event applies the deltas. */
            int relative_left = c->apollo_pointer && (e->ie_Qualifier & IEQUALIFIER_RELATIVEMOUSE) &&
                (e->ie_Code == SELECTDOWN || e->ie_Code == SELECTUP);
            int clean = !moved && (relative_left || (!e->ie_X && !e->ie_Y));
            int captured = 0;
            int already_external = c->external_held;
            notify = 1;
            if (e->ie_Code == MENUUP && c->right_held) {
                c->right_held = 0; e->ie_Class = IECLASS_NULL; continue;
            }
            if (e->ie_Code == MENUDOWN && c->right_held) {
                e->ie_Class = IECLASS_NULL; continue;
            }
            if (e->ie_Code == SELECTUP && c->close_held) {
                c->close_held = 0; e->ie_Class = IECLASS_NULL; continue;
            }
            if (e->ie_Code == SELECTDOWN && clean && c->close_valid && !c->held &&
                c->dock_screen && IntuitionBase->FirstScreen == c->dock_screen &&
                c->dock_screen->MouseX >= c->close_left && c->dock_screen->MouseX < c->close_right &&
                c->dock_screen->MouseY >= c->close_top && c->dock_screen->MouseY < c->close_bottom) {
                /* The close button owns this pair; the main task dismisses
                   the Folder Stack as for a right-click outside it. */
                c->close_held = c->popup_dismiss = 1;
                e->ie_Class = IECLASS_NULL; continue;
            }
            if (e->ie_Code == MENUDOWN && c->popup_open &&
                c->dock_screen && IntuitionBase->FirstScreen == c->dock_screen) {
                /* The popup owns this pair even after dismissal closes its window. */
                c->menu_screen = c->dock_screen;
                c->right_held = c->popup_dismiss = 1;
                e->ie_Class = IECLASS_NULL; continue;
            }
            if (e->ie_Code == MENUDOWN && clean && s && !c->ui_busy &&
                !c->held && !external_was_held && IntuitionBase->FirstScreen == s->screen) {
                int x = s->screen->MouseX-s->left, y = s->screen->MouseY-s->top;
                int hit = snapshot_hit(s,x,y);
                if (hit >= 0 || rounded_background(&s->panel,s->panel_radius,x,y)) {
                    c->menu_screen = s->screen;
                    c->menu_cancelled = 0;
                    if (hit >= 0) ++c->right_icons;
                    c->menu_pin_id = hit >= 0 ? s->pin_id[hit] : 0;
                    c->menu_x = s->left+x; c->menu_y = s->top+y;
                    ++c->menu_serial;
                    c->right_held = 1;
                    e->ie_Class = IECLASS_NULL; continue;
                }
            }
            if (e->ie_Code == SELECTUP && already_external) {
                /* Record the release, not the next pointer position observed
                   by the main task. Workbench still receives the event. */
                c->external_up_known = clean && IntuitionBase->FirstScreen == c->external_screen;
                c->external_up_x = c->external_screen->MouseX;
                c->external_up_y = c->external_screen->MouseY;
                c->external_held = 0;
            }
            if (e->ie_Code == SELECTUP && c->held) {
                struct Screen *held_screen = c->held_screen;
                int x = held_screen ? held_screen->MouseX-c->held_left : 0;
                int y = held_screen ? held_screen->MouseY-c->held_top : 0;
                int same_screen = held_screen && IntuitionBase->FirstScreen == held_screen;
                int hit = clean && same_screen && s && s->screen == c->held_screen ? snapshot_hit(s, x, y) : -1;
                unsigned int flags = (clean ? DOCK_INPUT_COORDS_VALID : 0) |
                    (same_screen ? DOCK_INPUT_SAME_SCREEN : 0) |
                    (c->held_dragged ? DOCK_INPUT_DRAGGED : 0);
                if (!clean || !same_screen || c->held_cancelled || (e->ie_Qualifier & IEQUALIFIER_RBUTTON))
                    flags |= DOCK_INPUT_CANCELLED;
                /* The original DOWN reserved this slot, including through rebuilds. */
                input_push(c, SELECTUP, x, y, c->held_token,
                    c->held_pin_id, hit >= 0 ? s->pin_id[hit] : 0, c->held_generation, flags);
                c->held = 0; ++c->ups; e->ie_Class = IECLASS_NULL; captured = 1;
            } else if (e->ie_Code == SELECTDOWN && c->held) {
                e->ie_Class = IECLASS_NULL; captured = 1;
            } else if (e->ie_Code == SELECTDOWN && !already_external && !c->right_held && !c->ui_busy && s &&
                IntuitionBase->FirstScreen == s->screen && !(e->ie_Qualifier & IEQUALIFIER_RBUTTON)) {
                int x = s->screen->MouseX-s->left, y = s->screen->MouseY-s->top;
                int hit = clean ? snapshot_hit(s, x, y) : -1;
                unsigned int available = (c->tail-c->head-1)&(INPUT_BUTTONS-1);
                if (!clean) ++c->ambiguous;
                else if (hit >= 0 && available < 2) ++c->overflow;
                else if (hit >= 0) {
                    if (!++c->next_token) ++c->next_token;
                    c->held_screen = s->screen; c->held_left = s->left; c->held_top = s->top;
                    c->held_down_x = x; c->held_down_y = y;
                    c->held_pin_id = s->pin_id[hit]; c->held_token = c->next_token;
                    c->held_generation = s->generation; c->held_dragged = c->held_cancelled = 0;
                    input_push(c, SELECTDOWN, x, y, c->held_token,
                        c->held_pin_id, c->held_pin_id, s->generation, DOCK_INPUT_VALID);
                    c->held = 1; ++c->downs; e->ie_Class = IECLASS_NULL; captured = 1;
                }
            }
            if (e->ie_Code == SELECTDOWN && !captured && !already_external) {
                c->external_held = 0;
                /* Physical USB mouse presses may include motion. The clean
                   test is required for consuming an icon click, but an
                   external press is observed and passed through unchanged. */
                if (c->dock_screen && IntuitionBase->FirstScreen == c->dock_screen &&
                    !(e->ie_Qualifier & IEQUALIFIER_RBUTTON)) {
                    c->external_screen = c->dock_screen;
                    c->external_x = c->dock_screen->MouseX; c->external_y = c->dock_screen->MouseY;
                    c->external_seconds = e->ie_TimeStamp.tv_secs;
                    c->external_micros = e->ie_TimeStamp.tv_micro;
                    c->external_dragged = c->external_cancelled = 0;
                    c->external_up_known = 0;
                    ++c->external_serial;
                    c->external_held = 1;
                }
            }
            /* Intuition applies acceleration later. Injected move/button chains
               must not use guessed button coordinates. Their owned UP cancels. */
            if (e->ie_Code == IECODE_NOBUTTON || (!relative_left && (e->ie_X || e->ie_Y))) moved = 1;
        } else if (e->ie_Class != IECLASS_NULL) moved = 1;
    }
    /* Pointer motion on another screen cannot change the dock. The live
       timer still notices when the dock screen returns to the front. */
    if (notify && (engaged || c->held || c->external_held || c->right_held || c->close_held ||
        (c->dock_screen && IntuitionBase->FirstScreen == c->dock_screen))) Signal(c->task, c->signal);
    return events;
}

static void message(const char *text)
{
    struct EasyStruct req = { sizeof(req), 0, (STRPTR)"DockAwesome", (STRPTR)"%s", (STRPTR)"OK" };
    ULONG arguments[] = { (ULONG)text };
    if (IntuitionBase) EasyRequestArgs(NULL, &req, NULL, arguments);
    else puts(text);
}

static int image_alloc(DockImage *im, int width, int height)
{
    im->width = width; im->height = height;
    im->pixels = calloc((size_t)width * height, 4);
    return im->pixels != NULL;
}

static int dump_ppm(const char *prefix, const char *suffix, const DockImage *image)
{
    char path[CONFIG_CAP+32];
    unsigned char row[3*256];
    FILE *file;
    int y, start, failed = 0;
    if (!image->pixels || image->width < 1 || image->height < 1) return 0;
    snprintf(path, sizeof(path), "%s-%s.ppm", prefix, suffix);
    file = fopen(path, "wb");
    if (!file) return 0;
    if (fprintf(file, "P6\n%d %d\n255\n", image->width, image->height) < 0) failed = 1;
    for (y = 0; y < image->height && !failed; ++y) for (start = 0; start < image->width && !failed; start += 256) {
        int count = image->width-start;
        if (count > 256) count = 256;
        dock_rgba_to_rgb24_c(row, image->pixels+((size_t)y*image->width+start)*4, (unsigned long)count);
        if (fwrite(row, 3, count, file) != (size_t)count) failed = 1;
    }
    if (fclose(file)) failed = 1;
    return !failed;
}

static int canonical_path(const char *path, char *absolute)
{
    BPTR lock = Lock((STRPTR)path,ACCESS_READ);
    int ok;
    if (!lock) return 0;
    ok = NameFromLock(lock,(STRPTR)absolute,PATH_CAP);
    UnLock(lock);
    return ok;
}

static int is_shell_launcher(const App *app, const char *path)
{
    return app->shell_launcher_path[0] && path &&
        !Stricmp((STRPTR)app->shell_launcher_path,(STRPTR)path);
}

static __attribute__((noinline)) int load_icon_mode(const App *app, const char *path, DockImage *out,
    int *kind, int custom_only)
{
    struct DiskObject *icon;
    const ULONG *argb = NULL;
    UBYTE *data = NULL;
    struct ColorRegister *palette = NULL;
    LONG width = 0, height = 0, colors = 0, transparent = -1;
    ULONG *rgb;
    int x, y, p;
    struct TagItem get_tags[] = {
        { ICONGETA_RemapIcon, FALSE }, { ICONGETA_GenerateImageMasks, !custom_only },
        { custom_only ? ICONGETA_FailIfUnavailable : TAG_IGNORE, TRUE },
        { TAG_DONE, 0 }
    };
    struct TagItem argb_tags[] = {
        { ICONCTRLA_GetARGBImageData1, (ULONG)&argb },
        { ICONCTRLA_GetWidth, (ULONG)&width }, { ICONCTRLA_GetHeight, (ULONG)&height },
        { TAG_DONE, 0 }
    };
    struct TagItem control[] = {
        { ICONCTRLA_GetWidth, (ULONG)&width }, { ICONCTRLA_GetHeight, (ULONG)&height },
        { ICONCTRLA_GetImageData1, (ULONG)&data }, { ICONCTRLA_GetPalette1, (ULONG)&palette },
        { ICONCTRLA_GetPaletteSize1, (ULONG)&colors },
        { ICONCTRLA_GetTransparentColor1, (ULONG)&transparent }, { TAG_DONE, 0 }
    };
    if (!strchr(path,':')) {
        /* An app without a known program file: the default tool icon. */
        struct TagItem default_tags[] = {
            { ICONGETA_RemapIcon, FALSE }, { ICONGETA_GenerateImageMasks, TRUE },
            { ICONGETA_GetDefaultType, WBTOOL }, { TAG_DONE, 0 }
        };
        icon = custom_only ? NULL : GetIconTagList(NULL, default_tags);
    } else {
        icon = GetIconTagList((STRPTR)path, get_tags);
        if (!icon && !custom_only) icon = GetDiskObjectNew((STRPTR)path);
    }
    if (!icon) return 0;
    if (kind && *kind == PIN_PROGRAM) {
        if (icon->do_Type == WBDRAWER || icon->do_Type == WBDISK || icon->do_Type == WBGARBAGE)
            *kind = PIN_DRAWER;
        else if (icon->do_Type == WBPROJECT) {
            *kind = PIN_DOCUMENT;
            if (app->shell_launcher_path[0] && icon->do_DefaultTool && icon->do_DefaultTool[0]) {
                char tool[PATH_CAP];
                if (canonical_path((const char *)icon->do_DefaultTool,tool) && is_shell_launcher(app,tool))
                    *kind = PIN_SHELL;
            }
        }
    }
    IconControlA(icon, argb_tags);
    if (argb && width > 0 && height > 0 && width <= 1024 && height <= 1024) {
        int i;
        unsigned char *dst;
        if (!image_alloc(out, width, height)) { FreeDiskObject(icon); return 0; }
        for (i = 0, dst = out->pixels; i < width*height; ++i, dst += 4) {
            ULONG pixel = argb[i];
            dst[0] = pixel>>16;
            dst[1] = pixel>>8;
            dst[2] = pixel;
            dst[3] = pixel>>24;
        }
        FreeDiskObject(icon);
        return 1;
    }
    IconControlA(icon, control);
    if (data && palette && colors > 0 && colors <= 256 && width > 0 && height > 0 &&
        width <= 1024 && height <= 1024) {
        if (!image_alloc(out, width, height)) { FreeDiskObject(icon); return 0; }
        for (y = 0; y < height; ++y) for (x = 0; x < width; ++x) {
            unsigned char *dst = out->pixels + ((size_t)y*width+x)*4;
            p = data[y*width+x];
            if (p < colors) {
                dst[0] = palette[p].red; dst[1] = palette[p].green; dst[2] = palette[p].blue;
                dst[3] = p == transparent ? 0 : 255;
            }
        }
    } else {
        /* Legacy planar Images have separate word-aligned planes, not interleaved rows. */
        struct Image *im = icon->do_Gadget.GadgetRender;
        if (!(icon->do_Gadget.Flags & GFLG_GADGIMAGE) || !im || !im->ImageData ||
            im->Depth < 0 || im->Depth > 8 || im->Width < 1 || im->Height < 1 ||
            im->Width > 1024 || im->Height > 1024 || im->NextImage) {
            FreeDiskObject(icon); return 0;
        }
        width = im->Width; height = im->Height;
        if (!app->screen) { FreeDiskObject(icon); return 0; } /* Workbench is closed. */
        rgb = calloc(256*3, sizeof(*rgb));
        if (!rgb) { FreeDiskObject(icon); return 0; }
        if (!image_alloc(out, width, height)) { free(rgb); FreeDiskObject(icon); return 0; }
        colors = app->screen->ViewPort.ColorMap->Count;
        if (colors > 256) colors = 256;
        GetRGB32(app->screen->ViewPort.ColorMap, 0, colors, rgb);
        for (y = 0; y < height; ++y) for (x = 0; x < width; ++x) {
            unsigned char *dst = out->pixels + ((size_t)y*width+x)*4;
            int bit, plane = 0, pen = 0;
            size_t row_words = (width+15)/16;
            for (bit = 0; bit < 8; ++bit) {
                if (im->PlanePick & (1<<bit)) {
                    if (plane < im->Depth && (im->ImageData[plane*row_words*height + y*row_words + x/16] & (0x8000>>(x&15)))) pen |= 1<<bit;
                    ++plane;
                } else if (im->PlaneOnOff & (1<<bit)) pen |= 1<<bit;
            }
            dst[0] = rgb[pen*3]>>24; dst[1] = rgb[pen*3+1]>>24; dst[2] = rgb[pen*3+2]>>24;
            dst[3] = pen ? 255 : 0;
        }
        free(rgb);
    }
    FreeDiskObject(icon);
    return 1;
}

static int read_folder_icon(const void *context, const char *path, DockImage *icon)
{
    return load_icon_mode(context,path,icon,NULL,1);
}

static int load_icon(const App *app, const char *path, DockImage *icon, int *kind)
{
    return load_icon_mode(app,path,icon,kind,0);
}

static int load_stack_icon(const App *app, const char *path, DockImage *icon)
{
    return load_icon(app,path,icon,NULL) || load_icon(app,"ENV:Sys/def_drawer",icon,NULL);
}

static int prepare_stack(App *app, const char *path, const DockSettings *settings, Item *item)
{
    BPTR lock = Lock((STRPTR)path,ACCESS_READ);
    struct FileInfoBlock *fib;
    DockImage icon = {0};
    int ok;
    if (!lock) return 0;
    fib = AllocDosObject(DOS_FIB,NULL);
    ok = fib && Examine(lock,fib) && fib->fib_DirEntryType > 0 &&
        NameFromLock(lock,(STRPTR)item->path,sizeof(item->path));
    if (fib) FreeDosObject(DOS_FIB,fib);
    UnLock(lock);
    if (!ok || !load_stack_icon(app,item->path,&icon)) return 0;
    ok = dock_cache_build(&item->cache,&icon,settings);
    free(icon.pixels);
    if (!ok) return 0;
    item->pin_id = new_pin_id(app);
    item->kind = PIN_DRAWER;
    item->stack = 1;
    return 1;
}

/* A deferred report waits until the caller has replied to its Workbench
   message and released the drop receiver. The first report is kept. */
static void pin_report(App *app, int defer, const char *text)
{
    if (!defer) message(text);
    else if (!app->notice[0]) snprintf(app->notice,sizeof(app->notice),"%s",text);
}

/* Positive: pin index plus one (also for an existing pin). Zero: unavailable
   icon/path. Negative: capacity/cache failure; startup must not save a subset. */
static int pin_path(App *app, const char *path, int defer)
{
    char clean[PATH_CAP], absolute[PATH_CAP];
    size_t n;
    int i, kind;
    DockImage source = {0};
    DockCache cache = {0};
    if (!path || !*path || strlen(path) >= sizeof(clean)) return 0;
    strcpy(clean, path); n = strlen(clean);
    if (n >= 5 && !Stricmp((STRPTR)(clean+n-5), (STRPTR)".info")) clean[n-5] = 0;
    if (canonical_path(clean,absolute)) strcpy(clean,absolute);
    i = item_path_index(app,clean);
    if (i >= 0) return i+1;
    if (!dock_items_fit(&app->settings,app->count+1,viewport_limit(app,app->edge))) {
        pin_report(app,defer,"No room for another pin. Remove a pin or lower Size or Scale.");
        return -1;
    }
    kind = is_shell_launcher(app,clean) ? PIN_SHELL : PIN_PROGRAM;
    if (!load_icon(app, clean, &source,&kind)) {
        char error[PATH_CAP+100];
        snprintf(error, sizeof(error), "Cannot read this icon:\n%s\nCheck its .info file and icon.library.", clean);
        pin_report(app,defer,error); return 0;
    }
    if (!dock_cache_build(&cache, &source, &app->settings)) {
        free(source.pixels);
        pin_report(app,defer,"Not enough memory for the icon cache. Preferences are unchanged.");
        return -1;
    }
    free(source.pixels);
    return (int)(append_item(app,clean,kind,&cache)-app->items)+1;
}

static int bounded_option(const char *text, int low, int high)
{
    char *end;
    long value = strtol(text, &end, 10);
    return end != text && !*end && value >= low && value <= high ? (int)value : INT_MAX;
}

/* If replacement was interrupted between renames, recover the old file. */
static int read_config(App *app)
{
    char backup[CONFIG_CAP+8];
    const char *path = app->config;
    BPTR lock = Lock((STRPTR)path, ACCESS_READ);
    FILE *file;
    int state = 1;
    if (!lock) {
        if (IoErr() != ERROR_OBJECT_NOT_FOUND) return -1;
        snprintf(backup, sizeof(backup), "%s.bak", path);
        path = backup;
        state = 2;
        lock = Lock((STRPTR)path, ACCESS_READ);
        if (!lock) return IoErr() == ERROR_OBJECT_NOT_FOUND ? 0 : -1;
    }
    file = fopen(path, "r");
    UnLock(lock);
    if (!file) return -1;
    if (!dock_prefs_read(file, &app->prefs)) state = -1;
    if (fclose(file)) state = -1;
    return state;
}

static __attribute__((noinline)) int save_config(App *app)
{
    static char claim_name[] = "DockAwesome.SaveConfig";
    struct SignalSemaphore claim;
    char temp[CONFIG_CAP+8], backup[CONFIG_CAP+8];
    const char *error = NULL;
    DockPrefs *prefs = &app->prefs;
    BPTR existing;
    FILE *file;
    int i, failed;
    if (app->incomplete_pins) {
        message("Cannot save while pins are missing.\n"
            "Restore their volumes or icons, then restart DockAwesome.");
        return 0;
    }
    prefs->size = app->settings.base_size;
    prefs->zoom = app->settings.max_size;
    prefs->scale_percent = app->scale_percent;
    prefs->levels = app->settings.levels;
    prefs->edge = app->edge;
    prefs->snapshot = app->requested_backend == DOCK_BACKEND_SNAPSHOT;
    prefs->pip_mode = app->requested_backend == DOCK_BACKEND_PIP;
    prefs->pip_x_offset = app->pip_x_offset;
    prefs->pip_y_offset = app->pip_y_offset;
    prefs->count = 0;
    for (i = 0; i < app->count; ++i)
        if (!app->items[i].temporary && !app->items[i].stack)
            strcpy(prefs->paths[prefs->count++],app->items[i].path);
    snprintf(temp, sizeof(temp), "%s.new", app->config);
    snprintf(backup, sizeof(backup), "%s.bak", app->config);
    memset(&claim, 0, sizeof(claim));
    claim.ss_Link.ln_Name = claim_name;
    Forbid();
    if (FindSemaphore((STRPTR)claim_name)) {
        Permit();
        message("Another DockAwesome instance is saving. Try again.");
        return 0;
    }
    AddSemaphore(&claim);
    Permit();
    file = fopen(temp, "w");
    if (!file) { error = "Cannot save preferences. Check the path and write access."; goto finish; }
    failed = !dock_prefs_write(file, prefs);
    if (fclose(file)) failed = 1;
    if (failed) {
        DeleteFile((STRPTR)temp);
        error = "Cannot write preferences.";
        goto finish;
    }
    existing = Lock((STRPTR)app->config, ACCESS_READ);
    if (existing) {
        UnLock(existing);
        DeleteFile((STRPTR)backup);
        if (!Rename((STRPTR)app->config, (STRPTR)backup)) {
            DeleteFile((STRPTR)temp);
            error = "Cannot back up preferences.";
            goto finish;
        }
    } else if (IoErr() != ERROR_OBJECT_NOT_FOUND) {
        DeleteFile((STRPTR)temp);
        error = "Cannot access preferences.";
        goto finish;
    }
    /* existing is used only as a flag after UnLock. */
    if (Rename((STRPTR)temp, (STRPTR)app->config)) {
        DeleteFile((STRPTR)backup);
    } else if (existing && !Rename((STRPTR)backup, (STRPTR)app->config)) {
        error = "Cannot replace preferences. The old file is in .bak and the new one in .new.";
    } else error = "Cannot replace preferences. Recover them from the .new file.";
finish:
    RemSemaphore(&claim);
    if (error) message(error);
    return !error;
}

/* Keep a specific surface error. Report it only after cleanup, when PiP,
   input, the drop receiver and the surface are released. */
static void fail_after_cleanup(App *app, const char *fallback)
{
    if (!app->surface_error[0]) strcpy(app->surface_error,fallback);
    app->failed = 1;
}

/* Pending AppMessages stay queued. accept_drops handles them on the next
   surface, and main replies to any that remain at exit. */
/* Queued drops are relative to the surface that received them. Make them
   screen-relative while no surface is open, and relative to the new surface
   once it opens, so a drop queued across Save or R lands where it was made. */
static void shift_held_drops(App *app, int closing)
{
    struct Node *node;
    int index = 0;
    if (!app->drop_port) return;
    Forbid();
    for (node = app->drop_port->mp_MsgList.lh_Head; node->ln_Succ; node = node->ln_Succ, ++index) {
        struct AppMessage *msg = (struct AppMessage *)node;
        if (closing ? index < app->held_drops : index >= app->held_drops) continue;
        msg->am_MouseX += closing ? app->left : -app->left;
        msg->am_MouseY += closing ? app->top : -app->top;
    }
    app->held_drops = closing ? index : 0;
    Permit();
}

static void close_surface(App *app)
{
    if (app->window) shift_held_drops(app,1);
    dock_ui_hide_label(app->ui);
    app->label_pin_id = 0; app->label_at = 0; app->label_visible = 0;
    amiga_drag_preview_close(app->drag_preview); app->drag_preview = NULL;
    interaction_cancel(app);
    dock_gesture_reset(&app->gesture);
    input_invalidate(app);
    animation_cancel(app);
    live_cancel(app);
    amiga_pip_close(app->pip); app->pip = NULL;
    if (app->appwindow) { RemoveAppWindow(app->appwindow); app->appwindow = NULL; }
    if (app->window) { CloseWindow(app->window); app->window = NULL; }
    amiga_drop_catcher_destroy(app->drop_catcher); app->drop_catcher = NULL;
    if (app->shape) { DisposeRegion(app->shape); app->shape = NULL; }
    if (app->shape_pending) { DisposeRegion(app->shape_pending); app->shape_pending = NULL; }
    free(app->source565); app->source565 = NULL;
    free(app->scratch); app->scratch = NULL;
    free(app->check_canvas.pixels); memset(&app->check_canvas,0,sizeof(app->check_canvas));
    free(app->check_coverage); app->check_coverage = NULL;
    free(app->check565); app->check565 = NULL;
    dock_renderer_release(&app->check_renderer);
    app->renderer.coverage = NULL;
    free(app->coverage); app->coverage = NULL;
    free(app->drag_mask); app->drag_mask = NULL;
    app->drag_reference = app->drag_mask_live = 0;
    app->drag_box = (DockRect){0,0,0,0};
    app->source_valid = app->pip_visible = 0;
    app->front_pending = 0;
    app->settling = 0;
    amiga_display_close(app->display); app->display = NULL;
    free(app->canvas.pixels); free(app->backdrop.pixels); dock_renderer_release(&app->renderer);
    memset(&app->canvas, 0, sizeof(app->canvas)); memset(&app->backdrop, 0, sizeof(app->backdrop));
}

static int viewport_limit(const App *app, DockEdge edge)
{
    int length = edge == DOCK_EDGE_BOTTOM ? app->screen->Width :
        app->screen->Height-app->screen->BarHeight-2;
    if (app->pip_mode) {
        int maximum = edge == DOCK_EDGE_BOTTOM ? AMIGA_PIP_MAX_WIDTH : AMIGA_PIP_MAX_HEIGHT;
        if (length > maximum) length = maximum;
        if (edge == DOCK_EDGE_BOTTOM) length &= ~31;
    }
    return length;
}

static int open_surface(App *app)
{
    enum { RIGHT_PIP_TAIL = 8 };
    int reserve = app->count, wanted, limit = viewport_limit(app,app->edge);
    int cross = app->settings.max_size+2*app->settings.padding+12;
    int available_height = app->screen->Height-app->screen->BarHeight-2;
    /* Keep the right PiP raster's keyed tail even where it crosses the screen edge. */
    int right_padding = app->edge == DOCK_EDGE_RIGHT && app->pip_mode ?
        RIGHT_PIP_TAIL-DOCK_DESKTOP_GAP : 0;
    int panel_cross = dock_panel_thickness(&app->settings)+DOCK_DESKTOP_GAP+right_padding;
    /* Each attempt reports its own error. Keep the text of an earlier failure. */
    if (!app->failed) app->surface_error[0] = 0;
    app->settings.edge_padding = right_padding;
    if (cross < panel_cross) cross = panel_cross;
    /* Pinning can use this spare room without closing and opening the surface. */
    if (app->pip_mode && reserve && app->settings.max_size > app->settings.base_size) {
        reserve += 2;
        if (reserve > DOCK_MAX_ITEMS) reserve = DOCK_MAX_ITEMS;
    }
    wanted = dock_viewport_length(&app->settings,reserve);
    if (wanted > limit) wanted = limit;
    if (wanted < 1) return 0;
    app->width = app->edge == DOCK_EDGE_BOTTOM ? wanted : cross;
    if (app->width > app->screen->Width) app->width = app->screen->Width;
    if (app->pip_mode) {
        app->width = (app->width+31)&~31;
        if (app->width > app->screen->Width) app->width = app->screen->Width&~31;
    }
    app->height = app->edge == DOCK_EDGE_BOTTOM ? cross : wanted;
    if (app->height > available_height) return 0;
    app->left = app->edge == DOCK_EDGE_LEFT ? 0 : app->edge == DOCK_EDGE_RIGHT ?
        app->screen->Width-app->width+right_padding : (app->screen->Width-app->width)/2;
    app->top = app->edge == DOCK_EDGE_BOTTOM ? app->screen->Height-app->height :
        app->screen->BarHeight+2+(available_height-app->height)/2;
    app->renderer.edge = app->edge;
    app->renderer.center_panel = 0;
    app->dialog_clip = (DockRect){0};
    app->drop_preview_slot = app->displayed_drop_slot = -1;
    app->screen_left = app->screen->LeftEdge; app->screen_top = app->screen->TopEdge;
    app->display = amiga_display_open(app->screen, app->width, app->height);
    if (!app->display || !image_alloc(&app->canvas, app->width, app->height) ||
        ((app->snapshot || app->pip_mode) && !image_alloc(&app->backdrop, app->width, app->height))) return 0;
    amiga_display_set_converters(app->display,app->renderer.pixels->rgba_to_rgb24,
        app->renderer.pixels->decode565,app->renderer.pixels->encode565);
    app->active = 0; app->hovered = -1;
    memset(app->placement, 0, sizeof(app->placement));
    if (!dock_edge_layout(&app->settings, app->edge, app->count, app->width, app->height,
                     0, 0, 0, app->placement)) return 0;
    memcpy(app->target, app->placement, sizeof(app->target));
    dock_animation_reset(&app->animation);
    dock_damage_set_full(&app->pip_pending[0]); dock_damage_set_full(&app->pip_pending[1]);
    app->pip_masked[0] = app->pip_masked[1] = 0;
    if (app->damage_check) {
        if (!image_alloc(&app->check_canvas, app->width, app->height)) return 0;
        if (app->pip_mode) {
            app->check_coverage = malloc((size_t)app->width*app->height);
            app->check565 = malloc((size_t)app->width*app->height*2);
            if (!app->check_coverage || !app->check565) return 0;
        }
        app->check_renderer.pixels = app->renderer.pixels;
        app->check_renderer.coverage = app->check_coverage;
    }
    if (app->pip_mode) {
        app->source565 = malloc((size_t)app->width*app->height*2);
        app->coverage = calloc((size_t)app->width, app->height);
        app->drag_mask = calloc((size_t)app->width, app->height);
        app->scratch = malloc((size_t)app->width);
        app->renderer.coverage = app->coverage;
        /* PiP owns all visible pixels. The native receiver only has a
           nonempty shape while accepting a Workbench drag. */
        app->shape = NewRegion();
        app->stats.shape_rects = region_rect_count(app->shape);
        if (app->stats.shape_rects > app->stats.shape_rects_peak) app->stats.shape_rects_peak = app->stats.shape_rects;
        if (!app->source565 || !app->coverage || !app->drag_mask || !app->scratch || !app->shape) return 0;
        app->pip = amiga_pip_open(app->screen, app->left, app->top, app->width,
            app->height, right_padding, app->surface_error, sizeof(app->surface_error));
        if (!app->pip) return 0;
        if (!amiga_pip_set_alignment(app->pip, app->pip_x_offset, app->pip_y_offset)) {
            strcpy(app->surface_error,"PiP alignment is out of range.");
            return 0;
        }
        if (!amiga_pip_set_keying(app->pip, 1)) return 0;
        if (amiga_display_capture565(app->display, app->source565, app->left,
                app->top, 1, NULL, app->renderer.pixels->bytes_equal, NULL) < 0) {
            strcpy(app->surface_error, "PiP needs a native big-endian RGB16 SAGA screen bitmap."); return 0;
        }
    } else if (app->snapshot && !amiga_display_capture(app->display, &app->backdrop, app->left, app->top)) return 0;
    dock_renderer_backdrop_changed(&app->renderer);
    app->window = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)app->screen,
        WA_Left, app->left, WA_Top, app->top, WA_Width, app->width-right_padding, WA_Height, app->height,
        WA_Borderless, TRUE, WA_Activate, FALSE, WA_ReportMouse, TRUE, WA_RMBTrap, TRUE,
        WA_SimpleRefresh, TRUE, WA_NoCareRefresh, app->pip_mode,
        WA_IDCMP, app->pip_mode ? IDCMP_FLAGS & ~IDCMP_REFRESHWINDOW : IDCMP_FLAGS, WA_MouseQueue, 1,
        app->pip_mode ? WA_ShapeRegion : TAG_IGNORE, (ULONG)app->shape,
        TAG_DONE);
    if (!app->window) return 0;
    if (app->pip_mode && amiga_change_window_shape(app->window, app->shape) != app->shape) {
        strcpy(app->surface_error, "PiP needs Intuition window shapes."); return 0;
    }
    app->appwindow = AddAppWindowA(0, 0, app->window, app->drop_port, NULL);
    if (!app->appwindow) return 0;
    if (app->pip_mode) {
        app->drop_catcher = amiga_drop_catcher_init(app->screen, app->window,
            app->shape, app->window->Width, app->height);
        if (!app->drop_catcher) return 0;
    }
    shift_held_drops(app,0);
    stack_button_build(app);
    return 1;
}

static int pointer_inside(App *app, int *x, int *y)
{
    int inside;
    struct Layer *layer;
    if (app->bench_active) {
        *x = app->bench_x; *y = app->bench_y;
        return *x >= 0 && *x < app->width && *y >= 0 && *y < app->height &&
            dock_edge_hover(&app->settings, app->edge, app->placement, app->count,
                app->width, app->height, *x, *y);
    }
    *x = app->screen->MouseX-app->left; *y = app->screen->MouseY-app->top;
    if (dock_ui_busy(app->ui)) return 0;
    inside = IntuitionBase->FirstScreen == app->screen && *x >= 0 && *x < app->width && *y >= 0 && *y < app->height;
    if (!inside) return 0;
    if (!dock_edge_hover(&app->settings, app->edge, app->placement, app->count,
            app->width, app->height, *x, *y)) return 0;
    if (app->pip_mode) return !native_menu_held(app);
    /* Respect windows above the dock. Never draw directly into their pixels.
       Workbench holds the layers through an icon drag. Never wait for them;
       keep the previous hover state until the lock is free. */
    if (!AttemptSemaphore(&app->screen->LayerInfo.Lock)) return app->active;
    layer = WhichLayer(&app->screen->LayerInfo, app->screen->MouseX, app->screen->MouseY);
    inside = layer == app->window->WLayer;
    ReleaseSemaphore(&app->screen->LayerInfo.Lock);
    return inside;
}

static int animation_needed(const App *app)
{
    if (app->morph != dock_ui_folder_progress(app->ui)) return 1;
    return (app->moving_item && app->held_icon.level != app->held_target.level) ||
        memcmp(row_placement(app), app->target, sizeof(*app->placement)*row_count(app)) != 0;
}

static void reset_hover_label(App *app)
{
    dock_ui_hide_label(app->ui);
    app->label_pin_id = 0; app->label_at = 0; app->label_visible = 0;
}

static DockRect hover_label_anchor(const App *app, int hovered)
{
    const DockPlacement *icon = &app->placement[hovered];
    DockRect anchor = {app->left+icon->x,app->top+icon->y,
        app->left+icon->x+icon->size,app->top+icon->y+icon->size};
    /* Use the fixed baseline and maximum inward growth. A wide label stays
       clear of every icon even while the hover shifts and the row moves. */
    if (app->edge == DOCK_EDGE_BOTTOM) anchor.top = anchor.bottom-app->settings.max_size;
    else if (app->edge == DOCK_EDGE_RIGHT) anchor.left = anchor.right-app->settings.max_size;
    else anchor.right = anchor.left+app->settings.max_size;
    return anchor;
}

static void update_hover_label(App *app)
{
    enum { LABEL_DELAY_MS = 280 };
    int hovered = app->hovered;
    uint32_t pin = 0;
    DockRect anchor;
    if (hovered >= 0 && hovered < app->count && app->active &&
        IntuitionBase->FirstScreen == app->screen && (!app->pip_mode || app->pip_visible) &&
        !app->gesture.active && !buttons_engaged(app,ANY_BUTTON) &&
        !amiga_drop_catcher_active(app->drop_catcher) && !dock_ui_busy(app->ui))
        pin = app->items[hovered].pin_id;
    if (!pin) {
        if (app->label_pin_id) reset_hover_label(app);
        return;
    }
    if (pin != app->label_pin_id) {
        reset_hover_label(app);
        app->label_pin_id = pin;
        app->label_at = profile_clock()+(uint64_t)app->clock_hz*LABEL_DELAY_MS/1000;
        return;
    }
    if ((!app->label_at && !app->label_visible) || animation_needed(app)) return;
    if (app->label_at) {
        const char *path, *first, *end;
        char label[PATH_CAP];
        size_t length;
        if (profile_clock() < app->label_at) return;
        app->label_at = 0;
        path = app->items[hovered].path;
        end = path+strlen(path);
        while (end > path && (end[-1] == '/' || end[-1] == ':')) --end;
        first = end;
        while (first > path && first[-1] != '/' && first[-1] != ':') --first;
        length = end-first;
        if (!length) return;
        memcpy(label,first,length); label[length] = 0;
        anchor = hover_label_anchor(app,hovered);
        app->label_visible = dock_ui_show_label(app->ui,app->edge,&anchor,label);
    } else {
        anchor = hover_label_anchor(app,hovered);
        dock_ui_move_label(app->ui,app->edge,&anchor);
    }
}

static int animate_row(App *app)
{
    DockPlacement *row = app->moving_item ? app->moving_placement : app->placement;
    /* The pin turns into the close button as far as the box is out, so
       both move together, opening and closing. */
    int morph = dock_ui_folder_progress(app->ui);
    int changed = dock_edge_animate(&app->settings, app->edge, &app->animation,
        row, app->target, row_count(app));
    if (app->morph != morph) {
        app->morph = morph;
        changed = 1;
    }
    if (app->moving_item && app->held_icon.level != app->held_target.level)
        changed |= dock_animate_step(&app->settings, &app->held_animation,
            &app->held_icon, &app->held_target, 1);
    return changed;
}

static void update_target(App *app)
{
    DockRect popup;
    int x, y, inside;
    int catching = amiga_drop_catcher_active(app->drop_catcher);
    if (dock_ui_popup_bounds(app->ui,&popup)) return;
    if (app->gesture.active && app->gesture.dragged && !app->gesture.cancelled) {
        DockGesture *gesture = &app->gesture;
        int count = row_count(app), zoom = !gesture->remove_armed;
        int slot = app->gesture.target_slot;
        int centered = 0;
        if (app->moving_item && zoom) {
            DockPlacement gap;
            if (slot < 0) slot = app->moving_item-1;
            if (dock_edge_gap_layout(&app->settings, app->edge, count, app->width, app->height,
                    slot, gesture->x, gesture->y, zoom, app->target, &gap)) {
                app->held_target = (DockPlacement){0, app->settings.max_size-gap.size, gap.size, gap.level};
                centered = slot == 0 || slot == count;
            }
        } else {
            dock_edge_layout(&app->settings, app->edge, count, app->width, app->height,
                gesture->x, gesture->y, zoom, app->target);
            if (app->moving_item)
                app->held_target = (DockPlacement){0, app->settings.max_size-app->settings.base_size,
                    app->settings.base_size, 0};
        }
        if (app->renderer.center_panel != centered) app->gesture_redraw = 1;
        app->renderer.center_panel = centered;
        app->hovered = -1;
        return;
    }
    if (app->snap_wait_motion) {
        if (IntuitionBase->FirstScreen == app->screen &&
            abs(app->screen->MouseX-app->snap_x) <= 4 && abs(app->screen->MouseY-app->snap_y) <= 4)
            return;
        app->snap_wait_motion = 0;
    }
    if (catching || app->renderer.center_panel) {
        /* Keep the shown gap while the AppWindow message is in flight. */
        if (catching && !app->input_context.external_held && app->drop_preview_slot >= 0) return;
        if (catching && app->input_context.external_held && app->input_context.external_dragged) {
            int slot = dock_edge_insertion_slot(&app->settings, app->edge, app->count,
                app->width, app->height, app->screen->MouseX-app->left, app->screen->MouseY-app->top);
            if (slot > stack_index(app)) slot = stack_index(app);
            if (dock_edge_insertion_layout(&app->settings, app->edge, app->count,
                    app->width, app->height, slot, app->target)) {
                app->renderer.center_panel = 1;
                app->drop_preview_slot = slot;
                app->hovered = -1;
                return;
            }
        }
        app->drop_preview_slot = app->displayed_drop_slot = -1;
        dock_edge_layout(&app->settings, app->edge, app->count, app->width, app->height,
            0, 0, 0, app->target);
        app->hovered = -1;
        if (animation_needed(app)) return;
        if (app->renderer.center_panel) {
            app->renderer.center_panel = 0;
            app->gesture_redraw = 1;
        }
        if (catching) return;
    }
    inside = pointer_inside(app, &x, &y);
    dock_edge_layout(&app->settings, app->edge, app->count, app->width, app->height,
                x, y, inside, app->target);
    /* Hit-tests always correspond to the currently displayed frame. */
    app->hovered = inside ? input_hit_test(app, x, y) : -1;
    app->active = inside;
    /* Resolve Shell windows while the pointer rests on a running Shell pin,
       so its menu has a fresh target without a packet on the main task. */
    if (app->hovered >= 0 && app->hovered < app->count &&
        app->items[app->hovered].kind == PIN_SHELL && app->items[app->hovered].running) {
        uint64_t now = profile_clock();
        if (now >= app->shell_probe_at) {
            app->shell_probe_wanted = 1;
            app->shell_probe_at = now+app->clock_hz;
        }
    }
}

static void animation_finish(App *app)
{
    int step;
    for (step = 0; step < 64 && animation_needed(app); ++step)
        animate_row(app);
}

static void animation_profile_begin(App *app)
{
    if (app->profile && !app->settling && animation_needed(app)) {
        app->settling = 1; app->settle_started = profile_clock();
    }
}

static void animation_profile_end(App *app)
{
    if (app->profile && app->settling && !animation_needed(app)) {
        uint64_t ticks = profile_clock()-app->settle_started;
        app->settling = 0; ++app->stats.settles; app->stats.settle_ticks += ticks;
        if (ticks > app->stats.settle_peak) app->stats.settle_peak = ticks;
        if (app->stats.settle_samples < 16)
            app->stats.settle_ms[app->stats.settle_samples++] = (ULONG)(ticks*1000/app->clock_hz);
    }
}

static void animation_schedule(App *app)
{
    int flash_pending = app->flash_pin_id && profile_clock() < app->flash_deadline;
    int fast = animation_needed(app) || flash_pending || dock_ui_folder_pending(app->ui);
    ULONG delay = dock_ui_folder_frame_delay(app->ui);
    if (!fast && !app->label_at) { animation_cancel(app); return; }
    if (fast && app->timer_pending && app->timer_label_only) animation_cancel(app);
    if (!app->timer_open || app->timer_pending || app->vblank.armed || app->timer_failed) return;
    if (app->vblank_open) {
        /* The curves count 20 ms ticks: follow the display only while its
           fields measure 19-21 ms, so 60 Hz modes keep the timer. Checked
           every 50 fields, so a screen mode change is noticed. */
        ULONG fields = app->vblank.fields-app->vblank_check_field;
        if (fields >= 50) {
            uint64_t now = profile_clock();
            uint64_t field_us = (now-app->vblank_check_at)*1000000/app->clock_hz/fields;
            app->vblank_rate_ok = field_us >= 19000 && field_us <= 21000;
            app->vblank_check_field += fields; app->vblank_check_at = now;
        }
    }
    /* Zoom ticks and Folder Stack motion frames (timed, 4 ms requests)
       both follow the display. */
    if (fast && delay <= 20000 && app->vblank_open && app->vblank_rate_ok && !app->no_vsync) {
        /* One tick per display field. A field that passes before the read
           below only delays the tick by one field. */
        app->vblank.armed = 1;
        app->vblank_field = app->vblank.fields;
        app->vblank_armed_at = profile_clock();
        return;
    }
    if (!fast) {
        uint64_t now = profile_clock();
        delay = now < app->label_at ?
            (ULONG)((app->label_at-now)*1000000/app->clock_hz)+1 : 1;
    }
    app->timer_io->tr_node.io_Command = TR_ADDREQUEST;
    app->timer_io->tr_node.io_Flags = 0;
    app->timer_io->tr_time.tv_secs = 0;
    app->timer_io->tr_time.tv_micro = delay;
    SendIO((struct IORequest *)app->timer_io);
    app->timer_pending = 1;
    app->timer_label_only = !fast;
}

static void animation_cancel(App *app)
{
    app->vblank.armed = 0;
    if (!app->timer_pending) return;
    if (!CheckIO((struct IORequest *)app->timer_io)) AbortIO((struct IORequest *)app->timer_io);
    WaitIO((struct IORequest *)app->timer_io);
    app->timer_pending = 0;
    SetSignal(0, 1UL<<app->timer_port->mp_SigBit);
}

static int animation_step(App *app)
{
    LONG error = 0;
    int closed;
    if (app->vblank.armed && (app->vblank.fields != app->vblank_field ||
            profile_clock()-app->vblank_armed_at > app->clock_hz*3/50)) {
        /* No field for three periods: the interrupt is off; use the timer. */
        if (app->vblank.fields == app->vblank_field) app->vblank_rate_ok = 0;
        app->vblank.armed = 0;
    } else {
        if (!app->timer_pending || !CheckIO((struct IORequest *)app->timer_io)) return 0;
        error = WaitIO((struct IORequest *)app->timer_io);
        app->timer_pending = 0;
    }
    if (error) {
        app->timer_failed = 1;
        app->flash_pin_id = 0;
        dock_ui_finish_folder_motion(app->ui);
        sync_input_ui(app);
        Signal(FindTask(NULL),1UL<<app->timer_port->mp_SigBit);
        animation_finish(app);
        return 1;
    }
    closed = dock_ui_folder_step(app->ui);
    if (closed) {
        sync_input_ui(app);
        Signal(FindTask(NULL),1UL<<app->timer_port->mp_SigBit);
    }
    if (animation_needed(app)) closed |= animate_row(app);
    return closed;
}

static uint64_t profile_clock(void)
{
    struct EClockVal clock;
    ReadEClock(&clock);
    return ((uint64_t)clock.ev_hi<<32) | clock.ev_lo;
}

/* Call under Forbid to read the process and its borrowed file handle. */
static int is_interactive_shell(const struct Process *process)
{
    const struct CommandLineInterface *cli = process ? BADDR(process->pr_CLI) : NULL;
    return cli && process->pr_CIS && process->pr_ConsoleTask &&
        cli->cli_StandardInput && IsInteractive(cli->cli_StandardInput);
}

/* Worker only: the console handler's reply can wait. Forbid keeps each
   process valid until its packet is queued. The main task validates the
   cached targets with shell_target_live before using them. */
static void poll_shells(ProgramPoll *poll)
{
    struct InfoData *info = poll->probe_consoles ?
        AllocVec(sizeof(*info),MEMF_PUBLIC|MEMF_CLEAR) : NULL;
    ULONG number, last = MaxCli();
    int i, running = 0;
    poll->shell_count = 0;
    poll->probed = info != NULL;
    for (number = 1; ; ++number) {
        struct Process *process;
        ShellTarget candidate = {0};
        int found;
        Forbid();
        process = FindCliProc(number);
        found = process != NULL;
        if (is_interactive_shell(process)) {
            running = 1;
            if (info && process->pr_CIS ==
                    ((struct CommandLineInterface *)BADDR(process->pr_CLI))->cli_StandardInput) {
                candidate.number = number;
                candidate.process = process;
                candidate.port = process->pr_ConsoleTask;
                memset(info,0,sizeof(*info));
                if (DoPkt(candidate.port,ACTION_DISK_INFO,MKBADDR(info),0,0,0,0)) {
                    candidate.window = (struct Window *)info->id_VolumeNode;
                    candidate.cooked = info->id_DiskType == 0x434f4e00UL; /* CON */
                }
            }
        }
        Permit();
        if (candidate.window && poll->shell_count < DOCK_MAX_ITEMS)
            poll->shells[poll->shell_count++] = candidate;
        /* ApolloDOS can report the last empty slot when the high slots are
           full. Include the occupied run just beyond its reported bound. */
        if (number > last && !found) break;
    }
    FreeVec(info);
    for (i = 0; i < poll->count; ++i)
        if (poll->pins[i].kind == PIN_SHELL) poll->pins[i].running = running;
    poll->valid |= PIN_SHELL;
}

static void poll_window_programs(ProgramPoll *poll, const struct List *programs);
static int rest_after_pin_change(App *app, int before);

static void poll_workbench_list(ProgramPoll *poll, int kind, ULONG get_tag, ULONG free_tag)
{
    struct List *list = NULL;
    struct TagItem get[] = {{get_tag,(ULONG)&list},{TAG_DONE,0}};
    struct TagItem release[] = {{free_tag,0},{TAG_DONE,0}};
    const struct Node *node;
    int i, success = WorkbenchControlA(NULL,get);
    if (kind == PIN_PROGRAM) {
        /* A failed Workbench query leaves its previous evidence intact.
           The window source can still report a Shell launch or closure. */
        if (success) for (i = 0; i < poll->count; ++i)
            if (poll->pins[i].kind == kind) poll->pins[i].running = 0;
    }
    if (!success || !list) {
        if (kind == PIN_PROGRAM) {
            poll_window_programs(poll,NULL);
            poll->valid |= kind;
        } else if (success) poll->unavailable |= kind;
        return;
    }
    for (node = list->lh_Head; node->ln_Succ; node = node->ln_Succ) {
        char absolute[PATH_CAP];
        const char *path = node->ln_Name;
        if (!path) continue;
        for (i = 0; i < poll->count; ++i)
            if (poll->pins[i].kind == kind &&
                !Stricmp((STRPTR)poll->pins[i].path,(STRPTR)path)) {
                poll->pins[i].running = kind == PIN_PROGRAM ? RUNNING_WORKBENCH : 1;
                break;
            }
        if (i < poll->count || kind != PIN_PROGRAM || !strchr(path,':')) continue;
        for (i = 0; i < poll->count; ++i) {
            const char *pin = poll->pins[i].path;
            if (poll->pins[i].kind != PIN_PROGRAM || poll->pins[i].running ||
                Stricmp(FilePart((STRPTR)pin),FilePart((STRPTR)path))) continue;
            if (path == node->ln_Name) {
                if (!canonical_path(path,absolute)) break;
                path = absolute;
            }
            if (!Stricmp((STRPTR)pin,(STRPTR)path)) {
                poll->pins[i].running = RUNNING_WORKBENCH;
                break;
            }
        }
    }
    if (kind == PIN_PROGRAM) poll_window_programs(poll,list);
    release[0].ti_Data = (ULONG)list;
    WorkbenchControlA(NULL,release);
    poll->valid |= kind;
}

/* Window chrome may add a label on either side. Single spaces are legal
   path characters and do not separate a path from that label. */
static int title_separator(const char *text)
{
    if (text[0] == ':' && text[1] == ' ') return 2;
    if (*text == '\t') return 1;
    if (*text != ' ') return 0;
    if (text[1] == ' ' || text[1] == '\t') return 1;
    return (text[1] == '-' || text[1] == '|') && text[2] == ' ' ? 3 : 0;
}

static const char *next_title_part(const char *part)
{
    int separator;
    while (*part && !title_separator(part)) ++part;
    while ((separator = title_separator(part))) part += separator;
    while (*part == ' ' || *part == '\t') ++part;
    return part;
}

static int title_names_document(const char *title, const char *path, size_t length)
{
    const char *part = title;
    while (*part) {
        if (*part == '"' && !Strnicmp((STRPTR)(part+1),(STRPTR)path,length) &&
            part[1+length] == '"') return 1;
        if (!Strnicmp((STRPTR)part,(STRPTR)path,length) &&
            (!part[length] || title_separator(part+length))) return 1;
        part = next_title_part(part);
    }
    return 0;
}

/* Extract only qualified title paths with the same file name as a pin.
   The caller validates the complete path through DOS after releasing IBase. */
static size_t title_document_alias(const char *title, const char *path, char *candidate)
{
    const char *name = strrchr(path,'/');
    const char *part;
    size_t length;
    if (!name) name = strrchr(path,':');
    if (!name || !*++name) return 0;
    length = strlen(name);
    for (part = title; *part; part = next_title_part(part)) {
        int quoted = *part == '"';
        const char *start = part+quoted, *volume = strchr(start,':'), *file;
        if (!volume || volume == start || volume[1] == ' ' ||
            memchr(start,'/',(size_t)(volume-start))) continue;
        for (file = volume+1; *file; ++file) {
            size_t n;
            const char *end;
            if (file[-1] != ':' && file[-1] != '/') continue;
            if (Strnicmp((STRPTR)file,(STRPTR)name,length)) continue;
            end = file+length;
            if (quoted ? *end != '"' : (*end && !title_separator(end))) continue;
            n = end-start;
            if (n >= PATH_CAP) continue;
            memcpy(candidate,start,n); candidate[n] = 0;
            return n;
        }
    }
    return 0;
}

static void poll_document_windows(ProgramPoll *poll)
{
    struct Screen *screen;
    char (*aliases)[PATH_CAP] = poll->aliases;
    size_t *lengths = poll->lengths;
    unsigned char alias_current[DOCK_MAX_ITEMS] = {0};
    ULONG lock;
    int i, alias_count = 0;
    for (i = 0; i < poll->count; ++i) {
        const char *path = poll->pins[i].path;
        const char *volume = strchr(path,':');
        lengths[i] = poll->pins[i].kind == PIN_DOCUMENT && volume && volume != path ? strlen(path) : 0;
    }
    lock = LockIBase(0);
    for (screen = IntuitionBase->FirstScreen; screen; screen = screen->NextScreen) {
        const struct Window *window;
        for (window = screen->FirstWindow; window; window = window->NextWindow) {
            char title[PATH_CAP+128];
            int exact = -1, best = -1;
            size_t n = 0;
            /* Title storage is owned by the window's application. Copy it
               without allowing that task to replace it halfway through. */
            Forbid();
            if (window->Title) while (n < sizeof(title)-1 && window->Title[n]) {
                title[n] = window->Title[n];
                ++n;
            }
            Permit();
            if (!n || n == sizeof(title)-1) continue;
            title[n] = 0;
            for (i = 0; i < poll->count; ++i) {
                if (!lengths[i]) continue;
                if (lengths[i] == n && !Stricmp((STRPTR)title,(STRPTR)poll->pins[i].path)) {
                    exact = i;
                    break;
                }
                if ((best < 0 || lengths[i] > lengths[best]) &&
                    title_names_document(title,poll->pins[i].path,lengths[i])) best = i;
            }
            if (exact >= 0) poll->pins[exact].running = 1;
            else if (best >= 0) poll->pins[best].running = 1;
            else if (alias_count < DOCK_MAX_ITEMS) {
                char candidate[PATH_CAP];
                size_t longest = 0;
                for (i = 0; i < poll->count; ++i) if (lengths[i]) {
                    size_t length = title_document_alias(title,poll->pins[i].path,candidate);
                    if (length > longest) {
                        strcpy(aliases[alias_count],candidate);
                        longest = length;
                    }
                }
                if (longest) ++alias_count;
            }
        }
    }
    UnlockIBase(lock);
    for (i = 0; i < alias_count; ++i) {
        char canonical[PATH_CAP];
        int pin;
        /* The regular title-only pass reuses the last verified DOS alias.
           A full refresh checks it again, as does any close request. */
        if (poll->query == PIN_DOCUMENT) {
            for (pin = 0; pin < poll->count; ++pin)
                if (lengths[pin] && !Stricmp((STRPTR)aliases[i],(STRPTR)poll->pins[pin].title_path)) break;
            if (pin < poll->count) {
                alias_current[pin] = 1;
                poll->pins[pin].running = 1;
                continue;
            }
        }
        if (!canonical_path(aliases[i],canonical)) continue;
        for (pin = 0; pin < poll->count; ++pin)
            if (lengths[pin] && !Stricmp((STRPTR)canonical,(STRPTR)poll->pins[pin].path)) {
                strcpy(poll->pins[pin].title_path,aliases[i]);
                alias_current[pin] = 1;
                poll->pins[pin].running = 1;
                break;
            }
    }
    for (i = 0; i < poll->count; ++i) if (!alias_current[i]) poll->pins[i].title_path[0] = 0;
    poll->valid |= PIN_DOCUMENT;
}

static DirectoryIdentity directory_identity(BPTR lock)
{
    const struct FileLock *file = BADDR(lock);
    return file ? (DirectoryIdentity){file->fl_Task,file->fl_Volume,file->fl_Key} :
        (DirectoryIdentity){0};
}

static int same_directory(DirectoryIdentity a, DirectoryIdentity b)
{
    return a.handler && a.handler == b.handler && a.volume == b.volume && a.key == b.key;
}

static DirectoryIdentity executable_directory(const char *path)
{
    char parent[PATH_CAP];
    size_t n = (const char *)FilePart((STRPTR)path)-path;
    DirectoryIdentity identity = {0};
    BPTR lock;
    if (n && path[n-1] == '/') --n;
    if (!n || n >= sizeof(parent)) return identity;
    memcpy(parent,path,n); parent[n] = 0;
    lock = Lock((STRPTR)parent,ACCESS_READ);
    if (lock) { identity = directory_identity(lock); UnLock(lock); }
    return identity;
}

/* Call with Intuition's window list locked and owner tasks unable to exit. */
static const struct Process *window_process(const struct Window *window)
{
    const struct MsgPort *port = window->UserPort;
    const struct Task *task = port && (port->mp_Flags & PF_ACTION) == PA_SIGNAL ? port->mp_SigTask : NULL;
    return task && task->tc_Node.ln_Type == NT_PROCESS ? (const struct Process *)task : NULL;
}

static void copy_cli_name(char dest[DOS_NAME_CAP], BSTR value)
{
    const unsigned char *name = BADDR(value);
    size_t length = name ? name[0] : 0;
    if (length) memcpy(dest,name+1,length);
    dest[length] = 0;
}

/* Capture process data while the owner cannot exit. A Shell runs commands
   inside its own task, so its task name is not the executable's name. */
static int process_program_name(const struct Process *owner, char name[DOS_NAME_CAP],
    char *current_dir)
{
    const struct CommandLineInterface *cli = BADDR(owner->pr_CLI);
    const char *task_name = owner->pr_Task.tc_Node.ln_Name;
    if (current_dir) current_dir[0] = 0;
    if (cli && cli->cli_Module) {
        copy_cli_name(name,cli->cli_CommandName);
        if (current_dir && name[0] && !strchr(name,':'))
            copy_cli_name(current_dir,cli->cli_SetName);
        if (name[0]) return 1;
    }
    if (!task_name || strlen(task_name) >= DOS_NAME_CAP) return 0;
    strcpy(name,task_name);
    return name[0] != 0;
}

static int program_path_matches(const WindowProgram *owner, const char *path)
{
    return !Stricmp(FilePart((STRPTR)owner->name),FilePart((STRPTR)path)) &&
        same_directory(owner->directory,executable_directory(path));
}

static void discover_path(ProgramPoll *poll, const char *path)
{
    DockImage source = {0};
    char absolute[PATH_CAP];
    int i;
    if (!strchr(path,':') || !canonical_path(path,absolute)) return;
    for (i = 0; i < poll->count; ++i)
        if (!Stricmp((STRPTR)poll->pins[i].path,(STRPTR)absolute)) return;
    if (!load_icon(poll->app,absolute,&source,NULL)) return;
    if (dock_cache_build(&poll->new_cache,&source,&poll->settings))
        strcpy(poll->new_path,absolute);
    free(source.pixels);
}

static void discover_program(ProgramPoll *poll, const WindowProgram *owner, const char *path)
{
    if (strchr(path,':') && program_path_matches(owner,path)) discover_path(poll,path);
}

/* A window a user can see as an application: it has a layer, is not a
   backdrop, is at least 16x16 and has a title, close gadget or drag bar.
   Call with IBase locked. */
static int visible_app_window(const struct Window *window)
{
    return window->WLayer && !(window->Flags & WFLG_BACKDROP) &&
        window->Width >= 16 && window->Height >= 16 &&
        ((window->Title && window->Title[0]) ||
         (window->Flags & (WFLG_CLOSEGADGET|WFLG_DRAGBAR)));
}

static int pin_by_id(const ProgramPoll *poll, uint32_t pin_id)
{
    int i;
    for (i = 0; i < poll->count; ++i) if (poll->pins[i].pin_id == pin_id) return i;
    return -1;
}

static void mark_owner(ProgramPoll *poll, int pin, const WindowProgram *owner)
{
    poll->pins[pin].running |= RUNNING_WINDOW;
    if (!poll->pins[pin].owner) poll->pins[pin].owner = &owner->process->pr_Task;
}

/* An app no pin explains, shown with the default tool icon and its task
   name. The name, without a volume, is the temporary item's path. */
static void discover_unnamed(ProgramPoll *poll, const WindowProgram *owner)
{
    DockImage source = {0};
    if (!owner->task_name[0] || strchr(owner->task_name,':') ||
        !load_icon(poll->app,owner->task_name,&source,NULL)) return;
    if (dock_cache_build(&poll->new_cache,&source,&poll->settings))
        strcpy(poll->new_path,owner->task_name);
    free(source.pixels);
}

/* Every process that shows a visible window is an app; background tasks
   are not. In order, an app belongs to: the pin it was attributed to, a
   pinned program with its path, the pin launched just before it appeared,
   a temporary icon for its program file, or a temporary default icon. */
static void poll_window_programs(ProgramPoll *poll, const struct List *programs)
{
    WindowProgram *owners = poll->owners;
    struct Screen *screen;
    const struct MsgPort *desktop;
    const struct Task *desktop_task;
    const DockLaunch *launch = NULL;
    int count = 0, i, k;
    ULONG lock = LockIBase(0);
    Forbid();
    desktop = FindPort((STRPTR)"WORKBENCH");
    desktop_task = desktop ? desktop->mp_SigTask : NULL;
    for (screen = IntuitionBase->FirstScreen; screen && count < DOCK_MAX_ITEMS; screen = screen->NextScreen) {
        const struct Window *window;
        for (window = screen->FirstWindow; window && count < DOCK_MAX_ITEMS; window = window->NextWindow) {
            const struct Process *owner = window_process(window);
            WindowProgram *entry = &owners[count];
            const char *task_name;
            int existing;
            if (!owner || &owner->pr_Task == poll->dock_task ||
                &owner->pr_Task == desktop_task || !window->WLayer) continue;
            for (existing = 0; existing < count; ++existing)
                if (owners[existing].process == owner) break;
            if (existing < count) {
                owners[existing].visible |= visible_app_window(window);
                continue;
            }
            task_name = owner->pr_Task.tc_Node.ln_Name;
            entry->process = owner;
            entry->visible = visible_app_window(window);
            entry->directory = directory_identity(owner->pr_HomeDir);
            entry->task_name[0] = 0;
            if (task_name && strlen(task_name) < DOS_NAME_CAP) strcpy(entry->task_name,task_name);
            if (!process_program_name(owner,entry->name,entry->current_dir)) entry->name[0] = 0;
            ++count;
        }
    }
    Permit();
    UnlockIBase(lock);
    /* Attributions end when the task no longer shows a visible window. */
    for (k = 0; k < poll->attribution_count; ) {
        for (i = 0; i < count; ++i)
            if (&owners[i].process->pr_Task == poll->attributions[k].task && owners[i].visible) break;
        if (i < count && pin_by_id(poll,poll->attributions[k].pin_id) >= 0) ++k;
        else poll->attributions[k] = poll->attributions[--poll->attribution_count];
    }
    for (k = poll->launch_count-1; k >= 0 && !launch; --k)
        if (poll->now-poll->launches[k].at <= poll->launch_ticks) launch = &poll->launches[k];
    for (i = 0; i < count; ++i) {
        const WindowProgram *owner = &owners[i];
        const struct Task *task = &owner->process->pr_Task;
        const struct Node *node;
        int pin, pinned = 0, seen = 0;
        for (k = 0; k < poll->seen_count; ++k) if (poll->seen[k] == task) seen = 1;
        for (k = 0; k < poll->attribution_count && !pinned; ++k)
            if (poll->attributions[k].task == task &&
                (pin = pin_by_id(poll,poll->attributions[k].pin_id)) >= 0) {
                mark_owner(poll,pin,owner);
                poll->pins[pin].attributed = 1;
                pinned = 1;
            }
        for (pin = 0; pin < poll->count && !pinned; ++pin)
            if (poll->pins[pin].kind == PIN_PROGRAM && owner->name[0] &&
                strchr(poll->pins[pin].path,':') && owner->directory.handler &&
                program_path_matches(owner,poll->pins[pin].path)) {
                mark_owner(poll,pin,owner);
                pinned = 1;
            }
        if (!pinned && launch && !seen && owner->visible &&
            (pin = pin_by_id(poll,launch->pin_id)) >= 0 &&
            poll->attribution_count < DOCK_MAX_ITEMS) {
            poll->attributions[poll->attribution_count].task = task;
            poll->attributions[poll->attribution_count++].pin_id = launch->pin_id;
            mark_owner(poll,pin,owner);
            poll->pins[pin].attributed = 1;
            pinned = 1;
        }
        for (pin = 0; pin < poll->count && !pinned; ++pin)
            if (poll->pins[pin].temporary && !strchr(poll->pins[pin].path,':') &&
                !Stricmp((STRPTR)poll->pins[pin].path,(STRPTR)owner->task_name)) {
                mark_owner(poll,pin,owner);
                pinned = 1;
            }
        if (pinned || !owner->visible || !poll->allow_new || poll->new_path[0]) continue;
        if (owner->name[0] && owner->directory.handler) {
            if (programs)
                for (node = programs->lh_Head; node->ln_Succ && !poll->new_path[0]; node = node->ln_Succ)
                    if (node->ln_Name) discover_program(poll,owner,node->ln_Name);
            if (!poll->new_path[0] && strchr(owner->name,':')) discover_program(poll,owner,owner->name);
            else if (!poll->new_path[0] && owner->current_dir[0]) {
                char path[PATH_CAP];
                strcpy(path,owner->current_dir);
                if (AddPart((STRPTR)path,(STRPTR)owner->name,sizeof(path)))
                    discover_program(poll,owner,path);
                if (!poll->new_path[0] && !strchr(owner->name,'/')) {
                    strcpy(path,"C:");
                    if (AddPart((STRPTR)path,(STRPTR)owner->name,sizeof(path)))
                        discover_program(poll,owner,path);
                }
            }
        }
        if (!poll->new_path[0]) discover_unnamed(poll,owner);
    }
    for (poll->seen_count = 0; poll->seen_count < count; ++poll->seen_count)
        poll->seen[poll->seen_count] = &owners[poll->seen_count].process->pr_Task;
}

/* Document-only polls skip the window pass: keep attributed pins running
   while their task still shows a visible window. */
static void apply_attributions(ProgramPoll *poll)
{
    int k;
    for (k = 0; k < poll->attribution_count; ++k) {
        const struct Screen *screen;
        int pin = pin_by_id(poll,poll->attributions[k].pin_id), shown = 0;
        ULONG lock;
        if (pin < 0 || poll->pins[pin].attributed) continue;
        lock = LockIBase(0);
        Forbid();
        for (screen = IntuitionBase->FirstScreen; screen && !shown; screen = screen->NextScreen) {
            const struct Window *window;
            for (window = screen->FirstWindow; window && !shown; window = window->NextWindow)
                shown = window_process(window) &&
                    &window_process(window)->pr_Task == poll->attributions[k].task &&
                    visible_app_window(window);
        }
        Permit();
        UnlockIBase(lock);
        if (!shown) continue;
        poll->pins[pin].attributed = 1;
        poll->pins[pin].owner = poll->attributions[k].task;
    }
}

enum { WINDOW_CHECK, WINDOW_CLOSE, WINDOW_FOCUS };

/* Call with IBase locked. */
static int window_listed(const struct Window *target)
{
    const struct Screen *screen;
    const struct Window *window;
    for (screen = IntuitionBase->FirstScreen; screen; screen = screen->NextScreen)
        for (window = screen->FirstWindow; window; window = window->NextWindow)
            if (window == target) return 1;
    return 0;
}

enum { WINDOW_CALL_SCREEN, WINDOW_CALL_FRONT, WINDOW_CALL_ACTIVATE, WINDOW_CALL_CLOSE };

/* Revalidate under IBase immediately before each Intuition call, never while
   holding it. Forbid keeps the owner from closing the window until Intuition
   has the request. A call that waits cannot invalidate the next call's check. */
static int window_call(struct Window *window, int call)
{
    enum { SEND_IDCMP_CLOSE = 3 };
    ULONG lock = LockIBase(0);
    int live;
    Forbid();
    live = window_listed(window);
    UnlockIBase(lock);
    if (live) {
        if (call == WINDOW_CALL_SCREEN) ScreenToFront(window->WScreen);
        else if (call == WINDOW_CALL_FRONT) WindowToFront(window);
        else if (call == WINDOW_CALL_ACTIVATE) ActivateWindow(window);
        else amiga_window_action(window,SEND_IDCMP_CLOSE,NULL);
    }
    Permit();
    return live;
}

static void focus_window(struct Window *window)
{
    if (window_call(window,WINDOW_CALL_SCREEN) && window_call(window,WINDOW_CALL_FRONT))
        window_call(window,WINDOW_CALL_ACTIVATE);
}

/* Use the worker's console probe. Prefer the active Shell, then layer order
   on the Dock's screen. No DOS packet is sent from the main task. */
static ShellTarget shell_menu_target(const App *app)
{
    struct Screen *screen = app->screen;
    ShellTarget targets[DOCK_MAX_ITEMS], chosen = {0};
    const struct Window *active = IntuitionBase->ActiveWindow;
    const struct Layer *layer;
    int count = 0, i;
    Forbid();
    for (i = 0; i < app->shell_count; ++i)
        if (shell_target_live(&app->shells[i])) targets[count++] = app->shells[i];
    Permit();
    for (i = 0; i < count; ++i) if (targets[i].window == active) return targets[i];
    if (!count || !AttemptSemaphore(&screen->LayerInfo.Lock)) return chosen;
    for (layer = screen->LayerInfo.top_layer; layer && !chosen.window; layer = layer->back)
        for (i = 0; i < count; ++i)
            if (layer->Window == targets[i].window) { chosen = targets[i]; break; }
    ReleaseSemaphore(&screen->LayerInfo.Lock);
    return chosen;
}

static int show_shell_window(const App *app, const ShellTarget *selected)
{
    struct Window *target = selected->window;
    ULONG lock;
    int i, live;
    if (!target) {
        Forbid();
        for (i = 0; i < app->shell_count && !target; ++i)
            if (shell_target_live(&app->shells[i])) target = app->shells[i].window;
        Permit();
        if (!target) return 0;
    }
    lock = LockIBase(0);
    Forbid();
    live = (!selected->window || shell_target_live(selected)) && window_listed(target);
    UnlockIBase(lock);
    Permit();
    if (live) focus_window(target);
    return live;
}

static InputHandlerCode apollo_console_close_handler(const App *app)
{
    const struct Library *console;
    InputHandlerCode code = NULL;
    enum { CD_INPUT_HANDLER_LVO = 42 };
    /* The close dispatch is specific to the audited Apollo implementations
       (caps.apollo_console). Validate the installed handler against
       console.device's own vector. */
    if (!app->caps.apollo_console) return NULL;
    Forbid();
    console = (const struct Library *)FindName(&SysBase->DeviceList,(STRPTR)"console.device");
    if (console && console->lib_NegSize >= CD_INPUT_HANDLER_LVO) {
        const unsigned char *vector = (const unsigned char *)console-CD_INPUT_HANDLER_LVO;
        if (vector[0] == 0x4e && vector[1] == 0xf9) memcpy(&code,vector+2,sizeof(code));
    }
    Permit();
    return code;
}

/* Validate and activate on the main task so input.device never blocks.
   Activation is asynchronous; wait up to 200 ms for it. The handler then
   delivers the close only if the window is still the active one. */
static int prepare_shell_close(const ShellTarget *target)
{
    int attempt;
    for (attempt = 0; attempt < 10; ++attempt) {
        ULONG lock = LockIBase(0);
        struct Window *window;
        int live, active;
        Forbid();
        live = shell_target_live(target);
        window = IntuitionBase->FirstScreen ? IntuitionBase->FirstScreen->FirstWindow : NULL;
        for (; window; window = window->NextWindow)
            if (window == target->window) break;
        live = live && window && !window->FirstRequest && (window->Flags & WFLG_CLOSEGADGET);
        active = live && IntuitionBase->ActiveWindow == window;
        UnlockIBase(lock);
        if (live && !active) ActivateWindow(window);
        Permit();
        if (!live) return 0;
        if (active) return 1;
        Delay(1);
    }
    return 0;
}

static int quit_shell_window(App *app)
{
    ShellClose close = {app->menu_shell,&app->input_handler,apollo_console_close_handler(app),0};
    struct InputEvent event = {0};
    LONG error;
    if (!close.target.window || !close.target.cooked || !close.code ||
        !prepare_shell_close(&close.target)) return 0;
    event.ie_EventAddress = &close;
    app->input_context.shell_close = &close;
    app->input_io->io_Command = IND_WRITEEVENT;
    app->input_io->io_Data = &event;
    app->input_io->io_Length = sizeof(event);
    error = DoIO((struct IORequest *)app->input_io);
    app->input_context.shell_close = NULL;
    return !error && close.sent;
}

static int document_window_matches(const App *app, const Item *item,
    const char *title, const char *alias)
{
    int i;
    if (!title || (!title_names_document(title,item->path,strlen(item->path)) &&
        (!alias || !title_names_document(title,alias,strlen(alias))))) return 0;
    /* A title that could refer to two pins is not a safe close target. */
    for (i = 0; i < app->count; ++i) {
        const Item *other = &app->items[i];
        if (other != item && other->kind == PIN_DOCUMENT &&
            (title_names_document(title,other->path,strlen(other->path)) ||
             (other->title_path[0] &&
              title_names_document(title,other->title_path,strlen(other->title_path))))) return 0;
    }
    return 1;
}

static int item_window_action(const App *app, const Item *item, int action)
{
    DirectoryIdentity directory = {0};
    const char *name = NULL, *alias = NULL;
    char canonical[PATH_CAP];
    struct Window *found = NULL;
    struct Screen *screen;
    ULONG lock;
    int matches = 0;
    if (action != WINDOW_FOCUS && !app->caps.window_action) return 0;
    if (item->owner) {
        /* The task known to show this item's window, whatever its name. */
    } else if (item->kind == PIN_PROGRAM) {
        directory = executable_directory(item->path);
        if (!directory.handler) return 0;
        name = (const char *)FilePart((STRPTR)item->path);
    } else if (item->kind != PIN_DOCUMENT || !strchr(item->path,':')) return 0;
    else if (item->title_path[0] && canonical_path(item->title_path,canonical) &&
        !Stricmp((STRPTR)canonical,(STRPTR)item->path)) alias = item->title_path;
    lock = LockIBase(0);
    Forbid();
    for (screen = IntuitionBase->FirstScreen; screen; screen = screen->NextScreen) {
        struct Window *window;
        for (window = screen->FirstWindow; window; window = window->NextWindow) {
            if (item->owner) {
                const struct Process *owner = window_process(window);
                if (!owner || &owner->pr_Task != item->owner || !visible_app_window(window)) continue;
            } else if (name) {
                const struct Process *owner = window_process(window);
                char command[DOS_NAME_CAP];
                if (!owner || !process_program_name(owner,command,NULL) ||
                    Stricmp((STRPTR)name,FilePart((STRPTR)command)) ||
                    !same_directory(directory,directory_identity(owner->pr_HomeDir))) continue;
            } else if (!document_window_matches(app,item,(const char *)window->Title,alias)) continue;
            ++matches;
            if (!found && (action == WINDOW_FOCUS || (window->UserPort &&
                (window->IDCMPFlags & IDCMP_CLOSEWINDOW) && (window->Flags & WFLG_CLOSEGADGET) &&
                !window->FirstRequest))) found = window;
        }
    }
    UnlockIBase(lock);
    Permit();
    /* Each call below revalidates the window before it runs. */
    if (found && action == WINDOW_FOCUS) focus_window(found);
    else if (found && matches == 1 && action == WINDOW_CLOSE) window_call(found,WINDOW_CALL_CLOSE);
    return found && (action == WINDOW_FOCUS || matches == 1);
}

static LONG program_worker_entry(void)
{
    struct Process *self = (struct Process *)FindTask(NULL);
    for (;;) {
        ProgramPoll *poll;
        WaitPort(&self->pr_MsgPort);
        poll = (ProgramPoll *)GetMsg(&self->pr_MsgPort);
        if (!poll) continue;
        if (poll->stop) {
            /* Return to DOS before the caller can unload our program code. */
            Forbid();
            ReplyMsg(&poll->message);
            return 0;
        }
        poll->valid = poll->unavailable = 0;
        if (poll->query & PIN_PROGRAM)
            poll_workbench_list(poll,PIN_PROGRAM,WBCTRLA_GetProgramList,WBCTRLA_FreeProgramList);
        if (poll->query & PIN_DRAWER)
            poll_workbench_list(poll,PIN_DRAWER,WBCTRLA_GetOpenDrawerList,WBCTRLA_FreeOpenDrawerList);
        if (poll->query & PIN_DOCUMENT) poll_document_windows(poll);
        if (poll->query & PIN_SHELL) poll_shells(poll);
        apply_attributions(poll);
        ReplyMsg(&poll->message);
    }
}

/* Native notifications include our tooltip and popup. Their windows do not
   change the Workbench objects being tracked. */
static uint32_t status_window_stamp(const App *app)
{
    struct Screen *screen;
    ULONG lock = LockIBase(0);
    uint32_t stamp = UINT32_C(2166136261);
    for (screen = IntuitionBase->FirstScreen; screen; screen = screen->NextScreen) {
        const struct Window *window;
        for (window = screen->FirstWindow; window; window = window->NextWindow) {
            if (window == app->window || dock_ui_owns_window(app->ui,window) ||
                amiga_drag_preview_owns(app->drag_preview,window)) continue;
            stamp = (stamp ^ (uintptr_t)window) * UINT32_C(16777619);
            stamp = (stamp ^ (uintptr_t)window->UserPort) * UINT32_C(16777619);
        }
    }
    UnlockIBase(lock);
    return stamp;
}

static void program_poll_open(App *app)
{
    if (!app->live_open) return;
    app->program_replies = CreateMsgPort();
    if (!app->program_replies) return;
    app->program_worker = CreateNewProcTags(NP_Entry,(ULONG)program_worker_entry,
        NP_Name,(ULONG)"DockAwesome Workbench status",NP_Priority,0,
        NP_StackSize,49152,NP_CopyVars,FALSE,NP_WindowPtr,(ULONG)-1,TAG_DONE);
    app->program_poll.app = app;
    app->program_poll.dock_task = FindTask(NULL);
    app->program_poll.message.mn_ReplyPort = app->program_replies;
    app->program_poll.message.mn_Length = offsetof(ProgramPoll,owners);
    if (app->program_worker) app->status_window_stamp = status_window_stamp(app);
    if (app->program_worker && app->caps.status_notify) {
        LONG bit = AllocSignal(-1);
        if (bit >= 0) {
            enum { AFTER_OPENWINDOW = 1<<4, AFTER_CLOSEWINDOW = 1<<10 };
            struct TagItem tags[] = {
                {TAG_USER+2,AFTER_OPENWINDOW|AFTER_CLOSEWINDOW},
                {TAG_USER+4,(ULONG)FindTask(NULL)}, {TAG_USER+5,(ULONG)bit}, {TAG_DONE,0}
            };
            app->status_notify = amiga_start_status_notify(tags);
            if (app->status_notify) app->status_notify_bit = bit;
            else FreeSignal(bit);
        }
    }
}

static void status_changed(App *app)
{
    app->status_event_at = profile_clock()+app->clock_hz/5;
    /* A window can close before Workbench removes its process record. */
    app->status_recheck = 2;
}

static void status_observe_windows(App *app)
{
    uint32_t stamp = status_window_stamp(app);
    if (stamp == app->status_window_stamp) return;
    app->status_window_stamp = stamp;
    if (app->profile) ++app->stats.status_events;
    status_changed(app);
}

static int room_for_running_icon(const App *app)
{
    return dock_items_fit(&app->settings,app->count+1,viewport_limit(app,app->edge));
}

static void program_poll_schedule(App *app)
{
    ProgramPoll *poll = &app->program_poll;
    uint64_t now;
    int i, query = 0, kinds = PIN_PROGRAM;
    if (!app->program_worker || app->program_poll_pending ||
        animation_needed(app) ||
        app->gesture.active || buttons_engaged(app,ANY_BUTTON) || dock_ui_busy(app->ui) ||
        IntuitionBase->FirstScreen != app->screen) return;
    now = profile_clock();
    /* Classic Intuition has no native notifications. A cheap window-list
       comparison replaces the per-second Workbench round trips there. */
    if (!app->status_notify && now >= app->status_watch_at) {
        app->status_watch_at = now+app->clock_hz/2;
        status_observe_windows(app);
    }
    if (now < app->program_poll_at && now < app->document_poll_at &&
        (!app->status_recheck || now < app->status_event_at) &&
        (!app->discovery_at || now < app->discovery_at) && !app->shell_probe_wanted) return;
    for (i = 0; i < app->count; ++i) kinds |= app->items[i].kind;
    if (app->shell_probe_wanted) query |= kinds & PIN_SHELL;
    if (now >= app->program_poll_at || (app->status_recheck && now >= app->status_event_at)) {
        query |= kinds;
        app->program_poll_at = now+(uint64_t)app->clock_hz*8;
        if (app->status_recheck) {
            --app->status_recheck;
            app->status_event_at = now+(uint64_t)app->clock_hz*3/4;
        }
    }
    if (app->discovery_at && now >= app->discovery_at) {
        query |= PIN_PROGRAM;
        app->discovery_at = 0;
    }
    if (now >= app->document_poll_at) query |= kinds & PIN_DOCUMENT;
    if (now >= app->document_poll_at || (query & PIN_DOCUMENT)) app->document_poll_at = now+app->clock_hz;
    query &= ~app->status_unavailable;
    if (!query) return;
    app->program_poll_started = now;
    poll->count = app->count;
    poll->query = query;
    poll->settings = app->settings;
    poll->allow_new = room_for_running_icon(app);
    poll->new_path[0] = 0;
    for (i = 0; i < poll->count; ++i) {
        strcpy(poll->pins[i].path,app->items[i].path);
        strcpy(poll->pins[i].title_path,app->items[i].title_path);
        poll->pins[i].pin_id = app->items[i].pin_id;
        poll->pins[i].running = app->items[i].kind == PIN_PROGRAM ?
            app->items[i].running & RUNNING_WORKBENCH : 0;
        poll->pins[i].kind = app->items[i].kind;
        poll->pins[i].temporary = app->items[i].temporary;
        poll->pins[i].attributed = 0;
        poll->pins[i].owner = NULL;
    }
    poll->now = now;
    poll->launch_ticks = (uint64_t)app->clock_hz*LAUNCH_SECONDS;
    poll->launch_count = app->launch_count;
    memcpy(poll->launches,app->launches,sizeof(poll->launches));
    poll->probe_consoles = app->shell_probe_wanted && (query & PIN_SHELL);
    app->shell_probe_wanted = 0;
    app->program_poll_pending = 1;
    PutMsg(&app->program_worker->pr_MsgPort,&poll->message);
}

static int program_poll_result(App *app, int *recreate)
{
    ProgramPoll *poll = &app->program_poll;
    int i, before = app->count, changed = 0, layout_changed = 0, added = -1;
    if (!app->program_replies || !GetMsg(app->program_replies)) return 0;
    app->program_poll_pending = 0;
    app->status_unavailable |= poll->unavailable;
    if ((poll->valid & PIN_SHELL) && poll->probed) {
        memcpy(app->shells,poll->shells,sizeof(app->shells));
        app->shell_count = poll->shell_count;
        app->shells_at = profile_clock();
    }
    if (poll->new_path[0]) app->discovery_at = profile_clock()+app->clock_hz/5;
    for (i = 0; i < poll->count; ++i) {
        int pin, running;
        if (!(poll->valid & poll->pins[i].kind) && !poll->pins[i].attributed) continue;
        pin = pin_index(app,poll->pins[i].pin_id);
        if (pin < 0) continue;
        running = poll->pins[i].running || poll->pins[i].attributed;
        app->items[pin].owner = running ? poll->pins[i].owner : NULL;
        if (poll->pins[i].kind == PIN_DOCUMENT)
            strcpy(app->items[pin].title_path,poll->pins[i].title_path);
        if (!!app->items[pin].running != !!running) changed = 1;
        app->items[pin].running = running;
    }
    if (!*recreate && !dock_ui_busy(app->ui) && !app->gesture.active &&
        !buttons_engaged(app,IEQUALIFIER_LEFTBUTTON|IEQUALIFIER_RBUTTON)) {
        if (poll->valid & PIN_PROGRAM) for (i = app->count-1; i >= 0; --i) {
            if (app->items[i].temporary && !app->items[i].running) {
                erase_item(app,i);
                layout_changed = 1;
            }
        }
        if (poll->new_path[0] && room_for_running_icon(app) &&
            app->settings.base_size == poll->settings.base_size &&
            app->settings.max_size == poll->settings.max_size && app->settings.levels == poll->settings.levels &&
            item_path_index(app,poll->new_path) < 0) {
            Item *item;
            input_invalidate(app);
            item = append_item(app,poll->new_path,PIN_PROGRAM,&poll->new_cache);
            item->running = RUNNING_WINDOW;
            item->temporary = 1;
            added = app->count-1;
            layout_changed = 1;
        }
        /* Like drops and unpins: a new icon appears in its slot and the rest
           glide to the new row. Snapping the whole row made the dock jump
           when a temporary icon's program quit. */
        if (layout_changed) {
            *recreate = !rest_after_pin_change(app,before);
            if (!*recreate && added >= 0) app->placement[added] = app->target[added];
        }
    }
    changed |= layout_changed;
    dock_cache_free(&poll->new_cache);
    if (app->profile) {
        uint64_t ticks = profile_clock()-app->program_poll_started;
        ++app->stats.program_polls;
        if (poll->query & PIN_PROGRAM) ++app->stats.program_checks;
        if (poll->query & PIN_DRAWER) ++app->stats.drawer_checks;
        if (poll->query & PIN_DOCUMENT) ++app->stats.document_checks;
        app->stats.program_changes += changed;
        app->stats.program_poll_ticks += ticks;
        if (ticks > app->stats.program_poll_peak) app->stats.program_poll_peak = ticks;
    }
    return changed;
}

static void program_poll_close(App *app)
{
    ProgramPoll *poll = &app->program_poll;
    if (app->status_notify) {
        /* Intuition signals this task until the notification ends, so the
           process must not exit before then: waiting is safer than a crash.
           Back off after 5 s instead of spinning. */
        int attempt;
        for (attempt = 0; !amiga_end_status_notify(app->status_notify); ++attempt)
            Delay(attempt < 250 ? 1 : 25);
        FreeSignal(app->status_notify_bit);
        app->status_notify = NULL;
    }
    if (app->program_worker) {
        if (app->program_poll_pending) {
            WaitPort(app->program_replies);
            GetMsg(app->program_replies);
        }
        poll->stop = 1;
        PutMsg(&app->program_worker->pr_MsgPort,&poll->message);
        WaitPort(app->program_replies);
        GetMsg(app->program_replies);
    }
    if (app->program_replies) DeleteMsgPort(app->program_replies);
    dock_cache_free(&poll->new_cache);
}

static void live_cancel(App *app)
{
    if (!app->live_pending) return;
    if (!CheckIO((struct IORequest *)app->live_io)) AbortIO((struct IORequest *)app->live_io);
    WaitIO((struct IORequest *)app->live_io);
    app->live_pending = 0;
    SetSignal(0, 1UL<<app->live_port->mp_SigBit);
}

static void live_schedule(App *app)
{
    int fast;
    if (!app->live_open || app->live_failed ||
        (!app->pip_mode && !app->program_worker)) return;
    fast = app->pip_mode && (animation_needed(app) || buttons_engaged(app,ANY_BUTTON));
    if (app->live_pending && fast && !app->live_fast) live_cancel(app);
    if (app->live_pending) return;
    app->live_io->tr_node.io_Command = TR_ADDREQUEST;
    app->live_io->tr_node.io_Flags = 0;
    app->live_io->tr_time.tv_secs = !app->pip_mode;
    app->live_io->tr_time.tv_micro = !app->pip_mode ? 0 : fast ? 20000 : 100000;
    app->live_fast = fast;
    SendIO((struct IORequest *)app->live_io); app->live_pending = 1;
}

static int live_due(App *app)
{
    if (!app->live_pending || !CheckIO((struct IORequest *)app->live_io)) return 0;
    app->live_pending = 0;
    if (WaitIO((struct IORequest *)app->live_io)) {
        if (app->pip_mode) {
            app->failed = 1; strcpy(app->surface_error, "Live background timer failed.");
        } else app->live_failed = 1;
        return 0;
    }
    return 1;
}

static void drop_catcher_flags(App *app, unsigned int flags)
{
    if (flags & AMIGA_DROP_SCANNED) ++app->stats.desktop_scans;
    if (flags & (AMIGA_DROP_STARTED|AMIGA_DROP_RESTORED)) ++app->stats.shape_updates;
    if (flags & AMIGA_DROP_STARTED) {
        /* The receiver backfill restores the underlying pixels while
           Layers exposes the region. No app-side layer access is needed. */
        app->drag_reference = 0;
    }
    if (flags & AMIGA_DROP_ERROR) {
        app->failed = 1;
        strcpy(app->surface_error, "Cannot restore the external drop surface.");
    }
    /* The native receiver has no artwork. Reshaping it does not change
       the PiP frame. Changes to the insertion slot or drag mask redraw it. */
}

static void update_drop_catcher(App *app)
{
    struct Screen *screen;
    ULONG serial;
    int x, y, held, dragged, cancelled, up_x, up_y, up_known;
    unsigned int flags;
    if (!app->drop_catcher) return;
    Disable();
    serial = app->input_context.external_serial;
    screen = app->input_context.external_screen;
    x = app->input_context.external_x; y = app->input_context.external_y;
    held = app->input_context.external_held;
    dragged = app->input_context.external_dragged;
    cancelled = app->input_context.external_cancelled;
    up_known = app->input_context.external_up_known;
    up_x = app->input_context.external_up_x; up_y = app->input_context.external_up_y;
    Enable();
    if (cancelled || app->gesture.active || app->input_context.held || dock_ui_busy(app->ui))
        flags = amiga_drop_catcher_cancel(app->drop_catcher);
    else flags = amiga_drop_catcher_update(app->drop_catcher, serial, screen, x, y, held, dragged,
        up_known, up_x, up_y, PeekQualifier(), profile_clock(), app->clock_hz);
    drop_catcher_flags(app, flags);
}

static int update_drag_mask(App *app, int sample)
{
    enum { DRAG_RADIUS = 128 };
    int x = app->screen->MouseX-app->left, y = app->screen->MouseY-app->top;
    int left = x-DRAG_RADIUS, top = y-DRAG_RADIUS;
    int right = x+DRAG_RADIUS+1, bottom = y+DRAG_RADIUS+1, row;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > app->window->Width) right = app->window->Width;
    if (bottom > app->height) bottom = app->height;
    if (amiga_drop_catcher_active(app->drop_catcher) && app->input_context.external_held &&
        app->input_context.external_dragged && right > left && bottom > top) {
        int changed, reference_changed = 0;
        if (!sample) return 0;
        if (!app->drag_reference) {
            if (amiga_display_drag_reference565(app->display,
                    amiga_drop_catcher_backing(app->drop_catcher),app->source565,app->window->Width)) {
                app->drag_reference = 1;
                app->renderer.pixels->decode565(app->backdrop.pixels,app->source565,
                    (unsigned long)app->width*app->height);
                dock_renderer_backdrop_changed(&app->renderer);
                reference_changed = 1;
            } else app->drag_reference = -1;
        }
        if (app->drag_reference > 0) {
            changed = amiga_display_drag_mask565(app->display,app->source565,app->drag_mask,
                &app->drag_box,app->left,app->top,left,top,right,bottom,app->renderer.pixels->bytes_equal);
            if (changed >= 0) {
                app->drag_mask_live = 1;
                return changed || reference_changed;
            }
        }
    }
    if (!app->drag_mask_live) return 0;
    for (row = app->drag_box.top; row < app->drag_box.bottom; ++row)
        memset(app->drag_mask+(size_t)row*app->width+app->drag_box.left,0,
            (size_t)(app->drag_box.right-app->drag_box.left));
    app->drag_box = (DockRect){0,0,0,0};
    app->drag_mask_live = 0;
    return 1;
}

/* A UI requester (ASL) blocks the main loop: hide PiP and let input pass
   through until it returns. The next loop pass presents PiP again. */
static void ui_blocking(void *context, int blocking)
{
    App *app = (App *)context;
    if (!blocking) return;
    interaction_cancel(app);
    amiga_pip_suspend(app->pip); app->pip_visible = 0; input_publish(app);
}

/* One cache level per pixel from Size to the zoom size, within limits. */
static int zoom_levels(int base_size, int max_size)
{
    int levels = max_size-base_size+1;
    return levels < 2 ? 2 : levels > DOCK_MAX_LEVELS ? DOCK_MAX_LEVELS : levels;
}

static int displayed_scale(const App *app);

/* The settings Save stores for a draft at this Size, and the scale it saves
   (0 keeps the pixel zoom). Returns 0 when the zoom exceeds 192 px. */
static int draft_settings(const App *app, const DockUIPrefs *draft, int size,
                          DockSettings *out, int *scale)
{
    const DockSettings *previous = &app->settings;
    DockSettings next = *previous;
    int percent = draft->scale_percent;
    next.base_size = size;
    next.end_gap = draft->folder[0] ? STACK_DIVIDER_GAP : 0;
    next.max_size = dock_scaled_size(size,percent);
    if (!app->scale_percent && size == previous->base_size && percent == displayed_scale(app) &&
        next.max_size != previous->max_size) {
        /* Some large pixel sizes cannot be represented by a two-decimal factor. */
        next.max_size = previous->max_size;
        percent = 0;
    }
    if (next.max_size > DOCK_MAX_ICON_SIZE) return 0;
    /* Unchanged sizes keep their levels, which may come from --levels. */
    if (next.base_size != previous->base_size || next.max_size != previous->max_size)
        next.levels = zoom_levels(next.base_size,next.max_size);
    *out = next;
    if (scale) *scale = percent;
    return 1;
}

/* Whether the draft's row fits its edge and its cache fits the budget. */
static int draft_fits(const App *app, const DockUIPrefs *draft, const DockSettings *s)
{
    int has_stack = stack_index(app) < app->count;
    return draft->edge >= DOCK_EDGE_BOTTOM && draft->edge <= DOCK_EDGE_RIGHT &&
        dock_items_fit(s,app->count+!!draft->folder[0]-has_stack,
            viewport_limit(app,(DockEdge)draft->edge));
}

/* The largest Size that Save accepts for the draft's Scale, edge and stack. */
static int ui_fit(void *context, const DockUIPrefs *draft)
{
    const App *app = (const App *)context;
    DockSettings s;
    int size;
    for (size = DOCK_MAX_ICON_SIZE; size > DOCK_MIN_ICON_SIZE; --size)
        if (draft_settings(app,draft,size,&s,NULL) && draft_fits(app,draft,&s)) return size;
    return DOCK_MIN_ICON_SIZE;
}

/* The saved preferences as Settings shows them. */
static DockUIPrefs current_ui_prefs(const App *app)
{
    DockUIPrefs prefs = {app->settings.base_size,displayed_scale(app),app->edge,{0}};
    strcpy(prefs.folder,app->prefs.folder);
    return prefs;
}

static int pip_blocked_by_screen_or_menu(App *app)
{
    int menu = native_menu_held(app);
    if (IntuitionBase->FirstScreen == app->screen && !menu) return 0;
    if (app->profile && app->pip_visible && menu) ++app->stats.menu_suspends;
    interaction_cancel(app);
    amiga_pip_suspend(app->pip); app->pip_visible = 0; input_publish(app);
    return 1;
}

/* Bound every pixel the displayed or next frame can cover: the plate and
   each icon image along the row, and the whole cross axis for the dots and
   rim. Uncovered pixels show live scanout, so they need no fresh capture.
   A change inside the bound still refreshes the whole cache. Fall back to a
   full compare when artwork can leave the row. */
static int frame_check(const App *app, DockRect *out)
{
    enum { MARGIN = 4 };
    int i, low, high, limit, bottom = app->edge == DOCK_EDGE_BOTTOM;
    int visible = app->window->Width;
    DockRect plate;
    if (app->moving_item || app->gesture.active || app->renderer.center_panel ||
        !dock_edge_panel_rect(&app->settings,app->edge,app->placement,app->count,
            app->width,app->height,&plate)) return 0;
    low = bottom ? plate.left : plate.top;
    high = bottom ? plate.right : plate.bottom;
    for (i = 0; i < app->count; ++i) {
        const DockPlacement *p = &app->placement[i];
        const DockImage *im = &app->items[i].cache.image[p->level];
        int start = bottom ? p->x : p->y;
        int extent = bottom ? im->width : im->height;
        if (extent < p->size) extent = p->size;
        if (start < low) low = start;
        if (start+extent > high) high = start+extent;
    }
    limit = bottom ? visible : app->height;
    low -= MARGIN; high += MARGIN;
    if (low < 0) low = 0;
    if (high > limit) high = limit;
    if (low >= high) return 0;
    *out = bottom ? (DockRect){low,0,high,app->height} : (DockRect){0,low,visible,high};
    return 1;
}

static int live_capture(App *app, int full_compare)
{
    DockRect clip, plate, box;
    const DockRect *check = NULL;
    int result, moved, screen_left, screen_top, dialog_moved, force;
    uint64_t started;
    if (!app->pip_mode) return 0;
    if (pip_blocked_by_screen_or_menu(app)) return 0;
    /* Native Workbench can retain layer locks through its drag loop. Do not
       enter a layer/bitmap operation while its external gesture is active. */
    if (amiga_drop_catcher_active(app->drop_catcher)) return 0;
    /* FirstWindow is creation order, so inspect the locked layer Z order.
       Retry asynchronous depth requests at most four times a second: another
       window can move ahead between checks. */
    {
        struct Layer *layer;
        int obscured = 0;
        if (!AttemptSemaphore(&app->screen->LayerInfo.Lock)) return 0;
        clip = dialog_clip(app);
        for (layer = app->window->WLayer->front; layer; layer = layer->front)
            if (layer->Window && layer->Window != app->window &&
                !dock_ui_owns_window(app->ui,layer->Window) &&
                !amiga_drag_preview_owns(app->drag_preview,layer->Window)) { obscured = 1; break; }
        ReleaseSemaphore(&app->screen->LayerInfo.Lock);
        if (!obscured || dock_ui_busy(app->ui)) app->front_pending = 0;
        else {
            uint64_t now = profile_clock();
            if (!app->front_pending || now-app->front_requested >= app->clock_hz/4) {
                app->front_pending = 1;
                app->front_requested = now;
                ++app->stats.front_requests;
                WindowToFront(app->window);
            }
        }
    }
    screen_left = app->screen->LeftEdge; screen_top = app->screen->TopEdge;
    moved = app->screen_left != screen_left || app->screen_top != screen_top;
    app->screen_left = screen_left; app->screen_top = screen_top;
    /* At rest all artwork and dots fit on the plate. A change in unused
       zoom headroom can wait until a redraw needs those pixels. */
    if (!moved && !app->drag_mask_live && !dock_ui_busy(app->ui)) {
        if (!full_compare && !app->live_fast && !app->active &&
            dock_edge_panel_rect(&app->settings,app->edge,app->placement,app->count,
                app->width,app->height,&plate)) check = &plate;
        else if (frame_check(app,&plate)) check = &plate;
    }
    started = app->profile ? profile_clock() : 0;
    force = !app->source_valid || !app->pip_visible;
    result = amiga_display_capture565(app->display, app->source565,
        app->left, app->top, force, check, app->renderer.pixels->bytes_equal, &box);
    ++app->stats.source_polls;
    if (app->profile) app->stats.capture_ticks += profile_clock()-started;
    if (result < 0) {
        amiga_pip_suspend(app->pip); app->pip_visible = 0; input_publish(app); app->failed = 1;
        strcpy(app->surface_error, "PiP lost the RGB565 screen. Restart the dock on a supported screen.");
        return 0;
    }
    if (result) {
        uint64_t decode_started = app->profile ? profile_clock() : 0;
        /* Only the changed box is decoded and redrawn; whole rows decode at once. */
        size_t at = (size_t)box.top*app->width+box.left;
        int rows = box.bottom-box.top, span = box.right-box.left;
        ++app->stats.source_changes;
        app->stats.source_pixels += (uint64_t)rows*span;
        if (span == app->width) { span *= rows; rows = 1; }
        for (; rows > 0; --rows, at += app->width)
            app->renderer.pixels->decode565(app->backdrop.pixels+at*4, app->source565+at*2,
                (unsigned long)span);
        if (force) dock_renderer_backdrop_changed(&app->renderer);
        else dock_renderer_backdrop_damaged(&app->renderer, box);
        app->source_valid = 1;
        if (app->profile) app->stats.decode_ticks += profile_clock()-decode_started;
    }
    dialog_moved = memcmp(&app->dialog_clip,&clip,sizeof(clip)) != 0;
    app->dialog_clip = clip;
    return result || moved || dialog_moved;
}

static int animation_open(App *app)
{
    struct EClockVal clock;
    app->timer_port = CreateMsgPort();
    if (!app->timer_port) return 0;
    app->timer_io = (struct timerequest *)CreateIORequest(app->timer_port, sizeof(struct timerequest));
    if (!app->timer_io || OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)app->timer_io, 0)) return 0;
    app->timer_open = 1;
    TimerBase = app->timer_io->tr_node.io_Device;
    app->clock_hz = ReadEClock(&clock);
    app->started = ((uint64_t)clock.ev_hi<<32) | clock.ev_lo;
    app->vblank_bit = AllocSignal(-1);
    if (app->vblank_bit >= 0) {
        app->vblank.task = FindTask(NULL);
        app->vblank.mask = 1UL<<app->vblank_bit;
        app->vblank_interrupt.is_Node.ln_Type = NT_INTERRUPT;
        app->vblank_interrupt.is_Node.ln_Name = (char *)"DockAwesome vblank";
        app->vblank_interrupt.is_Data = &app->vblank;
        app->vblank_interrupt.is_Code = (void (*)())dock_vblank_entry;
        AddIntServer(INTB_VERTB,&app->vblank_interrupt);
        app->vblank_open = 1;
        app->vblank_check_at = profile_clock(); app->vblank_check_field = app->vblank.fields;
    }
    app->stats.min_ticks = UINT64_MAX;
    {
        app->live_port = CreateMsgPort();
        if (app->live_port)
            app->live_io = (struct timerequest *)CreateIORequest(app->live_port, sizeof(struct timerequest));
        if (app->live_io && !OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ,
                (struct IORequest *)app->live_io, 0)) app->live_open = 1;
        if (app->pip_mode && !app->live_open) return 0;
    }
    return app->clock_hz != 0;
}

/* Microseconds per event for a tick total over n events; 0 when n is 0. */
static unsigned long ticks_us(const App *app, uint64_t ticks, ULONG n)
{
    return n ? (unsigned long)(ticks*1000000/app->clock_hz/n) : 0;
}

static void profile_report(App *app)
{
    uint64_t elapsed;
    ULONG elapsed_ms, any;
    if (!app->profile || !app->timer_open || !app->clock_hz) return;
    elapsed = profile_clock()-app->started;
    elapsed_ms = (ULONG)(elapsed*1000/app->clock_hz);
    any = app->frames ? 1 : 0;
    printf("DockAwesome profile: backend=%s depth=%d canvas=%dx%d icons=%d\n"
           "elapsed_ms=%lu wakeups=%lu redraws=%lu render_us_avg=%lu min=%lu max=%lu\n"
           "compose_us_avg=%lu present_us_avg=%lu conversion_us_avg=%lu upload_us_avg=%lu\n"
           "Timing includes composition, conversion and OS upload; not CPU utilization.\n",
           app->renderer.pixels ? app->renderer.pixels->name : "none",
           amiga_display_depth(app->display), app->width, app->height, app->count,
           (unsigned long)elapsed_ms, (unsigned long)app->stats.wakes, (unsigned long)app->frames,
           ticks_us(app,app->stats.render_ticks,app->frames),
           ticks_us(app,app->stats.min_ticks,any), ticks_us(app,app->stats.max_ticks,any),
           ticks_us(app,app->stats.render_ticks-app->stats.present_ticks,app->frames),
           ticks_us(app,app->stats.present_ticks,app->frames),
           ticks_us(app,app->stats.conversion_ticks,app->frames),
           ticks_us(app,app->stats.upload_ticks,app->frames));
    fflush(stdout);
    {
        uint64_t canvas = (uint64_t)app->width*app->height;
        printf("Damage: frames=%lu full=%lu compose_pixels_avg=%lu%% conversion_pixels_avg=%lu%%\n",
            (unsigned long)app->stats.damage_frames, (unsigned long)app->stats.damage_full,
            (unsigned long)(app->stats.damage_frames && canvas ?
                app->stats.compose_pixels*100/(canvas*app->stats.damage_frames) : 0),
            (unsigned long)(app->stats.conversion_frames && canvas ?
                app->stats.conversion_pixels*100/(canvas*app->stats.conversion_frames) : 0));
    }
    printf("Frame pacing: one_field=%lu same_field=%lu stalled=%lu vsync=%d\n",
        (unsigned long)app->stats.pace_one, (unsigned long)app->stats.pace_same,
        (unsigned long)app->stats.pace_stall, app->vblank_open && !app->no_vsync);
    if (app->damage_check)
        printf("Damage check: frames=%lu compose_mismatch=%lu convert_mismatch=%lu\n",
            (unsigned long)app->stats.check_frames, (unsigned long)app->stats.compose_mismatch,
            (unsigned long)app->stats.convert_mismatch);
    printf("Workbench programs: polls=%lu changes=%lu roundtrip_us_avg=%lu max=%lu native_menu_suspends=%lu\n",
        (unsigned long)app->stats.program_polls, (unsigned long)app->stats.program_changes,
        ticks_us(app,app->stats.program_poll_ticks,app->stats.program_polls),
        ticks_us(app,app->stats.program_poll_peak,1), (unsigned long)app->stats.menu_suspends);
    printf("Status queries: programs=%lu drawers=%lu documents=%lu window_events=%lu native_notify=%d\n",
        (unsigned long)app->stats.program_checks,(unsigned long)app->stats.drawer_checks,
        (unsigned long)app->stats.document_checks,(unsigned long)app->stats.status_events,!!app->status_notify);
    printf("Workbench drops: messages=%lu arguments=%lu added=%lu reordered=%lu bad_paths=%lu\n",
        (unsigned long)app->stats.drop_messages, (unsigned long)app->stats.drop_arguments,
        (unsigned long)app->stats.drop_added, (unsigned long)app->stats.drop_reordered,
        (unsigned long)app->stats.drop_bad_paths);
    printf("Workbench desktop selection scans: %lu\n",(unsigned long)app->stats.desktop_scans);
    fflush(stdout);
    {
        int sample;
        printf("Animation settle_ms: count=%lu avg=%lu max=%lu samples=",
            (unsigned long)app->stats.settles,
            (unsigned long)(app->stats.settles ? app->stats.settle_ticks*1000/app->clock_hz/app->stats.settles : 0),
            (unsigned long)(app->stats.settle_peak*1000/app->clock_hz));
        for (sample = 0; sample < app->stats.settle_samples; ++sample)
            printf("%s%lu", sample ? "," : "", (unsigned long)app->stats.settle_ms[sample]);
        puts(" (main-task target update through final present; includes retargeting)");
        fflush(stdout);
    }
    if (app->pip_mode) {
        printf("PiP drag reveal: frames=%lu peak_pixels=%lu\n",
            (unsigned long)app->stats.drag_reveal_frames,(unsigned long)app->stats.drag_reveal_peak);
        printf("PiP popup reveal: frames=%lu\n",(unsigned long)app->stats.popup_reveal_frames);
        printf("PiP dialog reveal: frames=%lu\n",(unsigned long)app->stats.dialog_reveal_frames);
        printf("PiP2: polls=%lu source_changes=%lu front_requests=%lu capture_compare_us_avg=%lu decode_us_avg=%lu changed_px_avg=%lu (settled100ms, motion20ms)\n",
            (unsigned long)app->stats.source_polls, (unsigned long)app->stats.source_changes,
            (unsigned long)app->stats.front_requests,
            ticks_us(app,app->stats.capture_ticks,app->stats.source_polls),
            ticks_us(app,app->stats.decode_ticks,app->stats.source_changes),
            app->stats.source_changes ? (unsigned long)(app->stats.source_pixels/app->stats.source_changes) : 0UL);
        fflush(stdout);
        printf("PiP input: downs=%lu ups=%lu right_icons=%lu ambiguous_pass=%lu overflow_pass=%lu\n",
            (unsigned long)app->input_context.downs, (unsigned long)app->input_context.ups,
            (unsigned long)app->input_context.right_icons,
            (unsigned long)app->input_context.ambiguous, (unsigned long)app->input_context.overflow);
        fflush(stdout);
        printf("PiP shape: updates=%lu rects=%lu rects_peak=%lu\n",
            (unsigned long)app->stats.shape_updates, (unsigned long)app->stats.shape_rects,
            (unsigned long)app->stats.shape_rects_peak);
        fflush(stdout);
    }
}

static void profile_close(App *app)
{
    animation_cancel(app);
    if (app->vblank_open) {
        /* The server must be gone before the task and App go away. */
        RemIntServer(INTB_VERTB,&app->vblank_interrupt);
        app->vblank_open = 0;
    }
    if (app->vblank_bit >= 0) { FreeSignal(app->vblank_bit); app->vblank_bit = -1; }
    live_cancel(app);
    if (app->live_open) CloseDevice((struct IORequest *)app->live_io);
    if (app->live_io) DeleteIORequest((struct IORequest *)app->live_io);
    if (app->live_port) DeleteMsgPort(app->live_port);
    app->live_open = 0;
    if (app->timer_open) CloseDevice((struct IORequest *)app->timer_io);
    if (app->timer_io) DeleteIORequest((struct IORequest *)app->timer_io);
    if (app->timer_port) DeleteMsgPort(app->timer_port);
    TimerBase = NULL;
}

/* Coverage masks for one row span of a PiP conversion, applied to the
   scratch copy: the native drag image under a Workbench drag, the popup or
   an owned dialog over the dock, and the keyed tail past the window. */
typedef struct {
    App *app;
    DockRect clip;
    int popup, visible_width, count, ui_revealed;
    ULONG revealed;
} PipMask;

static void pip_mask(void *context, int y, int left, int right, unsigned char *span)
{
    PipMask *m = context;
    App *app = m->app;
    int width = app->width;
    if (app->drag_mask_live && y >= app->drag_box.top && y < app->drag_box.bottom) {
        const unsigned char *mask = app->drag_mask+(size_t)y*width;
        const unsigned char *first = memchr(mask+left,1,(size_t)(right-left));
        int x;
        if (first) for (x = (int)(first-mask); x < right; ++x)
            if (mask[x] && span[x-left]) { span[x-left] = 0; if (m->count) ++m->revealed; }
    }
    if (y >= m->clip.top && y < m->clip.bottom) {
        int a, b;
        if (m->popup) {
            dock_ui_popup_span(app->ui,y-m->clip.top,&a,&b);
            a += m->clip.left; b += m->clip.left;
        } else { a = m->clip.left; b = m->clip.right; }
        if (a < 0) a = 0;
        if (b > width) b = width;
        if (a < left) a = left;
        if (b > right) b = right;
        if (b > a) {
            if (m->count && !m->ui_revealed)
                m->ui_revealed = memchr(span+a-left,255,(size_t)(b-a)) != NULL;
            memset(span+a-left,0,(size_t)(b-a));
        }
    }
    if (m->visible_width < right) {
        int a = m->visible_width > left ? m->visible_width : left;
        memset(span+a-left,0,(size_t)(right-a));
    }
}

static void present_frame(void *context, const DockImage *frame)
{
    App *app = context;
    if (app->pip_mode) {
        PipMask mask;
        DockDamage *pending;
        int back, masked, stride;
        unsigned long pixels;
        unsigned char *buffer;
        uint64_t started = app->profile ? profile_clock() : 0, encoded, converted;
        if (pip_blocked_by_screen_or_menu(app)) return;
        encoded = app->profile ? profile_clock() : 0;
        buffer = amiga_pip_back_buffer(app->pip);
        back = amiga_pip_back_index(app->pip);
        stride = amiga_pip_stride(app->pip);
        pending = &app->pip_pending[back];
        memset(&mask,0,sizeof(mask));
        mask.app = app; mask.visible_width = app->window->Width; mask.count = app->profile;
        mask.popup = dock_ui_popup_bounds(app->ui,&mask.clip);
        if (mask.popup) {
            mask.clip.left -= app->left; mask.clip.right -= app->left;
            mask.clip.top -= app->top; mask.clip.bottom -= app->top;
        } else mask.clip = app->dialog_clip;
        /* A mask can change any covered pixel, so a masked frame converts
           whole, and so does the next conversion into a buffer that holds
           a masked frame. */
        masked = app->drag_mask_live || mask.popup ||
            (mask.clip.right > mask.clip.left && mask.clip.bottom > mask.clip.top);
        if (masked || app->pip_masked[back]) dock_damage_set_full(pending);
        pixels = dock_convert565_damage(buffer,stride,frame,app->coverage,pending,app->scratch,
            app->renderer.pixels->encode565,app->renderer.pixels->key565,pip_mask,&mask);
        if (app->profile) {
            ++app->stats.conversion_frames;
            app->stats.conversion_pixels += pixels;
        }
        /* This buffer now holds the current frame, whether or not the
           present below succeeds. */
        dock_damage_clear(pending);
        app->pip_masked[back] = masked;
        if (app->check565) {
            PipMask reference = mask;
            DockDamage whole;
            int y;
            reference.count = 0;
            dock_damage_set_full(&whole);
            dock_convert565_damage(app->check565,frame->width*2,frame,app->coverage,&whole,app->scratch,
                app->renderer.pixels->encode565,app->renderer.pixels->key565,pip_mask,&reference);
            for (y = 0; y < frame->height; ++y)
                if (memcmp(buffer+(size_t)y*stride,app->check565+(size_t)y*frame->width*2,(size_t)frame->width*2)) {
                    ++app->stats.convert_mismatch;
                    break;
                }
        }
        if (app->profile && mask.revealed) {
            ++app->stats.drag_reveal_frames;
            if (mask.revealed > app->stats.drag_reveal_peak) app->stats.drag_reveal_peak = mask.revealed;
        }
        converted = app->profile ? profile_clock() : 0;
        app->pip_visible = amiga_pip_present(app->pip);
        if (!app->pip_visible) interaction_cancel(app);
        if (app->profile) {
            uint64_t finished = profile_clock();
            if (app->pip_visible && mask.ui_revealed) {
                if (mask.popup) ++app->stats.popup_reveal_frames;
                else ++app->stats.dialog_reveal_frames;
            }
            app->stats.present_ticks += finished-started;
            app->stats.conversion_ticks += converted-encoded;
            app->stats.upload_ticks += finished-converted;
        }
        return;
    }
    {
        /* The canvas always holds the whole frame, so uploading only what
           this draw wrote keeps the window current. A Simple Refresh repair
           needs every pixel, and so does a draw that drew nothing (its
           renderer state is invalid). */
        const DockDamage *damage = app->present_whole || !app->renderer.previous.valid ?
            NULL : &app->renderer.damage;
        app->present_whole = 0;
        if (app->profile) {
            AmigaDisplayTiming timing;
            uint64_t started = profile_clock();
            amiga_display_present_damage(app->display, app->window, frame, damage, profile_clock, &timing);
            app->stats.present_ticks += profile_clock()-started;
            app->stats.conversion_ticks += timing.conversion_ticks;
            app->stats.upload_ticks += timing.upload_ticks;
            ++app->stats.conversion_frames;
            app->stats.conversion_pixels += timing.pixels;
        } else amiga_display_present_damage(app->display, app->window, frame, damage, NULL, NULL);
    }
}

static DockOverlay running_dot(const App *app, const DockPlacement *icon)
{
    #define DOT(a) 175,179,188,a
    static unsigned char pixels[] = {
        DOT(32), DOT(210), DOT(32),
        DOT(210), DOT(255), DOT(210),
        DOT(32), DOT(210), DOT(32)
    };
    #undef DOT
    static const DockImage dot = {3,3,pixels};
    int inset = (app->settings.base_size-2*DOCK_ICON_INSET+4)/9;
    int x = icon->x+icon->size/2-dot.width/2;
    int y = icon->y+icon->size/2-dot.height/2;
    if (inset < 2) inset = 2;
    inset += DOCK_DESKTOP_GAP+app->settings.edge_padding;
    if (app->edge == DOCK_EDGE_BOTTOM) y = app->height-1-inset-dot.height/2;
    else if (app->edge == DOCK_EDGE_RIGHT) x = app->width-1-inset-dot.width/2;
    else x = inset-dot.width/2;
    return (DockOverlay){&dot,x,y};
}

/* A capsule from (ax,ay) to (bx,by) with radius r contains (x,y); no
   division: the perpendicular test compares cross^2 with r^2*length^2. */
static int in_capsule(int x, int y, int ax, int ay, int bx, int by, int r)
{
    long dx = bx-ax, dy = by-ay, px = x-ax, py = y-ay;
    long dot = px*dx+py*dy, length = dx*dx+dy*dy, cross = px*dy-py*dx;
    if (dot <= 0) return px*px+py*py <= (long)r*r;
    if (dot >= length) { px = x-bx; py = y-by; return px*px+py*py <= (long)r*r; }
    return (int64_t)cross*cross <= (int64_t)r*r*length;
}

/* While the Folder Stack is open its pin becomes a close button, as in the
   macOS Dock: smoked glass a little smaller than an icon (the cache scales
   it to 90%, since it fits any artwork to the box), with a light rim and a
   thin, soft white chevron toward the screen edge. It is drawn at the zoom
   size with 4x4 samples per pixel and built into a cache like any icon, so
   it zooms and filters like the others. Built with the surface: a 256 px
   source took 2.3 s on the 68080 and stalled the first Folder Stack
   opening. Each surface opening reuses it while the icon sizes, cache
   limit and edge it was built for are unchanged, and frees it without a
   stack. */
static void stack_button_build(App *app)
{
    enum { SURFACE = 44, SURFACE_ALPHA = 176, RIM = 132, RIM_ALPHA = 220, MARK = 212 };
    int n = app->settings.max_size, s16 = n*8; /* positions in 1/8 px */
    int radius = s16*22/100, rim = s16/40 > 8 ? s16/40 : 8, stroke = s16*26/1000;
    /* The chevron for a bottom dock ("v"), as percentages of the size. */
    int ax = 36, ay = 44, bx = 50, by = 58, cx = 64, cy = 44, x, y, ok;
    DockImage source = {n,n,NULL};
    const DockSettings *built = &app->button_settings, *now = &app->settings;
    if (stack_index(app) >= app->count) { dock_cache_free(&app->stack_button); return; }
    if (app->stack_button.levels && app->button_edge == app->edge &&
        built->base_size == now->base_size && built->max_size == now->max_size &&
        built->levels == now->levels && built->cache_limit == now->cache_limit) return;
    dock_cache_free(&app->stack_button);
    if (app->edge != DOCK_EDGE_BOTTOM) {
        /* ">" for a right dock, "<" for a left one: transpose, then mirror. */
        int t;
        t = ax; ax = ay; ay = t; t = bx; bx = by; by = t; t = cx; cx = cy; cy = t;
        if (app->edge == DOCK_EDGE_LEFT) { ax = 100-ax; bx = 100-bx; cx = 100-cx; }
    }
    ax = ax*s16/100; ay = ay*s16/100; bx = bx*s16/100; by = by*s16/100; cx = cx*s16/100; cy = cy*s16/100;
    if (!(source.pixels = malloc((size_t)n*n*4))) return;
    for (y = 0; y < n; ++y) for (x = 0; x < n; ++x) {
        unsigned char *p = source.pixels+((size_t)y*n+x)*4;
        int square = 0, inner = 0, mark = 0, sx, sy, c;
        unsigned int alpha;
        for (sy = 0; sy < 4; ++sy) for (sx = 0; sx < 4; ++sx) {
            int px = x*8+1+2*sx, py = y*8+1+2*sy, k;
            for (k = 0; k < 2; ++k) {
                /* The rounded square, then the same inset by the rim. */
                int edge = k ? rim : 0, far = s16-edge, r = radius-edge;
                int dx = px < edge+r ? edge+r-px : px >= far-r ? px-(far-r) : 0;
                int dy = py < edge+r ? edge+r-py : py >= far-r ? py-(far-r) : 0;
                if (px >= edge && py >= edge && px < far && py < far &&
                    dx*dx+dy*dy <= r*r) { if (k) ++inner; else ++square; }
            }
            mark += in_capsule(px,py,ax,ay,bx,by,stroke) || in_capsule(px,py,bx,by,cx,cy,stroke);
        }
        /* Premultiplied: glass, its rim, then the opaque mark over both. */
        alpha = (SURFACE_ALPHA*inner+RIM_ALPHA*(square-inner))*(16-mark)/16+255*mark;
        for (c = 0; c < 3; ++c) {
            unsigned int base = SURFACE+(c == 2 ? 4 : 0), value;
            value = (base*SURFACE_ALPHA*inner+RIM*RIM_ALPHA*(square-inner))*(16-mark)/16+
                MARK*255*mark;
            p[c] = (unsigned char)(alpha ? (value+alpha/2)/alpha : 0);
        }
        p[3] = (unsigned char)((alpha+8)/16);
    }
    ok = dock_cache_build_scaled(&app->stack_button,&source,&app->settings,230);
    free(source.pixels);
    if (!ok) dock_cache_free(&app->stack_button);
    else { app->button_settings = app->settings; app->button_edge = app->edge; }
}

/* The stack pin's image between its icon and the close button: alpha
   weighted, so soft edges do not darken. Full progress uses the button. */
static const DockCache *stack_morph(App *app, const DockCache *icon, int level)
{
    const DockImage *a, *b;
    DockImage *out;
    int t = app->morph, i;
    /* Without a blend, show whichever image is nearer. */
    const DockCache *nearer = t >= 128 ? &app->stack_button : icon;
    if (t >= 256 || level < 0 || level >= icon->levels || level >= app->stack_button.levels)
        return nearer;
    a = &icon->image[level]; b = &app->stack_button.image[level];
    if (a->width != b->width || a->height != b->height ||
        a->width > DOCK_MAX_ICON_SIZE || a->height > DOCK_MAX_ICON_SIZE) return nearer;
    if (!app->morph_pixels &&
        !(app->morph_pixels = malloc((size_t)DOCK_MAX_ICON_SIZE*DOCK_MAX_ICON_SIZE*4)))
        return nearer;
    /* Redraws without a change (a hover label, a running dot) reuse the
       blend, and its unchanged stamp leaves the slot undamaged. */
    if (app->morph_cache.stamp && app->morph_key.morph == t && app->morph_key.level == level &&
        app->morph_key.icon == icon->stamp && app->morph_key.button == app->stack_button.stamp)
        return &app->morph_cache;
    memset(&app->morph_cache,0,sizeof(app->morph_cache));
    app->morph_cache.levels = level+1;
    out = &app->morph_cache.image[level];
    *out = (DockImage){a->width,a->height,app->morph_pixels};
    app->morph_key.morph = t; app->morph_key.level = level;
    app->morph_key.icon = icon->stamp; app->morph_key.button = app->stack_button.stamp;
    dock_cache_restamp(&app->morph_cache);
    for (i = 0; i < a->width*a->height; ++i) {
        const unsigned char *p = a->pixels+i*4, *q = b->pixels+i*4;
        unsigned char *o = out->pixels+i*4;
        unsigned int wa, wb, sum, c;
        if ((p[3]&q[3]) == 255) {
            /* Both opaque, as most pixels are: a plain blend, no divide. */
            for (c = 0; c < 3; ++c) o[c] = (unsigned char)((p[c]*(256-t)+q[c]*t+128)>>8);
            o[3] = 255;
            continue;
        }
        wa = p[3]*(256-t); wb = q[3]*t; sum = wa+wb;
        if (!sum) { memset(o,0,4); continue; }
        for (c = 0; c < 3; ++c) o[c] = (unsigned char)((p[c]*wa+q[c]*wb+sum/2)/sum);
        o[3] = (unsigned char)((sum+128)>>8);
    }
    return &app->morph_cache;
}

/* Tell the input handler where the close button is while it is shown. */
static void publish_close_button(App *app, const DockPlacement *placement, int count)
{
    int i, valid = 0, left = 0, top = 0, right = 0, bottom = 0;
    if (app->stack_button_shown && app->pip_mode && !app->moving_item)
        for (i = 0; i < count; ++i) if (app->items[row_item(app,i)].stack) {
            left = app->left+placement[i].x; top = app->top+placement[i].y;
            right = left+placement[i].size; bottom = top+placement[i].size;
            valid = 1;
        }
    Disable();
    app->input_context.close_left = left; app->input_context.close_top = top;
    app->input_context.close_right = right; app->input_context.close_bottom = bottom;
    app->input_context.close_valid = valid;
    Enable();
}

static void draw(App *app)
{
    uint64_t started = app->profile ? profile_clock() : 0;
    const DockCache *icons[DOCK_MAX_ITEMS];
    DockOverlay overlays[DOCK_MAX_OVERLAYS];
    const DockPlacement *placement = row_placement(app);
    int i, count = row_count(app), overlay_count = 0;
    int dragged = app->gesture.active && app->gesture.dragged && !app->gesture.cancelled ?
        pin_index(app, app->gesture.pin_id) : -1;
    uint32_t selected = dock_gesture_pressed(&app->gesture);
    if (!selected) selected = app->flash_pin_id;
    app->renderer.pressed_item = 0;
    app->stack_button_shown = dock_ui_folder_open(app->ui);
    publish_close_button(app,placement,count);
    for (i = 0; i < count; ++i) {
        int item = row_item(app,i);
        icons[i] = item == dragged ? NULL : &app->items[item].cache;
        if (app->items[item].stack && icons[i] && app->stack_button.levels && app->morph)
            icons[i] = stack_morph(app,icons[i],placement[i].level);
        if (selected && selected == app->items[item].pin_id) app->renderer.pressed_item = i+1;
    }
    /* Running dots in slot order, then the dragged icon. */
    for (i = 0; i < count; ++i)
        if (icons[i] && app->items[row_item(app,i)].running)
            overlays[overlay_count++] = running_dot(app,&placement[i]);
    if (dragged >= 0 && !app->gesture.remove_armed) {
        int level = app->moving_item ? app->held_icon.level : app->placement[dragged].level;
        const DockImage *icon = &app->items[dragged].cache.image[level];
        overlays[overlay_count++] = (DockOverlay){icon, app->gesture.x-icon->width/2,
            app->gesture.y-icon->height/2};
    }
    app->renderer.overlays = overlays; app->renderer.overlay_count = overlay_count;
    dock_renderer_draw(&app->renderer, &app->canvas, (app->snapshot || app->pip_mode) ? &app->backdrop : NULL,
        icons, placement, count, &app->settings);
    /* Both PiP buffers miss this damage until a conversion reaches them;
       a skipped present keeps accumulating. */
    if (app->pip_mode) {
        dock_damage_merge(&app->pip_pending[0], &app->renderer.damage, app->width, app->height);
        dock_damage_merge(&app->pip_pending[1], &app->renderer.damage, app->width, app->height);
    }
    if (app->check_canvas.pixels) {
        /* A second renderer that starts over every frame is the reference. */
        DockRenderer *check = &app->check_renderer;
        check->pressed_item = app->renderer.pressed_item;
        check->center_panel = app->renderer.center_panel;
        check->edge = app->renderer.edge;
        check->overlays = overlays; check->overlay_count = overlay_count;
        dock_renderer_backdrop_changed(check);
        dock_renderer_draw(check, &app->check_canvas, (app->snapshot || app->pip_mode) ? &app->backdrop : NULL,
            icons, placement, count, &app->settings);
        check->overlays = NULL; check->overlay_count = 0;
        ++app->stats.check_frames;
        if (memcmp(app->check_canvas.pixels, app->canvas.pixels, (size_t)app->width*app->height*4) ||
            (app->coverage && memcmp(app->check_coverage, app->coverage, (size_t)app->width*app->height)))
            ++app->stats.compose_mismatch;
    }
    app->renderer.overlays = NULL; app->renderer.overlay_count = 0;
    if (app->profile) {
        const DockDamage *damage = &app->renderer.damage;
        ++app->stats.damage_frames;
        if (damage->full) {
            ++app->stats.damage_full;
            app->stats.compose_pixels += (uint64_t)app->width*app->height;
        } else for (i = 0; i < damage->count; ++i)
            app->stats.compose_pixels += (uint64_t)(damage->rect[i].right-damage->rect[i].left)*
                (uint64_t)(damage->rect[i].bottom-damage->rect[i].top);
    }
    present_frame(app, &app->canvas);
    if (app->pip_mode) {
        if (app->pip_visible) app->displayed_drop_slot = app->drop_preview_slot;
        input_publish(app);
    } else if (!app->count) {
        const char *label = "Drop Workbench icons here";
        SetAPen(app->window->RPort, 1); SetDrMd(app->window->RPort, JAM1);
        {
            WORD length = amiga_text_length(app->window->RPort,label,strlen(label));
            Move(app->window->RPort, (app->width-length)/2, app->height-25);
            amiga_text_draw(app->window->RPort,label,strlen(label));
        }
    }
    if (app->profile && app->vblank_open) {
        /* Consecutive frames of one motion: at most five fields apart. */
        ULONG field = app->vblank.fields, fields = field-app->stats.pace_field;
        if (app->frames && fields <= 5) {
            if (fields == 1) ++app->stats.pace_one;
            else if (!fields) ++app->stats.pace_same;
            else ++app->stats.pace_stall;
        }
        app->stats.pace_field = field;
    }
    if (app->profile) {
        uint64_t ticks = profile_clock()-started;
        ++app->frames;
        app->stats.render_ticks += ticks;
        if (ticks < app->stats.min_ticks) app->stats.min_ticks = ticks;
        if (ticks > app->stats.max_ticks) app->stats.max_ticks = ticks;
    }
}

/* Point the input handler at the dock's screen, or at none (NULL) before
   that screen is unlocked. State tied to the previous screen ends here:
   external press tracking and the close button stop, and a held left or
   right press stays cancelled but still owns its release. */
static void input_attach_screen(App *app, struct Screen *screen)
{
    InputContext *c = &app->input_context;
    Disable();
    if (!screen) c->active = NULL;
    c->dock_screen = screen;
    c->apollo_pointer = app->features.apollo_pointer;
    c->close_valid = 0;
    c->popup_open = 0;
    if (c->held) c->held_cancelled = 1;
    c->held_screen = NULL;
    if (c->external_held) { c->external_held = 0; c->external_cancelled = 1; }
    c->external_screen = NULL;
    if (c->right_held) c->menu_cancelled = 1;
    c->menu_screen = NULL;
    Enable();
}

static int install_input(App *app)
{
    app->input_bit = AllocSignal(-1);
    if (app->input_bit < 0) return 0;
    app->input_context.task = FindTask(NULL);
    input_attach_screen(app,app->screen);
    app->input_context.signal = 1UL << app->input_bit;
    app->input_port = CreateMsgPort();
    if (!app->input_port) return 0;
    app->input_io = (struct IOStdReq *)CreateIORequest(app->input_port, sizeof(struct IOStdReq));
    if (!app->input_io) return 0;
    if (OpenDevice((STRPTR)"input.device", 0, (struct IORequest *)app->input_io, 0)) return 0;
    app->input_open = 1;
    InputBase = app->input_io->io_Device;
    app->input_handler.is_Node.ln_Type = NT_INTERRUPT;
    app->input_handler.is_Node.ln_Pri = 60;
    app->input_handler.is_Node.ln_Name = "DockAwesome pointer observer";
    app->input_handler.is_Code = (VOID (*)())dock_input_entry;
    app->input_handler.is_Data = &app->input_context;
    app->input_io->io_Command = IND_ADDHANDLER;
    app->input_io->io_Data = &app->input_handler;
    if (DoIO((struct IORequest *)app->input_io)) return 0;
    app->handler_added = 1;
    return 1;
}

static void remove_input(App *app)
{
    input_invalidate(app);
    if (app->handler_added) {
        /* The application owns the swallowed DOWN until its physical UP.
           Wait without spinning; never leak an unmatched release to another app. */
        Disable(); app->input_context.close_valid = 0; Enable();
        while (app->input_context.held || app->input_context.right_held || app->input_context.close_held)
            Wait(app->input_context.signal);
        app->input_io->io_Command = IND_REMHANDLER;
        app->input_io->io_Data = &app->input_handler;
        DoIO((struct IORequest *)app->input_io);
    }
    if (app->input_open) CloseDevice((struct IORequest *)app->input_io);
    if (app->input_io) DeleteIORequest((struct IORequest *)app->input_io);
    if (app->input_port) DeleteMsgPort(app->input_port);
    if (app->input_bit >= 0) FreeSignal(app->input_bit);
}

/* Retain the current surface while it can still support the zoom layout.
   The empty Dock has a different viewport length. */
static int rest_after_pin_change(App *app, int before)
{
    if (!before || !app->count) return 0;
    if (app->count > before && app->settings.max_size > app->settings.base_size) {
        int needed = dock_viewport_length(&app->settings,app->count), limit = viewport_limit(app,app->edge);
        int length = app->edge == DOCK_EDGE_BOTTOM ? app->width : app->height;
        if (needed > limit) needed = limit;
        if (length < needed) return 0;
    }
    if (!dock_edge_layout(&app->settings, app->edge, app->count,
            app->width, app->height, 0, 0, 0, app->target)) return 0;
    app->renderer.center_panel = 0;
    dock_animation_reset(&app->animation);
    return 1;
}

static __attribute__((noinline)) int accept_drops(App *app, int *recreate)
{
    struct AppMessage *msg;
    int changed = 0;
    while ((msg = (struct AppMessage *)GetMsg(app->drop_port))) {
        int slot = dock_edge_insertion_slot(&app->settings, app->edge, app->count,
            app->width, app->height, msg->am_MouseX, msg->am_MouseY);
        int i, in_slot = slot >= 0;
        int snap = in_slot && amiga_drop_catcher_active(app->drop_catcher);
        if (app->gesture.active && !app->gesture.cancelled)
            interaction_cancel_for(app, app->gesture.token);
        ++app->stats.drop_messages;
        if (snap && app->displayed_drop_slot >= 0) slot = app->displayed_drop_slot;
        if (slot < 0 || slot > stack_index(app)) slot = stack_index(app);
        for (i = 0; i < msg->am_NumArgs; ++i) {
            const struct WBArg *arg = &msg->am_ArgList[i];
            char path[PATH_CAP];
            ++app->stats.drop_arguments;
            if (NameFromLock(arg->wa_Lock, (STRPTR)path, sizeof(path)) &&
                (!arg->wa_Name || !arg->wa_Name[0] || AddPart((STRPTR)path, arg->wa_Name, sizeof(path)))) {
                int before = app->count, index = pin_path(app,path,1)-1;
                int added = app->count != before;
                int target = slot;
                if (index < 0 || (!added && !in_slot)) continue;
                if (!added && index < target) --target;
                if (target >= stack_index(app)) target = stack_index(app)-1;
                slot = target+1;
                if (app->items[index].temporary) {
                    app->items[index].temporary = 0;
                    changed = 1;
                }
                if (!added && index == target) continue;
                input_invalidate(app);
                dock_reorder(app->items,sizeof(Item),app->count,index,target);
                if (!*recreate && rest_after_pin_change(app,before)) {
                    app->placement[index] = app->target[target];
                    dock_reorder(app->placement,sizeof(*app->placement),app->count,index,target);
                } else *recreate = 1;
                changed = 1;
                if (added) ++app->stats.drop_added;
                else ++app->stats.drop_reordered;
                if (snap) {
                    app->snap_wait_motion = 1;
                    app->snap_x = app->left+msg->am_MouseX;
                    app->snap_y = app->top+msg->am_MouseY;
                }
            } else ++app->stats.drop_bad_paths;
        }
        ReplyMsg((struct Message *)msg);
        drop_catcher_flags(app, amiga_drop_catcher_accepted(app->drop_catcher));
        app->drop_preview_slot = app->displayed_drop_slot = -1;
    }
    return changed;
}

static int remove_pin(App *app, uint32_t pin_id)
{
    int index = pin_index(app, pin_id);
    if (index < 0 || app->items[index].temporary || app->items[index].stack) return 0;
    interaction_cancel(app);
    dock_gesture_reset(&app->gesture);
    erase_item(app,index);
    app->snap_wait_motion = 0;
    app->hovered = -1;
    return 1; /* Only the pin/cache is removed. Its filesystem object is untouched. */
}

static DockGestureInput gesture_point(App *app, int kind, uint32_t token,
    uint32_t pin_id, uint32_t hit_id, int x, int y, unsigned int flags)
{
    DockGestureInput in = {0};
    in.kind = kind; in.token = token; in.pin_id = pin_id; in.hit_id = hit_id;
    in.x = x; in.y = y; in.flags = flags; in.target_slot = -1;
    if (kind != DOCK_GESTURE_DOWN && (flags & DOCK_INPUT_VALID) == DOCK_INPUT_VALID &&
        !(flags & DOCK_INPUT_CANCELLED)) {
        if (app->count > 1)
            in.target_slot = dock_edge_insertion_slot(&app->settings, app->edge, app->count-1,
                app->width, app->height, x, y);
        if (in.target_slot >= stack_index(app)) in.target_slot = stack_index(app)-1;
        in.outside = in.target_slot < 0 &&
            dock_edge_outside(&app->settings, app->edge, app->placement, app->count,
            app->width, app->height, x, y, 24);
    }
    return in;
}

static unsigned int current_input_flags(App *app, UWORD qualifier)
{
    unsigned int flags = DOCK_INPUT_COORDS_VALID;
    if (IntuitionBase->FirstScreen == app->screen) flags |= DOCK_INPUT_SAME_SCREEN;
    if (qualifier & IEQUALIFIER_RBUTTON) flags |= DOCK_INPUT_CANCELLED;
    return flags;
}

/* Apps that appear within LAUNCH_SECONDS belong to this pin. */
static void record_launch(App *app, uint32_t pin_id)
{
    if (app->launch_count == LAUNCH_SLOTS) {
        memmove(app->launches,app->launches+1,sizeof(app->launches[0])*(LAUNCH_SLOTS-1));
        --app->launch_count;
    }
    app->launches[app->launch_count].pin_id = pin_id;
    app->launches[app->launch_count++].at = profile_clock();
}

static int launch_pin(const char *path)
{
    LONG default_icon = FALSE;
    struct TagItem get[] = {
        { ICONGETA_FailIfUnavailable, FALSE },
        { ICONGETA_IsDefaultIcon, (ULONG)&default_icon },
        { TAG_DONE, 0 }
    };
    struct DiskObject *icon = GetIconTagList((STRPTR)path,get);
    int opened;
    if (icon && default_icon && icon->do_Type == WBPROJECT &&
        icon->do_DefaultTool && icon->do_DefaultTool[0]) {
        /* A document with a DefIcons image has no .info on disk. Pass its
           actual default tool the parent lock and name as a Workbench arg. */
        BPTR file = Lock((STRPTR)path,ACCESS_READ);
        BPTR parent = file ? ParentDir(file) : 0;
        struct TagItem args[] = {
            { WBOPENA_ArgLock, parent },
            { WBOPENA_ArgName, (ULONG)FilePart((STRPTR)path) },
            { TAG_DONE, 0 }
        };
        opened = parent && OpenWorkbenchObjectA(icon->do_DefaultTool,args);
        if (parent) UnLock(parent);
        if (file) UnLock(file);
    } else opened = OpenWorkbenchObjectA((STRPTR)path,NULL);
    if (icon) FreeDiskObject(icon);
    return opened;
}

static void open_stack_view(App *app, int index)
{
    uint64_t started = app->profile ? profile_clock() : 0;
    DockRect icon = hover_label_anchor(app,index);
    int x = app->edge == DOCK_EDGE_LEFT ? icon.right : app->edge == DOCK_EDGE_RIGHT ?
        icon.left : (icon.left+icon.right)/2;
    int y = app->edge == DOCK_EDGE_BOTTOM ? icon.top : (icon.top+icon.bottom)/2;
    reset_hover_label(app);
    interaction_cancel(app);
    if (dock_ui_open_folder(app->ui,app->edge,x,y,app->items[index].path,
            app->settings.base_size,read_folder_icon,app,app->profile)) {
        if (app->timer_failed) dock_ui_finish_folder_motion(app->ui);
        if (app->profile && app->clock_hz) {
            printf("Folder Stack first paint: %lu us\n",
                (unsigned long)((profile_clock()-started)*1000000/app->clock_hz));
            fflush(stdout);
        }
        memcpy(app->target,app->placement,(size_t)app->count*sizeof(*app->placement));
        dock_animation_reset(&app->animation);
    } else message("Cannot open the Folder Stack.");
    sync_input_ui(app);
}

static void apply_gesture(App *app, const DockGestureInput *in, int *redraw, int *save, int *recreate)
{
    int was_active = app->gesture.active;
    DockGestureEffects effect = dock_gesture_update(&app->gesture, in);
    int index = pin_index(app, effect.pin_id);
    /* Decide at the press whether it closes the stack. In native modes the
       press activates the dock window, which closes the stack, sometimes
       at once (no backdrop) or before the release arrives; the button the
       user pressed is the one last drawn. */
    if (!was_active && app->gesture.active)
        app->stack_close_press = app->stack_button_shown ||
            dock_ui_folder_open(app->ui) || dock_ui_folder_moving(app->ui);
    int movable = index >= 0 && !app->items[index].temporary && !app->items[index].stack;
    int commit = (effect.flags & DOCK_EFFECT_REORDER) && movable &&
        effect.target_slot >= 0 && effect.target_slot < stack_index(app);
    if (effect.flags & DOCK_EFFECT_REDRAW) *redraw = 1;
    if (effect.flags & DOCK_EFFECT_FOCUS) {
        if (app->label_pin_id) reset_hover_label(app);
        app->flash_pin_id = 0;
        ActivateWindow(app->window);
    }
    if (!app->moving_item && movable && app->count > 1 &&
        (commit || (app->gesture.active && app->gesture.dragged && !app->gesture.cancelled))) {
        memcpy(app->moving_placement, app->placement, index*sizeof(*app->placement));
        memcpy(app->moving_placement+index, app->placement+index+1,
            (app->count-index-1)*sizeof(*app->placement));
        app->held_icon = app->placement[index];
        app->held_icon.x = 0;
        app->held_icon.y = app->settings.max_size-app->held_icon.size;
        app->held_target = app->held_icon;
        dock_animation_reset(&app->held_animation);
        app->moving_item = index+1;
        app->snap_wait_motion = 0;
    }
    if (app->moving_item && (!app->gesture.active || app->gesture.cancelled))
        finish_move(app, commit ? effect.target_slot : app->moving_item-1, commit ? in : NULL);
    if (commit) {
        app->snap_wait_motion = 1;
        app->snap_x = app->left+in->x; app->snap_y = app->top+in->y;
        if (effect.target_slot != index) {
            input_invalidate(app);
            dock_reorder(app->items, sizeof(Item), app->count, index, effect.target_slot);
            *save = 1;
        }
    }
    if ((effect.flags & DOCK_EFFECT_REMOVE_PIN) && remove_pin(app, effect.pin_id)) {
        *recreate = !rest_after_pin_change(app,app->count+1);
        *redraw = *save = 1;
    }
    if ((effect.flags & DOCK_EFFECT_LAUNCH) && index >= 0) {
        Item *item = &app->items[index];
        int focused = item->running && (item->kind == PIN_PROGRAM || item->owner) &&
            item_window_action(app,item,WINDOW_FOCUS);
        /* A click on the open stack's close button (native modes; PiP
           consumes it in the handler) closes it rather than reopening. */
        if (item->stack && app->stack_close_press) dock_ui_dismiss_popup(app->ui);
        else if (item->stack) open_stack_view(app,index);
        else if (!focused && !item->temporary) {
            if (launch_pin(item->path)) {
                record_launch(app,item->pin_id);
                status_changed(app);
            }
            else message("Workbench could not open this object.");
        }
        if (!app->timer_failed) {
            app->flash_pin_id = effect.pin_id;
            app->flash_deadline = profile_clock()+(uint64_t)app->clock_hz*60/1000;
        }
        *redraw = 1;
    }
}

static int update_drag_preview(App *app)
{
    DockGesture *gesture = &app->gesture;
    int index = gesture->active && !gesture->cancelled && gesture->remove_armed ?
        pin_index(app,gesture->pin_id) : -1;
    int x = app->left+gesture->x, y = app->top+gesture->y;
    if (index < 0 || app->items[index].temporary || app->items[index].stack) {
        app->preview_attempt = 0;
        if (!app->drag_preview) return 0;
        amiga_drag_preview_close(app->drag_preview); app->drag_preview = NULL;
        return 1;
    }
    if (app->drag_preview) {
        amiga_drag_preview_move(app->drag_preview,x,y);
        return 0;
    }
    if (app->preview_attempt == gesture->token) return 0;
    app->preview_attempt = gesture->token;
    app->drag_preview = amiga_drag_preview_open(app->screen,&app->items[index].cache.image[0],
        app->pip_mode,x,y);
    return app->drag_preview != NULL;
}

static void sync_input_ui(App *app)
{
    DockRect bounds;
    Disable();
    app->input_context.popup_open = dock_ui_popup_bounds(app->ui,&bounds);
    app->input_context.ui_busy = dock_ui_busy(app->ui);
    if (app->input_context.ui_busy) app->input_context.active = NULL;
    Enable();
}

static void open_menu(App *app, int x, int y, uint32_t pin_id)
{
    int opened, can_keep = 0, stack = 0, action = DOCK_UI_NONE;
    if (pin_id) {
        int index = pin_index(app,pin_id);
        const Item *item;
        if (index < 0) return;
        item = &app->items[index];
        stack = item->stack;
        /* Only an app with a program file can become a pin. */
        can_keep = item->temporary && strchr(item->path,':') != NULL;
        if (item->kind == PIN_SHELL) {
            app->menu_shell = item->running ? shell_menu_target(app) : (ShellTarget){0};
            action = item->running ? DOCK_UI_SHOW_SHELL : DOCK_UI_NEW_SHELL;
            /* Quit closes a window: offer it only for a target probed in the
               last 2 s, so a reused Window address cannot be closed. */
            if (app->menu_shell.cooked && apollo_console_close_handler(app) &&
                profile_clock()-app->shells_at <= 2*app->clock_hz)
                action = DOCK_UI_QUIT_SHELL;
        } else if (item->running) {
            if (item->kind == PIN_DRAWER) action = DOCK_UI_CLOSE_ITEM;
            else if (item_window_action(app,item,WINDOW_CHECK))
                action = item->kind == PIN_DOCUMENT ? DOCK_UI_CLOSE_ITEM : DOCK_UI_QUIT_ITEM;
        }
        if (!action && !can_keep && !stack) return;
    }
    reset_hover_label(app);
    interaction_cancel(app);
    drop_catcher_flags(app,amiga_drop_catcher_cancel(app->drop_catcher));
    Disable();
    app->input_context.popup_open = app->input_context.ui_busy = 1;
    app->input_context.active = NULL;
    Enable();
    opened = dock_ui_open_popup(app->ui,app->edge,x,y,app->pip_mode ? NULL : app->window,
        action,can_keep,stack);
    sync_input_ui(app);
    if (opened) {
        app->menu_pin_id = pin_id;
        memcpy(app->target,app->placement,(size_t)app->count*sizeof(*app->placement));
        dock_animation_reset(&app->animation);
    } else message("Cannot open the Dock menu.");
}

static void close_menu_item(App *app)
{
    int index = pin_index(app,app->menu_pin_id);
    const Item *item;
    if (index < 0) return;
    item = &app->items[index];
    if (!item->running) return;
    if (item->kind == PIN_DRAWER) CloseWorkbenchObjectA((STRPTR)item->path,NULL);
    else item_window_action(app,item,WINDOW_CLOSE);
    status_changed(app);
}

static void keep_menu_item(App *app)
{
    int index = pin_index(app,app->menu_pin_id);
    if (index < 0 || !app->items[index].temporary || !strchr(app->items[index].path,':')) return;
    app->items[index].temporary = 0;
    if (!save_config(app)) app->items[index].temporary = 1;
}

static void shell_menu_action(App *app, int action)
{
    int index = pin_index(app,app->menu_pin_id);
    if (index < 0 || app->items[index].kind != PIN_SHELL) return;
    if (action == DOCK_UI_SHOW_SHELL) {
        if (!show_shell_window(app,&app->menu_shell)) message("No Shell window to show.");
    } else if (action == DOCK_UI_QUIT_SHELL) {
        if (quit_shell_window(app)) status_changed(app);
        else message("Could not close the selected Shell.");
    } else if (launch_pin(app->items[index].path)) status_changed(app);
    else message("Workbench could not open Shell.");
}

static int displayed_scale(const App *app)
{
    return app->scale_percent ? app->scale_percent :
        (app->settings.max_size*DOCK_SCALE_ONE+app->settings.base_size/2)/app->settings.base_size;
}

static int apply_preferences(App *app, const DockUIPrefs *prefs)
{
    DockSettings previous = app->settings, next;
    DockEdge previous_edge = app->edge;
    DockCache *swap = NULL;
    Item stack = {0};
    char previous_folder[PATH_CAP];
    int i, changed_sizes, ok, old_count = app->count;
    int slot = stack_index(app), has_stack = slot < old_count;
    int change_stack = strcmp(prefs->folder,app->prefs.folder) != 0 || (!!prefs->folder[0] != has_stack);
    int previous_scale = app->scale_percent, scale;
    /* The same rules as the Settings Size slider (ui_fit). */
    if (!draft_settings(app,prefs,prefs->size,&next,&scale)) {
        message("Zoom exceeds 192 px. Lower Size or Scale.");
        return 0;
    }
    if (!draft_fits(app,prefs,&next)) {
        message("The icons do not fit the screen or cache. Lower Size or Scale.");
        return 0;
    }
    changed_sizes = next.base_size != previous.base_size || next.max_size != previous.max_size;
    if (!change_stack && !changed_sizes && prefs->edge == (int)app->edge) {
        app->scale_percent = scale;
        ok = save_config(app);
        if (!ok) app->scale_percent = previous_scale;
        return ok;
    }
    if (changed_sizes) {
        swap = calloc((size_t)(app->count ? app->count : 1),sizeof(*swap));
        if (!swap) { message("Not enough memory to apply settings."); return 0; }
        for (i = 0; i < app->count; ++i) {
            DockImage image = {0};
            char status[64];
            if (change_stack && i == slot) continue;
            /* Rebuilding takes seconds at large sizes: show progress. */
            snprintf(status,sizeof(status),"Resizing icons %d/%d...",i+1,app->count);
            dock_ui_set_busy(app->ui,status);
            ok = (app->items[i].stack ? load_stack_icon(app,app->items[i].path,&image) :
                load_icon(app,app->items[i].path,&image,NULL)) && dock_cache_build(&swap[i],&image,&next);
            free(image.pixels);
            if (!ok) {
                int j;
                for (j = 0; j <= i; ++j) dock_cache_free(&swap[j]);
                free(swap);
                dock_ui_set_busy(app->ui,NULL);
                message("Cannot resize the icons. Settings are unchanged.");
                return 0;
            }
        }
    }
    if (change_stack && prefs->folder[0]) dock_ui_set_busy(app->ui,"Reading Folder Stack...");
    if (change_stack && prefs->folder[0] && !prepare_stack(app,prefs->folder,&next,&stack)) {
        if (swap) for (i = 0; i < old_count; ++i) dock_cache_free(&swap[i]);
        free(swap);
        dock_ui_set_busy(app->ui,NULL);
        message("Cannot open this drawer. Choose another one.");
        return 0;
    }
    dock_ui_set_busy(app->ui,"Opening the dock...");
    close_surface(app);
    if (swap) for (i = 0; i < app->count; ++i) {
        if (change_stack && i == slot) continue;
        DockCache old = app->items[i].cache; app->items[i].cache = swap[i]; swap[i] = old;
    }
    if (change_stack) {
        strcpy(previous_folder,app->prefs.folder);
        strcpy(app->prefs.folder,stack.path);
        exchange_stack(app,slot,&stack);
    }
    app->settings = next; app->edge = (DockEdge)prefs->edge;
    app->scale_percent = scale;
    ok = open_surface(app);
    if (ok && !save_config(app)) ok = 0;
    if (!ok) {
        close_surface(app);
        if (change_stack) {
            exchange_stack(app,slot,&stack);
            strcpy(app->prefs.folder,previous_folder);
        }
        if (swap) for (i = 0; i < app->count; ++i) {
            if (change_stack && i == slot) continue;
            DockCache failed = app->items[i].cache; app->items[i].cache = swap[i]; swap[i] = failed;
        }
        app->settings = previous; app->edge = previous_edge;
        app->scale_percent = previous_scale;
        if (!open_surface(app))
            fail_after_cleanup(app,"Cannot restore the dock surface. Check graphics memory and screen size.");
        else message("Cannot apply settings. Previous settings restored.");
    }
    if (swap) {
        for (i = 0; i < old_count; ++i) dock_cache_free(&swap[i]);
        free(swap);
    }
    dock_cache_free(&stack.cache);
    dock_ui_set_busy(app->ui,NULL);
    return ok;
}

/* screennotify.library: AddWorkbenchClient/RemWorkbenchClient and its
   message. Type 3 is Workbench; value FALSE: about to close (reply once
   every window on it is closed), TRUE: open again. A close that fails is
   followed by TRUE as well. */
enum { WORKBENCH_NOTIFY_TYPE = 3 };
typedef struct { struct Message message; ULONG type; APTR value; } WorkbenchNotify;

static void workbench_notify_open(App *app)
{
    if (!app->caps.screen_notify) return;
    app->screen_notify = OpenLibrary((STRPTR)"screennotify.library",1);
    if (!app->screen_notify) return;
    app->workbench_port = CreateMsgPort();
    if (app->workbench_port)
        app->workbench_client = LP2(0x36, APTR, AddWorkbenchClient, struct MsgPort *, app->workbench_port, a0,
            LONG, 0, d0, , app->screen_notify);
    if (!app->workbench_client) {
        if (app->workbench_port) DeleteMsgPort(app->workbench_port);
        app->workbench_port = NULL;
        CloseLibrary(app->screen_notify);
        app->screen_notify = NULL;
    }
}

static void workbench_notify_close(App *app)
{
    struct Message *msg;
    if (app->workbench_client) {
        /* Removal fails while a message is outstanding: reply and retry. */
        while (!LP1(0x3c, BOOL, RemWorkbenchClient, APTR, app->workbench_client, a0, , app->screen_notify)) {
            while ((msg = GetMsg(app->workbench_port))) ReplyMsg(msg);
            Delay(2);
        }
    }
    if (app->workbench_port) {
        while ((msg = GetMsg(app->workbench_port))) ReplyMsg(msg);
        DeleteMsgPort(app->workbench_port);
    }
    if (app->screen_notify) CloseLibrary(app->screen_notify);
    app->workbench_client = NULL; app->workbench_port = NULL; app->screen_notify = NULL;
}

/* Release everything on the Workbench screen so it can close. Icon caches
   and pins stay; they do not depend on the screen. */
static void release_workbench(App *app)
{
    input_attach_screen(app,NULL);
    interaction_cancel(app);
    close_surface(app);
    dock_ui_destroy(app->ui); app->ui = NULL;
    if (app->screen) UnlockPubScreen(NULL,app->screen);
    app->screen = NULL;
    app->workbench_closed = 1;
}

static void apply_profile(App *app, int backend, int explicit_pip)
{
    app->features = dock_choose_profile(&app->caps,backend,explicit_pip);
    app->pip_mode = app->features.backend == DOCK_BACKEND_PIP;
    app->snapshot = app->features.backend == DOCK_BACKEND_SNAPSHOT;
    /* The 68080 build's AMMX kernels serve every backend, not only PiP. */
    app->use_ammx = app->features.pixels == DOCK_PIXELS_AMMX;
}

/* Follow the (new) Workbench screen; features.error reports an explicit
   PiP request that cannot be met. */
static void select_backend(App *app, int explicit_pip)
{
    amiga_caps_probe_screen(&app->caps,app->screen);
    apply_profile(app,app->requested_backend,explicit_pip);
}

static DockUI *create_ui(const App *app)
{
    return dock_ui_create(app->screen,app->features.ui_shapes,app->features.ui_glass,app->features.aa_text);
}

/* Pixel kernels for use_ammx, shared with the controls. */
static void use_pixel_backend(App *app)
{
    app->renderer.pixels = dock_backend_portable();
#ifdef DOCK_AMMX
    if (app->use_ammx) app->renderer.pixels = dock_backend_ammx();
#endif
    dock_ui_set_converters(app->ui,app->renderer.pixels->rgba_to_rgb24,app->renderer.pixels->decode565,
        app->renderer.pixels->encode565);
}

/* After a closed PiP surface: open the snapshot backend instead. */
static int fall_back_to_snapshot(App *app)
{
    apply_profile(app,DOCK_BACKEND_SNAPSHOT,0);
    use_pixel_backend(app);
    app->surface_error[0] = 0;
    return open_surface(app);
}

static DockUIPrefs current_ui_prefs(const App *app);

/* Reopen on the new Workbench screen. The backend follows the new screen
   mode; a Size that no longer fits is lowered to the largest that does. */
static int acquire_workbench(App *app)
{
    DockUIPrefs prefs;
    int fit, ok;
    app->screen = LockPubScreen(NULL);
    if (!app->screen) return 0;
    select_backend(app,0);
    app->ui = create_ui(app);
    if (!app->ui) { UnlockPubScreen(NULL,app->screen); app->screen = NULL; return 0; }
    dock_ui_set_blocking_hook(app->ui,ui_blocking,app);
    dock_ui_set_fit_hook(app->ui,ui_fit,app);
    use_pixel_backend(app);
    app->workbench_closed = 0;
    prefs = current_ui_prefs(app);
    fit = ui_fit(app,&prefs);
    if (fit < app->settings.base_size) {
        prefs.size = fit;
        ok = apply_preferences(app,&prefs);
    } else if (!(ok = open_surface(app))) {
        close_surface(app);
        ok = app->pip_mode && fall_back_to_snapshot(app);
    }
    /* Like install_input: attach once the backend is final. A failure
       leaves the handler detached while the caller unlocks the screen. */
    if (ok) input_attach_screen(app,app->screen);
    return ok;
}

/* Returns 1 when the dock reopened and needs its first frame. */
static int workbench_notify_poll(App *app)
{
    WorkbenchNotify *msg;
    int reopened = 0;
    if (!app->workbench_port) return 0;
    while ((msg = (WorkbenchNotify *)GetMsg(app->workbench_port))) {
        int closing = msg->type == WORKBENCH_NOTIFY_TYPE && !msg->value;
        int opened = msg->type == WORKBENCH_NOTIFY_TYPE && msg->value;
        if (closing && !app->workbench_closed) release_workbench(app);
        ReplyMsg(&msg->message);
        if (opened && app->workbench_closed) {
            if (acquire_workbench(app)) reopened = 1;
            else {
                /* Stay closed until the next Workbench change. */
                close_surface(app);
                dock_ui_destroy(app->ui); app->ui = NULL;
                if (app->screen) UnlockPubScreen(NULL,app->screen);
                app->screen = NULL;
                app->workbench_closed = 1;
            }
        }
    }
    return reopened;
}

/* Retarget and draw everything; 0 when the dock has failed. */
static int full_frame(App *app)
{
    update_target(app);
    if (app->pip_mode) live_capture(app,1);
    if (app->failed) return 0;
    draw(app);
    animation_schedule(app); live_schedule(app);
    return 1;
}

static __attribute__((noinline)) void event_loop(App *app)
{
    int running = 1;
    update_target(app);
    animation_profile_begin(app);
    if (app->pip_mode) live_capture(app,1);
    if (app->failed) return; /* main reports after cleanup. */
    draw(app);
    animation_profile_end(app);
    animation_schedule(app); live_schedule(app);
    while (running) {
        ULONG workbench = app->workbench_port ? 1UL<<app->workbench_port->mp_SigBit : 0;
        if (app->workbench_closed) {
            /* Nothing is on screen: wait for Workbench to open again. */
            ULONG closed_signals = Wait(workbench|SIGBREAKF_CTRL_C);
            if (closed_signals & SIGBREAKF_CTRL_C) break;
            if (workbench_notify_poll(app) && !full_frame(app)) break;
            continue;
        }
        ULONG signals = Wait(workbench | (1UL<<app->window->UserPort->mp_SigBit) |
            (1UL<<app->drop_port->mp_SigBit) | (1UL<<app->timer_port->mp_SigBit) |
            (app->live_port ? (1UL<<app->live_port->mp_SigBit) : 0) |
            (app->vblank_open ? app->vblank.mask : 0) |
            (app->program_replies ? (1UL<<app->program_replies->mp_SigBit) : 0) |
            (app->status_notify ? (1UL<<app->status_notify_bit) : 0) |
            app->input_context.signal | dock_ui_signal_mask(app->ui) | SIGBREAKF_CTRL_C);
        struct IntuiMessage *msg;
        DockUIAction action;
        int redraw = app->gesture_redraw, recreate = 0, save = 0, due;
        int dismiss_popup;
        ++app->stats.wakes; app->gesture_redraw = 0;
        if (signals & SIGBREAKF_CTRL_C) break;
        if ((signals & workbench) && (workbench_notify_poll(app) || app->workbench_closed)) {
            if (app->workbench_closed) continue;
            if (!full_frame(app)) break;
            continue;
        }
        if (app->status_notify && (signals & (1UL<<app->status_notify_bit)))
            status_observe_windows(app);
        if (IntuitionBase->FirstScreen != app->screen || native_menu_held(app))
            interaction_cancel(app);
        Disable();
        dismiss_popup = app->input_context.popup_dismiss;
        app->input_context.popup_dismiss = 0;
        if (dismiss_popup) app->input_context.popup_open = 0;
        Enable();
        if (dismiss_popup) {
            dock_ui_dismiss_popup(app->ui);
            if (app->timer_failed) dock_ui_finish_folder_motion(app->ui);
            redraw = 1;
        }
        update_drop_catcher(app);
        while (dock_ui_poll(app->ui,&action)) {
            if (action.type == DOCK_UI_SETTINGS) {
                DockUIPrefs initial = current_ui_prefs(app);
                if (!dock_ui_open_settings(app->ui,&initial)) message("Cannot open Settings on this screen.");
            } else if (action.type == DOCK_UI_ABOUT) {
                if (!dock_ui_open_about(app->ui)) message("Cannot open About.");
            } else if (action.type == DOCK_UI_QUIT) running = 0;
            else if (action.type == DOCK_UI_CLOSE_ITEM || action.type == DOCK_UI_QUIT_ITEM)
                close_menu_item(app);
            else if (action.type == DOCK_UI_SHOW_CONTENTS) {
                int index = pin_index(app,app->menu_pin_id);
                if (index >= 0 && app->items[index].stack) open_stack_view(app,index);
            } else if (action.type == DOCK_UI_OPEN_PATH) {
                if (launch_pin(action.path)) status_changed(app);
                else message("Workbench could not open this object.");
            }
            else if (action.type == DOCK_UI_KEEP_ITEM) keep_menu_item(app);
            else if (action.type >= DOCK_UI_NEW_SHELL && action.type <= DOCK_UI_QUIT_SHELL)
                shell_menu_action(app,action.type);
            else if (action.type == DOCK_UI_SAVE) {
                if (apply_preferences(app,&action.prefs)) dock_ui_close_dialog(app->ui);
                if (app->failed) running = 0;
            }
            if (app->timer_failed) dock_ui_finish_folder_motion(app->ui);
            redraw = 1;
            if (!running) break;
        }
        {
            ULONG serial;
            uint32_t pin_id;
            int x,y,cancelled;
            struct Screen *screen;
            Disable(); serial = app->input_context.menu_serial;
            x = app->input_context.menu_x; y = app->input_context.menu_y;
            pin_id = app->input_context.menu_pin_id;
            cancelled = app->input_context.menu_cancelled;
            screen = app->input_context.menu_screen; Enable();
            if (serial != app->menu_seen) {
                app->menu_seen = serial;
                if (!cancelled && screen == app->screen && IntuitionBase->FirstScreen == app->screen &&
                    !dock_ui_busy(app->ui) && !app->gesture.active) { open_menu(app,x,y,pin_id); redraw = 1; }
            }
        }
        sync_input_ui(app);
        if (!running) break;
        if (accept_drops(app,&recreate)) redraw = save = 1;
        while ((msg = (struct IntuiMessage *)GetMsg(app->window->UserPort))) {
            ULONG cls = msg->Class, seconds = msg->Seconds, micros = msg->Micros;
            UWORD code = msg->Code, qualifier = msg->Qualifier;
            int x = msg->MouseX, y = msg->MouseY;
            ReplyMsg((struct Message *)msg);
            if (recreate && !(cls == IDCMP_RAWKEY && code == 0x45)) continue;
            if (cls == IDCMP_REFRESHWINDOW) {
                if (!app->pip_mode) {
                    BeginRefresh(app->window);
                    app->present_whole = 1;
                    draw(app);
                    EndRefresh(app->window, TRUE);
                }
            } else if (!app->pip_mode && cls == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {
                int hit = input_hit_test(app, x, y);
                uint32_t token;
                DockGestureInput in;
                if (app->gesture.cancelled) dock_gesture_reset(&app->gesture);
                Disable();
                if (!++app->input_context.next_token) ++app->input_context.next_token;
                token = app->input_context.next_token;
                app->native_external_serial = app->input_context.external_seconds == seconds &&
                    app->input_context.external_micros == micros ? app->input_context.external_serial : 0;
                Enable();
                in = gesture_point(app, DOCK_GESTURE_DOWN, token,
                    hit >= 0 ? app->items[hit].pin_id : 0, hit >= 0 ? app->items[hit].pin_id : 0,
                    x, y, current_input_flags(app, qualifier));
                apply_gesture(app, &in, &redraw, &save, &recreate);
            } else if (!app->pip_mode && cls == IDCMP_MOUSEBUTTONS && code == SELECTUP) {
                int hit = input_hit_test(app, x, y);
                unsigned int flags = current_input_flags(app, qualifier);
                DockGestureInput in;
                Disable();
                if (app->native_external_serial && app->native_external_serial == app->input_context.external_serial) {
                    if (app->input_context.external_dragged) flags |= DOCK_INPUT_DRAGGED;
                    if (app->input_context.external_cancelled) flags |= DOCK_INPUT_CANCELLED;
                }
                Enable();
                in = gesture_point(app, DOCK_GESTURE_UP, app->gesture.token, app->gesture.pin_id,
                    hit >= 0 ? app->items[hit].pin_id : 0, x, y, flags);
                apply_gesture(app, &in, &redraw, &save, &recreate);
            } else if (!app->pip_mode && cls == IDCMP_MOUSEBUTTONS && code == MENUDOWN &&
                !dock_ui_busy(app->ui) && !app->gesture.active) {
                int hit = input_hit_test(app,x,y);
                if (hit >= 0 || dock_background(app,x,y)) {
                    open_menu(app,app->left+x,app->top+y,hit >= 0 ? app->items[hit].pin_id : 0);
                    redraw = 1;
                }
            } else if (cls == IDCMP_INACTIVEWINDOW && app->gesture.active) {
                /* An older inactive message must not cancel a newer queued DOWN. */
                interaction_cancel_for(app, app->gesture.token);
            } else if (cls == IDCMP_RAWKEY && !(code & IECODE_UP_PREFIX)) {
                if (code == 0x45) { running = 0; break; }
                else if (code == 0x46) {
                    int hit = input_hit_test(app, x, y);
                    if (hit >= 0 && remove_pin(app, app->items[hit].pin_id)) {
                        recreate = !rest_after_pin_change(app,app->count+1);
                        redraw = save = 1;
                    }
                } else if (code == 0x13) recreate = 1;
            }
        }
        if (app->pip_mode && !recreate && running) {
            InputButton button = {0};
            while (input_pop(app, &button)) {
                DockGestureInput in;
                unsigned int flags = button.flags;
                if (button.code == SELECTDOWN && (button.generation != app->input_generation ||
                    pin_index(app, button.pin_id) < 0)) continue;
                if (IntuitionBase->FirstScreen != app->screen || (PeekQualifier() & IEQUALIFIER_RBUTTON) || !app->pip_visible)
                    flags |= DOCK_INPUT_CANCELLED;
                in = gesture_point(app, button.code == SELECTDOWN ? DOCK_GESTURE_DOWN : DOCK_GESTURE_UP,
                    button.token, button.pin_id, button.hit_id, button.x, button.y, flags);
                apply_gesture(app, &in, &redraw, &save, &recreate);
                if (recreate) break;
            }
        }
        if (!recreate && running && app->gesture.active &&
            (app->pip_mode ? app->input_context.held : !!(PeekQualifier() & IEQUALIFIER_LEFTBUTTON))) {
            DockGestureInput in;
            int x, y;
            unsigned int flags;
            Disable();
            x = app->screen->MouseX-app->left; y = app->screen->MouseY-app->top;
            flags = current_input_flags(app, PeekQualifier());
            if (app->pip_mode && app->input_context.held_token == app->gesture.token) {
                if (app->input_context.held_dragged) flags |= DOCK_INPUT_DRAGGED;
                if (app->input_context.held_cancelled) flags |= DOCK_INPUT_CANCELLED;
            }
            Enable();
            in = gesture_point(app, DOCK_GESTURE_MOVE, app->gesture.token, app->gesture.pin_id, 0, x, y, flags);
            apply_gesture(app, &in, &redraw, &save, &recreate);
        }
        if (update_drag_preview(app)) redraw = 1;
        /* The Folder Stack pin swaps to its close button and back. */
        if (dock_ui_folder_open(app->ui) != app->stack_button_shown) redraw = 1;
        if (save) save_config(app);
        if (!running) break;
        if (program_poll_result(app,&recreate)) redraw = 1;
        if (recreate) {
            close_surface(app);
            if (!open_surface(app)) {
                fail_after_cleanup(app,"Cannot reopen dock surface. Check graphics memory and screen size.");
                break;
            }
            redraw = 1;
        }
        update_target(app); animation_profile_begin(app);
        if (animation_step(app)) redraw = 1;
        if (app->timer_failed && animation_needed(app)) { animation_finish(app); redraw = 1; }
        if (app->flash_pin_id && profile_clock() >= app->flash_deadline) {
            app->flash_pin_id = 0; redraw = 1;
        }
        due = live_due(app);
        if (due) program_poll_schedule(app);
        if (app->pip_mode) {
            /* The moving Folder Stack covers the zoom headroom above the
               plate, never the plate: every poll would see it change and
               redraw the whole dock. Poll again once it stands still. */
            if (!app->failed && (due || redraw || app->gesture_redraw || !app->pip_visible) &&
                !(dock_ui_folder_moving(app->ui) && app->pip_visible)) {
                if (live_capture(app,redraw || app->gesture_redraw)) redraw = 1;
                live_cancel(app);
            }
            if (update_drag_mask(app,due || redraw)) redraw = 1;
            if (app->failed) { interaction_cancel(app); input_invalidate(app); break; }
        }
        if (app->gesture_redraw) { redraw = 1; app->gesture_redraw = 0; }
        update_hover_label(app);
        animation_schedule(app); live_schedule(app);
        if (redraw) draw(app);
        animation_profile_end(app);
        if (app->notice[0]) {
            /* The drop was replied and its receiver released before this frame. */
            char text[sizeof(app->notice)];
            strcpy(text,app->notice); app->notice[0] = 0;
            message(text);
        }
    }
    animation_cancel(app); live_cancel(app);
}

/* --bench-sweep: after the first frame, move a synthetic pointer along the
   icon centre line from the first plate end to the other and back at speed
   pixels per frame, leave the dock, and run until the row settles. WaitTOF
   paces each frame, so a retired PiP buffer is always free. The backdrop is
   not polled: this measures frame cost, not capture. */
static void bench_sweep(App *app)
{
    DockRect plate;
    int bottom = app->edge == DOCK_EDGE_BOTTOM, start, end, along, centre, phase = 0;
    update_target(app);
    if (app->pip_mode) live_capture(app,1);
    if (app->failed) return;
    draw(app);
    app->frames = 0;
    app->stats.render_ticks = app->stats.present_ticks = 0;
    app->stats.conversion_ticks = app->stats.upload_ticks = 0;
    app->stats.min_ticks = UINT64_MAX; app->stats.max_ticks = 0;
    app->stats.damage_frames = app->stats.damage_full = app->stats.conversion_frames = 0;
    app->stats.compose_pixels = app->stats.conversion_pixels = 0;
    if (!app->count || !dock_edge_panel_rect(&app->settings,app->edge,app->placement,app->count,
            app->width,app->height,&plate)) return;
    start = bottom ? plate.left : plate.top;
    end = (bottom ? plate.right : plate.bottom)-1;
    centre = bottom ? app->placement[0].y+app->placement[0].size/2 :
        app->placement[0].x+app->placement[0].size/2;
    along = start;
    app->bench_active = 1;
    while (!(SetSignal(0,0) & SIGBREAKF_CTRL_C)) {
        int changed = 0;
        if (phase < 2) {
            app->bench_x = bottom ? along : centre;
            app->bench_y = bottom ? centre : along;
            if (!phase && (along += app->bench_speed) > end) { along = end; phase = 1; }
            else if (phase == 1 && (along -= app->bench_speed) < start) phase = 2;
        } else app->bench_x = app->bench_y = -1;
        update_target(app); animation_profile_begin(app);
        if (animation_needed(app)) changed = animate_row(app);
        if (phase == 2 && !changed && !animation_needed(app)) break;
        WaitTOF();
        if (changed) draw(app);
        animation_profile_end(app);
    }
    app->bench_active = 0;
}

static void usage(void)
{
    puts("DockAwesome [--config file] [--size pixels] [--zoom pixels] [--levels n]\n"
         "            [--snapshot | --opaque | --pip] [--ammx] [--profile]\n"
         "            [--pip-dump prefix] [--pip-x-offset n] [--pip-y-offset n]\n"
         "            [--bench-sweep speed] [--damage-check] [--no-vsync]\n"
         "            [Workbench paths...] [--version] [--caps]\n"
         "Drop Workbench icons to pin them. Click a pin to open it.\n"
         "Drag to reorder. Drag out or press Delete to remove a pin.\n"
         "R refreshes the background. Esc or Ctrl-C quits.\n"
         "Default preferences: " DEFAULT_CONFIG "\n"
         "--ammx is kept for compatibility: the 68080 build always uses AMMX.\n"
         "--pip needs SAGA RGB565, Apollo core 11900+, and Intuition/Layers V50.\n"
         "Saved PiP settings use snapshot mode when PiP is unavailable.\n"
         "--pip-x-offset and --pip-y-offset set alignment from -128 to 128.\n"
         "--pip-dump writes prefix-source.ppm and prefix-composed.ppm on exit.\n"
         "--bench-sweep 1..64 sweeps a synthetic pointer at that many pixels per\n"
         "frame, prints the profile and exits. --damage-check compares every\n"
         "frame with a full composition and PiP conversion and reports mismatches.\n"
         "--no-vsync paces zoom animation with the 20 ms timer instead of the display.\n"
         "--caps prints the probed CPU, chipset and OS features and the chosen\n"
         "backend, then exits. Include it in bug reports.");
}

/* Before the libraries are open. A Workbench start has no console. */
static void early_message(int argc, const char *text)
{
    if (argc) puts(text);
    else if ((IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 39))) {
        message(text);
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }
}

int main(int argc, char **argv)
{
    App *app = calloc(1, sizeof(*app));
    int i, status = 20, initial_count, config_state;
    int explicit_pip = 0, caps_only = 0;
    if (!app) return 20;
    app->settings = dock_settings_default(); app->input_bit = app->vblank_bit = -1;
    {
        /* The icon cache may use a quarter of the free memory, at least the
           portable 16 MB default; a fixed budget refused sizes that fit. */
        size_t budget = (size_t)AvailMem(MEMF_ANY)/4;
        if (budget > app->settings.cache_limit) app->settings.cache_limit = budget;
    }
    strcpy(app->config, DEFAULT_CONFIG);
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help")) { usage(); free(app); return 0; }
        if (!strcmp(argv[i], "--version")) {
            puts(dock_build_tag+sizeof(DOCK_BUILD_TAG_PREFIX)-1);
            free(app);
            return 0;
        }
        if (!strcmp(argv[i], "--config")) {
            if (i+1 >= argc) { puts("--config requires a path."); free(app); return 20; }
            if (strlen(argv[++i]) >= sizeof(app->config)) { free(app); return 20; }
            strcpy(app->config, argv[i]);
        }
        else if (!strcmp(argv[i], "--caps")) caps_only = 1;
    }
    /* The binary is built for the 68080: stop before any AMMX kernel runs.
       --caps still reports what this machine offers. */
    if (!caps_only && !amiga_caps_cpu_080()) {
        early_message(argc, "DockAwesome needs a Vampire with a 68080 CPU.\n"
            "exec reports no 68080 (AttnFlags bit 10). Run DockAwesome --caps for details.");
        free(app); return 20;
    }
    dock_prefs_defaults(&app->prefs);
    config_state = read_config(app);
    if (config_state < 0) {
        char error[CONFIG_CAP+48];
        snprintf(error, sizeof(error), "Cannot read JSON preferences from %s.", app->config);
        early_message(argc, error);
        free(app); return 20;
    }
    app->settings.base_size = app->prefs.size;
    app->settings.max_size = app->prefs.zoom;
    app->scale_percent = app->prefs.scale_percent;
    app->settings.levels = app->prefs.levels;
    app->edge = app->prefs.edge;
    app->snapshot = app->prefs.snapshot;
    app->pip_mode = app->prefs.pip_mode;
    app->pip_x_offset = app->prefs.pip_x_offset;
    app->pip_y_offset = app->prefs.pip_y_offset;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--config") && i+1 < argc) ++i;
        else if (!strcmp(argv[i], "--size"))
            app->settings.base_size = i+1 < argc ?
                bounded_option(argv[++i],DOCK_MIN_ICON_SIZE,DOCK_MAX_ICON_SIZE) : INT_MAX;
        else if (!strcmp(argv[i], "--zoom")) {
            app->settings.max_size = i+1 < argc ?
                bounded_option(argv[++i],DOCK_MIN_ICON_SIZE,DOCK_MAX_ICON_SIZE) : INT_MAX;
            app->scale_percent = 0;
        }
        else if (!strcmp(argv[i], "--levels"))
            app->settings.levels = i+1 < argc ? bounded_option(argv[++i],2,DOCK_MAX_LEVELS) : INT_MAX;
        else if (!strcmp(argv[i], "--opaque")) { app->snapshot = 0; app->pip_mode = explicit_pip = 0; }
        else if (!strcmp(argv[i], "--snapshot")) { app->snapshot = 1; app->pip_mode = explicit_pip = 0; }
        else if (!strcmp(argv[i], "--pip")) { app->pip_mode = explicit_pip = 1; app->snapshot = 0; }
        else if (!strcmp(argv[i], "--ammx")) app->use_ammx = 1;
        else if (!strcmp(argv[i], "--profile")) app->profile = 1;
        else if (!strcmp(argv[i], "--damage-check")) app->damage_check = app->profile = 1;
        else if (!strcmp(argv[i], "--no-vsync")) app->no_vsync = 1;
        else if (!strcmp(argv[i], "--bench-sweep")) {
            app->bench_speed = i+1 < argc ? bounded_option(argv[++i],1,64) : INT_MAX;
            if (app->bench_speed == INT_MAX) { puts("--bench-sweep needs a speed from 1 to 64."); free(app); return 20; }
            app->profile = 1;
        }
        else if (!strcmp(argv[i], "--pip-x-offset") || !strcmp(argv[i], "--pip-y-offset")) {
            int is_x = !strcmp(argv[i], "--pip-x-offset");
            if (i+1 >= argc) { puts("PiP alignment needs an integer from -128 to 128."); free(app); return 20; }
            if (is_x) app->pip_x_offset = bounded_option(argv[++i],-128,128);
            else app->pip_y_offset = bounded_option(argv[++i],-128,128);
        }
        else if (!strcmp(argv[i], "--pip-dump")) {
            if (i+1 >= argc || strlen(argv[i+1]) >= sizeof(app->pip_dump)) {
                puts("--pip-dump requires a valid output prefix."); free(app); return 20;
            }
            strcpy(app->pip_dump, argv[++i]);
        }
    }
    if (app->scale_percent)
        app->settings.max_size = dock_scaled_size(app->settings.base_size,app->scale_percent);
    if (!dock_settings_valid(&app->settings)) { puts("Invalid size/zoom/levels. Use --help."); free(app); return 20; }
    if (app->pip_x_offset == INT_MAX || app->pip_y_offset == INT_MAX) {
        puts("PiP alignment must be an integer from -128 to 128."); free(app); return 20;
    }
    if (app->pip_dump[0] && !app->pip_mode) { puts("--pip-dump requires --pip."); free(app); return 20; }
    app->requested_backend = app->pip_mode ? DOCK_BACKEND_PIP :
        app->snapshot ? DOCK_BACKEND_SNAPSHOT : DOCK_BACKEND_OPAQUE;
    explicit_pip |= !!app->pip_dump[0];
#ifndef DOCK_AMMX
    if (app->use_ammx) { puts("Use the DockAwesome080 build for --ammx."); free(app); return 20; }
#endif
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 39);
    IconBase = OpenLibrary((STRPTR)"icon.library", 44);
    WorkbenchBase = OpenLibrary((STRPTR)"workbench.library", 44);
    LayersBase = OpenLibrary((STRPTR)"layers.library", 39);
    UtilityBase = OpenLibrary((STRPTR)"utility.library", 37);
    if (!IntuitionBase || !GfxBase || !IconBase || !WorkbenchBase || !LayersBase || !UtilityBase) {
        message("Needs AmigaOS 3.x, graphics/intuition V39 and icon/workbench V44 or newer."); goto done;
    }
    app->screen = LockPubScreen(NULL);
    if (!app->screen) { message("Cannot access the Workbench screen."); goto done; }
    amiga_caps_probe(&app->caps,app->screen);
    /* --pip and --pip-dump always request PiP. */
    select_backend(app,explicit_pip);
    if (caps_only) {
        amiga_caps_print(&app->caps,&app->features,app->requested_backend);
        status = 0; goto done;
    }
    if (app->features.error) { message(dock_profile_error_text(app->features.error)); goto done; }
    app->ui = create_ui(app);
    if (!app->ui) { message("Cannot set up the dock controls."); goto done; }
    dock_ui_set_blocking_hook(app->ui,ui_blocking,app);
    dock_ui_set_fit_hook(app->ui,ui_fit,app);
    use_pixel_backend(app);
    {
        DockSettings configured = app->settings;
        int count = app->prefs.count+!!app->prefs.folder[0];
        configured.end_gap = app->prefs.folder[0] ? STACK_DIVIDER_GAP : 0;
        if (!dock_items_fit(&configured,count,viewport_limit(app,app->edge))) {
            const char *error = "The saved pins do not fit the screen or cache.\n"
                "Use smaller icons or less zoom. Preferences are unchanged.";
            if (argc) puts(error);
            else message(error);
            goto done;
        }
    }
    if (!canonical_path("SYS:System/CLI",app->shell_launcher_path)) app->shell_launcher_path[0] = 0;
    for (i = 0; i < app->prefs.count; ++i) {
        int loaded = pin_path(app,app->prefs.paths[i],0);
        if (loaded < 0) goto done;
        if (!loaded) app->incomplete_pins = 1;
    }
    if (app->prefs.folder[0]) {
        Item stack = {0};
        int count = app->count+1;
        DockSettings settings = app->settings;
        settings.end_gap = STACK_DIVIDER_GAP;
        if (dock_items_fit(&settings,count,viewport_limit(app,app->edge)) &&
            prepare_stack(app,app->prefs.folder,&settings,&stack)) {
            exchange_stack(app,app->count,&stack);
            app->settings.end_gap = settings.end_gap;
        }
    }
    initial_count = app->count;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--config") || !strcmp(argv[i], "--size") || !strcmp(argv[i], "--zoom") || !strcmp(argv[i], "--levels") || !strcmp(argv[i], "--pip-dump") || !strcmp(argv[i], "--pip-x-offset") || !strcmp(argv[i], "--pip-y-offset") || !strcmp(argv[i], "--bench-sweep")) { ++i; continue; }
        if (!strncmp(argv[i], "--", 2)) continue;
        pin_path(app, argv[i], 0);
    }
    app->drop_port = CreateMsgPort();
    if (!app->drop_port) { message("Cannot open the Workbench drop port."); goto done; }
    i = open_surface(app);
    if (!i && app->pip_mode && !explicit_pip) {
        close_surface(app);
        i = fall_back_to_snapshot(app);
    }
    /* A partly opened surface can hold the PiP lease: report after cleanup. */
    if (!i) {
        fail_after_cleanup(app,"Cannot open the dock. Use an 8-bit or higher public screen.\n"
            "True color needs cybergraphics.library V40 or newer."); goto done;
    }
    if (!animation_open(app)) { fail_after_cleanup(app,"Cannot open timer.device for dock animation."); goto done; }
    if (!install_input(app)) { fail_after_cleanup(app,"Cannot install pointer observer in input.device."); goto done; }
    if (app->count != initial_count || config_state != 1) save_config(app);
    if (app->pip_mode) {
        struct Task *self = FindTask(NULL);
        int priority = self->tc_Node.ln_Pri, desktop_priority = priority;
        struct MsgPort *desktop;
        Forbid();
        desktop = FindPort((STRPTR)"WORKBENCH");
        if (desktop && desktop->mp_SigTask && ((struct Task *)desktop->mp_SigTask)->tc_Node.ln_Pri <= 2)
            desktop_priority = ((struct Task *)desktop->mp_SigTask)->tc_Node.ln_Pri;
        Permit();
        /* Workbench runs at +1 on the audited target and its drag loop can
           starve a priority 0 receiver. Share its priority; never busy-poll. */
        if (desktop_priority > priority) {
            app->saved_priority = SetTaskPri(self, desktop_priority);
            app->priority_changed = 1;
        }
    }
    program_poll_open(app);
    workbench_notify_open(app);
    if (app->bench_speed) bench_sweep(app);
    else event_loop(app);
    workbench_notify_close(app);
    program_poll_close(app);
    status = app->failed ? 20 : 0;
done:
    app->input_context.popup_open = 0;
    dock_ui_destroy(app->ui); app->ui = NULL;
    app->input_context.ui_busy = 0;
    remove_input(app);
    if (app->pip_dump[0] && app->source_valid && app->canvas.pixels) {
        int source_ok = dump_ppm(app->pip_dump, "source", &app->backdrop);
        int composed_ok = dump_ppm(app->pip_dump, "composed", &app->canvas);
        if (!source_ok || !composed_ok) {
            puts("Cannot write the PiP dump."); status = 20;
        }
    }
    profile_report(app);
    profile_close(app);
    close_surface(app);
    if (app->drop_port) {
        /* The AppWindow is gone. Reply to drops that were never handled. */
        struct Message *msg;
        while ((msg = GetMsg(app->drop_port))) ReplyMsg(msg);
        DeleteMsgPort(app->drop_port);
    }
    for (i = 0; i < app->count; ++i) item_free(&app->items[i]);
    dock_cache_free(&app->stack_button);
    free(app->morph_pixels);
    /* PiP, input, the drop receiver and the surface are released. */
    if (app->failed && app->surface_error[0]) message(app->surface_error);
    if (app->screen) UnlockPubScreen(NULL, app->screen);
    if (UtilityBase) CloseLibrary(UtilityBase);
    if (LayersBase) CloseLibrary(LayersBase);
    if (WorkbenchBase) CloseLibrary(WorkbenchBase);
    if (IconBase) CloseLibrary(IconBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    if (app->priority_changed) SetTaskPri(FindTask(NULL), app->saved_priority);
    free(app);
    return status;
}
