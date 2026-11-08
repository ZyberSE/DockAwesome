#ifndef AMIGA_ABI_H
#define AMIGA_ABI_H
/* ApolloOS/AROS v50 Intuition entry points missing from the classic NDK.
   Classic Intuition has no entry at these LVOs: check lib_Version first. */
#include <inline/macros.h>
#include <intuition/intuitionbase.h>
#include <graphics/regions.h>
extern struct IntuitionBase *IntuitionBase;

static inline struct Region *amiga_change_window_shape(struct Window *window, struct Region *shape)
{
    return LP3(0x35a, struct Region *, ChangeWindowShape,
        struct Window *, window, a0, struct Region *, shape, a1,
        struct Hook *, NULL, a2, , IntuitionBase);
}
#endif
