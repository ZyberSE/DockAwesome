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

static inline void amiga_window_action(struct Window *window, ULONG action, struct TagItem *tags)
{
    LP3NR(0x3ae, DockWindowAction, struct Window *, window, a0,
        ULONG, action, d0, struct TagItem *, tags, a1, , IntuitionBase);
}

static inline APTR amiga_start_status_notify(struct TagItem *tags)
{
    return LP1(0x39c, APTR, StartDockStatusNotify, struct TagItem *, tags, a0, , IntuitionBase);
}

static inline BOOL amiga_end_status_notify(APTR notify)
{
    return LP1(0x3a2, BOOL, EndDockStatusNotify, APTR, notify, a0, , IntuitionBase);
}
#endif
