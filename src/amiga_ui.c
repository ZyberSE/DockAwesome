#include "amiga_ui.h"
#include "amiga_display.h"
#include "amiga_font_aa.h"
#include "about_font.h"
#include "amiga_folder.h"
#include "amiga_text.h"
#include "amiga_version.h"
#include <exec/libraries.h>
#include <exec/memory.h>
#include <graphics/clip.h>
#include <graphics/gfxbase.h>
#include <graphics/regions.h>
#include <graphics/text.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <graphics/layers.h>
#include <devices/inputevent.h>
#include <devices/timer.h>
#include <libraries/asl.h>
#include <libraries/gadtools.h>
#include <proto/asl.h>
#include <proto/diskfont.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/layers.h>
#include <proto/timer.h>
#include <proto/utility.h>
#include <utility/hooks.h>
#define GADTOOLS_BASE_NAME ui->gadtools
#include <proto/gadtools.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WA_ShapeRegion
#define WA_ShapeRegion (WA_Dummy + 65)
#endif
#ifndef RPTAG_PenMode
#define RPTAG_PenMode 0x80000080UL
#endif
#ifndef RPTAG_FgColor
#define RPTAG_FgColor 0x80000081UL
#endif

enum { GID_CANCEL = 1, GID_SIZE, GID_SCALE, GID_POSITION, GID_PATH, GID_BROWSE, GID_CLEAR, GID_SAVE };
enum { SIZE_SLIDER, SCALE_SLIDER, SLIDER_COUNT };
static const int slider_min[] = {DOCK_MIN_ICON_SIZE,DOCK_SCALE_ONE};
static const char *const settings_positions[] = {"Left","Bottom","Right",NULL};
static const DockEdge settings_edges[] = {DOCK_EDGE_LEFT,DOCK_EDGE_BOTTOM,DOCK_EDGE_RIGHT};
/* Size stops at 72 px and Scale at 2.50x, and the zoomed icon at 128 px
   like the macOS Dock: larger zooms blur the 48 px icons and take more
   than a 20 ms frame. */
enum { SIZE_SLIDER_MAX = 72, SCALE_SLIDER_MAX = 5*DOCK_SCALE_ONE/2, ZOOM_CEILING = 128 };
/* Hover labels: a pill with semicircular ends; the text's ink sits
   LABEL_PADDING px from each end and its capitals LABEL_VPAD px from the
   top and bottom. */
enum { POPUP_RADIUS = 6, LABEL_GAP = 8, LABEL_MARGIN = 4, LABEL_TIP = 4, LABEL_PADDING = 10,
    LABEL_VPAD = 5 };
enum {
    MENU_SURFACE, MENU_RIM, MENU_HOVER, MENU_TEXT, MENU_HOVER_TEXT,
    FOLDER_MUTED, MENU_COLORS
};
static const ULONG menu_colors[MENU_COLORS] = {
    0xff1b1f27UL, 0xff4b515cUL, 0xff374153UL, 0xfff2f4f8UL, 0xfff2f4f8UL,
    0xffaeb8c7UL
};
typedef struct { const char *label; int action; } MenuItem;
static const MenuItem menu_items[] = {
    {"Settings...", DOCK_UI_SETTINGS}, {"About...", DOCK_UI_ABOUT}, {"Quit", DOCK_UI_QUIT}
};
static const MenuItem close_item = {"Close", DOCK_UI_CLOSE_ITEM};
static const MenuItem drawer_items[] = {
    {"Show Contents", DOCK_UI_SHOW_CONTENTS}, {"Close", DOCK_UI_CLOSE_ITEM}
};
static const MenuItem quit_item = {"Quit", DOCK_UI_QUIT_ITEM};
static const MenuItem temporary_items[] = {
    {"Keep in Dock", DOCK_UI_KEEP_ITEM}, {"Quit", DOCK_UI_QUIT_ITEM}
};
static const MenuItem shell_items[] = {
    {"New Shell", DOCK_UI_NEW_SHELL}, {"Show Shell", DOCK_UI_SHOW_SHELL},
    {"Quit", DOCK_UI_QUIT_SHELL}
};
/* About typography: Vera Sans, prerendered and antialiased (about_font.c),
   because opening it through diskfont takes about 11 s after a reboot.
   gap: space after the line; the larger gap separates the credit. */
static const struct { const char *text; int style, gap; } about_lines[] = {
    {"DockAwesome",ABOUT_TITLE,8}, {dock_build_version,ABOUT_BODY,3},
    {dock_build_date,ABOUT_SMALL,16}, {"Created by",ABOUT_SMALL,3},
    {"Reine \"ZyBeR\" Delwall",ABOUT_NAME,0}
};
enum { ABOUT_LINES = sizeof(about_lines)/sizeof(about_lines[0]), ABOUT_MARGIN = 18 };
enum {
    FOLDER_COLS = 12, FOLDER_ROWS = 10, FOLDER_SCALE_ONE = 256,

    FOLDER_OPEN_MS = 250, FOLDER_CLOSE_MS = 200,
    FOLDER_TIP = 7, FOLDER_TIP_HALF = 9, FOLDER_BACKDROP_LAYERS = 16,
    FOLDER_OUTSIDE = -1, FOLDER_BACK = -2, FOLDER_PREV = -3, FOLDER_NEXT = -4
};
enum { FOLDER_STILL, FOLDER_OPENING, FOLDER_CLOSING };
typedef struct {
    DockEdge edge;
    int x, y;
    int icon_size, profile;
    DockUIReadIcon read_icon;
    const void *context;
} FolderAnchor;
typedef struct {
    struct Layer *layer;
    struct Rectangle bounds;
    WORD scroll_x, scroll_y;
} FolderBackdropLayer;
typedef struct {
    FolderAnchor anchor;
    char path[DOCK_PREFS_PATH_CAP];
    DockFolderEntry *entries;
    size_t count, page, root_length;
    int cols, rows, cell_width, cell_height, header, footer, read_ok;
    int body_left, body_width, body_height, tip;
    int pad, label_gap, action_wrap;
    int next_icon, artwork_ready, needs_repaint;
    int motion, scale, pivot_x, pivot_y, motion_frames, handoff;
    /* Reveal from the tip side: pixels composed or shown along the axis
       away from the dock, those already on screen, and the extent at the
       start of this motion. */
    int reveal_shown, reveal_presented, reveal_from;
    int scene_final; /* motion_scene holds the open box's final pixels */
    ULONG motion_start, motion_ticks, motion_paint_ticks;
    ULONG motion_prepare_sum, motion_present_sum; /* --profile: EClock ticks per stage */
    DockImage motion_backdrop;
    FolderBackdropLayer backdrop_layers[FOLDER_BACKDROP_LAYERS];
    int backdrop_layer_count;
    struct BitMap *handoff_bitmap;
    DockImage motion_scene, motion_frame;
    DockImage artwork;
    DockImage loaded[FOLDER_COLS*FOLDER_ROWS];
} FolderView;

typedef struct {
    struct Gadget *slider[SLIDER_COUNT], *readout[SLIDER_COUNT], *position, *path;
    struct Gadget *browse, *clear, *cancel, *save;
    int limit[SLIDER_COUNT]; /* What fits now. */
    int cap[SLIDER_COUNT];   /* Slider range, raised for larger saved values. */
    int zoom_cap;            /* ZOOM_CEILING, raised for a larger saved zoom. */
    char value[SLIDER_COUNT][16];
} SettingsView;

typedef struct FolderReveal FolderReveal;

struct DockUI {
    struct Screen *screen;
    struct Library *gadtools;
    APTR visual;
    struct DrawInfo *draw;
    struct TextAttr font;
    struct TextFont *folder_font;
    AmigaFontAA *folder_aa;
    AmigaFontAA *chrome_aa;
    struct Library *text_alpha;
    struct Window *popup, *dialog;
    struct Window *label;
    struct Region *popup_shape;
    struct Region *label_shape;
    int label_left, label_top;
    const MenuItem *popup_items;
    int popup_rows;
    AmigaDisplay *popup_display;
    DockConvertRGB24 encode;
    DockConvertRGB565 encode565;
    DockMixHalf mix_half;
    DockConvertRGB565 decode;
    DockImage popup_glass;
    FolderView folder;
    FolderReveal *folder_reveal;
    char folder_open_path[DOCK_PREFS_PATH_CAP];
    DockImage backdrop_carry;
    int carry_left, carry_top;
    DockUIBlocking blocking;
    void *blocking_context;
    DockUIFit fit;
    void *fit_context;
    char dialog_title[64], busy_title[96];
    struct Gadget *gadgets;
    struct Library *asl;
    DockUIPrefs draft;
    SettingsView settings;
    int font_height, font_width, popup_row, popup_selected, popup_pressed;
    int window_shapes, popup_rgb, showing_about;
    UWORD popup_pen[MENU_COLORS];
};

static int clamp(int value, int low, int high)
{
    return value < low ? low : (value > high ? high : value);
}

static int text_length32(struct RastPort *rp, const char *text)
{
    return amiga_text_length(rp,text,(ULONG)strlen(text));
}

static void text32(struct RastPort *rp, const char *text)
{
    amiga_text_draw(rp,text,(ULONG)strlen(text));
}

static int ready(DockUI *ui)
{
    if (!ui->gadtools) ui->gadtools = OpenLibrary((STRPTR)"gadtools.library", 37);
    if (!ui->gadtools) return 0;
    if (!ui->visual) ui->visual = GetVisualInfo(ui->screen, TAG_DONE);
    return ui->visual != NULL;
}

static void free_popup_glass(DockUI *ui)
{
    amiga_display_close(ui->popup_display);
    ui->popup_display = NULL;
    free(ui->popup_glass.pixels);
    ui->popup_glass = (DockImage){0};
}

static void free_folder_icons(FolderView *folder)
{
    int i;
    for (i = 0; i < FOLDER_COLS*FOLDER_ROWS; ++i) {
        free(folder->loaded[i].pixels);
        folder->loaded[i] = (DockImage){0};
    }
}

struct FolderBackfillMessage {
    struct Layer *layer;
    struct Rectangle bounds;
    LONG offset_x, offset_y;
};

typedef struct {
    struct Hook hook;
    struct Hook *original;
    struct BitMap *source, *screen;
    struct Rectangle bounds;
    FolderBackdropLayer target;
    FolderReveal *reveal;
    int restored;
} FolderBackfill;

struct FolderReveal {
    FolderBackfill fills[FOLDER_BACKDROP_LAYERS];
    AmigaDisplay *display;
    volatile ULONG deadline;
    volatile int closing, expired;
    int count, kept;
};

extern ULONG dock_folder_backfill_entry(void);

/* Layers supplies a screen-clipped rectangle. Never reenter a layer-aware
   graphics call from its own backfill callback. */
ULONG dock_folder_backfill(struct Hook *hook, struct RastPort *port,
    const struct FolderBackfillMessage *message)
{
    FolderBackfill *fill = hook->h_Data;
    const struct Rectangle *r = &message->bounds;
    const struct Layer *layer = message->layer;
    int x = r->MinX, y = r->MinY;
    int w = r->MaxX-x+1, h = r->MaxY-y+1;
    int preserve = fill->reveal->closing;
    if (!preserve && !fill->reveal->expired && layer && (layer->Flags&LAYERUPDATING)) {
        struct EClockVal now;
        ReadEClock(&now);
        preserve = (LONG)(now.ev_lo-fill->reveal->deadline) < 0;
    }
    if (preserve && layer == fill->target.layer &&
        layer->Scroll_X == fill->target.scroll_x && layer->Scroll_Y == fill->target.scroll_y &&
        !memcmp(&layer->bounds,&fill->target.bounds,sizeof(layer->bounds)) &&
        port->BitMap == fill->screen && x >= fill->bounds.MinX && y >= fill->bounds.MinY &&
        r->MaxX <= fill->bounds.MaxX && r->MaxY <= fill->bounds.MaxY) {
        BltBitMap(fill->source,x-fill->bounds.MinX,y-fill->bounds.MinY,
            port->BitMap,x,y,w,h,0xc0,0xff,NULL);
    } else if (fill->original == LAYERS_BACKFILL) {
        BltBitMap(port->BitMap,0,0,port->BitMap,x,y,w,h,0,0xff,NULL);
    } else if (fill->original != LAYERS_NOBACKFILL) {
        CallHookPkt(fill->original,port,(APTR)message);
    }
    return 0;
}

static int folder_backdrop_layers(struct Layer *layer, const struct Rectangle *bounds,
    FolderBackdropLayer *result)
{
    int n = 0;
    for (; layer; layer = layer->back) {
        const struct Rectangle *r = &layer->bounds;
        if (r->MaxX < bounds->MinX || r->MinX > bounds->MaxX ||
            r->MaxY < bounds->MinY || r->MinY > bounds->MaxY) continue;
        if (n == FOLDER_BACKDROP_LAYERS) return -1;
        result[n++] = (FolderBackdropLayer){layer,*r,layer->Scroll_X,layer->Scroll_Y};
    }
    return n;
}

/* Hook code that must outlive the dock: a foreign layer owner may call a hook
   we could not remove after DockAwesome has quit. Allocated once, never freed.
   [0] moveq #0,d0; rts           no-op for the default backfill choices
   [2] movea.l h_Data(a0),a0; move.l h_Entry(a0),-(sp); rts
                                   forward to the original hook in h_Data */
static const UWORD *persistent_hook_code(void)
{
    static UWORD *code;
    static const UWORD words[] = {0x7000,0x4e75,0x2068,0x0010,0x2f28,0x0008,0x4e75};
    if (!code && (code = AllocVec(sizeof(words),MEMF_PUBLIC))) {
        CopyMem((APTR)words,code,sizeof(words));
        CacheClearU();
    }
    return code;
}

static void finish_folder_reveal(DockUI *ui, int wait)
{
    FolderReveal *reveal = ui->folder_reveal;
    struct Layer_Info *info = &ui->screen->LayerInfo;
    struct Layer *layer;
    int i, busy = 0;
    if (!reveal) return;
    reveal->expired = 1;
    if (wait) LockLayerInfo(info);
    else if (!AttemptSemaphore(&info->Lock)) return;
    for (i = 0; i < reveal->count; ++i) {
        FolderBackfill *fill = &reveal->fills[i];
        if (fill->restored) continue;
        for (layer = info->top_layer; layer && layer != fill->target.layer; layer = layer->back) {}
        if (!layer) { fill->restored = 1; continue; }
        if (wait) LockLayer(0,layer);
        else if (!AttemptSemaphore(&layer->Lock)) { busy = 1; continue; }
        if (layer->BackFill == &fill->hook) InstallLayerHook(layer,fill->original);
        else {
            /* The owner installed its own hook and may restore or chain ours
               later, even after we quit. Keep this hook allocated and point
               it at persistent code: forward to the original, or do nothing
               for the default backfill choices. */
            const UWORD *code = persistent_hook_code();
            int real = fill->original != LAYERS_BACKFILL && fill->original != LAYERS_NOBACKFILL;
            Forbid();
            if (code) {
                fill->hook.h_SubEntry = NULL;
                fill->hook.h_Data = real ? (APTR)fill->original : NULL;
                fill->hook.h_Entry = (APTR)(real ? code+2 : code);
            } else if (real) {
                fill->hook.h_SubEntry = fill->original->h_SubEntry;
                fill->hook.h_Data = fill->original->h_Data;
                fill->hook.h_Entry = fill->original->h_Entry;
            }
            Permit();
            reveal->kept = 1;
        }
        fill->restored = 1;
        if (wait) UnlockLayer(layer);
        else ReleaseSemaphore(&layer->Lock);
    }
    if (wait) UnlockLayerInfo(info);
    else ReleaseSemaphore(&info->Lock);
    if (busy) return;
    amiga_display_close(reveal->display);
    reveal->display = NULL;
    if (!reveal->kept) FreeVec(reveal);
    ui->folder_reveal = NULL;
}

static void close_folder_window(DockUI *ui, int preserve)
{
    FolderView *f = &ui->folder;
    struct Window *w = ui->popup;
    struct Layer_Info *info = &ui->screen->LayerInfo;
    struct Rectangle bounds = {w->LeftEdge,w->TopEdge,
        w->LeftEdge+w->Width-1,w->TopEdge+w->Height-1};
    FolderBackdropLayer current[FOLDER_BACKDROP_LAYERS];
    /* Public memory: a foreign layer owner may keep a pointer to a hook. */
    FolderReveal *reveal = preserve && f->handoff_bitmap && f->backdrop_layer_count > 0 ?
        AllocVec(sizeof(*reveal),MEMF_PUBLIC|MEMF_CLEAR) : NULL;
    int n, i;
    if (reveal) {
        reveal->closing = 1;
        LockLayers(info);
        n = folder_backdrop_layers(w->WLayer->back,&bounds,current);
        if (n == f->backdrop_layer_count) {
            for (i = 0; i < n; ++i) {
                const FolderBackdropLayer *a = &current[i], *b = &f->backdrop_layers[i];
                if (a->layer != b->layer || a->scroll_x != b->scroll_x || a->scroll_y != b->scroll_y ||
                    memcmp(&a->bounds,&b->bounds,sizeof(a->bounds))) break;
            }
            if (i == n) for (i = 0; i < n; ++i) {
                struct Layer *layer = current[i].layer;
                FolderBackfill *fill = &reveal->fills[reveal->count];
                if (!layer->Window || !(layer->Flags&LAYERSIMPLE)) continue;
                fill->hook.h_Entry = dock_folder_backfill_entry;
                fill->hook.h_Data = fill;
                fill->bounds = bounds;
                fill->target = current[i];
                fill->reveal = reveal;
                fill->source = f->handoff_bitmap;
                fill->screen = ui->screen->RastPort.BitMap;
                fill->original = InstallLayerHook(layer,&fill->hook);
                ++reveal->count;
            }
        }
        UnlockLayers(info);
    }
    CloseWindow(w);
    if (reveal && reveal->count) {
        struct EClockVal now;
        ULONG hz = ReadEClock(&now);
        /* The exposed Simple Refresh windows can repaint after CloseWindow
           returns. Keep their backdrop available during that refresh only. */
        reveal->deadline = now.ev_lo+hz*140/1000;
        reveal->closing = 0;
        reveal->display = ui->popup_display;
        ui->popup_display = NULL;
        ui->folder_reveal = reveal;
    } else if (reveal) FreeVec(reveal);
}

static void close_popup(DockUI *ui, int preserve)
{
    struct IntuiMessage *msg;
    finish_folder_reveal(ui,1);
    if (ui->popup) {
        while ((msg = (struct IntuiMessage *)GetMsg(ui->popup->UserPort))) ReplyMsg((struct Message *)msg);
        close_folder_window(ui,preserve);
        ui->popup = NULL;
    }
    if (ui->popup_shape) {
        DisposeRegion(ui->popup_shape);
        ui->popup_shape = NULL;
    }
    free_popup_glass(ui);
    free_folder_icons(&ui->folder);
    free(ui->folder.entries);
    free(ui->folder.artwork.pixels);
    free(ui->folder.motion_backdrop.pixels);
    free(ui->folder.motion_scene.pixels);
    free(ui->folder.motion_frame.pixels);
    ui->folder = (FolderView){0};
    ui->popup_selected = ui->popup_pressed = -1;
}

void dock_ui_close_popup(DockUI *ui)
{
    close_popup(ui,0);
}

void dock_ui_close_dialog(DockUI *ui)
{
    struct IntuiMessage *msg;
    if (!ui) return;
    if (ui->dialog) {
        while ((msg = GT_GetIMsg(ui->dialog->UserPort))) GT_ReplyIMsg(msg);
        CloseWindow(ui->dialog);
        ui->dialog = NULL;
    }
    if (ui->gadgets) FreeGadgets(ui->gadgets);
    ui->gadgets = NULL;
    memset(&ui->settings,0,sizeof(ui->settings));
    if (ui->asl) CloseLibrary(ui->asl);
    ui->asl = NULL;
    ui->showing_about = 0;
}

DockUI *dock_ui_create(struct Screen *screen, int window_shapes)
{
    DockUI *ui;
    struct Library *DiskfontBase;
    struct TextAttr folder_face = {(STRPTR)"Helvetica.font",9,FS_NORMAL,0};
    if (!screen || !screen->Font) return NULL;
    ui = (DockUI *)calloc(1, sizeof(*ui));
    if (!ui) return NULL;
    ui->screen = screen;
    ui->window_shapes = window_shapes;
    /* ApolloOS exposes the AROS RGB RastPort tags on its RTG screens. */
    ui->popup_rgb = window_shapes && GfxBase->LibNode.lib_NegSize >= 0x40e &&
        GetBitMapAttr(screen->RastPort.BitMap,BMA_DEPTH) > 8;
    ui->draw = GetScreenDrawInfo(screen);
    if (!ui->draw || !ui->draw->dri_Font) {
        if (ui->draw) FreeScreenDrawInfo(screen,ui->draw);
        free(ui); return NULL;
    }
    ui->font = *screen->Font;
    ui->font_height = ui->draw->dri_Font->tf_YSize;
    ui->font_width = ui->draw->dri_Font->tf_XSize;
    if (ui->font_height < 8) ui->font_height = 8;
    if (ui->font_width < 6) ui->font_width = 6;
    DiskfontBase = OpenLibrary((STRPTR)"diskfont.library",37);
    if (DiskfontBase) {
        ui->folder_font = OpenDiskFont(&folder_face);
        CloseLibrary(DiskfontBase);
    }
    if (ui->folder_font && ui->folder_font->tf_YSize > folder_face.ta_YSize) {
        CloseFont(ui->folder_font);
        ui->folder_font = NULL;
    }
    if (!ui->folder_font) {
        folder_face.ta_Name = (STRPTR)"topaz.font";
        folder_face.ta_YSize = 8;
        ui->folder_font = OpenFont(&folder_face);
    }
    if (ui->popup_rgb && ui->folder_font) ui->folder_aa = amiga_font_aa_create(ui->folder_font);
    if (ui->popup_rgb) {
        ui->text_alpha = OpenLibrary((STRPTR)"cybergraphics.library",51);
        if (ui->text_alpha && ui->text_alpha->lib_NegSize < 0xde) {
            CloseLibrary(ui->text_alpha);
            ui->text_alpha = NULL;
        }
        if (ui->text_alpha) ui->chrome_aa = amiga_font_aa_create(ui->draw->dri_Font);
    }
    ui->popup_selected = ui->popup_pressed = -1;
    return ui;
}

void dock_ui_destroy(DockUI *ui)
{
    if (!ui) return;
    dock_ui_hide_label(ui);
    dock_ui_close_popup(ui);
    dock_ui_close_dialog(ui);
    if (ui->visual) FreeVisualInfo(ui->visual);
    if (ui->gadtools) CloseLibrary(ui->gadtools);
    amiga_font_aa_free(ui->folder_aa);
    amiga_font_aa_free(ui->chrome_aa);
    if (ui->text_alpha) CloseLibrary(ui->text_alpha);
    if (ui->folder_font) CloseFont(ui->folder_font);
    FreeScreenDrawInfo(ui->screen, ui->draw);
    free(ui);
}

ULONG dock_ui_signal_mask(const DockUI *ui)
{
    ULONG mask = 0;
    if (!ui) return 0;
    if (ui->popup) mask |= 1UL << ui->popup->UserPort->mp_SigBit;
    if (ui->dialog) mask |= 1UL << ui->dialog->UserPort->mp_SigBit;
    return mask;
}

int dock_ui_owns_window(const DockUI *ui, const struct Window *window)
{
    return ui && window && (window == ui->popup || window == ui->dialog || window == ui->label);
}

int dock_ui_busy(const DockUI *ui)
{
    return ui && (ui->popup || ui->dialog);
}

static int popup_hit(const DockUI *ui, int x, int y)
{
    if (x < 3 || x >= ui->popup->Width-3 || y < 3 || y >= 3+ui->popup_rows*ui->popup_row) return -1;
    return (y-3)/ui->popup_row;
}

static int rounded_inset(int radius, int row)
{
    int inset = 0, dy = 2*(radius-row)-1;
    if (row >= radius) return 0;
    while (inset < radius) {
        int dx = 2*(radius-inset)-1;
        if (dx*dx+dy*dy <= 4*radius*radius) break;
        ++inset;
    }
    return inset;
}

static void popup_span(const FolderView *folder, int width, int height, int radius,
    int row, int *left, int *right)
{
    int body_height = folder ? folder->body_height : height;
    int opposite = body_height-1-row;
    if (folder && row >= body_height) {
        int half = (height-1-row)*FOLDER_TIP_HALF/FOLDER_TIP+1;
        *left = folder->tip-half;
        *right = folder->tip+half+1;
    } else {
        int inset = rounded_inset(radius,row < opposite ? row : opposite);
        *left = inset+(folder ? folder->body_left : 0);
        *right = (folder ? folder->body_left+folder->body_width : width)-inset;
        if (folder && folder->anchor.edge != DOCK_EDGE_BOTTOM) {
            int distance = abs(row-folder->tip);
            int tip = distance < FOLDER_TIP_HALF ? FOLDER_TIP-distance*FOLDER_TIP/FOLDER_TIP_HALF : 0;
            if (folder->anchor.edge == DOCK_EDGE_LEFT) *left -= tip;
            else *right += tip;
        }
    }
}

static void popup_inner_span(const FolderView *folder, int width, int height, int radius,
    int row, int lo, int hi, int *inner_lo, int *inner_hi)
{
    int above_lo, above_hi, below_lo, below_hi;
    popup_span(folder,width,height,radius,row ? row-1 : row,&above_lo,&above_hi);
    popup_span(folder,width,height,radius,row+1 < height ? row+1 : row,&below_lo,&below_hi);
    *inner_lo = clamp(above_lo,lo+1,hi);
    if (*inner_lo < below_lo) *inner_lo = below_lo;
    *inner_hi = clamp(above_hi,lo,hi-1);
    if (*inner_hi > below_hi) *inner_hi = below_hi;
    if (!row || row+1 == height || *inner_hi < *inner_lo) *inner_lo = *inner_hi = lo;
}

void dock_ui_popup_span(const DockUI *ui, int row, int *left, int *right)
{
    int width = ui->popup->Width, height = ui->popup->Height;
    if (!ui->popup_shape) { *left = 0; *right = width; return; }
    popup_span(ui->folder.path[0] && ui->window_shapes ? &ui->folder : NULL,
        width,height,POPUP_RADIUS,row,left,right);
}

static struct Region *make_folder_shape(const FolderView *folder, int width, int height)
{
    struct Region *shape = NewRegion();
    int start = 0, left, right, row;
    if (!shape) return NULL;
    popup_span(folder,width,height,POPUP_RADIUS,0,&left,&right);
    for (row = 1; row <= height; ++row) {
        int next_left = 0, next_right = 0;
        if (row < height)
            popup_span(folder,width,height,POPUP_RADIUS,row,&next_left,&next_right);
        if (left != next_left || right != next_right) {
            struct Rectangle rect = {left,start,right-1,row-1};
            if (!OrRectRegion(shape,&rect)) { DisposeRegion(shape); return NULL; }
            start = row;
            left = next_left;
            right = next_right;
        }
    }
    return shape;
}

static struct Region *make_rounded_shape(int width, int height, int radius)
{
    struct Region *shape = NewRegion();
    int row;
    if (!shape) return NULL;
    for (row = 0; row < radius; ++row) {
        int inset = rounded_inset(radius,row);
        struct Rectangle rect = {inset, row, width-1-inset, row};
        if (!OrRectRegion(shape,&rect)) goto fail;
        rect.MinY = rect.MaxY = height-1-row;
        if (!OrRectRegion(shape,&rect)) goto fail;
    }
    {
        struct Rectangle middle = {0, radius, width-1, height-1-radius};
        if (OrRectRegion(shape,&middle)) return shape;
    }
fail:
    DisposeRegion(shape);
    return NULL;
}

static void prepare_popup_colors(DockUI *ui)
{
    static const UWORD fallback[MENU_COLORS] = {
        BACKGROUNDPEN, SHADOWPEN, FILLPEN, TEXTPEN, FILLTEXTPEN,
        TEXTPEN
    };
    struct ColorMap *map = ui->screen->ViewPort.ColorMap;
    int i, last_pen;
    last_pen = map && map->Count ? (map->Count < 256 ? map->Count-1 : 255) : -1;
    for (i = 0; i < MENU_COLORS; ++i) {
        ULONG rgb = menu_colors[i];
        LONG pen = last_pen >= 0 ? FindColor(map,((rgb>>16)&255)*0x01010101UL,
            ((rgb>>8)&255)*0x01010101UL,(rgb&255)*0x01010101UL,last_pen) : -1;
        ui->popup_pen[i] = pen >= 0 ? (UWORD)pen : ui->draw->dri_Pens[fallback[i]];
    }
    if (ui->popup_pen[MENU_TEXT] == ui->popup_pen[MENU_SURFACE]) {
        ui->popup_pen[MENU_SURFACE] = ui->draw->dri_Pens[BACKGROUNDPEN];
        ui->popup_pen[MENU_TEXT] = ui->draw->dri_Pens[TEXTPEN];
    }
    if (ui->popup_pen[MENU_HOVER] == ui->popup_pen[MENU_SURFACE] ||
        ui->popup_pen[MENU_HOVER] == ui->popup_pen[MENU_HOVER_TEXT]) {
        ui->popup_pen[MENU_HOVER] = ui->draw->dri_Pens[FILLPEN];
        ui->popup_pen[MENU_HOVER_TEXT] = ui->draw->dri_Pens[FILLTEXTPEN];
    }
}

static void popup_color(const DockUI *ui, struct RastPort *rp, int role)
{
    if (ui->popup_rgb) {
        const struct TagItem tags[] = {{RPTAG_PenMode, FALSE},
            {RPTAG_FgColor, menu_colors[role]}, {TAG_DONE, 0}};
        SetRPAttrsA(rp,tags);
    } else SetAPen(rp,ui->popup_pen[role]);
}

static void paint_ui_text(DockUI *ui, struct RastPort *rp, AmigaFontAA *font,
    const char *text, int x, int top, int color)
{
    popup_color(ui,rp,color);
    if (!amiga_font_aa_blit(font,ui->text_alpha,rp,text,x,top)) {
        Move(rp,x,top+rp->TxBaseline);
        text32(rp,text);
    }
}

static void fill_rounded(struct RastPort *rp, int x, int y, int width, int height, int radius)
{
    int row, right = x+width-1, bottom = y+height-1;
    RectFill(rp,x,y+radius,right,bottom-radius);
    for (row = 0; row < radius; ++row) {
        int inset = rounded_inset(radius,row);
        RectFill(rp,x+inset,y+row,right-inset,y+row);
        RectFill(rp,x+inset,bottom-row,right-inset,bottom-row);
    }
}

static AmigaDisplay *capture_glass(const DockUI *ui, DockImage *glass,
    int left, int top, int width, int height)
{
    AmigaDisplay *display;
    if (!ui->popup_rgb) return NULL;
    display = amiga_display_open(ui->screen,width,height);
    if (!display) return NULL;
    amiga_display_set_converters(display,ui->encode,ui->decode,ui->encode565);
    *glass = (DockImage){width,height,malloc((size_t)width*height*4)};
    if (glass->pixels && amiga_display_capture(display,glass,left,top)) return display;
    free(glass->pixels);
    *glass = (DockImage){0};
    amiga_display_close(display);
    return NULL;
}

static uint32_t glass_tint(int shift)
{
    uint32_t tint = 255;
    int c;
    for (c = 0; c < 3; ++c) {
        unsigned int channel = (menu_colors[MENU_SURFACE] >> (16-8*c)) & 255;
        unsigned int base = ((1U<<shift)-1)*channel+(1U<<(shift-1));
        tint |= (base>>shift)<<(24-8*c);
    }
    return tint;
}

/* Copy the previous view's clean backdrop where it overlaps the new view.
   Simple Refresh windows behind the old view have not repainted yet. */
static void apply_backdrop_carry(const DockUI *ui, DockImage *glass, int left, int top)
{
    const DockImage *carry = &ui->backdrop_carry;
    int x0 = left > ui->carry_left ? left : ui->carry_left;
    int y0 = top > ui->carry_top ? top : ui->carry_top;
    int x1 = left+glass->width, y1 = top+glass->height, y;
    if (!carry->pixels) return;
    if (x1 > ui->carry_left+carry->width) x1 = ui->carry_left+carry->width;
    if (y1 > ui->carry_top+carry->height) y1 = ui->carry_top+carry->height;
    if (x1 <= x0 || y1 <= y0) return;
    for (y = y0; y < y1; ++y)
        memcpy(glass->pixels+((size_t)(y-top)*glass->width+(x0-left))*4,
            carry->pixels+((size_t)(y-ui->carry_top)*carry->width+(x0-ui->carry_left))*4,
            (size_t)(x1-x0)*4);
}

static void capture_popup_glass(DockUI *ui, int left, int top, int width, int height, int tint_shift)
{
    DockImage *glass = &ui->popup_glass;
    const FolderView *folder = ui->popup_shape && ui->folder.path[0] ? &ui->folder : NULL;
    int radius = ui->popup_shape ? POPUP_RADIUS : 0;
    const uint32_t mask = (255U>>tint_shift)*UINT32_C(0x01010100);
    const uint32_t rim = (menu_colors[MENU_RIM]<<8)|255;
    const uint32_t tint = glass_tint(tint_shift);
    int y;
    ui->popup_display = capture_glass(ui,glass,left,top,width,height);
    if (!ui->popup_display) return;
    apply_backdrop_carry(ui,glass,left,top);
    if (folder) {
        size_t bytes = (size_t)width*height*4;
        DockImage *under = &ui->folder.motion_backdrop;
        *under = (DockImage){width,height,malloc(bytes)};
        if (under->pixels) {
            struct Layer_Info *info = &ui->screen->LayerInfo;
            struct Rectangle bounds = {left,top,left+width-1,top+height-1};
            memcpy(under->pixels,glass->pixels,bytes);
            LockLayers(info);
            ui->folder.backdrop_layer_count =
                folder_backdrop_layers(info->top_layer,&bounds,ui->folder.backdrop_layers);
            UnlockLayers(info);
        }
    }
    for (y = 0; y < height; ++y) {
        int lo, hi, inner_lo, inner_hi, x;
        unsigned char *row = glass->pixels+(size_t)y*width*4;
        popup_span(folder,width,height,radius,y,&lo,&hi);
        popup_inner_span(folder,width,height,radius,y,lo,hi,&inner_lo,&inner_hi);
        /* Native RGBA bytes are a big-endian word. Independent lanes keep
           the background contribution and tint from spilling across channels. */
        for (x = lo; x < inner_lo; ++x) memcpy(row+x*4,&rim,4);
        for (; x < inner_hi; ++x) {
            uint32_t pixel;
            memcpy(&pixel,row+x*4,4);
            pixel = ((pixel>>tint_shift)&mask)+tint;
            memcpy(row+x*4,&pixel,4);
        }
        for (; x < hi; ++x) memcpy(row+x*4,&rim,4);
    }
}

static void paint_popup_surface(DockUI *ui, const DockImage *surface)
{
    struct RastPort *rp = ui->popup->RPort;
    int width = ui->popup->Width, height = ui->popup->Height;
    SetFont(rp, ui->draw->dri_Font);
    SetDrMd(rp, JAM1);
    if (!surface->pixels || !amiga_display_present(ui->popup_display,ui->popup,surface)) {
        popup_color(ui,rp,MENU_RIM);
        RectFill(rp,0,0,width-1,height-1);
        popup_color(ui,rp,MENU_SURFACE);
        fill_rounded(rp,1,1,width-2,height-2,ui->popup_shape ? POPUP_RADIUS-1 : 0);
    }
}

static void paint_popup(DockUI *ui)
{
    struct RastPort *rp = ui->popup->RPort;
    int i, width = ui->popup->Width;
    paint_popup_surface(ui,&ui->popup_glass);
    for (i = 0; i < ui->popup_rows; ++i) {
        int top = 3+i*ui->popup_row;
        if (i == ui->popup_selected) {
            popup_color(ui,rp,MENU_HOVER);
            fill_rounded(rp,3,top,width-6,ui->popup_row,4);
        }
        paint_ui_text(ui,rp,ui->chrome_aa,ui->popup_items[i].label,10,
            top+(ui->popup_row-ui->font_height)/2,
            i == ui->popup_selected ? MENU_HOVER_TEXT : MENU_TEXT);
    }
}

static size_t folder_page_size(const FolderView *f)
{
    return f->cols*f->rows-1; /* Reserve one tile for opening the drawer in Workbench. */
}

static size_t folder_last_page(const FolderView *f)
{
    return f->count ? (f->count-1)/folder_page_size(f) : 0;
}

static int folder_visible_files(const FolderView *f)
{
    size_t remaining = f->count-f->page*folder_page_size(f);
    return remaining < folder_page_size(f) ? remaining : folder_page_size(f);
}

static int folder_nested(const FolderView *f)
{
    return strlen(f->path) > f->root_length;
}

static struct TextFont *folder_font(const DockUI *ui)
{
    return ui->folder_font ? ui->folder_font : ui->draw->dri_Font;
}

static void fit_folder_text(struct RastPort *rp, const char *text, int width, char *label)
{
    int length = strlen(text);
    const char *extension = strrchr(text,'.');
    int suffix = extension && extension != text ? (int)strlen(extension) : 1;
    int lo = 2, hi = length-1;
    strcpy(label,text);
    if (text_length32(rp,text) <= width) return;
    strcpy(label,"...");
    while (lo <= hi) {
        char candidate[DOCK_PREFS_PATH_CAP+3];
        int keep = (lo+hi)/2;
        int tail = (keep+1)/2;
        if (tail < suffix && suffix < 7) tail = suffix;
        if (tail >= keep) tail = keep-1;
        int head = keep-tail;
        memcpy(candidate,text,head);
        memcpy(candidate+head,"...",3);
        strcpy(candidate+head+3,text+length-tail);
        if (text_length32(rp,candidate) <= width) {
            strcpy(label,candidate);
            lo = keep+1;
        } else hi = keep-1;
    }
}


static void centered_folder_text(DockUI *ui, struct RastPort *rp, DockImage *target,
    const char *text, int center, int top, int width)
{
    char label[DOCK_PREFS_PATH_CAP+3];
    int measured;
    fit_folder_text(rp,text,width,label);
    measured = text_length32(rp,label);
    if (measured > width) return;
    if (target) {
        amiga_font_aa_draw(ui->folder_aa,target,label,center-measured/2,top,menu_colors[MENU_TEXT]);
    } else {
        popup_color(ui,rp,MENU_TEXT);
        Move(rp,center-measured/2,top+rp->TxBaseline);
        text32(rp,label);
    }
}

/* The two parts of the arrow each cover one span per row. Coordinates are
   relative to the ring center in units of 1/64 of its radius. */
static void fill_arrow_part(struct RastPort *rp, int x, int y, int radius,
    const BYTE (*outline)[2], int count)
{
    int row;
    for (row = -radius/2; row <= radius/2; ++row) {
        int sample = row*64/radius;
        int left = 64, right = -64, i, previous;
        for (i = 0, previous = count-1; i < count; previous = i++) {
            int from_y = outline[previous][1], to_y = outline[i][1];
            if ((sample < from_y) != (sample < to_y)) {
                int from_x = outline[previous][0], to_x = outline[i][0];
                int hit = from_x+(to_x-from_x)*(sample-from_y)/(to_y-from_y);
                if (hit < left) left = hit;
                if (hit > right) right = hit;
            }
        }
        if (left <= right)
            RectFill(rp,x+left*radius/64,y+row,x+right*radius/64,y+row);
    }
}

static void folder_open_glyph(DockUI *ui, struct RastPort *rp, int x, int y, int radius)
{
    static const BYTE tail[][2] = {
        {4,-15}, {-8,-14}, {-17,-11}, {-24,-6}, {-29,2}, {-32,12},
        {-33,22}, {-31,25}, {-28,24}, {-24,18}, {-19,13}, {-12,10}, {4,8}
    };
    static const BYTE head[][2] = {
        {1,-28}, {4,-31}, {7,-30}, {33,-6}, {35,-3}, {33,1},
        {7,24}, {4,25}, {1,23}
    };
    int thickness = clamp((radius+6)/13,1,radius), inset;
    popup_color(ui,rp,FOLDER_MUTED);
    for (inset = 0; inset < thickness; ++inset)
        DrawEllipse(rp,x,y,radius-inset,radius-inset);
    fill_arrow_part(rp,x,y,radius,tail,sizeof(tail)/sizeof(tail[0]));
    fill_arrow_part(rp,x,y,radius,head,sizeof(head)/sizeof(head[0]));
}

/* The span [*left,*right] where a sample row y crosses an outline given in
   1/64 of the radius, as fill_arrow_part fills it; positions in 1/16 px. */
static int outline_span(const BYTE (*outline)[2], int count, int radius, int y, int *left, int *right)
{
    int i, previous, found = 0;
    for (i = 0, previous = count-1; i < count; previous = i++) {
        int y1 = outline[previous][1]*radius/4, y2 = outline[i][1]*radius/4;
        if ((y1 > y) != (y2 > y)) {
            int x1 = outline[previous][0]*radius/4, x2 = outline[i][0]*radius/4;
            int hit = x1+(x2-x1)*(y-y1)/(y2-y1);
            if (!found || hit < *left) *left = hit;
            if (!found || hit > *right) *right = hit;
            found = 1;
        }
    }
    return found;
}

static int isqrt(int value)
{
    int root = 0, bit = 1<<28;
    if (value <= 0) return 0;
    while (bit > value) bit >>= 2;
    while (bit) {
        if (value >= root+bit) { value -= root+bit; root = (root>>1)+bit; }
        else root >>= 1;
        bit >>= 2;
    }
    return root;
}

/* folder_open_glyph antialiased into an RGBA target: 4x4 samples per pixel.
   Each sample row is resolved to spans once (the ring, the arrow's tail and
   head), then the samples only compare positions. */
static void folder_open_glyph_aa(DockImage *target, int cx, int cy, int radius, uint32_t color)
{
    static const BYTE tail[][2] = {
        {4,-15}, {-8,-14}, {-17,-11}, {-24,-6}, {-29,2}, {-32,12},
        {-33,22}, {-31,25}, {-28,24}, {-24,18}, {-19,13}, {-12,10}, {4,8}
    };
    static const BYTE head[][2] = {
        {1,-28}, {4,-31}, {7,-30}, {33,-6}, {35,-3}, {33,1},
        {7,24}, {4,25}, {1,23}
    };
    unsigned int red = (color>>16)&255, green = (color>>8)&255, blue = color&255;
    /* The ring: one native pixel plus half a pixel wide, so it keeps its
       weight once antialiased. Radii and positions in 1/16 px. */
    int thickness = clamp((radius+6)/13,1,radius);
    int outer = radius*16+8, inner = outer-thickness*16-8;
    unsigned char covered[2*DOCK_MAX_ICON_SIZE+4];
    int x, y, size = 2*radius+3;
    if (radius < 4 || size > (int)sizeof(covered) || !target || !target->pixels) return;
    for (y = 0; y < size; ++y) {
        int py = cy-radius-1+y, sy;
        if (py < 0 || py >= target->height) continue;
        memset(covered,0,(size_t)size);
        for (sy = 0; sy < 4; ++sy) {
            int dy = (py-cy)*16+2+4*sy-8, ring_out = -1, ring_in = -1, sx;
            int tail_l = 0, tail_r = -1, head_l = 0, head_r = -1;
            if (dy*dy <= outer*outer) ring_out = isqrt(outer*outer-dy*dy);
            if (dy*dy < inner*inner) ring_in = isqrt(inner*inner-dy*dy);
            if (!outline_span(tail,sizeof(tail)/sizeof(tail[0]),radius,dy,&tail_l,&tail_r)) tail_r = tail_l-1;
            if (!outline_span(head,sizeof(head)/sizeof(head[0]),radius,dy,&head_l,&head_r)) head_r = head_l-1;
            for (x = 0; x < size; ++x) for (sx = 0; sx < 4; ++sx) {
                int dx = (x-radius-1)*16+2+4*sx-8, ax = dx < 0 ? -dx : dx;
                if ((ax <= ring_out && ax >= ring_in) || (dx >= tail_l && dx <= tail_r) ||
                    (dx >= head_l && dx <= head_r)) ++covered[x];
            }
        }
        for (x = 0; x < size; ++x) {
            int px = cx-radius-1+x;
            unsigned int alpha, remaining;
            unsigned char *dst;
            if (!covered[x] || px < 0 || px >= target->width) continue;
            alpha = (covered[x]*255+8)/16; remaining = 255-alpha;
            dst = target->pixels+((size_t)py*target->width+px)*4;
            dst[0] = (red*alpha+dst[0]*remaining+127)/255;
            dst[1] = (green*alpha+dst[1]*remaining+127)/255;
            dst[2] = (blue*alpha+dst[2]*remaining+127)/255;
            dst[3] = alpha+(dst[3]*remaining+127)/255;
        }
    }
}

static void folder_glyph(DockUI *ui, struct RastPort *rp, int x, int y, int drawer, int open)
{
    const FolderView *f = &ui->folder;
    int size = f->anchor.icon_size;
    int half = size/2-2, radius = size/16;
    if (open) {
        folder_open_glyph(ui,rp,x,y,half);
    } else if (drawer) {
        popup_color(ui,rp,MENU_RIM);
        fill_rounded(rp,x-half,y-half*3/4,half,half/2,radius/2);
        popup_color(ui,rp,FOLDER_MUTED);
        fill_rounded(rp,x-half,y-half/2,half*2,half*5/4,radius);
    } else {
        int width = half*3/2, fold = half/2;
        popup_color(ui,rp,MENU_TEXT);
        fill_rounded(rp,x-width/2,y-half,width,half*2,radius);
        popup_color(ui,rp,MENU_RIM);
        Move(rp,x+width/2-fold,y-half);
        Draw(rp,x+width/2-fold,y-half+fold);
        Draw(rp,x+width/2-1,y-half+fold);
    }
}

static void folder_button(DockUI *ui, struct RastPort *rp, int direction,
    int x, int y, int width, int height)
{
    popup_color(ui,rp,MENU_HOVER);
    fill_rounded(rp,x,y,width,height,4);
    popup_color(ui,rp,MENU_TEXT);
    x += (width-7)/2+3;
    y += (height-7)/2+3;
    Move(rp,x-3*direction,y-3);
    Draw(rp,x+3*direction,y);
    Draw(rp,x-3*direction,y+3);
}

enum { FOLDER_PAINT_LABELS = 1, FOLDER_PAINT_GLYPHS = 2 };

static void paint_folder_content(DockUI *ui, struct RastPort *rp, int parts, DockImage *target)
{
    FolderView *f = &ui->folder;
    int font_height = folder_font(ui)->tf_YSize;
    int i, width = f->body_width, offset = f->body_left, control = font_height+10;
    int visible = folder_visible_files(f), icon = f->anchor.icon_size;
    size_t first = f->page*folder_page_size(f);
    const char *title = (const char *)FilePart((STRPTR)f->path);
    int labels = parts&FOLDER_PAINT_LABELS, glyphs = parts&FOLDER_PAINT_GLYPHS;
    if (!title[0]) title = f->path;

    if (!target) SetDrMd(rp,JAM1);
    SetFont(rp,folder_font(ui));
    if (labels)
        centered_folder_text(ui,rp,target,title,offset+width/2,(f->header-font_height)/2,width-4*control);
    if (glyphs && folder_nested(f))
        folder_button(ui,rp,-1,offset+f->pad,(f->header-control)/2,control,control);
    if (labels && !f->count) {
        centered_folder_text(ui,rp,target,f->read_ok ? "Empty drawer" : "Cannot read drawer",
            offset+width/2,f->header,width-2*f->pad);
    }
    for (i = 0; i <= visible; ++i) {
        int open = i == visible;
        int center = offset+(f->count ? f->pad+(i%f->cols)*f->cell_width+f->cell_width/2 : width/2);
        int top = f->header+(i/f->cols)*f->cell_height+(f->count ? 0 : font_height+6);
        int label_top = top+icon+f->label_gap;
        /* The open glyph is antialiased wherever the labels are (into an
           RGBA target); a native pass draws it only with native labels. */
        if (open && labels && target)
            folder_open_glyph_aa(target,center,top+icon/2,icon/2-2,menu_colors[FOLDER_MUTED]);
        else if (glyphs && (open ? labels : !f->loaded[i].pixels))
            folder_glyph(ui,rp,center,top+icon/2,!open && f->entries[first+i].drawer,open);
        if (!labels) continue;
        if (open && f->action_wrap) {
            centered_folder_text(ui,rp,target,"Open in",center,label_top,f->cell_width-6);
            centered_folder_text(ui,rp,target,"Workbench",center,label_top+font_height,f->cell_width-6);
        } else {
            centered_folder_text(ui,rp,target,open ? "Open in Workbench" : f->entries[first+i].name,
                center,label_top,f->cell_width-6);
        }
    }
    if (folder_last_page(f)) {
        int y = f->body_height-f->footer;
        if (labels) {
            char page[36];
            snprintf(page,sizeof(page),"%lu / %lu",(unsigned long)f->page+1,
                (unsigned long)folder_last_page(f)+1);
            centered_folder_text(ui,rp,target,page,offset+width/2,y+(control-font_height)/2,width);
        }
        if (glyphs && f->page) folder_button(ui,rp,-1,offset+f->pad,y,control,control);
        if (glyphs && f->page < folder_last_page(f))
            folder_button(ui,rp,1,offset+width-f->pad-control,y,control,control);
    }
}

static void paint_folder_labels(DockUI *ui, DockImage *target)
{
    struct RastPort measure;
    InitRastPort(&measure);
    paint_folder_content(ui,&measure,FOLDER_PAINT_LABELS,target);
}

static int prepare_folder_artwork(DockUI *ui);

static const DockImage *folder_background(const DockUI *ui)
{
    return ui->folder.artwork_ready ? &ui->folder.artwork : &ui->popup_glass;
}

/* Final glass: alpha 0 adds an eighth of the backdrop, alpha 255 is
   artwork, partial alpha scales the glass. */
static uint32_t folder_glass(uint32_t pixel, uint32_t under)
{
    unsigned int transmission = 255-(pixel&255);
    if (transmission) {
        uint32_t glass = (under>>3)&UINT32_C(0x1f1f1f00);
        if (transmission < 255) {
            uint32_t rb = ((glass>>8)&UINT32_C(0x00ff00ff))*transmission+UINT32_C(0x00800080);
            uint32_t g = ((glass>>16)&255)*transmission+128;
            glass = (rb&UINT32_C(0xff00ff00))+((g&0xff00)<<8);
        }
        pixel += glass;
    }
    return pixel|255;
}

/* The box grows out of the dock: rows from the tip upward for a bottom
   dock, columns away from a side dock. */
static int folder_reveal_extent(const FolderView *f, int width, int height)
{
    return f->anchor.edge == DOCK_EDGE_BOTTOM ? height : width;
}

/* The window rectangle of reveal positions [from,to), counted from the tip. */
static void folder_reveal_rect(const FolderView *f, int width, int height, int from, int to,
    int *x, int *y, int *w, int *h)
{
    if (f->anchor.edge == DOCK_EDGE_BOTTOM) { *x = 0; *w = width; *y = height-to; *h = to-from; }
    else if (f->anchor.edge == DOCK_EDGE_LEFT) { *y = 0; *h = height; *x = from; *w = to-from; }
    else { *y = 0; *h = height; *x = width-to; *w = to-from; }
}

/* The open box's final pixels, in place of the scene: glass over the
   backdrop, artwork as drawn, the backdrop outside the outline. */
static void finish_folder_scene(FolderView *f)
{
    DockImage *scene = &f->motion_scene;
    int width = scene->width, height = scene->height, y;
    for (y = 0; y < height; ++y) {
        size_t offset = (size_t)y*width;
        uint32_t *row = (uint32_t *)scene->pixels+offset;
        const uint32_t *under = (const uint32_t *)f->motion_backdrop.pixels+offset;
        int lo, hi, x;
        popup_span(f,width,height,POPUP_RADIUS,y,&lo,&hi);
        for (x = 0; x < width; ++x)
            row[x] = x < lo || x >= hi ? under[x] : folder_glass(row[x],under[x]);
    }
}

/* Average the frame with target over the reveal [0,shown): one fade step,
   halving the remaining difference per frame (an ease-out). */
static void fade_folder_reveal(DockUI *ui, const DockImage *target, int shown)
{
    FolderView *f = &ui->folder;
    int width = f->motion_frame.width, height = f->motion_frame.height, x, y, w, h, row;
    if (shown <= 0) return;
    folder_reveal_rect(f,width,height,0,shown,&x,&y,&w,&h);
    if (w == width)
        ui->mix_half(f->motion_frame.pixels+(size_t)y*width*4,target->pixels+(size_t)y*width*4,
            (unsigned long)w*h);
    else for (row = y; row < y+h; ++row)
        ui->mix_half(f->motion_frame.pixels+((size_t)row*width+x)*4,target->pixels+((size_t)row*width+x)*4,
            (unsigned long)w);
}

/* Put [0,extent) of the reveal on screen. A frame is skipped rather than
   waiting while Workbench holds the layers (a desktop click); the next one
   covers the whole region again. Returns 1 when it was shown. */
static int present_folder_reveal(DockUI *ui, int extent)
{
    FolderView *f = &ui->folder;
    struct Layer *layer = ui->popup->WLayer;
    int x, y, w, h, shown;
    if (extent <= 0 || !f->motion_frame.pixels) return 1;
    folder_reveal_rect(f,ui->popup->Width,ui->popup->Height,0,extent,&x,&y,&w,&h);
    if (!AttemptLockLayerRom(layer)) return 0;
    shown = amiga_display_present_tile(ui->popup_display,ui->popup,&f->motion_frame,x,y,w,h);
    UnlockLayerRom(layer);
    return shown;
}

static void paint_folder(DockUI *ui)
{
    FolderView *f = &ui->folder;
    const DockImage *background;
    struct RastPort *buffer;
    int parts = FOLDER_PAINT_GLYPHS|FOLDER_PAINT_LABELS;
    if (f->motion && f->handoff) {
        if (f->handoff_bitmap) amiga_display_end_frame(ui->popup_display,ui->popup);
        else amiga_display_present(ui->popup_display,ui->popup,&f->motion_backdrop);
        return;
    }
    if (f->motion && f->motion_frame.pixels) {
        /* The frame holds the box as far as it is revealed and faded. */
        amiga_display_present(ui->popup_display,ui->popup,&f->motion_frame);
        f->reveal_presented = f->reveal_shown;
        return;
    }
    if (ui->folder_aa && ui->popup_glass.pixels && (f->artwork_ready || prepare_folder_artwork(ui)))
        parts = FOLDER_PAINT_GLYPHS;
    background = folder_background(ui);
    buffer = amiga_display_begin_frame(ui->popup_display,background);
    if (buffer) {
        paint_folder_content(ui,buffer,parts,NULL);
        amiga_display_end_frame(ui->popup_display,ui->popup);
    } else {
        paint_popup_surface(ui,background);
        paint_folder_content(ui,ui->popup->RPort,parts,NULL);
    }
}

static void skip_standard_icons(FolderView *f)
{
    int visible = folder_visible_files(f);
    size_t first = f->page*folder_page_size(f);
    while (f->next_icon < visible && !f->entries[first+f->next_icon].custom_icon) ++f->next_icon;
}

static void clear_folder_motion(FolderView *f)
{
    free(f->motion_scene.pixels);
    free(f->motion_frame.pixels);
    f->motion_scene = f->motion_frame = (DockImage){0};
    f->motion = FOLDER_STILL;
    f->scene_final = 0;
    f->handoff = 0;
    f->handoff_bitmap = NULL;
    f->scale = FOLDER_SCALE_ONE;
}

void dock_ui_finish_folder_motion(DockUI *ui)
{
    FolderView *f = &ui->folder;
    finish_folder_reveal(ui,1);
    if (f->motion == FOLDER_CLOSING) {
        /* End on the desktop pixels while the layer still owns them. */
        if (!f->handoff) amiga_display_present(ui->popup_display,ui->popup,&f->motion_backdrop);
        dock_ui_close_popup(ui);
    } else if (f->motion == FOLDER_OPENING) {
        f->motion = FOLDER_STILL;
        paint_folder(ui);
        clear_folder_motion(f);
    }
}

static int folder_icon_left(const FolderView *f, int slot)
{
    return f->body_left+f->pad+(slot%f->cols)*f->cell_width+f->cell_width/2-f->anchor.icon_size/2;
}

static int folder_icon_top(const FolderView *f, int slot)
{
    return f->header+(slot/f->cols)*f->cell_height;
}

/* RGB is the panel's contribution over black. Alpha is the opacity of
   artwork over the glass, with zero for unobstructed glass. */
static void finish_folder_layer(FolderView *f)
{
    DockImage *layer = &f->motion_scene;
    int width = layer->width, height = layer->height, y, i;
    const uint32_t tint = glass_tint(3)&UINT32_C(0xffffff00);
    const uint32_t rim = (menu_colors[MENU_RIM]<<8)|255;
    for (y = 0; y < height; ++y) {
        int lo, hi, inner_lo, inner_hi, x;
        uint32_t *row = (uint32_t *)layer->pixels+(size_t)y*width;
        popup_span(f,width,height,POPUP_RADIUS,y,&lo,&hi);
        popup_inner_span(f,width,height,POPUP_RADIUS,y,lo,hi,&inner_lo,&inner_hi);
        for (x = lo; x < inner_lo; ++x) row[x] = rim;
        for (; x < inner_hi; ++x)
            if (row[x] == UINT32_C(0xff00ffff)) row[x] = tint;
        for (; x < hi; ++x) row[x] = rim;
    }
    for (i = 0; i < folder_visible_files(f); ++i) {
        const DockImage *icon = &f->loaded[i];
        int x, top, left;
        if (!icon->pixels) continue;
        left = folder_icon_left(f,i);
        top = folder_icon_top(f,i);
        dock_blend(layer,icon,left,top);
        for (y = 0; y < icon->height; ++y) {
            unsigned char *out = layer->pixels+((size_t)(top+y)*width+left)*4;
            const unsigned char *in = icon->pixels+(size_t)y*icon->width*4;
            for (x = 0; x < icon->width; ++x, out += 4, in += 4)
                out[3] = in[3];
        }
    }
}

static int prepare_folder_scene(DockUI *ui)
{
    FolderView *f = &ui->folder;
    const DockImage *background = &f->motion_backdrop;
    static const struct TagItem key_color[] = {
        {RPTAG_PenMode,FALSE}, {RPTAG_FgColor,0xffff00ffUL}, {TAG_DONE,0}
    };
    struct RastPort *buffer;
    size_t bytes = (size_t)background->width*background->height*4;
    if (!background->pixels) return 0;
    if (f->motion_scene.pixels) return 1;
    buffer = amiga_display_begin_frame(ui->popup_display,NULL);
    if (!buffer) return 0;
    f->motion_scene = (DockImage){background->width,background->height,malloc(bytes)};
    f->motion_frame = (DockImage){background->width,background->height,malloc(bytes)};
    if (!f->motion_scene.pixels || !f->motion_frame.pixels) {
        clear_folder_motion(f);
        return 0;
    }
    /* Native text and glyphs are opaque. A reserved color marks untouched
       pixels, even when the actual backdrop matches a glyph's color. */
    SetDrMd(buffer,JAM1);
    SetRPAttrsA(buffer,key_color);
    RectFill(buffer,0,0,background->width-1,background->height-1);
    paint_folder_content(ui,buffer,FOLDER_PAINT_GLYPHS|
        (ui->folder_aa ? 0 : FOLDER_PAINT_LABELS),NULL);
    if (amiga_display_capture_frame(ui->popup_display,&f->motion_scene)) {
        finish_folder_layer(f);
        if (ui->folder_aa) paint_folder_labels(ui,&f->motion_scene);
        return 1;
    }
    clear_folder_motion(f);
    return 0;
}

static void start_folder_clock(FolderView *f, int duration)
{
    struct EClockVal now;
    f->motion_ticks = ReadEClock(&now)*duration/1000;
    f->motion_start = now.ev_lo;
    f->motion_frames = 0;
    f->motion_paint_ticks = 0;
    f->motion_prepare_sum = f->motion_present_sum = 0;
}

static void reset_folder_page(DockUI *ui)
{
    FolderView *f = &ui->folder;
    f->next_icon = f->artwork_ready = f->needs_repaint = 0;
    free_folder_icons(f);
    if (f->count) skip_standard_icons(f);
    if (f->motion == FOLDER_OPENING) {
        /* Start from the bare backdrop; the box grows out of the tip. */
        if (prepare_folder_scene(ui)) {
            if (!f->scene_final) { finish_folder_scene(f); f->scene_final = 1; }
            memcpy(f->motion_frame.pixels,f->motion_backdrop.pixels,
                (size_t)f->motion_frame.width*f->motion_frame.height*4);
            f->reveal_shown = f->reveal_presented = f->reveal_from = 0;
        } else clear_folder_motion(f);
    }
    paint_folder(ui);
    if (f->motion == FOLDER_OPENING) start_folder_clock(f,FOLDER_OPEN_MS);
}

void dock_ui_dismiss_popup(DockUI *ui)
{
    FolderView *f = &ui->folder;
    if (f->motion == FOLDER_CLOSING) return;
    if (!ui->popup || !f->path[0] || !f->motion_backdrop.pixels || !ui->popup_display) {
        dock_ui_close_popup(ui);
        return;
    }
    /* Fade out and hide strips back toward the tip from wherever the box
       is. An opening in progress keeps its frame; an open box is read back
       from the screen, so no scene has to be built first. */
    if (f->motion != FOLDER_OPENING || !f->motion_frame.pixels) {
        DockImage *frame = &f->motion_frame;
        if (!frame->pixels) {
            *frame = (DockImage){ui->popup->Width,ui->popup->Height,
                malloc((size_t)ui->popup->Width*ui->popup->Height*4)};
            if (!frame->pixels ||
                !amiga_display_capture(ui->popup_display,frame,ui->popup->LeftEdge,ui->popup->TopEdge)) {
                free(frame->pixels); *frame = (DockImage){0};
                dock_ui_close_popup(ui);
                return;
            }
        }
        f->reveal_shown = folder_reveal_extent(f,ui->popup->Width,ui->popup->Height);
    }
    f->reveal_from = f->reveal_presented = f->reveal_shown;
    f->motion = FOLDER_CLOSING;
    ui->popup_pressed = FOLDER_OUTSIDE;
    start_folder_clock(f,FOLDER_CLOSE_MS);
}

static int advance_folder_motion(DockUI *ui)
{
    static const UWORD ease[] = {0,12,35,68,105,138,165,188,205,220,231,239,246,251,254,255,256};
    FolderView *f = &ui->folder;
    struct EClockVal now;
    ULONG hz = ReadEClock(&now), elapsed = now.ev_lo-f->motion_start;
    int closing = f->motion == FOLDER_CLOSING;
    int extent = folder_reveal_extent(f,ui->popup->Width,ui->popup->Height);
    if (f->handoff) {
        close_popup(ui,1);
        return 1;
    }
    if (elapsed < f->motion_ticks) {
        unsigned int progress = elapsed*4096/f->motion_ticks;
        /* Opening reveals over the first three fifths, then the fade
           finishes; closing hides over the whole time while fading out. */
        unsigned int reveal = closing ? progress : (progress*5/3 < 4096 ? progress*5/3 : 4095);
        unsigned int index = reveal>>8;
        int weight = ease[index]+((ease[index+1]-ease[index])*(reveal&255)+128)/256;
        int target = closing ? f->reveal_from*(256-weight)/256 :
            f->reveal_from+(extent-f->reveal_from)*weight/256;
        ULONG composed;
        if (!closing) {
            if (target > f->reveal_shown) f->reveal_shown = target;
            fade_folder_reveal(ui,&f->motion_scene,f->reveal_shown);
        } else {
            if (target < f->reveal_shown) {
                /* Newly hidden strips return to the backdrop at once. */
                int x, y, w, h, row;
                folder_reveal_rect(f,ui->popup->Width,ui->popup->Height,target,f->reveal_shown,&x,&y,&w,&h);
                for (row = y; row < y+h; ++row)
                    memcpy(f->motion_frame.pixels+((size_t)row*f->motion_frame.width+x)*4,
                        f->motion_backdrop.pixels+((size_t)row*f->motion_frame.width+x)*4,(size_t)w*4);
                f->reveal_shown = target;
            }
            fade_folder_reveal(ui,&f->motion_backdrop,f->reveal_shown);
        }
        ReadEClock(&now); composed = now.ev_lo;
        /* Closing also repaints strips hidden since the last shown frame. */
        if (present_folder_reveal(ui,closing ? f->reveal_presented : f->reveal_shown))
            f->reveal_presented = f->reveal_shown;
        ++f->motion_frames;
        ReadEClock(&now);
        f->motion_prepare_sum += composed-f->motion_start-elapsed;
        f->motion_present_sum += now.ev_lo-composed;
        return 0;
    }
    if (f->anchor.profile && hz) {
        int frames = f->motion_frames ? f->motion_frames : 1;
        printf("Folder Stack %s: %lu us, %d frames, icon %d px, font %d px, "
            "compose_us_avg=%lu present_us_avg=%lu box=%dx%d\n",
            closing ? "dismissal" : "entrance",(unsigned long)((uint64_t)elapsed*1000000/hz),
            f->motion_frames,f->anchor.icon_size,folder_font(ui)->tf_YSize,
            (unsigned long)((uint64_t)f->motion_prepare_sum*1000000/hz/frames),
            (unsigned long)((uint64_t)f->motion_present_sum*1000000/hz/frames),
            ui->popup->Width,ui->popup->Height);
        fflush(stdout);
    }
    if (closing) {
        struct RastPort *buffer = amiga_display_begin_frame(ui->popup_display,&f->motion_backdrop);
        f->handoff_bitmap = buffer ? buffer->BitMap : NULL;
        f->handoff = 1;
        paint_folder(ui);
    } else dock_ui_finish_folder_motion(ui);
    return 0;
}

static int prepare_folder_artwork(DockUI *ui)
{
    FolderView *f = &ui->folder;
    DockImage *art = &f->artwork;
    int width = ui->popup->Width, height = ui->popup->Height;
    size_t pixels = (size_t)width*height;
    if (!ui->popup_display) ui->popup_display = amiga_display_open(ui->screen,width,height);
    if (!ui->popup_display) return 0;
    amiga_display_set_converters(ui->popup_display,ui->encode,ui->decode,ui->encode565);
    if (!art->pixels) *art = (DockImage){width,height,malloc(pixels*4)};
    if (!art->pixels) return 0;
    if (ui->popup_glass.pixels) {
        memcpy(art->pixels,ui->popup_glass.pixels,pixels*4);
        if (ui->folder_aa) paint_folder_labels(ui,art);
    } else {
        int x, y;
        for (y = 0; y < height; ++y) {
            int lo, hi;
            dock_ui_popup_span(ui,y,&lo,&hi);
            for (x = 0; x < width; ++x) {
                unsigned char *p = art->pixels+((size_t)y*width+x)*4;
                ULONG color = menu_colors[y && y+1 < height && x > lo && x+1 < hi ?
                    MENU_SURFACE : MENU_RIM];
                p[0] = color>>16; p[1] = color>>8; p[2] = color; p[3] = 255;
            }
        }
    }
    f->artwork_ready = 1;
    return 1;
}

int dock_ui_folder_open(const DockUI *ui)
{
    return ui && ui->popup && ui->folder.path[0] && ui->folder.motion != FOLDER_CLOSING;
}

int dock_ui_folder_pending(const DockUI *ui)
{
    const FolderView *f = &ui->folder;
    return ui->folder_reveal ||
        (ui->popup && (f->motion || (f->count && f->next_icon < folder_visible_files(f))));
}

ULONG dock_ui_folder_frame_delay(const DockUI *ui)
{
    return ui->folder.motion && !ui->folder.handoff ? 4000 : 20000;
}

int dock_ui_folder_step(DockUI *ui)
{
    FolderView *f = &ui->folder;
    char path[DOCK_PREFS_PATH_CAP];
    DockImage source = {0};
    DockImage icon;
    void *requesters;
    int i, x, y, loaded;
    size_t entry;
    if (ui->folder_reveal) {
        struct EClockVal now;
        ReadEClock(&now);
        if ((LONG)(now.ev_lo-ui->folder_reveal->deadline) >= 0) finish_folder_reveal(ui,0);
        return 0;
    }
    if (!dock_ui_folder_pending(ui)) return 0;
    if (f->motion) return advance_folder_motion(ui);
    i = f->next_icon++;
    entry = f->page*folder_page_size(f)+i;
    skip_standard_icons(f);
    strcpy(path,f->path);
    requesters = amiga_requesters_off();
    loaded = AddPart((STRPTR)path,(STRPTR)f->entries[entry].name,sizeof(path)) &&
        f->anchor.read_icon(f->anchor.context,path,&source);
    amiga_requesters_restore(requesters);
    if (loaded) {
        int size = f->anchor.icon_size;
        if ((f->artwork_ready || prepare_folder_artwork(ui)) && dock_icon_build(&icon,&source,size)) {
            x = folder_icon_left(f,i);
            y = folder_icon_top(f,i);
            dock_blend(&f->artwork,&icon,x,y);
            f->loaded[i] = icon;
            if (!ui->popup_glass.pixels ||
                !amiga_display_present_tile(ui->popup_display,ui->popup,&f->artwork,x,y,size,size))
                f->needs_repaint = 1;
        }
    }
    free(source.pixels);
    if (f->needs_repaint && !dock_ui_folder_pending(ui)) {
        f->needs_repaint = 0;
        paint_folder(ui);
    }
    return 0;
}

void dock_ui_set_busy(DockUI *ui, const char *status)
{
    if (!ui || !ui->dialog) return;
    if (status) {
        strncpy(ui->busy_title,status,sizeof(ui->busy_title)-1);
        ui->busy_title[sizeof(ui->busy_title)-1] = 0;
    }
    SetWindowTitles(ui->dialog,(STRPTR)(status ? ui->busy_title : ui->dialog_title),(UBYTE *)~0);
    SetWindowPointer(ui->dialog,WA_BusyPointer,status != NULL,WA_PointerDelay,FALSE,TAG_DONE);
    if (ui->settings.save) {
        /* Nothing can change while the draft is applied. Browse stays
           disabled without asl.library. */
        SettingsView *s = &ui->settings;
        struct Gadget *const gadgets[] = {s->slider[SIZE_SLIDER], s->slider[SCALE_SLIDER],
            s->position, s->path, s->clear, s->cancel, s->save};
        size_t i;
        for (i = 0; i < sizeof(gadgets)/sizeof(gadgets[0]); ++i)
            if (gadgets[i]) GT_SetGadgetAttrs(gadgets[i],ui->dialog,NULL,GA_Disabled,status != NULL,TAG_DONE);
        if (s->browse && ui->asl)
            GT_SetGadgetAttrs(s->browse,ui->dialog,NULL,GA_Disabled,status != NULL,TAG_DONE);
    }
}

void dock_ui_set_fit_hook(DockUI *ui, DockUIFit hook, void *context)
{
    if (!ui) return;
    ui->fit = hook;
    ui->fit_context = context;
}

void dock_ui_set_blocking_hook(DockUI *ui, DockUIBlocking hook, void *context)
{
    if (!ui) return;
    ui->blocking = hook;
    ui->blocking_context = context;
}

void dock_ui_set_converters(DockUI *ui, DockConvertRGB24 encode, DockConvertRGB565 decode,
    DockConvertRGB565 encode565, DockMixHalf mix_half)
{
    ui->encode565 = encode565;
    ui->mix_half = mix_half ? mix_half : dock_mix_half_c;
    ui->encode = encode;
    ui->decode = decode;
}

void dock_ui_hide_label(DockUI *ui)
{
    if (!ui) return;
    if (ui->label) { CloseWindow(ui->label); ui->label = NULL; }
    if (ui->label_shape) { DisposeRegion(ui->label_shape); ui->label_shape = NULL; }
}

static int label_position(const DockUI *ui, DockEdge edge, const DockRect *anchor,
    int width, int height, int *left, int *top)
{
    int bar = ui->screen->BarHeight+2;
    if (edge == DOCK_EDGE_BOTTOM) {
        *left = clamp((anchor->left+anchor->right-width)/2,LABEL_MARGIN,
            ui->screen->Width-width-LABEL_MARGIN);
        *top = anchor->top-LABEL_GAP-height;
    } else {
        *left = edge == DOCK_EDGE_LEFT ? anchor->right+LABEL_GAP : anchor->left-LABEL_GAP-width;
        *top = clamp((anchor->top+anchor->bottom-height)/2,bar,
            ui->screen->Height-height-LABEL_MARGIN);
    }
    return *left >= LABEL_MARGIN && *left+width <= ui->screen->Width-LABEL_MARGIN &&
        *top >= bar && *top+height <= ui->screen->Height-LABEL_MARGIN;
}

/* Inside the label outline moved inward by inset px: a pill of height body
   with semicircular ends, plus a tip of tip rows under tip_x. In 1/16 px. */
static int label_inside(int x, int y, int width, int body, int tip, int tip_x, int inset)
{
    int r = body*8-inset*16, left = body*8, right = width*16-body*8;
    int dx = x < left ? left-x : x > right ? x-right : 0, dy = y-body*8;
    if (r > 0 && dx*dx+dy*dy <= r*r) return 1;
    if (tip && y >= body*16-24) {
        /* 45-degree sides; moving them inward 1 px lowers the apex by sqrt 2. */
        int half = (body+tip)*16-inset*23-y;
        if (half > 0 && abs(x-(tip_x*16+8)) <= half) return 1;
    }
    return 0;
}

/* Area coverage (0-255) of pixel (px,py), from 8x8 samples where the
   outline crosses it. */
static int label_coverage(int px, int py, int width, int body, int tip, int tip_x, int inset)
{
    int sx, sy, inside = 0, x = px*16, y = py*16;
    if (py >= body && abs(px-tip_x) > tip+2) return 0;
    if (label_inside(x,y,width,body,0,0,inset) && label_inside(x+16,y,width,body,0,0,inset) &&
        label_inside(x,y+16,width,body,0,0,inset) && label_inside(x+16,y+16,width,body,0,0,inset))
        return 255; /* The pill is convex: all four corners inside means all of it. */
    for (sy = 0; sy < 8; ++sy) for (sx = 0; sx < 8; ++sx)
        inside += label_inside(x+1+2*sx,y+1+2*sy,width,body,tip,tip_x,inset);
    return (inside*255+32)/64;
}

/* The window shape: on each row, every pixel the outline touches. Without
   a captured backdrop to blend with, only pixels at least half covered. */
static int label_shape(DockUI *ui, int width, int height, const unsigned char *outer, int threshold)
{
    int y;
    ui->label_shape = NewRegion();
    if (!ui->label_shape) return 0;
    for (y = 0; y < height; ++y) {
        const unsigned char *row = outer+(size_t)y*width;
        int left = 0, right = width-1;
        struct Rectangle rect;
        while (left < width && row[left] <= threshold) ++left;
        while (right > left && row[right] <= threshold) --right;
        if (left > right) continue;
        rect = (struct Rectangle){left,y,right,y};
        if (!OrRectRegion(ui->label_shape,&rect)) return 0;
    }
    return 1;
}

/* Ink box of text that paint_ui_text draws with its top-left at (0,0). */
static int label_ink(DockUI *ui, struct RastPort *rp, const char *text, DockRect *ink)
{
    struct TextExtent extent;
    if (ui->chrome_aa && ui->text_alpha)
        return amiga_font_aa_ink(ui->chrome_aa,text,&ink->left,&ink->top,&ink->right,&ink->bottom);
    amiga_text_extent(rp,text,strlen(text),&extent);
    if (extent.te_Extent.MaxX < extent.te_Extent.MinX) return 0;
    ink->left = extent.te_Extent.MinX; ink->right = extent.te_Extent.MaxX+1;
    ink->top = rp->TxBaseline+extent.te_Extent.MinY;
    ink->bottom = rp->TxBaseline+extent.te_Extent.MaxY+1;
    return 1;
}

/* With a captured backdrop, each pixel mixes backdrop, rim and the
   translucent surface by its outer and inner (1 px inset) coverage, so the
   ends and the tip are antialiased. Otherwise the rim and the surface are
   filled where they cover at least half of a pixel. */
static void paint_label(DockUI *ui, DockImage *glass, AmigaDisplay *display,
    const unsigned char *outer, const unsigned char *inner, int text_x, int text_top, const char *label)
{
    struct RastPort *rp = ui->label->RPort;
    int width = ui->label->Width, height = ui->label->Height, y;
    int capture = glass->pixels && glass->width == width && glass->height == height &&
        ui->label->LeftEdge == ui->label_left && ui->label->TopEdge == ui->label_top;
    if (capture) {
        ULONG rim = menu_colors[MENU_RIM], surface = menu_colors[MENU_SURFACE];
        for (y = 0; y < height; ++y) {
            int x;
            for (x = 0; x < width; ++x) {
                size_t i = (size_t)y*width+x;
                unsigned int o = outer[i], n = inner[i], c;
                unsigned char *pixel = glass->pixels+i*4;
                if (!o) continue;
                for (c = 0; c < 3; ++c) {
                    unsigned int back = pixel[c], shift = 16-8*c;
                    unsigned int glass_tint = (4*((surface>>shift)&255)+back+2)/5;
                    pixel[c] = (unsigned char)((back*(255-o)+((rim>>shift)&255)*(o-n)+glass_tint*n+127)/255);
                }
            }
        }
        capture = amiga_display_present(display,ui->label,glass);
    }
    if (!capture) {
        popup_color(ui,rp,MENU_RIM);
        RectFill(rp,0,0,width-1,height-1);
        popup_color(ui,rp,MENU_SURFACE);
        for (y = 0; y < height; ++y) {
            const unsigned char *row = inner+(size_t)y*width;
            int left = 0, right = width-1;
            while (left < width && row[left] < 128) ++left;
            while (right > left && row[right] < 128) --right;
            if (left <= right) RectFill(rp,left,y,right,y);
        }
    }
    SetFont(rp,ui->draw->dri_Font);
    SetDrMd(rp,JAM1);
    paint_ui_text(ui,rp,ui->chrome_aa,label,text_x,text_top,MENU_TEXT);
}

int dock_ui_show_label(DockUI *ui, DockEdge edge, const DockRect *anchor, const char *text)
{
    struct RastPort measure;
    DockImage glass = {0};
    AmigaDisplay *display = NULL;
    char label[128];
    size_t length = strlen(text);
    int text_width, width, height, body, max_width, tip_x, tip, x, y, text_x, text_top;
    DockRect ink, caps;
    unsigned char *outer, *inner;
    dock_ui_hide_label(ui);
    max_width = ui->screen->Width-2*LABEL_MARGIN;
    if (edge == DOCK_EDGE_LEFT) max_width = ui->screen->Width-LABEL_MARGIN-anchor->right-LABEL_GAP;
    else if (edge == DOCK_EDGE_RIGHT) max_width = anchor->left-LABEL_GAP-LABEL_MARGIN;
    if (max_width > ui->screen->Width/2) max_width = ui->screen->Width/2;
    if (max_width < 2*LABEL_PADDING+16) return 0;
    InitRastPort(&measure); SetFont(&measure,ui->draw->dri_Font);
    if (length < sizeof(label)) strcpy(label,text);
    else {
        length = sizeof(label)-4;
        memcpy(label,text,length); strcpy(label+length,"...");
    }
    text_width = text_length32(&measure,label);
    if (text_width+2*LABEL_PADDING > max_width) {
        if (length > sizeof(label)-4) length = sizeof(label)-4;
        do {
            strcpy(label+--length,"...");
            text_width = text_length32(&measure,label);
        } while (length && text_width+2*LABEL_PADDING > max_width);
    }
    if (text_width <= 0 || text_width+2*LABEL_PADDING > max_width) return 0;
    /* Centre the ink, not the advance box: equal ink margins at both ends,
       and the capitals of the font (measured on "H") centred vertically. */
    if (!label_ink(ui,&measure,label,&ink)) ink = (DockRect){0,0,text_width,ui->font_height};
    if (!label_ink(ui,&measure,"H",&caps)) caps = (DockRect){0,0,1,ui->font_height};
    {
        /* Accents, descenders and the label's own ink must stay inside the
           rim: grow the padding on both sides alike, so the capitals stay
           centred and every label of one font has the same height. */
        static const char reference[] = "\xc5\xc4\xd6gjpqy_|";
        DockRect tall;
        int pad = LABEL_VPAD;
        if (label_ink(ui,&measure,reference,&tall)) {
            if (caps.top-tall.top+2 > pad) pad = caps.top-tall.top+2;
            if (tall.bottom-caps.bottom+2 > pad) pad = tall.bottom-caps.bottom+2;
        }
        if (caps.top-ink.top+2 > pad) pad = caps.top-ink.top+2;
        if (ink.bottom-caps.bottom+2 > pad) pad = ink.bottom-caps.bottom+2;
        body = caps.bottom-caps.top+2*pad;
        text_top = pad-caps.top;
    }
    width = ink.right-ink.left+2*LABEL_PADDING;
    if (width < body) width = body;
    text_x = (width-(ink.right-ink.left))/2-ink.left;
    tip = ui->window_shapes && edge == DOCK_EDGE_BOTTOM ? LABEL_TIP : 0;
    height = body+tip;
    if (!label_position(ui,edge,anchor,width,height,&ui->label_left,&ui->label_top)) return 0;
    tip_x = clamp((anchor->left+anchor->right)/2-ui->label_left,body/2,width-body/2-1);
    outer = malloc((size_t)width*height*2);
    if (!outer) return 0;
    inner = outer+(size_t)width*height;
    for (y = 0; y < height; ++y) for (x = 0; x < width; ++x) {
        outer[y*width+x] = (unsigned char)label_coverage(x,y,width,body,tip,tip_x,0);
        inner[y*width+x] = (unsigned char)label_coverage(x,y,width,body,tip,tip_x,1);
    }
    display = capture_glass(ui,&glass,ui->label_left,ui->label_top,width,height);
    if (ui->window_shapes && !label_shape(ui,width,height,outer,glass.pixels ? 0 : 127)) {
        free(outer); free(glass.pixels); amiga_display_close(display);
        dock_ui_hide_label(ui); return 0;
    }
    prepare_popup_colors(ui);
    ui->label = OpenWindowTags(NULL,WA_CustomScreen,(ULONG)ui->screen,
        WA_Left,ui->label_left,WA_Top,ui->label_top,WA_Width,width,WA_Height,height,
        WA_Borderless,TRUE,WA_Activate,FALSE,WA_SmartRefresh,TRUE,WA_NoCareRefresh,TRUE,
        ui->label_shape ? WA_ShapeRegion : TAG_IGNORE,(ULONG)ui->label_shape,
        WA_IDCMP,0,TAG_DONE);
    if (ui->label) {
        paint_label(ui,&glass,display,outer,inner,text_x,text_top,label);
        ui->label_left = ui->label->LeftEdge; ui->label_top = ui->label->TopEdge;
    } else dock_ui_hide_label(ui);
    free(outer);
    free(glass.pixels);
    amiga_display_close(display);
    return ui->label != NULL;
}

void dock_ui_move_label(DockUI *ui, DockEdge edge, const DockRect *anchor)
{
    int left, top;
    struct Window *label = ui->label;
    if (!label || label->LeftEdge != ui->label_left || label->TopEdge != ui->label_top) return;
    if (!label_position(ui,edge,anchor,label->Width,label->Height,&left,&top)) {
        dock_ui_hide_label(ui); return;
    }
    if (left == ui->label_left && top == ui->label_top) return;
    MoveWindow(label,left-ui->label_left,top-ui->label_top);
    ui->label_left = left; ui->label_top = top;
}

static int window_bounds(const struct Window *window, DockRect *bounds)
{
    if (!window) return 0;
    *bounds = (DockRect){window->LeftEdge, window->TopEdge,
        window->LeftEdge+window->Width, window->TopEdge+window->Height};
    return 1;
}

int dock_ui_popup_bounds(const DockUI *ui, DockRect *bounds)
{
    return window_bounds(ui->popup,bounds);
}

int dock_ui_dialog_bounds(const DockUI *ui, DockRect *bounds)
{
    return window_bounds(ui->dialog,bounds);
}

static int open_popup_window(DockUI *ui, int left, int top, int width, int height,
    int report_mouse, int capture_backdrop)
{
    if (ui->window_shapes)
        ui->popup_shape = ui->folder.path[0] ? make_folder_shape(&ui->folder,width,height) :
            make_rounded_shape(width,height,POPUP_RADIUS);
    prepare_popup_colors(ui);
    if (capture_backdrop) capture_popup_glass(ui,left,top,width,height,report_mouse ? 2 : 3);
    ui->popup = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)ui->screen,
        WA_Left, left, WA_Top, top, WA_Width, width, WA_Height, height,
        WA_Borderless, TRUE, WA_Activate, TRUE, WA_RMBTrap, TRUE,
        WA_ReportMouse, report_mouse, WA_SimpleRefresh, TRUE,
        WA_BackFill, (ULONG)LAYERS_NOBACKFILL,
        ui->popup_shape ? WA_ShapeRegion : TAG_IGNORE, (ULONG)ui->popup_shape,
        WA_IDCMP, IDCMP_MOUSEBUTTONS|IDCMP_RAWKEY|IDCMP_REFRESHWINDOW|IDCMP_INACTIVEWINDOW|
            (report_mouse ? IDCMP_MOUSEMOVE : 0), TAG_DONE);
    if (!ui->popup) { dock_ui_close_popup(ui); return 0; }
    if (ui->popup->LeftEdge != left || ui->popup->TopEdge != top) free_popup_glass(ui);
    ui->popup_selected = ui->popup_pressed = -1;
    return 1;
}

int dock_ui_open_popup(DockUI *ui, DockEdge edge, int anchor_x, int anchor_y,
    const struct Window *native_dock,
    int item_action, int can_keep, int drawer)
{
    struct RastPort measure;
    DockRect dock;
    int i, width = 0, height, left, top, screen_top, covers_native_dock;
    enum { POINTER_GAP = 4 };
    if (!ui || ui->dialog) return 0;
    dock_ui_close_popup(ui);
    if (drawer) {
        ui->popup_items = drawer_items;
        ui->popup_rows = item_action == DOCK_UI_CLOSE_ITEM ? 2 : 1;
    } else if (item_action >= DOCK_UI_NEW_SHELL && item_action <= DOCK_UI_QUIT_SHELL) {
        ui->popup_items = shell_items;
        ui->popup_rows = item_action-DOCK_UI_NEW_SHELL+1;
    } else {
        ui->popup_items = can_keep ? temporary_items : item_action == DOCK_UI_CLOSE_ITEM ? &close_item :
            item_action == DOCK_UI_QUIT_ITEM ? &quit_item : menu_items;
        ui->popup_rows = can_keep ? 1+(item_action == DOCK_UI_QUIT_ITEM) :
            item_action ? 1 : sizeof(menu_items)/sizeof(*menu_items);
    }
    InitRastPort(&measure); SetFont(&measure, ui->draw->dri_Font);
    for (i = 0; i < ui->popup_rows; ++i) {
        int label_width = text_length32(&measure,ui->popup_items[i].label);
        if (label_width > width) width = label_width;
    }
    width += 24;
    ui->popup_row = ui->font_height+10;
    height = 6+ui->popup_rows*ui->popup_row;
    screen_top = ui->screen->BarHeight+2;
    if (width > ui->screen->Width || height > ui->screen->Height-screen_top) return 0;
    if (edge == DOCK_EDGE_BOTTOM) {
        left = anchor_x-width/2;
        top = anchor_y-height-POINTER_GAP;
    } else {
        left = edge == DOCK_EDGE_LEFT ? anchor_x+POINTER_GAP : anchor_x-width-POINTER_GAP;
        top = anchor_y-3-ui->popup_row/2;
    }
    left = clamp(left, 0, ui->screen->Width-width);
    top = clamp(top, screen_top, ui->screen->Height-height);
    /* A native Dock is part of the screen bitmap, unlike the PiP overlay. */
    covers_native_dock = window_bounds(native_dock,&dock) &&
        left < dock.right && left+width > dock.left && top < dock.bottom && top+height > dock.top;
    if (!open_popup_window(ui,left,top,width,height,1,!covers_native_dock)) return 0;
    paint_popup(ui);
    return 1;
}

static int show_folder(DockUI *ui, FolderAnchor anchor, const char *path, size_t root_length, int entrance)
{
    FolderView next = {0};
    struct RastPort measure;
    int font_height = folder_font(ui)->tf_YSize;
    int width, height, left, top, max_width, max_height, rows, control = font_height+10;
    int tip_size = ui->window_shapes ? FOLDER_TIP : 0;
    int icon = anchor.icon_size, text_width, extra_bottom;
    int screen_top = ui->screen->BarHeight+2;
    enum { GAP = 10 };

    max_width = ui->screen->Width;
    max_height = ui->screen->Height-screen_top;
    if (anchor.edge == DOCK_EDGE_BOTTOM) max_height = anchor.y-screen_top-GAP;
    else if (anchor.edge == DOCK_EDGE_LEFT) max_width -= anchor.x+GAP;
    else max_width = anchor.x-GAP;
    next.anchor = anchor;
    next.scale = FOLDER_SCALE_ONE;
    next.motion = entrance && ui->popup_rgb && tip_size ? FOLDER_OPENING : FOLDER_STILL;
    next.root_length = root_length;
    strcpy(next.path,path);
    InitRastPort(&measure);
    SetFont(&measure,folder_font(ui));
    next.pad = clamp((icon+2)/5,6,24);
    next.label_gap = clamp((icon+11)/22,2,8);
    next.cell_width = (icon*246+87)/174;
    text_width = text_length32(&measure,"Workbench 0")+6;
    if (next.cell_width < text_width) next.cell_width = text_width;
    next.action_wrap = text_length32(&measure,"Open in Workbench")+6 > next.cell_width;
    next.cell_height = icon+next.label_gap+font_height+clamp(icon/6,4,16);
    next.header = control+clamp(icon/5,8,22);
    next.footer = control+8;
    extra_bottom = (next.action_wrap ? font_height : 0)+next.pad;
    next.cols = clamp((max_width-2*next.pad)/next.cell_width,0,FOLDER_COLS);
    rows = clamp((max_height-next.header-next.footer-extra_bottom)/next.cell_height,0,FOLDER_ROWS);
    if (!next.cols || !rows) return 0;
    next.read_ok = amiga_folder_read(path,&next.entries,&next.count);
    while (next.cols > 3 && (size_t)(next.cols-1)*(next.cols-1) > next.count &&
        (size_t)(next.cols-1)*rows > next.count) --next.cols;
    next.rows = next.count/(size_t)next.cols >= (size_t)rows ? rows : (int)(next.count/next.cols)+1;
    if (next.count && !folder_page_size(&next)) {
        free(next.entries);
        return 0;
    }
    width = 2*next.pad+next.cols*next.cell_width;
    height = next.header+next.rows*next.cell_height+
        (folder_last_page(&next) ? next.footer : 0)+extra_bottom+(next.count ? 0 : font_height+6);
    if (height > max_height) {
        free(next.entries);
        return 0;
    }
    next.body_width = width;
    next.body_height = height;
    if (anchor.edge == DOCK_EDGE_BOTTOM) {
        left = clamp(anchor.x-width/2,0,ui->screen->Width-width);
        top = anchor.y-GAP-height;
        next.tip = clamp(anchor.x-left,POPUP_RADIUS+FOLDER_TIP_HALF,
            width-POPUP_RADIUS-FOLDER_TIP_HALF-1);
        height += tip_size;
    } else {
        left = anchor.edge == DOCK_EDGE_LEFT ? anchor.x+GAP : anchor.x-GAP-width;
        top = clamp(anchor.y-height/2,screen_top,ui->screen->Height-height);
        next.tip = clamp(anchor.y-top,POPUP_RADIUS+FOLDER_TIP_HALF,
            height-POPUP_RADIUS-FOLDER_TIP_HALF-1);
        if (anchor.edge == DOCK_EDGE_LEFT) {
            left -= tip_size;
            next.body_left = tip_size;
        }
        width += tip_size;
    }
    next.pivot_x = anchor.edge == DOCK_EDGE_BOTTOM ? next.tip : anchor.edge == DOCK_EDGE_LEFT ? 0 : width-1;
    next.pivot_y = anchor.edge == DOCK_EDGE_BOTTOM ? height-1 : next.tip;
    if (ui->folder.path[0] && ui->popup->Width == width && ui->popup->Height == height &&
        ui->popup->LeftEdge == left && ui->popup->TopEdge == top) {
        next.artwork = ui->folder.artwork;
        free(ui->folder.entries);
        next.motion_backdrop = ui->folder.motion_backdrop;
        next.backdrop_layer_count = ui->folder.backdrop_layer_count;
        memcpy(next.backdrop_layers,ui->folder.backdrop_layers,sizeof(next.backdrop_layers));
        free_folder_icons(&ui->folder);
        free(ui->folder.motion_scene.pixels);
        free(ui->folder.motion_frame.pixels);
        ui->folder = next;
    } else {
        const DockImage *old = &ui->folder.motion_backdrop;
        int opened;
        if (ui->popup && ui->folder.path[0] && old->pixels &&
            old->width == ui->popup->Width && old->height == ui->popup->Height) {
            ui->backdrop_carry = *old;
            ui->carry_left = ui->popup->LeftEdge;
            ui->carry_top = ui->popup->TopEdge;
            ui->folder.motion_backdrop = (DockImage){0};
        }
        dock_ui_close_popup(ui);
        ui->folder = next;
        opened = open_popup_window(ui,left,top,width,height,0,1);
        free(ui->backdrop_carry.pixels);
        ui->backdrop_carry = (DockImage){0};
        if (!opened) return 0;
    }
    ui->popup_pressed = FOLDER_OUTSIDE;
    reset_folder_page(ui);
    return 1;
}

int dock_ui_open_folder(DockUI *ui, DockEdge edge, int x, int y, const char *path,
    int icon_size, DockUIReadIcon read_icon, const void *context, int profile)
{
    return ui && !ui->dialog && *path &&
        show_folder(ui,(FolderAnchor){edge,x,y,icon_size,profile,read_icon,context},path,strlen(path),1);
}

static struct Gadget *gadget(DockUI *ui, struct Gadget *previous, ULONG kind,
                             int id, const char *label, int x, int y, int w, int h,
                             ULONG flags, const struct TagItem *tags)
{
    struct NewGadget ng;
    if (!previous) return NULL;
    memset(&ng, 0, sizeof(ng));
    ng.ng_LeftEdge = x; ng.ng_TopEdge = y; ng.ng_Width = w; ng.ng_Height = h;
    ng.ng_GadgetText = (CONST_STRPTR)label; ng.ng_TextAttr = &ui->font;
    ng.ng_GadgetID = id; ng.ng_Flags = flags; ng.ng_VisualInfo = ui->visual;
    return CreateGadgetA(kind, previous, &ng, tags);
}

static struct Window *open_dialog(DockUI *ui, const char *title, int width, int height, ULONG idcmp)
{
    int left, top;
    if (width+ui->screen->WBorLeft+ui->screen->WBorRight > ui->screen->Width ||
        height+ui->screen->WBorTop+ui->font_height+2 > ui->screen->Height-ui->screen->BarHeight-2) return NULL;
    left = (ui->screen->Width-width)/2;
    top = (ui->screen->Height-height)/2;
    if (top < ui->screen->BarHeight+2) top = ui->screen->BarHeight+2;
    return OpenWindowTags(NULL, WA_CustomScreen, (ULONG)ui->screen,
        WA_Left, left, WA_Top, top, WA_InnerWidth, width, WA_InnerHeight, height,
        WA_Title, (ULONG)title, WA_DragBar, TRUE, WA_DepthGadget, TRUE,
        WA_CloseGadget, TRUE, WA_Activate, TRUE, WA_RMBTrap, TRUE,
        WA_SimpleRefresh, TRUE, WA_NotifyDepth, TRUE,
        WA_IDCMP, idcmp|IDCMP_CLOSEWINDOW|IDCMP_REFRESHWINDOW|IDCMP_RAWKEY|IDCMP_CHANGEWINDOW,
        TAG_DONE);
}

static void read_folder_text(DockUI *ui)
{
    const struct StringInfo *input = ui->settings.path->SpecialInfo;
    strcpy(ui->draft.folder,(const char *)input->Buffer);
}

static void set_folder_text(DockUI *ui, const char *path)
{
    GT_SetGadgetAttrs(ui->settings.path,ui->dialog,NULL,GTST_String,(ULONG)path,TAG_DONE);
}

static void browse_folder(DockUI *ui)
{
    struct Library *AslBase = ui->asl;
    struct FileRequester *request;
    BOOL chosen;
    const char *current = ui->draft.folder;
    const struct TagItem tags[] = {
        {ASLFR_Window,(ULONG)ui->dialog}, {ASLFR_TitleText,(ULONG)"Choose Folder Stack"},
        {ASLFR_DrawersOnly,TRUE}, {ASLFR_PrivateIDCMP,TRUE}, {ASLFR_SleepWindow,TRUE},
        {ASLFR_PositiveText,(ULONG)"Select"},
        {ASLFR_InitialDrawer,(ULONG)(current[0] ? current : "SYS:")},
        {ASLFR_InitialLeftEdge,(ui->screen->Width-ui->dialog->Width)/2},
        {ASLFR_InitialTopEdge,(ui->screen->Height-ui->dialog->Height)/2},
        {ASLFR_InitialWidth,ui->dialog->Width}, {ASLFR_InitialHeight,ui->dialog->Height},
        {TAG_DONE,0}
    };
    if (!AslBase) return;
    read_folder_text(ui);
    request = AllocAslRequest(ASL_FileRequest,tags);
    if (!request) return;
    if (ui->blocking) ui->blocking(ui->blocking_context,1);
    chosen = AslRequest(request,NULL);
    if (ui->blocking) ui->blocking(ui->blocking_context,0);
    if (chosen) {
        const char *drawer = (const char *)request->fr_Drawer;
        if (drawer && strlen(drawer) < sizeof(ui->draft.folder)) set_folder_text(ui,drawer);
    }
    FreeAslRequest(request);
}

static void format_slider(char *text, int slider, int value)
{
    if (slider == SIZE_SLIDER) sprintf(text,"%d px",value);
    else sprintf(text,"%d.%02dx",value/DOCK_SCALE_ONE,value%DOCK_SCALE_ONE);
}

static int position_index(int edge)
{
    return edge == DOCK_EDGE_LEFT ? 0 : edge == DOCK_EDGE_RIGHT ? 2 : 1;
}

static void update_limits(DockUI *ui, int live);

int dock_ui_open_settings(DockUI *ui, const DockUIPrefs *initial)
{
    static const char *const slider_labels[] = {"_Size","Sc_ale"};
    SettingsView *s;
    struct Window *w;
    struct RastPort measure;
    struct Gadget *g;
    int width, height, line, row, left, right, x, y, readout, browse, clear, save, cancel, i;
    enum { MARGIN = 12, GAP = 6 };

    if (!ui || !initial || !ready(ui)) return 0;
    if (ui->dialog) { WindowToFront(ui->dialog); ActivateWindow(ui->dialog); return 1; }
    dock_ui_close_popup(ui);
    s = &ui->settings;
    ui->draft = *initial;
    ui->draft.size = clamp(initial->size,DOCK_MIN_ICON_SIZE,DOCK_MAX_ICON_SIZE);
    ui->draft.scale_percent = clamp(initial->scale_percent,DOCK_SCALE_ONE,DOCK_MAX_SCALE);
    if (initial->edge != DOCK_EDGE_LEFT && initial->edge != DOCK_EDGE_RIGHT)
        ui->draft.edge = DOCK_EDGE_BOTTOM;
    s->cap[SIZE_SLIDER] = ui->draft.size > SIZE_SLIDER_MAX ? ui->draft.size : SIZE_SLIDER_MAX;
    s->cap[SCALE_SLIDER] = ui->draft.scale_percent > SCALE_SLIDER_MAX ? ui->draft.scale_percent : SCALE_SLIDER_MAX;
    s->zoom_cap = dock_scaled_size(ui->draft.size,ui->draft.scale_percent);
    if (s->zoom_cap < ZOOM_CEILING) s->zoom_cap = ZOOM_CEILING;
    update_limits(ui,0);

    line = ui->font_height > ui->font.ta_YSize ? ui->font_height : ui->font.ta_YSize;
    row = line+8;
    if (row < 18) row = 18;
    width = (ui->screen->Width < 366 ? ui->screen->Width-12 : 354)-
        ui->screen->WBorLeft-ui->screen->WBorRight;
    height = 5*row+line+4*MARGIN+2*GAP+4;
    if (width < 300) return 0;
    strcpy(ui->dialog_title,"Dock Settings");
    ui->dialog = w = open_dialog(ui,ui->dialog_title,width,height,
        BUTTONIDCMP|CYCLEIDCMP|SLIDERIDCMP|STRINGIDCMP|IDCMP_VANILLAKEY);
    if (!w) return 0;
    ui->asl = OpenLibrary((STRPTR)AslName,38);

    InitRastPort(&measure);
    SetFont(&measure,ui->draw->dri_Font);
    left = w->BorderLeft+MARGIN;
    right = w->Width-w->BorderRight-MARGIN;
    x = left+text_length32(&measure,"Position")+MARGIN;
    y = w->BorderTop+MARGIN;
    format_slider(s->value[SIZE_SLIDER],SIZE_SLIDER,s->cap[SIZE_SLIDER]);
    format_slider(s->value[SCALE_SLIDER],SCALE_SLIDER,s->cap[SCALE_SLIDER]);
    readout = text_length32(&measure,s->value[SIZE_SLIDER]);
    i = text_length32(&measure,s->value[SCALE_SLIDER]);
    readout = (readout > i ? readout : i)+8;
    browse = text_length32(&measure,"Browse...")+18;
    clear = text_length32(&measure,"Clear")+18;
    save = text_length32(&measure,"Save")+24;
    cancel = text_length32(&measure,"Cancel")+24;
    if (right-x-readout < 64 || right-left-browse-clear-2*GAP < 80 ||
        right-left-save-cancel < GAP) goto fail;

    g = CreateContext(&ui->gadgets);
    for (i = 0; i < SLIDER_COUNT; ++i) {
        int value = i == SIZE_SLIDER ? ui->draft.size : ui->draft.scale_percent;
        const struct TagItem slider_tags[] = {
            {GTSL_Min,slider_min[i]}, {GTSL_Max,s->limit[i]}, {GTSL_Level,value},
            {GT_Underscore,'_'}, {TAG_DONE,0}
        };
        const struct TagItem readout_tags[] = {
            {GTTX_Text,(ULONG)s->value[i]}, {GTTX_Justification,GTJ_RIGHT}, {TAG_DONE,0}
        };
        g = s->slider[i] = gadget(ui,g,SLIDER_KIND,GID_SIZE+i,slider_labels[i],
            x,y,right-x-readout-GAP,row,PLACETEXT_LEFT,slider_tags);
        format_slider(s->value[i],i,value);
        g = s->readout[i] = gadget(ui,g,TEXT_KIND,0,NULL,right-readout,
            y+(row-line)/2,readout,line,0,readout_tags);
        y += row+GAP;
    }
    {
        const struct TagItem position_tags[] = {
            {GTCY_Labels,(ULONG)settings_positions},
            {GTCY_Active,position_index(ui->draft.edge)},
            {GT_Underscore,'_'}, {TAG_DONE,0}
        };
        g = s->position = gadget(ui,g,CYCLE_KIND,GID_POSITION,"_Position",x,y,
            text_length32(&measure,"Bottom")+48,row,PLACETEXT_LEFT,position_tags);
    }
    y += row+MARGIN;
    {
        const struct TagItem title[] = {{GTTX_Text,(ULONG)"Folder Stack"},{TAG_DONE,0}};
        const struct TagItem path[] = {
            {GTST_String,(ULONG)ui->draft.folder},{GTST_MaxChars,sizeof(ui->draft.folder)-1},{TAG_DONE,0}
        };
        const struct TagItem browse_tags[] = {{GA_Disabled,!ui->asl},{GT_Underscore,'_'},{TAG_DONE,0}};
        const struct TagItem shortcut[] = {{GT_Underscore,'_'},{TAG_DONE,0}};
        g = gadget(ui,g,TEXT_KIND,0,NULL,left,y,right-left,line,0,title);
        y += line+4;
        g = s->path = gadget(ui,g,STRING_KIND,GID_PATH,NULL,left,y,
            right-left-browse-clear-2*GAP,row,0,path);
        g = s->browse = gadget(ui,g,BUTTON_KIND,GID_BROWSE,"_Browse...",right-browse-clear-GAP,y,browse,row,
            PLACETEXT_IN,browse_tags);
        g = s->clear = gadget(ui,g,BUTTON_KIND,GID_CLEAR,"_Clear",right-clear,y,clear,row,PLACETEXT_IN,shortcut);
        y += row+MARGIN;
    }
    g = s->cancel = gadget(ui,g,BUTTON_KIND,GID_CANCEL,"Cancel",right-save-cancel-GAP,y,cancel,row,PLACETEXT_IN,NULL);
    g = s->save = gadget(ui,g,BUTTON_KIND,GID_SAVE,"Save",right-save,y,save,row,PLACETEXT_IN,NULL);
    if (!g) goto fail;
    AddGList(w,ui->gadgets,(UWORD)-1,-1,NULL);
    RefreshGList(ui->gadgets,w,NULL,-1);
    GT_RefreshWindow(w,NULL);
    return 1;
fail:
    dock_ui_close_dialog(ui);
    return 0;
}

/* Prerendered lines need cybergraphics alpha; otherwise use the screen
   font, with a soft bold for the title. */
static int about_prerendered(const DockUI *ui)
{
    return ui->text_alpha != NULL;
}

static int about_height(const DockUI *ui, int line)
{
    return about_prerendered(ui) ? about_font[about_lines[line].style].height :
        ui->draw->dri_Font->tf_YSize;
}

/* Extent of a prerendered line: *left is the first covered column
   relative to the pen start. */
static int about_extent(int style, const char *text, int *left)
{
    const AboutFace *face = &about_font[style];
    const unsigned char *c;
    int pen = 0, lo = 0, hi = 0;
    for (c = (const unsigned char *)text; *c; ++c) {
        const AboutGlyph *glyph;
        if (*c < ABOUT_FIRST_CHAR || *c > ABOUT_LAST_CHAR) continue;
        glyph = &face->glyphs[*c-ABOUT_FIRST_CHAR];
        if (glyph->width) {
            if (pen+glyph->bearing < lo) lo = pen+glyph->bearing;
            if (pen+glyph->bearing+glyph->width > hi) hi = pen+glyph->bearing+glyph->width;
        }
        pen += glyph->advance;
    }
    *left = lo;
    return hi-lo;
}

static int about_width(const DockUI *ui, struct RastPort *rp, int line)
{
    int left;
    if (about_prerendered(ui)) return about_extent(about_lines[line].style,about_lines[line].text,&left);
    return text_length32(rp,about_lines[line].text);
}

static void about_draw(DockUI *ui, struct RastPort *rp, int line, int center, int top)
{
    const char *text = about_lines[line].text;
    if (about_prerendered(ui)) {
        const AboutFace *face = &about_font[about_lines[line].style];
        const unsigned char *c;
        int left, width = about_extent(about_lines[line].style,text,&left), pen = -left, y, x;
        unsigned char *alpha = width > 0 ? calloc((size_t)width*face->height,1) : NULL;
        if (!alpha) return;
        for (c = (const unsigned char *)text; *c; ++c) {
            const AboutGlyph *glyph;
            if (*c < ABOUT_FIRST_CHAR || *c > ABOUT_LAST_CHAR) continue;
            glyph = &face->glyphs[*c-ABOUT_FIRST_CHAR];
            for (y = 0; y < face->height; ++y) {
                const unsigned char *src = face->coverage+glyph->offset+(size_t)y*glyph->width;
                unsigned char *dst = alpha+(size_t)y*width+pen+glyph->bearing;
                for (x = 0; x < glyph->width; ++x) if (src[x] > dst[x]) dst[x] = src[x];
            }
            pen += glyph->advance;
        }
        amiga_alpha_template_blit(ui->text_alpha,rp,alpha,width,face->height,center-width/2,top);
        free(alpha);
        return;
    }
    SetSoftStyle(rp,about_lines[line].style == ABOUT_TITLE ? FSF_BOLD : FS_NORMAL,FSF_BOLD);
    Move(rp,center-text_length32(rp,text)/2,top+ui->draw->dri_Font->tf_Baseline);
    text32(rp,text);
    SetSoftStyle(rp,FS_NORMAL,FSF_BOLD);
}

static void paint_about(DockUI *ui)
{
    struct Window *w = ui->dialog;
    struct RastPort *rp = w->RPort;
    int i, y = w->BorderTop+ABOUT_MARGIN;
    SetFont(rp,ui->draw->dri_Font);
    SetDrMd(rp,JAM1);
    SetAPen(rp,ui->draw->dri_Pens[TEXTPEN]);
    for (i = 0; i < ABOUT_LINES; ++i) {
        about_draw(ui,rp,i,w->Width/2,y);
        y += about_height(ui,i)+about_lines[i].gap;
    }
}

int dock_ui_open_about(DockUI *ui)
{
    struct RastPort measure;
    struct Gadget *previous;
    int i, width = 240, height = 2*ABOUT_MARGIN, button_width, button_height;
    if (!ui || ui->dialog || !ready(ui)) return 0;
    dock_ui_close_popup(ui);
    InitRastPort(&measure); SetFont(&measure,ui->draw->dri_Font);
    for (i = 0; i < ABOUT_LINES; ++i) {
        int line_width = about_width(ui,&measure,i)+2*ABOUT_MARGIN+16;
        if (line_width > width) width = line_width;
        height += about_height(ui,i)+about_lines[i].gap;
    }
    button_width = ui->font_width*10;
    button_height = ui->font_height+12;
    height += button_height+10; /* 16 px between the credit and the button. */
    ui->dialog = open_dialog(ui,"About",width,height,BUTTONIDCMP);
    if (!ui->dialog) return 0;
    previous = CreateContext(&ui->gadgets);
    if (!previous || !gadget(ui,previous,BUTTON_KIND,GID_CANCEL,"OK",
            (ui->dialog->Width-button_width)/2,ui->dialog->BorderTop+height-button_height-12,
            button_width,button_height,PLACETEXT_IN,NULL)) {
        dock_ui_close_dialog(ui);
        return 0;
    }
    ui->showing_about = 1;
    AddGList(ui->dialog,ui->gadgets,(UWORD)-1,-1,NULL);
    RefreshGList(ui->gadgets,ui->dialog,NULL,-1);
    GT_RefreshWindow(ui->dialog,NULL);
    paint_about(ui);
    return 1;
}

static int folder_hit(const DockUI *ui, int x, int y)
{
    const FolderView *f = &ui->folder;
    int font_height = folder_font(ui)->tf_YSize;
    int width = f->body_width, control = font_height+10;
    int top = (f->header-control)/2;
    int col, row, slot;

    if (f->motion == FOLDER_OPENING) {
        x = f->pivot_x+(x-f->pivot_x)*FOLDER_SCALE_ONE/f->scale;
        y = f->pivot_y+(y-f->pivot_y)*FOLDER_SCALE_ONE/f->scale;
    }
    x -= f->body_left;
    if (folder_nested(f) && y >= top && y < top+control &&
        x >= f->pad && x < f->pad+control) return FOLDER_BACK;
    top = f->body_height-f->footer;
    if (folder_last_page(f) && y >= top && y < top+control) {
        if (f->page && x >= f->pad && x < f->pad+control) return FOLDER_PREV;
        if (f->page < folder_last_page(f) && x >= width-f->pad-control &&
            x < width-f->pad) return FOLDER_NEXT;
    }
    y -= f->header+(f->count ? 0 : font_height+6);
    x -= f->count ? f->pad : (width-f->cell_width)/2;
    if (x < 0 || y < 0) return FOLDER_OUTSIDE;
    col = x/f->cell_width;
    row = y/f->cell_height;
    if (col >= f->cols || row >= f->rows) return FOLDER_OUTSIDE;
    slot = row*f->cols+col;
    return slot <= folder_visible_files(f) ? slot : FOLDER_OUTSIDE;
}

static int folder_select(DockUI *ui, int hit, char *open_path)
{
    FolderView *f = &ui->folder;
    char path[DOCK_PREFS_PATH_CAP];
    size_t entry;
    if (hit == FOLDER_PREV || hit == FOLDER_NEXT) {
        if (hit == FOLDER_PREV && f->page) --f->page;
        else if (hit == FOLDER_NEXT && f->page < folder_last_page(f)) ++f->page;
        else return DOCK_UI_NONE;
        if (f->motion) clear_folder_motion(f);
        reset_folder_page(ui);
        return DOCK_UI_REFRESH;
    }
    strcpy(path,f->path);
    if (hit == FOLDER_BACK) {
        char *end = (char *)FilePart((STRPTR)path);
        if (!folder_nested(f)) return DOCK_UI_NONE;
        if ((size_t)(end-path) > f->root_length && end[-1] == '/') --end;
        *end = '\0';
    } else {
        if (hit == folder_visible_files(f)) {
            strcpy(open_path,path);
            return DOCK_UI_OPEN_PATH;
        }
        entry = f->page*folder_page_size(f)+hit;
        if (!AddPart((STRPTR)path,(STRPTR)f->entries[entry].name,sizeof(path))) return DOCK_UI_NONE;
        if (!f->entries[entry].drawer) {
            strcpy(open_path,path);
            return DOCK_UI_OPEN_PATH;
        }
    }
    show_folder(ui,f->anchor,path,f->root_length,0);
    return DOCK_UI_REFRESH;
}

static int popup_poll(DockUI *ui, DockUIAction *result)
{
    struct IntuiMessage *msg;
    int action = DOCK_UI_NONE, close = 0;
    while (ui->popup && (msg = (struct IntuiMessage *)GetMsg(ui->popup->UserPort))) {
        ULONG cls = msg->Class;
        UWORD code = msg->Code;
        int folder = ui->folder.path[0] != 0;
        int hit = folder ? folder_hit(ui,msg->MouseX,msg->MouseY) : popup_hit(ui,msg->MouseX,msg->MouseY);
        if (!close && cls == IDCMP_REFRESHWINDOW) {
            BeginRefresh(ui->popup);
            if (folder) paint_folder(ui); else paint_popup(ui);
            EndRefresh(ui->popup,TRUE);
        }
        ReplyMsg((struct Message *)msg);
        if (close || ui->folder.motion == FOLDER_CLOSING) continue;
        if (cls == IDCMP_INACTIVEWINDOW || (cls == IDCMP_RAWKEY && code == 0x45)) close = 1;
        else if (folder) {
            int selected = FOLDER_OUTSIDE;
            if (cls == IDCMP_MOUSEBUTTONS && code == MENUDOWN) close = 1;
            if (cls == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) ui->popup_pressed = hit;
            if (cls == IDCMP_MOUSEBUTTONS && code == SELECTUP) {
                if (hit == ui->popup_pressed) selected = hit;
                ui->popup_pressed = FOLDER_OUTSIDE;
            }
            if (cls == IDCMP_RAWKEY) {
                if (code == 0x41) selected = FOLDER_BACK;
                else if (code == 0x4f) selected = FOLDER_PREV;
                else if (code == 0x4e) selected = FOLDER_NEXT;
            }
            if (selected != FOLDER_OUTSIDE) {
                action = folder_select(ui,selected,result->path);
                if (action == DOCK_UI_OPEN_PATH) {
                    dock_ui_dismiss_popup(ui);
                    if (ui->popup) {
                        strcpy(ui->folder_open_path,result->path);
                        return DOCK_UI_REFRESH;
                    }
                }
                if (action) return action;
            }
        }
        else if (cls == IDCMP_MOUSEMOVE && hit != ui->popup_selected) {
            ui->popup_selected = hit; paint_popup(ui);
        } else if (cls == IDCMP_MOUSEBUTTONS && code == SELECTDOWN) {
            if (hit < 0) close = 1;
            else { ui->popup_pressed = hit; ui->popup_selected = hit; paint_popup(ui); }
        } else if (cls == IDCMP_MOUSEBUTTONS && code == SELECTUP) {
            if (hit >= 0 && hit == ui->popup_pressed) action = ui->popup_items[hit].action;
            close = 1;
        }
        /* Opening RMB release is intentionally ignored. Selection is a
           subsequent left click, with matching DOWN and UP inside one row. */
    }
    if (close) { dock_ui_dismiss_popup(ui); return action ? action : DOCK_UI_CANCEL; }
    return DOCK_UI_NONE;
}

/* Keep the draft within what the dock can show: Size up to the largest that
   fits this edge, scale and Folder Stack, and Scale up to the 192 px zoom.
   cap holds the slider ranges, raised for larger saved values. */
static void update_limits(DockUI *ui, int live)
{
    SettingsView *s = &ui->settings;
    DockUIPrefs probe = ui->draft;
    int slider, size_max = s->cap[SIZE_SLIDER], scale_max = s->cap[SCALE_SLIDER];
    if (live && s->path) {
        const struct StringInfo *input = s->path->SpecialInfo;
        strcpy(probe.folder,(const char *)input->Buffer);
    }
    if (ui->fit) {
        int fit = ui->fit(ui->fit_context,&probe);
        if (fit < size_max) size_max = fit;
    }
    if (size_max < slider_min[SIZE_SLIDER]) size_max = slider_min[SIZE_SLIDER];
    if (ui->draft.size > size_max) ui->draft.size = size_max;
    while (scale_max > DOCK_SCALE_ONE &&
        ((unsigned int)(dock_scaled_size(ui->draft.size,scale_max)-1) >= DOCK_MAX_ICON_SIZE ||
         dock_scaled_size(ui->draft.size,scale_max) > s->zoom_cap)) --scale_max;
    if (ui->draft.scale_percent > scale_max) ui->draft.scale_percent = scale_max;
    s->limit[SIZE_SLIDER] = size_max;
    s->limit[SCALE_SLIDER] = scale_max;
    if (!live) return;
    for (slider = 0; slider < SLIDER_COUNT; ++slider) {
        int value = slider == SIZE_SLIDER ? ui->draft.size : ui->draft.scale_percent;
        GT_SetGadgetAttrs(s->slider[slider],ui->dialog,NULL,
            GTSL_Max,s->limit[slider],GTSL_Level,value,TAG_DONE);
        format_slider(s->value[slider],slider,value);
        GT_SetGadgetAttrs(s->readout[slider],ui->dialog,NULL,GTTX_Text,(ULONG)s->value[slider],TAG_DONE);
    }
}

static void update_slider(DockUI *ui, int slider, int value)
{
    SettingsView *s = &ui->settings;
    int *setting = slider == SIZE_SLIDER ? &ui->draft.size : &ui->draft.scale_percent;
    value = clamp(value,slider_min[slider],s->limit[slider]);
    if (value == *setting) return;
    *setting = value;
    format_slider(s->value[slider],slider,value);
    GT_SetGadgetAttrs(s->readout[slider],ui->dialog,NULL,GTTX_Text,(ULONG)s->value[slider],TAG_DONE);
    update_limits(ui,1);
}

static int settings_key(DockUI *ui, UWORD code)
{
    SettingsView *s = &ui->settings;
    switch (code) {
        case 9:
            ActivateGadget(s->path,ui->dialog,NULL);
            break;
        case 13: return DOCK_UI_SAVE;
        case 27: return DOCK_UI_CANCEL;
        case 'b': case 'B': browse_folder(ui); update_limits(ui,1); break;
        case 'c': case 'C': set_folder_text(ui,""); update_limits(ui,1); break;
        case 'p': case 'P': {
            int choice = (position_index(ui->draft.edge)+(code == 'p' ? 1 : 2))%3;
            ui->draft.edge = settings_edges[choice];
            GT_SetGadgetAttrs(s->position,ui->dialog,NULL,GTCY_Active,choice,TAG_DONE);
            update_limits(ui,1);
            break;
        }
        case 's': case 'S': case 'a': case 'A': {
            int slider = code == 's' || code == 'S' ? SIZE_SLIDER : SCALE_SLIDER;
            int value = slider == SIZE_SLIDER ? ui->draft.size : ui->draft.scale_percent;
            update_slider(ui,slider,value+(code == 's' || code == 'a' ? 1 : -1));
            value = slider == SIZE_SLIDER ? ui->draft.size : ui->draft.scale_percent;
            GT_SetGadgetAttrs(s->slider[slider],ui->dialog,NULL,GTSL_Level,value,TAG_DONE);
            break;
        }
    }
    return DOCK_UI_NONE;
}

static int dialog_poll(DockUI *ui)
{
    struct IntuiMessage *msg;
    int action = DOCK_UI_NONE, moved = 0;
    if (!ui->dialog) return 0;
    while ((msg = GT_GetIMsg(ui->dialog->UserPort))) {
        ULONG cls = msg->Class;
        UWORD code = msg->Code;
        const struct Gadget *g = msg->IAddress;
        int id = (cls == IDCMP_GADGETUP && g) ? g->GadgetID : 0;
        GT_ReplyIMsg(msg);
        if (action) continue;
        if (cls == IDCMP_REFRESHWINDOW) {
            GT_BeginRefresh(ui->dialog);
            if (ui->showing_about) paint_about(ui);
            GT_EndRefresh(ui->dialog,TRUE);
        } else if (cls == IDCMP_CHANGEWINDOW) moved = 1;
        else if (cls == IDCMP_CLOSEWINDOW || id == GID_CANCEL ||
            (cls == IDCMP_RAWKEY && (code == 0x45 || (ui->showing_about && code == 0x44))))
            action = DOCK_UI_CANCEL;
        else if (!ui->showing_about) {
            if (cls == IDCMP_VANILLAKEY) action = settings_key(ui,code);
            else if (cls == IDCMP_MOUSEMOVE || cls == IDCMP_GADGETDOWN || cls == IDCMP_GADGETUP) {
                if (g == ui->settings.slider[SIZE_SLIDER]) update_slider(ui,SIZE_SLIDER,code);
                else if (g == ui->settings.slider[SCALE_SLIDER]) update_slider(ui,SCALE_SLIDER,code);
                else if (id == GID_POSITION && code < sizeof(settings_edges)/sizeof(settings_edges[0])) {
                    ui->draft.edge = settings_edges[code];
                    update_limits(ui,1);
                } else if (id == GID_BROWSE) { browse_folder(ui); update_limits(ui,1); }
                else if (id == GID_CLEAR) { set_folder_text(ui,""); update_limits(ui,1); }
                else if (id == GID_PATH) update_limits(ui,1);
                else if (id == GID_SAVE) action = DOCK_UI_SAVE;
            }
        }
    }
    if (action == DOCK_UI_CANCEL) dock_ui_close_dialog(ui);
    else if (action == DOCK_UI_SAVE) read_folder_text(ui);
    return !action && moved ? DOCK_UI_REFRESH : action;
}

int dock_ui_poll(DockUI *ui, DockUIAction *action)
{
    int type;
    if (!ui || !action) return 0;
    action->type = DOCK_UI_NONE;
    if (!ui->popup && ui->folder_open_path[0]) {
        strcpy(action->path,ui->folder_open_path);
        ui->folder_open_path[0] = '\0';
        type = DOCK_UI_OPEN_PATH;
    } else type = popup_poll(ui,action);
    if (!type) type = dialog_poll(ui);
    if (!type) return 0;
    action->type = type;
    action->prefs = ui->draft;
    return 1;
}
