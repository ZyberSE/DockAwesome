#include "dock_interaction.h"
#include <assert.h>
#include <stdio.h>

static DockGestureInput input(int kind, uint32_t token, uint32_t pin, uint32_t hit, int x, int y)
{
    DockGestureInput in = {0};
    in.kind = kind; in.token = token; in.pin_id = pin; in.hit_id = hit;
    in.x = x; in.y = y; in.target_slot = -1; in.flags = DOCK_INPUT_VALID;
    return in;
}

static void test_click_and_pairing(void)
{
    DockGesture g = {0}; DockGestureEffects e; DockGestureInput in;
    in = input(DOCK_GESTURE_DOWN, 1, 42, 42, 20, 30); e = dock_gesture_update(&g, &in);
    assert((e.flags & (DOCK_EFFECT_REDRAW|DOCK_EFFECT_FOCUS)) == (DOCK_EFFECT_REDRAW|DOCK_EFFECT_FOCUS));
    assert(dock_gesture_pressed(&g) == 42);
    in = input(DOCK_GESTURE_UP, 99, 0, 42, 20, 30); e = dock_gesture_update(&g, &in);
    assert(!e.flags && g.active);
    in.token = 0; assert(!dock_gesture_update(&g, &in).flags && g.active);
    in.token = 1; e = dock_gesture_update(&g, &in);
    assert((e.flags & DOCK_EFFECT_LAUNCH) && !(e.flags & DOCK_EFFECT_REMOVE_PIN));
    assert(!g.active && !dock_gesture_pressed(&g));
    assert(!dock_gesture_update(&g, &in).flags);
}

static void test_reorder_and_outside(void)
{
    DockGesture g = {0}; DockGestureInput in; DockGestureEffects e;
    uint32_t pins[] = {42, 77, 99};
    in = input(DOCK_GESTURE_DOWN, 2, 42, 42, 20, 30); dock_gesture_update(&g, &in);
    in = input(DOCK_GESTURE_MOVE, 2, 0, 77, 90, 30); in.target_slot = 2;
    e = dock_gesture_update(&g, &in);
    assert((e.flags & DOCK_EFFECT_REDRAW) && !(e.flags & DOCK_EFFECT_REORDER));
    assert(g.target_slot == 2 && pins[0] == 42 && dock_gesture_pressed(&g) == 42);
    in = input(DOCK_GESTURE_UP, 2, 0, 42, 90, 30); e = dock_gesture_update(&g, &in);
    assert((e.flags & DOCK_EFFECT_REORDER) && e.pin_id == 42 && e.target_slot == 2);
    assert(dock_reorder(pins, sizeof(*pins), 3, 0, e.target_slot));
    assert(pins[2] == e.pin_id && pins[0] == 77 && pins[1] == 99);
    assert(!(e.flags & (DOCK_EFFECT_LAUNCH|DOCK_EFFECT_REMOVE_PIN)) && !g.active);
    in = input(DOCK_GESTURE_DOWN, 3, 42, 42, 20, 30); dock_gesture_update(&g, &in);
    in = input(DOCK_GESTURE_UP, 3, 0, 0, 20, -70); in.outside = 1;
    e = dock_gesture_update(&g, &in);
    assert((e.flags & DOCK_EFFECT_REMOVE_PIN) && e.pin_id == 42 && !(e.flags & DOCK_EFFECT_LAUNCH));
}

static void test_global_cancel_and_invalid_down(void)
{
    DockGesture g = {0}; DockGestureInput in; DockGestureEffects e;
    in = input(DOCK_GESTURE_DOWN, 9, 42, 42, 20, 30); in.flags = DOCK_INPUT_COORDS_VALID;
    assert(!dock_gesture_update(&g, &in).flags && !g.active);
    in.flags = DOCK_INPUT_VALID; dock_gesture_update(&g, &in);
    in = input(DOCK_GESTURE_DOWN, 10, 99, 99, 20, 30);
    assert(!dock_gesture_update(&g, &in).flags && g.pin_id == 42 && g.token == 9);
    in = input(DOCK_GESTURE_MOVE, 9, 0, 0, 90, 30); in.target_slot = 2;
    assert(!(dock_gesture_update(&g, &in).flags & DOCK_EFFECT_REORDER) && g.target_slot == 2);
    in = input(DOCK_GESTURE_CANCEL, 99, 0, 0, 0, 0);
    assert(!dock_gesture_update(&g, &in).flags && !g.cancelled);
    in.token = 0; e = dock_gesture_update(&g, &in);
    assert((e.flags & DOCK_EFFECT_REDRAW) && g.cancelled && !dock_gesture_pressed(&g));
    in = input(DOCK_GESTURE_UP, 9, 0, 42, 20, -100); in.outside = 1;
    e = dock_gesture_update(&g, &in);
    assert(!(e.flags & (DOCK_EFFECT_LAUNCH|DOCK_EFFECT_REORDER|DOCK_EFFECT_REMOVE_PIN)) && !g.active);
}

static void test_cancellation_and_excursion(void)
{
    DockGesture g; DockGestureInput in; DockGestureEffects e;
    unsigned int flags[] = {DOCK_INPUT_COORDS_VALID, DOCK_INPUT_SAME_SCREEN,
        DOCK_INPUT_VALID|DOCK_INPUT_CANCELLED};
    unsigned int i;
    for (i = 0; i < sizeof(flags)/sizeof(flags[0]); ++i) {
        dock_gesture_reset(&g);
        in = input(DOCK_GESTURE_DOWN, 5, 42, 42, 20, 30); dock_gesture_update(&g, &in);
        in = input(DOCK_GESTURE_MOVE, 5, 0, 0, 20, -80); in.outside = 1; in.flags = flags[i];
        dock_gesture_update(&g, &in);
        assert(!dock_gesture_pressed(&g));
        in = input(DOCK_GESTURE_UP, 5, 0, 42, 20, 30); in.outside = 1;
        e = dock_gesture_update(&g, &in);
        assert(!(e.flags & (DOCK_EFFECT_LAUNCH|DOCK_EFFECT_REORDER|DOCK_EFFECT_REMOVE_PIN)) && !g.active);
    }
    dock_gesture_reset(&g);
    in = input(DOCK_GESTURE_DOWN, 6, 42, 42, 20, 30); dock_gesture_update(&g, &in);
    in = input(DOCK_GESTURE_UP, 6, 0, 42, 20, 30); in.flags |= DOCK_INPUT_DRAGGED;
    e = dock_gesture_update(&g, &in);
    assert(!(e.flags & DOCK_EFFECT_LAUNCH)); /* Away and back between consumer wakes. */
    in = input(DOCK_GESTURE_DOWN, 7, 42, 42, 20, 30); dock_gesture_update(&g, &in);
    in = input(DOCK_GESTURE_MOVE, 7, 0, 0, 20, -80); in.outside = 1; dock_gesture_update(&g, &in);
    assert(g.remove_armed);
    in = input(DOCK_GESTURE_UP, 7, 0, 42, 20, 30); e = dock_gesture_update(&g, &in);
    assert(!(e.flags & (DOCK_EFFECT_LAUNCH|DOCK_EFFECT_REMOVE_PIN)));
}

static void test_geometry(void)
{
    DockSettings s = dock_settings_default(); DockPlacement p[3];
    int gap, x, y;
    assert(dock_layout(&s, 3, 384, 92, 0, 0, 0, p));
    gap = p[0].x+p[0].size+s.gap/2; y = 92-s.padding-s.base_size/2;
    assert(dock_interaction_slot(&s, p, 3, 384, 92, gap, y) >= 0);
    assert(!dock_interaction_outside(&s, p, 3, 384, 92, gap, y, 24));
    x = p[0].x-s.padding-24;
    assert(!dock_interaction_outside(&s, p, 3, 384, 92, x, y, 24));
    assert(dock_interaction_outside(&s, p, 3, 384, 92, x-1, y, 24));
    assert(dock_interaction_outside(&s, p, 3, 384, 92, gap, -100, 24));
    assert(dock_interaction_slot(&s, p, 3, 384, 92, gap, -100) == -1);
}

int main(void)
{
    test_click_and_pairing(); test_reorder_and_outside();
    test_cancellation_and_excursion(); test_global_cancel_and_invalid_down(); test_geometry();
    puts("interaction tests passed"); return 0;
}
