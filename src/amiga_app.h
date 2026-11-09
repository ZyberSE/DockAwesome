/* Private to the Amiga program: the App state and the declarations the
   modules split from amiga_main.c share. Field order is fixed: amiga_input.s
   reads VBlankState by offset, and keeping App's order keeps every compiled
   field offset of the single-file build. */
#ifndef AMIGA_APP_H
#define AMIGA_APP_H
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
#include "amiga_float.h"
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
enum { RUNNING_WORKBENCH = 1, RUNNING_WINDOW = 2, RUNNING_SELF = 4 };
typedef struct {
    char path[PATH_CAP], title_path[PATH_CAP]; DockCache cache;
    uint32_t pin_id;
    int running, kind, temporary, stack;
    const struct Task *owner; /* A task showing this item's window; compare only. */
    ULONG lister;             /* The Directory Opus lister showing this drawer. */
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
    /* The window modes' dock layer, which other windows can cover; NULL
       for PiP, which is always in front. */
    struct Layer *layer;
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
    volatile ULONG downs, ups, right_icons, ambiguous, overflow, right_seen, right_ambiguous;
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
        ULONG lister;              /* Output: the Directory Opus lister showing it. */
    } pins[DOCK_MAX_ITEMS];
    int dopus_drawers;             /* Directory Opus listers are the open drawers. */
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
    ULONG source_polls, layer_busy, source_changes, front_requests, shape_updates, shape_rects, shape_rects_peak;
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
/* A surface backend: how the dock reaches the screen. One table per backend,
   chosen with the profile (apply_profile) while no surface is open, so open,
   close and every frame use the same one. Each entry runs at most once per
   frame or poll; pixel loops stay inside the backend. See docs/architecture.md. */
typedef struct SurfaceOps {
    const char *name;
    /* The backend's part of open_surface, after the common steps have placed
       the surface, opened the display layer, allocated the canvas (and the
       backdrop for a backend that composes over the desktop) and laid out the
       row: fill the backdrop, open the native window and its AppWindow, and
       whatever present needs. 0 on failure, leaving close_surface to release
       a partial open (surface_error may say why). */
    int (*open)(struct App *app);
    /* Release everything open allocated, before the common steps release the
       display layer and canvas. Safe after a partial or no open. */
    void (*close)(struct App *app);
    /* Refresh the backdrop from the screen before a whole frame; full_compare
       compares every source pixel. Nonzero when the frame must be redrawn. */
    int (*capture)(struct App *app, int full_compare);
    /* Once per event-loop wake: due is set when the live timer fired, redraw
       when a redraw is already pending. Nonzero when the frame must be
       redrawn. Sets app->failed (with surface_error) if the surface is lost. */
    int (*poll)(struct App *app, int due, int redraw);
    /* Show the composed frame; called once per draw. */
    void (*present)(struct App *app, const DockImage *frame);
} SurfaceOps;
extern const SurfaceOps amiga_surface_pip, amiga_surface_window;

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
    unsigned char *source565, *coverage, *drag_mask, *drag_color; /* drag_color: the native drag image (RGB565) where drag_mask is set */
    int drag_reference, drag_mask_live;
    DockRect drag_box; /* bounds every nonzero drag_mask pixel */
    uint64_t front_requested;
    uint64_t settle_started;
    int settling;
    ProfileStats stats; /* --profile counters, reported at exit. */
    DockLaunch launches[LAUNCH_SLOTS]; /* Recent dock launches, newest last. */
    int launch_count;
    struct MsgPort *launch_port;       /* WBStartup replies of our own launches */
    struct DockLaunched *launched;     /* programs we started, until they reply */
    int self_launch;                   /* --self-launch: never ask Workbench */
    int rexx_pending;                  /* ARexx commands not yet replied */
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
    struct AmigaFloat *float_layer; /* PiP1 for labels and drags: PiP without window shapes */
    /* A dragged pin shown in float_layer: its gesture, Remove state, and the
       image's width and icon height for moves. */
    int drag_float, drag_float_remove, drag_float_width, drag_float_icon_height;
    uint32_t drag_float_token;
    const DockImage *drag_float_icon; /* the cache image shown, to see a level change */
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
    /* Added last so every other field keeps its offset. */
    const SurfaceOps *surface; /* The backend; set with the profile. */
} App;

/* Small helpers shared by several modules. Each was inlined into every
   caller in the single-file build; static inline keeps that per module. */

/* The EClock in ticks (clock_hz per second). The window present also
   passes it to the display layer as a callback. */
static inline uint64_t profile_clock(void)
{
    struct EClockVal clock;
    ReadEClock(&clock);
    return ((uint64_t)clock.ev_hi<<32) | clock.ev_lo;
}

/* A launch, close or Shell action: recheck Workbench status soon. */
static inline void status_changed(App *app)
{
    app->status_event_at = profile_clock()+app->clock_hz/5;
    /* A window can close before Workbench removes its process record. */
    app->status_recheck = 2;
}

static inline int pin_index(const App *app, uint32_t pin_id)
{
    int i;
    if (pin_id) for (i = 0; i < app->count; ++i) if (app->items[i].pin_id == pin_id) return i;
    return -1;
}

static inline int item_path_index(const App *app, const char *path)
{
    int i;
    for (i = 0; i < app->count; ++i)
        if (!app->items[i].stack && !Stricmp((STRPTR)app->items[i].path,(STRPTR)path)) return i;
    return -1;
}

static inline int row_count(const App *app)
{
    return app->count-!!app->moving_item;
}

static inline int row_item(const App *app, int slot)
{
    return slot+(app->moving_item && slot >= app->moving_item-1);
}

static inline const DockPlacement *row_placement(const App *app)
{
    return app->moving_item ? app->moving_placement : app->placement;
}

static inline int native_menu_held(const App *app)
{
    return (PeekQualifier() & IEQUALIFIER_RBUTTON) && !app->input_context.right_held;
}

#define ANY_BUTTON (IEQUALIFIER_LEFTBUTTON|IEQUALIFIER_RBUTTON|IEQUALIFIER_MIDBUTTON)

/* A button the dock tracks, or one of mask, is down. */
static inline int buttons_engaged(const App *app, UWORD mask)
{
    return app->input_context.held || app->input_context.external_held ||
        app->input_context.right_held || (PeekQualifier() & mask);
}

/* Cache lifetimes and slot order change only after the handler loses access.
   A swallowed DOWN still owns its UP, including across a surface rebuild. */
static inline void input_invalidate(App *app)
{
    Disable();
    app->input_context.active = NULL;
    ++app->input_generation;
    Enable();
}

static inline int image_alloc(DockImage *im, int width, int height)
{
    im->width = width; im->height = height;
    im->pixels = calloc((size_t)width * height, 4);
    return im->pixels != NULL;
}

static inline int viewport_limit(const App *app, DockEdge edge)
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

static inline DockUI *create_ui(const App *app)
{
    DockUI *ui = dock_ui_create(app->screen,app->features.ui_shapes,app->features.ui_glass,
        app->features.ui_rgb_pens,app->features.aa_text);
    dock_ui_set_float(ui,app->float_layer);
    return ui;
}

void interaction_cancel_for(App *app, uint32_t token);
static inline void interaction_cancel(App *app) { interaction_cancel_for(app, 0); }

__attribute__((noinline)) int load_icon_mode(const App *app, const char *path, DockImage *out,
    int *kind, int custom_only);
static inline int load_icon(const App *app, const char *path, DockImage *icon, int *kind)
{
    return load_icon_mode(app,path,icon,kind,0);
}

/* amiga_main.c: items, animation, timers, preferences and the event loop. */
int canonical_path(const char *path, char *absolute);
Item *append_item(App *app, const char *path, int kind, DockCache *cache);
void erase_item(App *app, int index);
int rest_after_pin_change(App *app, int before);
int animation_needed(const App *app);
void animation_cancel(App *app);
void live_cancel(App *app);
void stack_button_build(App *app);
void ui_blocking(void *context, int blocking);
int ui_fit(void *context, const DockUIPrefs *draft);
DockUIPrefs current_ui_prefs(const App *app);
int apply_preferences(App *app, const DockUIPrefs *prefs);

/* amiga_surface.c: the common surface steps and backend selection. */
int open_surface(App *app);
void close_surface(App *app);
void select_backend(App *app, int explicit_pip);
void use_pixel_backend(App *app);
int fall_back_to_snapshot(App *app);

/* amiga_input_handler.c: the input.device handler and its snapshots.
   dock_input_observe (amiga_platform.h) runs on input.device's task. */
int rounded_background(const DockRect *r, int radius, int x, int y);
void input_publish(App *app);
int input_pop(App *app, InputButton *button);
void input_attach_screen(App *app, struct Screen *screen);
int install_input(App *app);
void remove_input(App *app);

/* amiga_status.c: the Workbench status worker, window list and Shells. */
enum { WINDOW_CHECK, WINDOW_CLOSE, WINDOW_FOCUS };
void status_observe_windows(App *app);
void program_poll_open(App *app);
void program_poll_schedule(App *app);
int program_poll_result(App *app, int *recreate);
void program_poll_close(App *app);
int item_window_action(const App *app, const Item *item, int action);
ShellTarget shell_menu_target(const App *app);
int show_shell_window(const App *app, const ShellTarget *selected);
int quit_shell_window(App *app);
InputHandlerCode apollo_console_close_handler(const App *app);

/* amiga_launch.c: Workbench-style launches when Workbench is not running. */
int launch_open(App *app);
int launch_object(App *app, const char *path, LONG *error);
/* A file without its own icon that is a script (script bit, or not a
   loadable program) runs with Execute: 1 started, 0 failed (error set),
   -1 when path is no such script. */
int launch_script(const char *path, LONG *error);
int launch_active(const App *app, const char *path);
int launch_reap(App *app);
void launch_close(App *app);
int workbench_running(void);
int dopus_running(void);
int self_launching(const App *app);
void dopus_close_lister(App *app, ULONG lister);
void dopus_drawers(ProgramPoll *poll);

/* amiga_workbench.c: the screennotify.library Workbench client. */
void workbench_notify_open(App *app);
void workbench_notify_close(App *app);
int workbench_notify_poll(App *app);

#endif
