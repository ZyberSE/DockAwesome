#ifndef AMIGA_PLATFORM_H
#define AMIGA_PLATFORM_H
/* Native input bridge. Ordinary input only captures buttons and signals the
   main task. A marked Shell close is dispatched on Apollo's input task. */
struct InputEvent;
struct InputEvent *dock_input_observe(struct InputEvent *events, void *context);
#endif
