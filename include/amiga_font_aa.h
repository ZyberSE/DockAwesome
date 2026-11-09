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
/* Blends a color over an RGBA pixel by coverage alpha (1-255), keeping
   the destination's own alpha under it. */
static inline void amiga_blend_coverage(unsigned char *dst, unsigned int red,
    unsigned int green, unsigned int blue, unsigned int alpha)
{
    unsigned int remaining = 255-alpha;
    dst[0] = (red*alpha+dst[0]*remaining+127)/255;
    dst[1] = (green*alpha+dst[1]*remaining+127)/255;
    dst[2] = (blue*alpha+dst[2]*remaining+127)/255;
    dst[3] = alpha+(dst[3]*remaining+127)/255;
}
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
