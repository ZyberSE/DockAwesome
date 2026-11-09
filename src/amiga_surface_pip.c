/* The SAGA PiP2 surface backend: a keyed RGB565 overlay over live
   Workbench scanout, an input-transparent receiver window (shaped on
   ApolloOS, one pixel on AmigaOS 3.x), and the drop receiver. See
   docs/pip-backend.md. */
#include "amiga_app.h"
#include "amiga_float.h"

/* The overlay's columns on the screen. A right dock's keyed transport
   padding lies past the screen edge. */
static int pip_visible_width(const App *app)
{
    return app->width-app->settings.edge_padding;
}

static ULONG region_rect_count(const struct Region *region)
{
    const struct RegionRectangle *rect;
    ULONG count = 0;
    if (region) for (rect = region->RegionRectangle; rect; rect = rect->Next) ++count;
    return count;
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

static int update_drag_mask(App *app, int sample)
{
    enum { DRAG_RADIUS = 128 };
    int x = app->screen->MouseX-app->left, y = app->screen->MouseY-app->top;
    int left = x-DRAG_RADIUS, top = y-DRAG_RADIUS;
    int right = x+DRAG_RADIUS+1, bottom = y+DRAG_RADIUS+1, row;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > pip_visible_width(app)) right = pip_visible_width(app);
    if (bottom > app->height) bottom = app->height;
    if (amiga_drop_catcher_active(app->drop_catcher) && app->input_context.external_held &&
        app->input_context.external_dragged && right > left && bottom > top) {
        int changed, reference_changed = 0;
        if (!sample) return 0;
        if (!app->drag_reference) {
            if (amiga_display_drag_reference565(app->display,
                    amiga_drop_catcher_backing(app->drop_catcher),app->source565,pip_visible_width(app))) {
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
    int visible = pip_visible_width(app);
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
    if (pip_blocked_by_screen_or_menu(app)) return 0;
    /* Native Workbench can retain layer locks through its drag loop. Do not
       enter a layer/bitmap operation while its external gesture is active. */
    if (amiga_drop_catcher_active(app->drop_catcher)) return 0;
    /* FirstWindow is creation order, so inspect the locked layer Z order.
       Retry asynchronous depth requests at most four times a second: another
       window can move ahead between checks. A busy layer list (Intuition
       holds it through a window drag on AmigaOS 3.x) skips only this check:
       the capture below still follows the drag. */
    if (!AttemptSemaphore(&app->screen->LayerInfo.Lock)) {
        ++app->stats.layer_busy;
        clip = app->dialog_clip;
    } else {
        struct Layer *layer;
        int obscured = 0;
        clip = dialog_clip(app);
        /* The one-pixel receiver (no shapes) comes to the front when it
           catches a drop; its place at rest does not matter. */
        if (app->caps.shaped_windows) for (layer = app->window->WLayer->front; layer; layer = layer->front)
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
        strcpy(app->surface_error, "PiP lost the screen. Restart the dock on a supported screen.");
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

/* Encode and key the frame into the back buffer and show it, unless
   another screen or a native menu is in front. */
static void pip_show(App *app, const DockImage *frame)
{
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
    mask.app = app; mask.visible_width = pip_visible_width(app); mask.count = app->profile;
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
    /* Unmasked, pip_mask changes nothing unless the window is narrower
       than the canvas (the right-edge transport padding). */
    pixels = dock_convert565_damage(buffer,stride,frame,app->coverage,pending,app->scratch,
        app->renderer.pixels->encode565,app->renderer.pixels->key565,
        masked || mask.visible_width < frame->width ? pip_mask : NULL,&mask);
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
}

static void pip_present(App *app, const DockImage *frame)
{
    /* Both PiP buffers miss this damage until a conversion reaches them;
       a skipped present keeps accumulating. */
    dock_damage_merge(&app->pip_pending[0], &app->renderer.damage, app->width, app->height);
    dock_damage_merge(&app->pip_pending[1], &app->renderer.damage, app->width, app->height);
    pip_show(app, frame);
    if (app->pip_visible) app->displayed_drop_slot = app->drop_preview_slot;
    input_publish(app);
}

/* The event loop's background poll: the live capture while the dock rests,
   moves or a redraw is pending, then the native drag image. */
static int pip_poll(App *app, int due, int redraw)
{
    /* The moving Folder Stack covers the zoom headroom above the plate,
       never the plate: every poll would see it change and redraw the whole
       dock. Poll again once it stands still. */
    if (!app->failed && (due || redraw || app->gesture_redraw || !app->pip_visible) &&
        !(dock_ui_folder_moving(app->ui) && app->pip_visible)) {
        if (live_capture(app,redraw || app->gesture_redraw)) redraw = 1;
        live_cancel(app);
    }
    if (update_drag_mask(app,due || redraw)) redraw = 1;
    return redraw;
}

static int pip_open(App *app)
{
    /* The keyed tail of a right dock lies past the native window. */
    int right_padding = app->settings.edge_padding;
    dock_damage_set_full(&app->pip_pending[0]); dock_damage_set_full(&app->pip_pending[1]);
    app->pip_masked[0] = app->pip_masked[1] = 0;
    if (app->damage_check) {
        app->check_coverage = malloc((size_t)app->width*app->height);
        app->check565 = malloc((size_t)app->width*app->height*2);
        if (!app->check_coverage || !app->check565) return 0;
        app->check_renderer.coverage = app->check_coverage;
    }
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
        strcpy(app->surface_error, "PiP cannot read this screen's pixel format."); return 0;
    }
    dock_renderer_backdrop_changed(&app->renderer);
    if (!app->caps.shaped_windows) {
        /* Classic Intuition has no window shapes. The receiver rests as one
           undrawn pixel in the screen corner, outside the captured box, and
           waits on the dock while the pointer is on a Workbench icon. */
        app->window = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)app->screen,
            WA_Left, 0, WA_Top, 0, WA_Width, 1, WA_Height, 1,
            WA_Borderless, TRUE, WA_Activate, FALSE, WA_RMBTrap, TRUE,
            WA_SimpleRefresh, TRUE, WA_NoCareRefresh, TRUE,
            WA_BackFill, (ULONG)LAYERS_NOBACKFILL,
            WA_IDCMP, IDCMP_FLAGS & ~IDCMP_REFRESHWINDOW, TAG_DONE);
        if (!app->window) return 0;
        /* Labels and dragged pins float in PiP1 when it is free. */
        app->float_layer = amiga_float_create(app->screen, app->pip_x_offset, app->pip_y_offset);
        dock_ui_set_float(app->ui, app->float_layer);
        app->appwindow = AddAppWindowA(0, 0, app->window, app->drop_port, NULL);
        if (!app->appwindow) return 0;
        app->drop_catcher = amiga_drop_catcher_init(app->screen, app->window, NULL,
            app->left, app->top, pip_visible_width(app), app->height);
        return app->drop_catcher != NULL;
    }
    app->window = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)app->screen,
        WA_Left, app->left, WA_Top, app->top, WA_Width, app->width-right_padding, WA_Height, app->height,
        WA_Borderless, TRUE, WA_Activate, FALSE, WA_ReportMouse, TRUE, WA_RMBTrap, TRUE,
        WA_SimpleRefresh, TRUE, WA_NoCareRefresh, TRUE,
        WA_IDCMP, IDCMP_FLAGS & ~IDCMP_REFRESHWINDOW, WA_MouseQueue, 1,
        WA_ShapeRegion, (ULONG)app->shape,
        TAG_DONE);
    if (!app->window) return 0;
    if (amiga_change_window_shape(app->window, app->shape) != app->shape) {
        strcpy(app->surface_error, "PiP needs Intuition window shapes."); return 0;
    }
    app->appwindow = AddAppWindowA(0, 0, app->window, app->drop_port, NULL);
    if (!app->appwindow) return 0;
    app->drop_catcher = amiga_drop_catcher_init(app->screen, app->window,
        app->shape, app->left, app->top, pip_visible_width(app), app->height);
    return app->drop_catcher != NULL;
}

/* Stop the overlay before the window goes; window regions must outlive
   their windows. */
static void pip_close(App *app)
{
    dock_ui_set_float(app->ui, NULL);
    amiga_float_destroy(app->float_layer); app->float_layer = NULL;
    amiga_pip_close(app->pip); app->pip = NULL;
    if (app->appwindow) { RemoveAppWindow(app->appwindow); app->appwindow = NULL; }
    if (app->window) { CloseWindow(app->window); app->window = NULL; }
    amiga_drop_catcher_destroy(app->drop_catcher); app->drop_catcher = NULL;
    if (app->shape) { DisposeRegion(app->shape); app->shape = NULL; }
    if (app->shape_pending) { DisposeRegion(app->shape_pending); app->shape_pending = NULL; }
    free(app->source565); app->source565 = NULL;
    free(app->scratch); app->scratch = NULL;
    free(app->check_coverage); app->check_coverage = NULL;
    free(app->check565); app->check565 = NULL;
    app->renderer.coverage = NULL;
    free(app->coverage); app->coverage = NULL;
    free(app->drag_mask); app->drag_mask = NULL;
    app->drag_reference = app->drag_mask_live = 0;
    app->drag_box = (DockRect){0,0,0,0};
    app->source_valid = app->pip_visible = 0;
    app->front_pending = 0;
}

const SurfaceOps amiga_surface_pip = {
    "pip", pip_open, pip_close, live_capture, pip_poll, pip_present
};
