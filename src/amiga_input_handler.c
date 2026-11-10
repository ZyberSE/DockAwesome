/* The input.device handler (dock_input_observe) runs on input.device's
   task: it must not render, allocate or wait. It calls only the hit tests
   below and dock_image_opaque_at, exec Signal and AttemptSemaphore,
   layers WhichLayer under that semaphore, and the verified console
   handler (dispatch_shell_close). The main task publishes hit snapshots
   (input_publish) and reads the queued button pairs (input_pop). */
#include "amiga_app.h"
#include <proto/layers.h>

static int snapshot_hit(const InputSnapshot *s, int x, int y)
{
    int i;
    for (i = s->count-1; i >= 0; --i)
        if (dock_image_opaque_at(s->image[i],x-s->placement[i].x,y-s->placement[i].y)) return i;
    return -1;
}

/* In the window modes another window can cover the dock. The pointer is
   on the dock only where its layer is the front one; when the layer list
   is busy the press goes on to Intuition, as before. */
static int dock_layer_in_front(const InputSnapshot *s)
{
    struct Layer_Info *info = &s->screen->LayerInfo;
    int front;
    if (!s->layer) return 1;
    if (!AttemptSemaphore(&info->Lock)) return 0;
    front = WhichLayer(info,s->screen->MouseX,s->screen->MouseY) == s->layer;
    ReleaseSemaphore(&info->Lock);
    return front;
}

int rounded_background(const DockRect *r, int radius, int x, int y)
{
    int dx = 0, dy = 0;
    if (x < r->left || x >= r->right || y < r->top || y >= r->bottom) return 0;
    if (x < r->left+radius) dx = 2*(r->left+radius-x)-1;
    else if (x >= r->right-radius) dx = 2*(x-r->right+radius)+1;
    if (y < r->top+radius) dy = 2*(r->top+radius-y)-1;
    else if (y >= r->bottom-radius) dy = 2*(y-r->bottom+radius)+1;
    return !dx || !dy || dx*dx+dy*dy <= 4*radius*radius;
}

void input_publish(App *app)
{
    InputContext *c = &app->input_context;
    InputSnapshot *s;
    const DockPlacement *row = row_placement(app);
    int i;
    if (app->pip_mode ? !app->pip_visible : !app->window) {
        Disable(); c->active = NULL; Enable(); return;
    }
    if (dock_ui_busy(app->ui) ||
        IntuitionBase->FirstScreen != app->screen || native_menu_held(app)) {
        Disable(); c->active = NULL; Enable(); return;
    }
    s = c->active == &c->snapshots[0] ? &c->snapshots[1] : &c->snapshots[0];
    s->screen = app->screen; s->left = app->left; s->top = app->top;
    s->count = row_count(app); s->generation = app->input_generation;
    dock_edge_panel_rect(&app->settings,app->edge,row,s->count,
        app->width,app->height,&s->panel);
    s->panel_radius = app->renderer.panel.radius;
    s->layer = app->pip_mode ? NULL : app->window->WLayer;
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

int input_pop(App *app, InputButton *button)
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
            if (e->ie_Code == MENUDOWN) ++c->right_seen;
            if (e->ie_Code == MENUDOWN && s && !c->ui_busy &&
                !c->held && !external_was_held && IntuitionBase->FirstScreen == s->screen) {
                int x = s->screen->MouseX-s->left, y = s->screen->MouseY-s->top;
                int hit = snapshot_hit(s,x,y);
                int on_dock = (hit >= 0 || rounded_background(&s->panel,s->panel_radius,x,y)) &&
                    dock_layer_in_front(s);
                if (on_dock && !clean) ++c->right_ambiguous;
                if (on_dock && clean) {
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
                !s->layer && /* the window modes take left presses through IDCMP */
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

/* Point the input handler at the dock's screen, or at none (NULL) before
   that screen is unlocked. State tied to the previous screen ends here:
   external press tracking and the close button stop, and a held left or
   right press stays cancelled but still owns its release. */
void input_attach_screen(App *app, struct Screen *screen)
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

int install_input(App *app)
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

void remove_input(App *app)
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
