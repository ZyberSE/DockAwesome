#ifndef AMIGA_CAPS_H
#define AMIGA_CAPS_H
#include "dock_profile.h"

struct Screen;
struct MsgPort;

/* Reads exec AttnFlags only: callable before any library is opened. */
int amiga_caps_cpu_080(void);
/* Fill every field once at startup. Needs intuition, graphics and layers
   open; screen may be NULL, then only the screen fields stay zero.
   Returns 1. */
int amiga_caps_probe(DockCaps *caps, struct Screen *screen);
/* The fields that follow the Workbench screen; run again on reopen. */
void amiga_caps_probe_screen(DockCaps *caps, struct Screen *screen);
/* Directory Opus 5's ARexx port, or NULL. Call under Forbid. */
struct MsgPort *amiga_caps_dopus_port(void);
/* --caps: the probed table, library versions and the chosen profile. */
void amiga_caps_print(const DockCaps *caps, const DockProfile *profile, int requested_backend);

#endif
