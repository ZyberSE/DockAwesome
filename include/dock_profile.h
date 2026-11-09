#ifndef DOCK_PROFILE_H
#define DOCK_PROFILE_H

/* What the machine offers, probed once at startup (and the screen fields
   again when Workbench reopens), and what the dock chooses from it. No
   decision here is taken per frame or per pixel. Each flag is 0 or 1. */
typedef struct {
    int cpu_080;        /* exec AttnFlags reports an Apollo 68080 */
    int ammx;           /* 68080 and a build with the AMMX kernels */
    int saga_core;      /* SAGA core revision, 0 when not read (no 68080) */
    int saga_pip2;      /* core 11900 or newer: PiP2 registers */
    int shaped_windows; /* Intuition V50 shape API and Layers V50 */
    int rgb_rastport;   /* graphics.library takes RGB RastPort tags (AROS) */
    int alpha_text;     /* cybergraphics.library V51 BltTemplateAlpha */
    int window_action;  /* Intuition V50 WindowAction (quit and close items) */
    int status_notify;  /* Intuition V50 window open/close notification */
    int apollo_console; /* audited Apollo console close dispatch (Intuition
                           50.7, input 41.4, console 41.7) */
    int screen_notify;  /* screennotify.library: Workbench close and reopen */
    int screen_depth;   /* Workbench screen depth in bits */
    int workbench_port; /* the WORKBENCH port: Workbench itself is running */
    int scalos;         /* the SCALOS port: Scalos runs as the desktop */
    int dopus;          /* Directory Opus 5 runs (DOPUS.1 and dopus5.library) */
} DockCaps;

enum { DOCK_BACKEND_OPAQUE, DOCK_BACKEND_SNAPSHOT, DOCK_BACKEND_PIP };
enum { DOCK_PIXELS_PORTABLE, DOCK_PIXELS_AMMX };
/* Why an explicit PiP request cannot be met, the first missing need. */
enum {
    DOCK_PROFILE_OK, DOCK_PROFILE_PIP_NO_AMMX, DOCK_PROFILE_PIP_NO_CORE,
    DOCK_PROFILE_PIP_SCREEN
};

typedef struct {
    int backend;        /* DOCK_BACKEND_*: PiP falls back to snapshot */
    int pixels;         /* DOCK_PIXELS_* for every backend */
    int ui_shapes;      /* shaped labels, menus and Folder Stack */
    int ui_glass;       /* RGB glass for menus, labels and Folder Stack */
    int aa_text;        /* antialiased menu and label text */
    int apollo_pointer; /* input handler reads ApolloOS relative pointer events */
    int error;          /* DOCK_PROFILE_*, set only for an explicit PiP request */
} DockProfile;

DockProfile dock_choose_profile(const DockCaps *caps, int requested_backend, int explicit_pip);
const char *dock_profile_error_text(int error);
const char *dock_backend_name(int backend);

#endif
