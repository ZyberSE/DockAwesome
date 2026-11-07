#ifndef DOCK_INTERACTION_H
#define DOCK_INTERACTION_H
#include "dock_core.h"

enum { DOCK_GESTURE_DOWN, DOCK_GESTURE_MOVE, DOCK_GESTURE_UP, DOCK_GESTURE_CANCEL };
enum {
    DOCK_INPUT_COORDS_VALID = 1,
    DOCK_INPUT_SAME_SCREEN = 2,
    DOCK_INPUT_CANCELLED = 4,
    DOCK_INPUT_DRAGGED = 8,
    DOCK_INPUT_VALID = DOCK_INPUT_COORDS_VALID | DOCK_INPUT_SAME_SCREEN
};
enum {
    DOCK_EFFECT_REDRAW = 1,
    DOCK_EFFECT_FOCUS = 2,
    DOCK_EFFECT_LAUNCH = 4,
    DOCK_EFFECT_REORDER = 8,
    DOCK_EFFECT_REMOVE_PIN = 16
};
typedef struct {
    int kind, x, y, target_slot, outside;
    unsigned int flags;
    uint32_t token, pin_id, hit_id;
} DockGestureInput;
typedef struct {
    uint32_t token, pin_id;
    int active, dragged, cancelled, remove_armed, target_slot;
    int down_x, down_y, x, y;
} DockGesture;
typedef struct {
    unsigned int flags;
    uint32_t pin_id;
    int target_slot;
} DockGestureEffects;

void dock_gesture_reset(DockGesture *gesture);
/* Pure state machine: effects never access files, caches, windows, or the OS.
   Pin IDs survive reordering. Tokens pair presses and releases. Canceled
   gestures remain inert until their matching UP. A stale UP cannot end another. */
DockGestureEffects dock_gesture_update(DockGesture *gesture, const DockGestureInput *input);
uint32_t dock_gesture_pressed(const DockGesture *gesture);
/* Input-only geometry. Rendering/layout are untouched. The continuous hover
   band supplies nearest-slot reorder targets, including gaps between icons. */
int dock_interaction_slot(const DockSettings *settings, const DockPlacement *placement,
    int count, int width, int height, int x, int y);
/* True only beyond the panel AND every rendered icon box, with margin pixels
   of tolerance around each. Coordinates may lie outside the native window. */
int dock_interaction_outside(const DockSettings *settings, const DockPlacement *placement,
    int count, int width, int height, int x, int y, int margin);
#endif
