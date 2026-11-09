#include "dock_profile.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Probed capabilities of the machines DockAwesome meets. Fields in
   DockCaps order: cpu_080, ammx, saga_core, saga_pip2, shaped_windows,
   rgb_rastport, alpha_text, window_action, status_notify, apollo_console,
   screen_notify, screen_depth, workbench_port, scalos, dopus. */
typedef struct {
    const char *name;
    DockCaps caps;
    /* Expected for a saved PiP setting (not explicit), and the error for an
       explicit --pip. Snapshot and opaque requests always keep their mode. */
    int pip_backend, pip_error;
    int pixels, ui_shapes, ui_glass, aa_text;
} Machine;

static const Machine machines[] = {
    {"ApolloOS R9.5, Vampire V4, 16-bit SAGA",
        {1,1,12515,1,1,1,1,1,1,1,1,16,1,0,0},
        DOCK_BACKEND_PIP, DOCK_PROFILE_OK, DOCK_PIXELS_AMMX, 1, 1, 1},
    {"ApolloOS R9.5, Vampire V4, 24-bit screen",
        {1,1,12515,1,1,1,1,1,1,1,1,24,1,0,0},
        DOCK_BACKEND_SNAPSHOT, DOCK_PROFILE_PIP_SCREEN, DOCK_PIXELS_AMMX, 1, 1, 1},
    {"ApolloOS R9.5, Vampire V4, 32-bit screen",
        {1,1,12515,1,1,1,1,1,1,1,1,32,1,0,0},
        DOCK_BACKEND_SNAPSHOT, DOCK_PROFILE_PIP_SCREEN, DOCK_PIXELS_AMMX, 1, 1, 1},
    {"ApolloOS R9.5, Vampire V4, 8-bit screen",
        {1,1,12515,1,1,1,1,1,1,1,1,8,1,0,0},
        DOCK_BACKEND_SNAPSHOT, DOCK_PROFILE_PIP_SCREEN, DOCK_PIXELS_AMMX, 1, 0, 0},
    {"ApolloOS, Vampire with a core older than 11900",
        {1,1,11000,0,1,1,1,1,1,1,1,16,1,0,0},
        DOCK_BACKEND_SNAPSHOT, DOCK_PROFILE_PIP_NO_CORE, DOCK_PIXELS_AMMX, 1, 1, 1},
    /* AmigaOS 3.2 has no V50 Intuition or Layers and no AROS RGB tags. */
    {"AmigaOS 3.2, Vampire V4, 16-bit SAGA",
        {1,1,12515,1,0,0,0,0,0,0,0,16,1,0,0},
        DOCK_BACKEND_SNAPSHOT, DOCK_PROFILE_PIP_NO_SHAPES, DOCK_PIXELS_AMMX, 0, 0, 0},
    {"AmigaOS 3.2, Vampire V4, screennotify installed",
        {1,1,12515,1,0,0,0,0,0,0,1,16,1,0,0},
        DOCK_BACKEND_SNAPSHOT, DOCK_PROFILE_PIP_NO_SHAPES, DOCK_PIXELS_AMMX, 0, 0, 0},
    /* AmiKit ships screennotify.library and runs Directory Opus instead of
       Workbench. P96's cybergraphics emulation is older than V51; even one
       with BltTemplateAlpha gives no AA text without the glass. */
    {"AmigaOS 3.9/AmiKit, Vampire V4, P96 16-bit",
        {1,1,12515,1,0,0,0,0,0,0,1,16,0,0,1},
        DOCK_BACKEND_SNAPSHOT, DOCK_PROFILE_PIP_NO_SHAPES, DOCK_PIXELS_AMMX, 0, 0, 0},
    {"AmigaOS 3.9/AmiKit, Vampire V4, P96 24-bit, CGX alpha",
        {1,1,12515,1,0,0,1,0,0,0,1,24,0,0,1},
        DOCK_BACKEND_SNAPSHOT, DOCK_PROFILE_PIP_NO_SHAPES, DOCK_PIXELS_AMMX, 0, 0, 0},
    /* The 68080 binary does not run here; the profile still answers. */
    {"AmigaOS 3.x, 68030 AGA, 8-bit",
        {0,0,0,0,0,0,0,0,0,0,0,8,1,0,0},
        DOCK_BACKEND_SNAPSHOT, DOCK_PROFILE_PIP_NO_AMMX, DOCK_PIXELS_PORTABLE, 0, 0, 0},
};

static void check_machine(const Machine *m)
{
    const int backends[] = {DOCK_BACKEND_OPAQUE, DOCK_BACKEND_SNAPSHOT, DOCK_BACKEND_PIP};
    int i, explicit_pip;
    for (i = 0; i < 3; ++i) {
        for (explicit_pip = 0; explicit_pip < 2; ++explicit_pip) {
            int pip = backends[i] == DOCK_BACKEND_PIP;
            DockProfile p;
            if (explicit_pip && !pip) continue;
            p = dock_choose_profile(&m->caps,backends[i],explicit_pip);
            if (p.backend != (pip ? m->pip_backend : backends[i]) ||
                p.error != (pip && explicit_pip ? m->pip_error : DOCK_PROFILE_OK) ||
                p.pixels != m->pixels || p.ui_shapes != m->ui_shapes ||
                p.ui_glass != m->ui_glass || p.aa_text != m->aa_text ||
                p.apollo_pointer != (p.backend == DOCK_BACKEND_PIP)) {
                fprintf(stderr,"%s, requested %s%s: backend %s error %d pixels %d shapes %d glass %d aa %d\n",
                    m->name,dock_backend_name(backends[i]),explicit_pip ? " (explicit)" : "",
                    dock_backend_name(p.backend),p.error,p.pixels,p.ui_shapes,p.ui_glass,p.aa_text);
                assert(0);
            }
        }
    }
}

static void test_machines(void)
{
    size_t i;
    for (i = 0; i < sizeof(machines)/sizeof(machines[0]); ++i) check_machine(&machines[i]);
}

/* The decisions select_backend and dock_ui_create made before the profile:
   PiP needed the AMMX build, window shapes and a 16-bit screen; popup_rgb
   needed shapes, graphics RGB tags and more than 8 bits; text_alpha also
   needed cybergraphics alpha. Every combination must match. */
static void test_previous_rules(void)
{
    unsigned int bits;
    const int depths[] = {1, 8, 15, 16, 24, 32};
    for (bits = 0; bits < 64; ++bits) {
        size_t d;
        for (d = 0; d < sizeof(depths)/sizeof(depths[0]); ++d) {
            DockCaps caps;
            DockProfile p;
            int pip_ready, popup_rgb;
            memset(&caps,0,sizeof(caps));
            caps.cpu_080 = caps.ammx = !!(bits&1);
            caps.saga_pip2 = caps.ammx;
            caps.saga_core = caps.ammx ? 12515 : 0;
            caps.shaped_windows = !!(bits&2);
            caps.rgb_rastport = !!(bits&4);
            caps.alpha_text = !!(bits&8);
            caps.window_action = caps.status_notify = !!(bits&16);
            caps.screen_notify = caps.apollo_console = !!(bits&32);
            caps.screen_depth = depths[d];
            pip_ready = caps.ammx && caps.shaped_windows && caps.screen_depth == 16;
            popup_rgb = caps.shaped_windows && caps.rgb_rastport && caps.screen_depth > 8;
            p = dock_choose_profile(&caps,DOCK_BACKEND_PIP,0);
            assert(p.backend == (pip_ready ? DOCK_BACKEND_PIP : DOCK_BACKEND_SNAPSHOT));
            assert(p.apollo_pointer == pip_ready);
            assert(p.ui_glass == popup_rgb);
            assert(p.aa_text == (popup_rgb && caps.alpha_text));
            assert(p.ui_shapes == caps.shaped_windows);
            p = dock_choose_profile(&caps,DOCK_BACKEND_PIP,1);
            assert((p.error == DOCK_PROFILE_OK) == pip_ready);
            assert(p.error == DOCK_PROFILE_OK || dock_profile_error_text(p.error)[0]);
        }
    }
}

static void test_names(void)
{
    assert(!strcmp(dock_backend_name(DOCK_BACKEND_PIP),"pip"));
    assert(!strcmp(dock_backend_name(DOCK_BACKEND_SNAPSHOT),"snapshot"));
    assert(!strcmp(dock_backend_name(DOCK_BACKEND_OPAQUE),"opaque"));
    assert(!dock_profile_error_text(DOCK_PROFILE_OK)[0]);
}

int main(void)
{
    test_machines();
    test_previous_rules();
    test_names();
    puts("profile tests passed");
    return 0;
}
