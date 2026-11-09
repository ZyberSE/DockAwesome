#include "dock_profile.h"
#include <stddef.h>

static int pip_missing(const DockCaps *caps)
{
    if (!caps->ammx) return DOCK_PROFILE_PIP_NO_AMMX;
    if (!caps->saga_pip2) return DOCK_PROFILE_PIP_NO_CORE;
    /* The overlay is RGB565 whatever the screen; the capture reads 16-,
       24- and 32-bit screens. Without window shapes (AmigaOS 3.x) the drop
       receiver grows only during a Workbench icon drag. */
    if (caps->screen_depth != 16 && caps->screen_depth != 24 && caps->screen_depth != 32)
        return DOCK_PROFILE_PIP_SCREEN;
    return DOCK_PROFILE_OK;
}

DockProfile dock_choose_profile(const DockCaps *caps, int requested_backend, int explicit_pip)
{
    DockProfile p = {0};
    p.backend = requested_backend == DOCK_BACKEND_PIP || requested_backend == DOCK_BACKEND_SNAPSHOT ?
        requested_backend : DOCK_BACKEND_OPAQUE;
    if (p.backend == DOCK_BACKEND_PIP) {
        int missing = pip_missing(caps);
        if (missing) {
            p.backend = DOCK_BACKEND_SNAPSHOT;
            if (explicit_pip) p.error = missing;
        }
    }
    p.pixels = caps->ammx ? DOCK_PIXELS_AMMX : DOCK_PIXELS_PORTABLE;
    p.ui_shapes = !!caps->shaped_windows;
    /* The glass draws RGB through the AROS RastPort tags into shaped
       windows; indexed screens keep the pen-drawn UI. */
    p.ui_glass = caps->shaped_windows && caps->rgb_rastport && caps->screen_depth > 8;
    p.aa_text = p.ui_glass && caps->alpha_text;
    /* ApolloOS Intuition (V50, with shapes) reports relative left-button
       events its own way; classic Intuition does not. */
    p.apollo_pointer = p.backend == DOCK_BACKEND_PIP && caps->shaped_windows;
    return p;
}

const char *dock_profile_error_text(int error)
{
    switch (error) {
    case DOCK_PROFILE_OK: return "";
    case DOCK_PROFILE_PIP_NO_AMMX: return "PiP needs a Vampire 68080 with AMMX.";
    case DOCK_PROFILE_PIP_NO_CORE: return "PiP needs Apollo core 11900 or newer.";
    case DOCK_PROFILE_PIP_SCREEN: return "PiP needs a 16-, 24- or 32-bit Workbench screen.";
    default: return "PiP is not available.";
    }
}

const char *dock_backend_name(int backend)
{
    return backend == DOCK_BACKEND_PIP ? "pip" : backend == DOCK_BACKEND_SNAPSHOT ? "snapshot" : "opaque";
}
