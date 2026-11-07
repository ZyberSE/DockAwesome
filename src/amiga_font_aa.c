#include "amiga_font_aa.h"
#include <graphics/text.h>
#include <inline/macros.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t offset;
    int width, bearing, advance;
} Glyph;

struct AmigaFontAA {
    int first, last, height;
    Glyph glyphs[257];
    unsigned char *template;
    size_t template_size;
    unsigned char coverage[];
};

static int ink(const struct TextFont *font, int start, int width, int x, int y)
{
    const unsigned char *bitmap = font->tf_CharData;
    if (x < 0 || x >= width || y < 0 || y >= font->tf_YSize) return 0;
    return !!(bitmap[(size_t)y*font->tf_Modulo+(start+x)/8] & (128>>((start+x)%8)));
}

AmigaFontAA *amiga_font_aa_create(const struct TextFont *font)
{
    const UWORD *locations = font->tf_CharLoc;
    const WORD *spacing = font->tf_CharSpace, *kern = font->tf_CharKern;
    int count = font->tf_HiChar-font->tf_LoChar+2, i;
    size_t size = 0;
    AmigaFontAA *aa;
    if (!font->tf_CharData || !locations || (font->tf_Style&FSF_COLORFONT) ||
        (font->tf_Flags&FPF_REVPATH) || count < 2 ||
        !font->tf_YSize || font->tf_YSize > 32) return NULL;
    for (i = 0; i < count; ++i) {
        int start = locations[i*2], width = locations[i*2+1];
        if (width > 64 || start+width > font->tf_Modulo*8) return NULL;
        size += width*font->tf_YSize;
    }
    aa = malloc(sizeof(*aa)+size);
    if (!aa) return NULL;
    aa->first = font->tf_LoChar;
    aa->last = font->tf_HiChar;
    aa->height = font->tf_YSize;
    aa->template = NULL;
    aa->template_size = 0;
    size = 0;
    for (i = 0; i < count; ++i) {
        int start = locations[i*2], width = locations[i*2+1], x, y;
        int bearing = kern ? kern[i] : 0, spacing_x = spacing ? spacing[i] : font->tf_XSize;
        aa->glyphs[i] = (Glyph){size,width,bearing,bearing+spacing_x};
        for (y = 0; y < aa->height; ++y) for (x = 0; x < width; ++x) {
            int solid = ink(font,start,width,x,y);
            int up = ink(font,start,width,x,y-1), down = ink(font,start,width,x,y+1);
            int left = ink(font,start,width,x-1,y), right = ink(font,start,width,x+1,y);
            /* When both pairs differ, a diagonal crosses one quarter of
               this pixel. Straight stems and isolated dots retain full ink. */
            aa->coverage[size++] = up != down && left != right ?
                (solid ? 191 : 64) : (solid ? 255 : 0);
        }
    }
    return aa;
}

void amiga_font_aa_free(AmigaFontAA *font)
{
    if (!font) return;
    free(font->template);
    free(font);
}

static const Glyph *glyph_for(const AmigaFontAA *font, unsigned char c)
{
    int index = c < font->first || c > font->last ? font->last-font->first+1 : c-font->first;
    return &font->glyphs[index];
}

void amiga_font_aa_draw(const AmigaFontAA *font, DockImage *target,
    const char *text, int left, int top, uint32_t color)
{
    const unsigned char *c = (const unsigned char *)text;
    unsigned int red = (color>>16)&255, green = (color>>8)&255, blue = color&255;
    int first_row = top < 0 ? -top : 0;
    int end_row = target->height-top;
    if (end_row > font->height) end_row = font->height;
    for (; *c; ++c) {
        const Glyph *glyph = glyph_for(font,*c);
        int gx = left+glyph->bearing, y, x;
        int first = gx < 0 ? -gx : 0, end = target->width-gx;
        if (end > glyph->width) end = glyph->width;
        left += glyph->advance;
        if (first >= end) continue;
        for (y = first_row; y < end_row; ++y) {
            const unsigned char *mask = font->coverage+glyph->offset+y*glyph->width+first;
            unsigned char *dst = target->pixels+((size_t)(top+y)*target->width+gx+first)*4;
            for (x = first; x < end; ++x, ++mask, dst += 4) {
                unsigned int alpha = *mask, remaining = 255-alpha;
                if (!alpha) continue;
                if (alpha == 255) {
                    dst[0] = red;
                    dst[1] = green;
                    dst[2] = blue;
                    dst[3] = 255;
                    continue;
                }
                dst[0] = (red*alpha+dst[0]*remaining+127)/255;
                dst[1] = (green*alpha+dst[1]*remaining+127)/255;
                dst[2] = (blue*alpha+dst[2]*remaining+127)/255;
                dst[3] = alpha+(dst[3]*remaining+127)/255;
            }
        }
    }
}

int amiga_font_aa_blit(AmigaFontAA *font, struct Library *cyber,
    struct RastPort *target, const char *text, int left, int top)
{
    const unsigned char *start = (const unsigned char *)text, *c;
    int pen = 0, lo = 0, hi = 0, width;
    size_t bytes;
    if (!font || !cyber) return 0;
    for (c = start; *c; ++c) {
        const Glyph *glyph = glyph_for(font,*c);
        int x = pen+glyph->bearing;
        if (x < lo) lo = x;
        if (x+glyph->width > hi) hi = x+glyph->width;
        pen += glyph->advance;
    }
    width = hi-lo;
    if (!width) return 1;
    if (width > 32767) return 0;
    bytes = (size_t)width*font->height;
    if (bytes > font->template_size) {
        unsigned char *buffer = realloc(font->template,bytes);
        if (!buffer) return 0;
        font->template = buffer;
        font->template_size = bytes;
    }
    memset(font->template,0,bytes);
    pen = -lo;
    for (c = start; *c; ++c) {
        const Glyph *glyph = glyph_for(font,*c);
        const unsigned char *mask = font->coverage+glyph->offset;
        int x, y, start_x = pen+glyph->bearing;
        for (y = 0; y < font->height; ++y) {
            unsigned char *row = font->template+(size_t)y*width+start_x;
            for (x = 0; x < glyph->width; ++x, ++mask)
                if (*mask > row[x]) row[x] = *mask;
        }
        pen += glyph->advance;
    }
    /* ApolloOS cybergraphics.library V51 BltTemplateAlpha. */
    LP8NR(0xde, DockBltTemplateAlpha, APTR, font->template, a0,
        LONG, 0, d0, LONG, width, d1, struct RastPort *, target, a1,
        LONG, left+lo, d2, LONG, top, d3, LONG, width, d4, LONG, font->height, d5, , cyber);
    return 1;
}
