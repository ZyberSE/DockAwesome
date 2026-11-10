/* A keyboard reset handler: on Ctrl-Amiga-Amiga turn SAGA PiP off before
   the machine resets. SAGA keeps PiP on through a reboot, so the next
   system showed this dock's last frame (or a black box) until a dock
   started there. A reboot that skips keyboard.device's handlers is
   covered by amiga_pip_off_if_free at the next start and --pip-off. */
#include "amiga_app.h"
#include <devices/keyboard.h>
#include <exec/interrupts.h>
#include <proto/exec.h>

extern ULONG dock_reset_entry(void);

int reset_watch_open(App *app)
{
    ResetWatch *w = &app->reset;
    w->bit = AllocSignal(-1);
    if (w->bit < 0) return 0;
    w->state.task = FindTask(NULL);
    w->state.mask = 1UL<<w->bit;
    w->state.pip2 = amiga_pip_dma_register(2);
    w->state.pip1 = amiga_pip_dma_register(1);
    w->port = CreateMsgPort();
    w->io = w->port ? (struct IOStdReq *)CreateIORequest(w->port,sizeof(struct IOStdReq)) : NULL;
    if (!w->io || OpenDevice((STRPTR)"keyboard.device",0,(struct IORequest *)w->io,0)) {
        if (w->io) DeleteIORequest((struct IORequest *)w->io);
        if (w->port) DeleteMsgPort(w->port);
        w->io = NULL; w->port = NULL;
        FreeSignal(w->bit); w->bit = -1;
        return 0;
    }
    w->interrupt.is_Node.ln_Type = NT_INTERRUPT;
    w->interrupt.is_Node.ln_Pri = 16;
    w->interrupt.is_Node.ln_Name = (char *)"DockAwesome PiP off";
    w->interrupt.is_Data = &w->state;
    w->interrupt.is_Code = (void (*)())dock_reset_entry;
    w->io->io_Command = KBD_ADDRESETHANDLER;
    w->io->io_Data = &w->interrupt;
    w->added = DoIO((struct IORequest *)w->io) == 0;
    return w->added;
}

ULONG reset_watch_mask(const App *app)
{
    return app->reset.bit >= 0 && app->reset.added ? 1UL<<app->reset.bit : 0;
}

void reset_watch_done(App *app)
{
    ResetWatch *w = &app->reset;
    if (!w->added) return;
    w->io->io_Command = KBD_RESETHANDLERDONE;
    w->io->io_Data = &w->interrupt;
    DoIO((struct IORequest *)w->io);
}

void reset_watch_close(App *app)
{
    ResetWatch *w = &app->reset;
    if (w->added) {
        w->io->io_Command = KBD_REMRESETHANDLER;
        w->io->io_Data = &w->interrupt;
        DoIO((struct IORequest *)w->io);
        w->added = 0;
    }
    if (w->io) { CloseDevice((struct IORequest *)w->io); DeleteIORequest((struct IORequest *)w->io); w->io = NULL; }
    if (w->port) { DeleteMsgPort(w->port); w->port = NULL; }
    if (w->bit >= 0) { FreeSignal(w->bit); w->bit = -1; }
}
