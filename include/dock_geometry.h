#ifndef DOCK_GEOMETRY_H
#define DOCK_GEOMETRY_H
#include "dock_core.h"

typedef enum {
    DOCK_EDGE_BOTTOM = 0,
    DOCK_EDGE_LEFT = 1,
    DOCK_EDGE_RIGHT = 2
} DockEdge;

/* Half-open pixel bounds: [left,right) by [top,bottom). */
typedef struct { int left, top, right, bottom; } DockRect;

enum { DOCK_DESKTOP_GAP = 3 };

int dock_panel_thickness(const DockSettings *settings);
void dock_panel_cross_bounds(const DockSettings *settings, int extent, int *inner, int *outer);

/* Preferred long axis, including room for the normal magnification wave.
   Bound this by the available screen length before allocating a surface. */
int dock_viewport_length(const DockSettings *settings, int count);

/* Side layouts reuse bottom geometry with the axes swapped. Icons stay upright. */
int dock_edge_layout(const DockSettings *settings, DockEdge edge, int count,
    int width, int height, int mouse_x, int mouse_y, int active,
    DockPlacement *out);
/* Animation state always contains virtual longitudinal centers. Reset it
   when changing edges or rebuilding the surface. No dimensions are needed:
   the left reflection's constant width cancels from baseline animation. */
int dock_edge_animate(const DockSettings *settings, DockEdge edge,
    DockAnimation *animation, DockPlacement *current,
    const DockPlacement *target, int count);
int dock_edge_hover(const DockSettings *settings, DockEdge edge,
    const DockPlacement *placement, int count, int width, int height,
    int x, int y);
/* External insertion uses stable resting centers, independent of the
   current animation. Returns 0..count, or -1 outside the drop band. */
int dock_edge_insertion_slot(const DockSettings *settings, DockEdge edge,
    int count, int width, int height, int x, int y);
int dock_edge_insertion_layout(const DockSettings *settings, DockEdge edge,
    int count, int width, int height, int slot, DockPlacement *out);
/* Lay out the complete row, then return the neighbors and the vacant slot
   separately. Coordinates are on the actual surface, as for normal hover. */
int dock_edge_gap_layout(const DockSettings *settings, DockEdge edge,
    int count, int width, int height, int slot, int x, int y, int active,
    DockPlacement *out, DockPlacement *gap);
int dock_edge_outside(const DockSettings *settings, DockEdge edge,
    const DockPlacement *placement, int count, int width, int height,
    int x, int y, int margin);
/* Uses the existing bottom shelf thickness and padding, transformed as
   rectangle edges, then clipped to the actual surface. */
int dock_edge_panel_rect(const DockSettings *settings, DockEdge edge,
    const DockPlacement *placement, int count, int width, int height,
    DockRect *out);
#endif
