#ifndef AMIGA_UI_H
#define AMIGA_UI_H

#include "dock_geometry.h"
#include "dock_prefs.h"
#include <exec/types.h>

struct Screen;
struct Window;
typedef struct DockUI DockUI;

/* edge uses DOCK_EDGE_* from dock_geometry.h. UI edits remain a draft. */
typedef struct {
    int size, scale_percent, edge;
    char folder[DOCK_PREFS_PATH_CAP];
} DockUIPrefs;
enum {
    DOCK_UI_NONE, DOCK_UI_SETTINGS, DOCK_UI_ABOUT, DOCK_UI_QUIT,
    DOCK_UI_SAVE, DOCK_UI_CANCEL, DOCK_UI_REFRESH, DOCK_UI_CLOSE_ITEM, DOCK_UI_QUIT_ITEM,
    DOCK_UI_KEEP_ITEM, DOCK_UI_NEW_SHELL, DOCK_UI_SHOW_SHELL, DOCK_UI_QUIT_SHELL,
    DOCK_UI_SHOW_CONTENTS, DOCK_UI_OPEN_PATH
};
typedef struct {
    int type;
    DockUIPrefs prefs;
    char path[DOCK_PREFS_PATH_CAP];
} DockUIAction;
typedef int (*DockUIReadIcon)(const void *context, const char *path, DockImage *icon);

/* The caller keeps screen and the graphics/intuition libraries alive. */
DockUI *dock_ui_create(struct Screen *screen, int window_shapes);
void dock_ui_set_converters(DockUI *ui, DockConvertRGB24 encode, DockConvertRGB565 decode,
    DockConvertRGB565 encode565);
/* Called with 1 before and 0 after a UI call that blocks the main task, such
   as the ASL drawer requester. Suspend PiP and input capture in between. */
typedef void (*DockUIBlocking)(void *context, int blocking);
void dock_ui_set_blocking_hook(DockUI *ui, DockUIBlocking hook, void *context);
/* Returns the largest Size the dock can show for a draft (its edge, scale
   and Folder Stack). Settings never offer a larger Size. */
typedef int (*DockUIFit)(void *context, const DockUIPrefs *draft);
/* Shows work in progress in the open dialog: a busy pointer and the status
   as its title. NULL restores the normal pointer and title. */
void dock_ui_set_busy(DockUI *ui, const char *status);
void dock_ui_set_fit_hook(DockUI *ui, DockUIFit hook, void *context);
void dock_ui_destroy(DockUI *ui);
ULONG dock_ui_signal_mask(const DockUI *ui);
int dock_ui_open_popup(DockUI *ui, DockEdge edge, int anchor_x, int anchor_y,
    const struct Window *native_dock,
    int item_action, int can_keep, int drawer);
int dock_ui_open_folder(DockUI *ui, DockEdge edge, int x, int y, const char *path,
    int icon_size, DockUIReadIcon read_icon, const void *context, int profile);
void dock_ui_dismiss_popup(DockUI *ui);
int dock_ui_popup_bounds(const DockUI *ui, DockRect *bounds);
void dock_ui_popup_span(const DockUI *ui, int row, int *left, int *right);
int dock_ui_folder_pending(const DockUI *ui);
/* The Folder Stack view is open and not closing. */
int dock_ui_folder_open(const DockUI *ui);
/* The Folder Stack view is opening or closing. */
int dock_ui_folder_moving(const DockUI *ui);
/* How far the Folder Stack box is out on screen: 0 (hidden) to 256 (open). */
int dock_ui_folder_progress(const DockUI *ui);
ULONG dock_ui_folder_frame_delay(const DockUI *ui);
int dock_ui_folder_step(DockUI *ui);
void dock_ui_finish_folder_motion(DockUI *ui);
/* The label is nonactivating and lives outside the visible Dock artwork. */
int dock_ui_show_label(DockUI *ui, DockEdge edge, const DockRect *anchor, const char *text);
void dock_ui_move_label(DockUI *ui, DockEdge edge, const DockRect *anchor);
void dock_ui_hide_label(DockUI *ui);
int dock_ui_dialog_bounds(const DockUI *ui, DockRect *bounds);
int dock_ui_open_settings(DockUI *ui, const DockUIPrefs *initial);
int dock_ui_open_about(DockUI *ui);
/* Nonblocking: call until zero after a wake. Every returned message is replied.
   SAVE leaves the settings window open until the caller commits successfully. */
int dock_ui_poll(DockUI *ui, DockUIAction *action);
void dock_ui_close_dialog(DockUI *ui);
int dock_ui_owns_window(const DockUI *ui, const struct Window *window);
int dock_ui_busy(const DockUI *ui);

#endif
