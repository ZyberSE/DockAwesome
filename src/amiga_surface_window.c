/* The native window backend: snapshot mode composes over a copy of the
   desktop taken when the surface opens (R takes a new one), opaque mode over
   the plate colour. The window clips the dock under other windows; Intuition
   asks for a repaint after Simple Refresh damage. */
#include "amiga_app.h"

static int window_open(App *app)
{
    if (app->snapshot && !amiga_display_capture(app->display, &app->backdrop, app->left, app->top)) return 0;
    dock_renderer_backdrop_changed(&app->renderer);
    app->window = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)app->screen,
        WA_Left, app->left, WA_Top, app->top, WA_Width, app->width, WA_Height, app->height,
        WA_Borderless, TRUE, WA_Activate, FALSE, WA_ReportMouse, TRUE, WA_RMBTrap, TRUE,
        WA_SimpleRefresh, TRUE, WA_NoCareRefresh, FALSE,
        WA_IDCMP, IDCMP_FLAGS, WA_MouseQueue, 1,
        TAG_DONE);
    if (!app->window) return 0;
    app->appwindow = AddAppWindowA(0, 0, app->window, app->drop_port, NULL);
    return app->appwindow != NULL;
}

static void window_close(App *app)
{
    if (app->appwindow) { RemoveAppWindow(app->appwindow); app->appwindow = NULL; }
    if (app->window) { CloseWindow(app->window); app->window = NULL; }
}

/* The backdrop is fixed while the surface is open. */
static int window_capture(App *app, int full_compare)
{
    (void)app; (void)full_compare;
    return 0;
}

static int window_poll(App *app, int due, int redraw)
{
    (void)app; (void)due; (void)redraw;
    return 0;
}

static void window_present(App *app, const DockImage *frame)
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
    if (!app->count) {
        const char *label = "Drop Workbench icons here";
        SetAPen(app->window->RPort, 1); SetDrMd(app->window->RPort, JAM1);
        {
            WORD length = amiga_text_length(app->window->RPort,label,strlen(label));
            Move(app->window->RPort, (app->width-length)/2, app->height-25);
            amiga_text_draw(app->window->RPort,label,strlen(label));
        }
    }
}

const SurfaceOps amiga_surface_window = {
    "window", window_open, window_close, window_capture, window_poll, window_present
};
