/* DockAwesome: items and icons, gestures, animation and timers,
   preferences, profiling and the event loop. The input handler, Workbench
   status, Workbench client and the surface backends are separate modules
   (amiga_app.h). */
#include "amiga_app.h"

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

static void sync_input_ui(App *app);

static void item_free(Item *item)
{
    dock_cache_free(&item->cache);
}

static uint32_t new_pin_id(App *app)
{
    do { if (!++app->next_pin_id) ++app->next_pin_id; }
    while (pin_index(app, app->next_pin_id) >= 0);
    return app->next_pin_id;
}

static int stack_index(const App *app)
{
    return app->count && app->items[app->count-1].stack ? app->count-1 : app->count;
}

enum { STACK_DIVIDER_GAP = 12 };

Item *append_item(App *app, const char *path, int kind, DockCache *cache)
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

static int dock_background(App *app, int x, int y)
{
    DockRect rect;
    return input_hit_test(app,x,y) < 0 &&
        dock_edge_panel_rect(&app->settings,app->edge,app->placement,app->count,
            app->width,app->height,&rect) &&
        rounded_background(&rect,app->renderer.panel.radius,x,y);
}

void erase_item(App *app, int index)
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

void interaction_cancel_for(App *app, uint32_t token)
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

static void message(const char *text)
{
    struct EasyStruct req = { sizeof(req), 0, (STRPTR)"DockAwesome", (STRPTR)"%s", (STRPTR)"OK" };
    ULONG arguments[] = { (ULONG)text };
    if (IntuitionBase) EasyRequestArgs(NULL, &req, NULL, arguments);
    else puts(text);
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

int canonical_path(const char *path, char *absolute)
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

__attribute__((noinline)) int load_icon_mode(const App *app, const char *path, DockImage *out,
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

int animation_needed(const App *app)
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

void animation_cancel(App *app)
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

void live_cancel(App *app)
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

/* A UI requester (ASL) blocks the main loop: hide PiP and let input pass
   through until it returns. The next loop pass presents PiP again. */
void ui_blocking(void *context, int blocking)
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
int ui_fit(void *context, const DockUIPrefs *draft)
{
    const App *app = (const App *)context;
    DockSettings s;
    int size;
    for (size = DOCK_MAX_ICON_SIZE; size > DOCK_MIN_ICON_SIZE; --size)
        if (draft_settings(app,draft,size,&s,NULL) && draft_fits(app,draft,&s)) return size;
    return DOCK_MIN_ICON_SIZE;
}

/* The saved preferences as Settings shows them. */
DockUIPrefs current_ui_prefs(const App *app)
{
    DockUIPrefs prefs = {app->settings.base_size,displayed_scale(app),app->edge,{0}};
    strcpy(prefs.folder,app->prefs.folder);
    return prefs;
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
    printf("Input: downs=%lu ups=%lu right_seen=%lu right_icons=%lu right_ambiguous=%lu ambiguous_pass=%lu overflow_pass=%lu\n",
        (unsigned long)app->input_context.downs, (unsigned long)app->input_context.ups,
        (unsigned long)app->input_context.right_seen, (unsigned long)app->input_context.right_icons,
        (unsigned long)app->input_context.right_ambiguous,
        (unsigned long)app->input_context.ambiguous, (unsigned long)app->input_context.overflow);
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
        printf("PiP2: layer_busy=%lu\n",(unsigned long)app->stats.layer_busy);
        {
            AmigaDropCatcherStats drop = amiga_drop_catcher_stats(app->drop_catcher);
            printf("PiP receiver: arms=%lu disarms=%lu presses_armed=%lu presses_unarmed=%lu\n",
                drop.arms,drop.disarms,drop.presses_armed,drop.presses_unarmed);
        }
        printf("PiP2: polls=%lu source_changes=%lu front_requests=%lu capture_compare_us_avg=%lu decode_us_avg=%lu changed_px_avg=%lu (settled100ms, motion20ms)\n",
            (unsigned long)app->stats.source_polls, (unsigned long)app->stats.source_changes,
            (unsigned long)app->stats.front_requests,
            ticks_us(app,app->stats.capture_ticks,app->stats.source_polls),
            ticks_us(app,app->stats.decode_ticks,app->stats.source_changes),
            app->stats.source_changes ? (unsigned long)(app->stats.source_pixels/app->stats.source_changes) : 0UL);
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
void stack_button_build(App *app)
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
    /* The one indirect call per frame. */
    app->surface->present(app, &app->canvas);
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

/* Retain the current surface while it can still support the zoom layout.
   The empty Dock has a different viewport length. */
int rest_after_pin_change(App *app, int before)
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

/* Opens a pin through Workbench, or starts it the way Workbench would when
   another desktop (Directory Opus on AmiKit) runs instead. A failure names
   the path and the reason dos reports. */
static int launch_pin(App *app, const char *path)
{
    LONG default_icon = FALSE, error = 0;
    struct TagItem get[] = {
        { ICONGETA_FailIfUnavailable, FALSE },
        { ICONGETA_IsDefaultIcon, (ULONG)&default_icon },
        { TAG_DONE, 0 }
    };
    struct DiskObject *icon;
    int opened;
    if (self_launching(app)) {
        opened = launch_object(app,path,&error);
        icon = NULL;
    } else if (!(icon = GetIconTagList((STRPTR)path,get))) {
        if (!(opened = OpenWorkbenchObjectA((STRPTR)path,NULL))) error = IoErr();
    } else if (default_icon && icon->do_Type == WBPROJECT &&
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
        if (!opened) error = IoErr();
        if (parent) UnLock(parent);
        if (file) UnLock(file);
    } else if (!(opened = OpenWorkbenchObjectA((STRPTR)path,NULL))) error = IoErr();
    if (icon) FreeDiskObject(icon);
    if (!opened) {
        char text[PATH_CAP+128], why[96];
        why[0] = 0;
        if (error) Fault(error,NULL,(STRPTR)why,sizeof(why));
        snprintf(text,sizeof(text),"Could not open\n%s%s%s",path,why[0] ? "\n" : "",why);
        message(text);
    }
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
            if (launch_pin(app,item->path)) {
                record_launch(app,item->pin_id);
                status_changed(app);
            }
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
    /* Shaped windows (ApolloOS) give the preview the icon's outline; without
       them it saves what lies under it. */
    app->drag_preview = amiga_drag_preview_open(app->screen,&app->items[index].cache.image[0],
        app->pip_mode && app->caps.shaped_windows,x,y);
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
        /* A pin with nothing to offer here (no WindowAction on AmigaOS 3.x)
           shows the dock's own menu rather than ignoring the click. */
        if (!action && !can_keep && !stack) pin_id = 0;
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
    if (item->kind == PIN_DRAWER && !self_launching(app)) CloseWorkbenchObjectA((STRPTR)item->path,NULL);
    else if (item->kind == PIN_DRAWER) { if (item->lister) dopus_close_lister(app,item->lister); }
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
    } else if (launch_pin(app,app->items[index].path)) status_changed(app);
}

static int displayed_scale(const App *app)
{
    return app->scale_percent ? app->scale_percent :
        (app->settings.max_size*DOCK_SCALE_ONE+app->settings.base_size/2)/app->settings.base_size;
}

int apply_preferences(App *app, const DockUIPrefs *prefs)
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

/* Retarget and draw everything; 0 when the dock has failed. */
static int full_frame(App *app)
{
    update_target(app);
    app->surface->capture(app,1);
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
    app->surface->capture(app,1);
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
            (app->launch_port ? (1UL<<app->launch_port->mp_SigBit) : 0) |
            (app->status_notify ? (1UL<<app->status_notify_bit) : 0) |
            app->input_context.signal | dock_ui_signal_mask(app->ui) | SIGBREAKF_CTRL_C);
        struct IntuiMessage *msg;
        DockUIAction action;
        int redraw = app->gesture_redraw, recreate = 0, save = 0, due;
        int dismiss_popup;
        ++app->stats.wakes; app->gesture_redraw = 0;
        if (signals & SIGBREAKF_CTRL_C) break;
        if (launch_reap(app)) { redraw = 1; status_changed(app); }
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
                if (launch_pin(app,action.path)) status_changed(app);
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
        /* The backend's background poll (PiP: live capture and drag image).
           Only a backend sets failed here; every other failure has already
           left the loop. */
        if (app->surface->poll(app,due,redraw)) redraw = 1;
        if (app->failed) { interaction_cancel(app); input_invalidate(app); break; }
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
    app->surface->capture(app,1);
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
         "            [--self-launch] [Workbench paths...] [--version] [--caps]\n"
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
         "--self-launch starts pins the way Workbench does without asking it, as\n"
         "the dock does when another desktop replaces Workbench.\n"
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
        else if (!strcmp(argv[i], "--self-launch")) app->self_launch = 1;
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
    launch_open(app);
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
    launch_close(app);
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
