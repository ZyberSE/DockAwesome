/* The common surface steps around the backends (SurfaceOps in
   amiga_app.h): placement, the display layer, canvas and layout, the queued
   drops, and the choice of backend from the probed profile. */
#include "amiga_app.h"

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

void close_surface(App *app)
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
    /* The backend releases its hardware, native window and buffers. No
       backend is chosen before the first select_backend. */
    if (app->surface) app->surface->close(app);
    free(app->check_canvas.pixels); memset(&app->check_canvas,0,sizeof(app->check_canvas));
    dock_renderer_release(&app->check_renderer);
    app->settling = 0;
    amiga_display_close(app->display); app->display = NULL;
    free(app->canvas.pixels); free(app->backdrop.pixels); dock_renderer_release(&app->renderer);
    memset(&app->canvas, 0, sizeof(app->canvas)); memset(&app->backdrop, 0, sizeof(app->backdrop));
}

int open_surface(App *app)
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
    if (app->damage_check) {
        if (!image_alloc(&app->check_canvas, app->width, app->height)) return 0;
        app->check_renderer.pixels = app->renderer.pixels;
        app->check_renderer.coverage = app->check_coverage;
    }
    /* The backdrop, the native window and AppWindow, and present's needs. */
    if (!app->surface->open(app)) return 0;
    shift_held_drops(app,0);
    stack_button_build(app);
    return 1;
}

static void apply_profile(App *app, int backend, int explicit_pip)
{
    app->features = dock_choose_profile(&app->caps,backend,explicit_pip);
    app->pip_mode = app->features.backend == DOCK_BACKEND_PIP;
    app->snapshot = app->features.backend == DOCK_BACKEND_SNAPSHOT;
    /* The 68080 build's AMMX kernels serve every backend, not only PiP. */
    app->use_ammx = app->features.pixels == DOCK_PIXELS_AMMX;
    /* Only while no surface is open: close uses the table that opened it. */
    app->surface = app->pip_mode ? &amiga_surface_pip : &amiga_surface_window;
}

/* Follow the (new) Workbench screen; features.error reports an explicit
   PiP request that cannot be met. */
void select_backend(App *app, int explicit_pip)
{
    amiga_caps_probe_screen(&app->caps,app->screen);
    apply_profile(app,app->requested_backend,explicit_pip);
}

/* Pixel kernels for use_ammx, shared with the controls. */
void use_pixel_backend(App *app)
{
    app->renderer.pixels = dock_backend_portable();
#ifdef DOCK_AMMX
    if (app->use_ammx) app->renderer.pixels = dock_backend_ammx();
#endif
    dock_ui_set_converters(app->ui,app->renderer.pixels->rgba_to_rgb24,app->renderer.pixels->decode565,
        app->renderer.pixels->encode565);
}

/* After a closed PiP surface: open the snapshot backend instead. */
int fall_back_to_snapshot(App *app)
{
    apply_profile(app,DOCK_BACKEND_SNAPSHOT,0);
    use_pixel_backend(app);
    app->surface_error[0] = 0;
    return open_surface(app);
}
