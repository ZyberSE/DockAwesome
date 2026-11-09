#ifndef DOCK_PIXEL_H
#define DOCK_PIXEL_H
/* Private pixel helpers shared by dock_core.c, dock_compose.c and
   dock_renderer.c. They stay static inline so each hot loop keeps them in
   its own translation unit. */
#include "dock_core.h"
#include <string.h>

/* Bit 15 of a run length in DockCache.runs marks an opaque run. */
enum { RUN_OPAQUE = 0x8000 };

static inline uint32_t load32(const unsigned char *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
#ifdef DOCK_EMULATE_BIG_ENDIAN
    v = (v>>24)|((v>>8)&UINT32_C(0xff00))|((v<<8)&UINT32_C(0xff0000))|(v<<24);
#endif
    return v;
}

static inline void store32(unsigned char *p, uint32_t v)
{
#ifdef DOCK_EMULATE_BIG_ENDIAN
    v = (v>>24)|((v>>8)&UINT32_C(0xff00))|((v<<8)&UINT32_C(0xff0000))|(v<<24);
#endif
    memcpy(p, &v, 4);
}

/* The image a placement draws, or NULL when its level has none. */
static inline const DockImage *level_image(const DockCache *icon, int level)
{
    return icon && level >= 0 && level < DOCK_MAX_LEVELS && level < icon->levels ? &icon->image[level] : NULL;
}
#endif
