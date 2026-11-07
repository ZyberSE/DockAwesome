#ifndef AMIGA_FONT_AA_H
#define AMIGA_FONT_AA_H

#include "dock_core.h"

struct TextFont;
struct Library;
struct RastPort;
typedef struct AmigaFontAA AmigaFontAA;

AmigaFontAA *amiga_font_aa_create(const struct TextFont *font);
void amiga_font_aa_free(AmigaFontAA *font);
void amiga_font_aa_draw(const AmigaFontAA *font, DockImage *target,
    const char *text, int left, int top, uint32_t color);
/* Blends a grayscale template with the RastPort's foreground color. */
int amiga_font_aa_blit(AmigaFontAA *font, struct Library *cyber,
    struct RastPort *target, const char *text, int left, int top);

#endif
