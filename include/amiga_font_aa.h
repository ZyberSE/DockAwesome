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
/* Blends a ready grayscale template (width bytes per row) with the
   RastPort's foreground color. */
void amiga_alpha_template_blit(struct Library *cyber, struct RastPort *target,
    const unsigned char *alpha, int width, int height, int left, int top);
/* The inked pixels of text drawn at (0,0) with amiga_font_aa_blit or
   amiga_font_aa_draw, half-open, in pixels from the drawing origin (the
   top of the font). Returns 0 when nothing is inked. */
int amiga_font_aa_ink(const AmigaFontAA *font, const char *text,
    int *left, int *top, int *right, int *bottom);
/* Blends a grayscale template with the RastPort's foreground color. */
int amiga_font_aa_blit(AmigaFontAA *font, struct Library *cyber,
    struct RastPort *target, const char *text, int left, int top);

#endif
