#ifndef AMIGA_PIP_H
#define AMIGA_PIP_H
#include <exec/types.h>
#include <stddef.h>
struct Screen;
typedef struct AmigaPip AmigaPip;
enum { AMIGA_PIP_MAX_WIDTH = 2048, AMIGA_PIP_MAX_HEIGHT = 1200 };
/* PiP2 uses a lease shared by cooperating apps. Registers are write-only.
   Use big-endian RGB565, 32-pixel width steps, 64-byte row alignment, and zero
   modulo. Wait for VBlank before reusing a buffer. Flips written at once can
   tear; amiga_pip_flip_at_vblank moves them into the vertical blank.
   The right_padding columns may extend past the screen and must stay clear. */
AmigaPip *amiga_pip_open(struct Screen *screen, int x, int y, int width, int height, int right_padding,
                       char *error, size_t error_capacity);
/* unit 1 or 2: SAGA has two PiP overlays, each leased on its own. */
AmigaPip *amiga_pip_open_unit(struct Screen *screen, int unit, int x, int y, int width, int height,
                       int right_padding, char *error, size_t error_capacity);
/* Place the overlay at a new screen position (whole overlay on screen). */
int amiga_pip_move(AmigaPip *pip, int x, int y);
void amiga_pip_close(AmigaPip *pip);
void amiga_pip_suspend(AmigaPip *pip);
/* Apply offsets from -128 to 128 pixels. Check alignment on the display. */
int amiga_pip_set_alignment(AmigaPip *pip, int x_offset, int y_offset);
/* Enable the 0xF81F key on the next frame. Escape opaque magenta first. */
int amiga_pip_set_keying(AmigaPip *pip, int enable);
/* Flip buffers in the vertical blank: present leaves the new front's
   address in *buffer_slot for a VBlank server, which writes it to the
   address in *register_slot and clears the slot. A pointer written at once
   tore a tall surface (a dock on the left or right edge) across the field.
   NULL slots write at once again. */
void amiga_pip_flip_at_vblank(AmigaPip *pip, volatile ULONG *register_slot, volatile ULONG *buffer_slot);
/* Buffer 0 or 1, to fill both before the first present. */
unsigned char *amiga_pip_buffer(AmigaPip *pip, int index);
unsigned char *amiga_pip_back_buffer(AmigaPip *pip);
/* 0 or 1: which buffer amiga_pip_back_buffer returns. A failed or suspended
   present keeps it, so it then holds the newest conversion. */
int amiga_pip_back_index(const AmigaPip *pip);
int amiga_pip_stride(const AmigaPip *pip);
int amiga_pip_present(AmigaPip *pip);
unsigned int amiga_pip_core_revision(const AmigaPip *pip);
#endif
