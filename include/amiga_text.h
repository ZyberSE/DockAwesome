#ifndef AMIGA_TEXT_H
#define AMIGA_TEXT_H

#include <inline/macros.h>
#include <proto/graphics.h>

/* ApolloOS reads the full D0 count. Use ULONG even though the classic
   NDK wrappers for TextLength and Text declare a UWORD count. */
static inline WORD amiga_text_length(struct RastPort *port, const char *text, ULONG length)
{
    return LP3(0x36, WORD, DockTextLength32, struct RastPort *, port, a1,
        CONST_STRPTR, (CONST_STRPTR)text, a0, ULONG, length, d0, , GfxBase);
}

static inline void amiga_text_draw(struct RastPort *port, const char *text, ULONG length)
{
    (void)LP3(0x3c, LONG, DockText32, struct RastPort *, port, a1,
        CONST_STRPTR, (CONST_STRPTR)text, a0, ULONG, length, d0, , GfxBase);
}

#endif
