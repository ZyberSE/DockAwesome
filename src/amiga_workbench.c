/* The screennotify.library Workbench client: release everything on the
   Workbench screen before it closes and reopen when it is back. */
#include "amiga_app.h"

/* screennotify.library: AddWorkbenchClient/RemWorkbenchClient and its
   message. Type 3 is Workbench; value FALSE: about to close (reply once
   every window on it is closed), TRUE: open again. A close that fails is
   followed by TRUE as well. */
enum { WORKBENCH_NOTIFY_TYPE = 3 };
typedef struct { struct Message message; ULONG type; APTR value; } WorkbenchNotify;

void workbench_notify_open(App *app)
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

void workbench_notify_close(App *app)
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
int workbench_notify_poll(App *app)
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
