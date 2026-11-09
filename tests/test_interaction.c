#include "dock_interaction.h"
#include "dock_geometry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

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
    assert(!dock_interaction_outside(&s, p, 3, 384, 92, gap, y, 24));
    x = p[0].x-s.padding-24;
    assert(!dock_interaction_outside(&s, p, 3, 384, 92, x, y, 24));
    assert(dock_interaction_outside(&s, p, 3, 384, 92, x-1, y, 24));
    assert(dock_interaction_outside(&s, p, 3, 384, 92, gap, -100, 24));
}

/* The plate bounds as both functions derived them before using
   dock_edge_panel_rect: outside clipped only the outer sides. */
static int reference_outside(const DockSettings *s, const DockPlacement *p, int count,
    int width, int height, int x, int y, int margin)
{
    int64_t left, right;
    int top, bottom, i;
    left = count ? (int64_t)p[0].x-s->padding : s->padding;
    right = count ? (int64_t)p[count-1].x+p[count-1].size+s->padding : width-s->padding;
    if (left < 0) left = 0;
    if (right > width) right = width;
    dock_panel_cross_bounds(s,height,&top,&bottom);
    if ((int64_t)x >= left-margin && (int64_t)x < right+margin &&
        (int64_t)y >= (int64_t)top-margin && (int64_t)y < (int64_t)bottom+margin) return 0;
    for (i = 0; i < count; ++i)
        if ((int64_t)x >= (int64_t)p[i].x-margin && (int64_t)x < (int64_t)p[i].x+p[i].size+margin &&
            (int64_t)y >= (int64_t)p[i].y-margin && (int64_t)y < (int64_t)p[i].y+p[i].size+margin) return 0;
    return 1;
}

static int reference_hover(const DockSettings *s, const DockPlacement *p, int count,
    int width, int height, int x, int y)
{
    int64_t left, right;
    int top, bottom, i;
    if (x < 0 || y < 0 || x >= width || y >= height) return 0;
    left = count ? (int64_t)p[0].x-s->padding : s->padding;
    right = count ? (int64_t)p[count-1].x+p[count-1].size+s->padding : width-s->padding;
    dock_panel_cross_bounds(s,height,&top,&bottom);
    for (i = 0; i < count; ++i) if (p[i].y < top) top = p[i].y;
    if (x >= left && x < right && y >= top && y < bottom) return 1;
    return dock_hit_test(p, count, x, y) >= 0;
}

static void check_plate(const DockSettings *s, const DockPlacement *p, int count, int width, int height)
{
    int x, y;
    for (y = -40; y < height+40; y += 3)
        for (x = -40; x < width+40; ++x) {
            assert(dock_interaction_outside(s,p,count,width,height,x,y,5) ==
                reference_outside(s,p,count,width,height,x,y,5));
            assert(dock_hover_contains(s,p,count,width,height,x,y) ==
                reference_hover(s,p,count,width,height,x,y));
        }
}

/* Rest, wave, animated and gap rows: the shared plate rectangle gives the
   same answers as the old per-function bounds. */
static void test_plate_equivalence(void)
{
    DockSettings s = dock_settings_default();
    DockPlacement p[DOCK_MAX_ITEMS], current[DOCK_MAX_ITEMS], gap;
    DockAnimation a;
    int count, mx, step, pad;
    for (pad = 0; pad <= 64; pad += 32) {
        s.padding = pad; s.end_gap = pad ? 6 : 0;
        for (count = 0; count <= 7; ++count) {
            int width = dock_viewport_length(&s,count), height = 200;
            assert(dock_layout(&s,count,width,height,0,0,0,p));
            check_plate(&s,p,count,width,height);
            if (!count) continue;
            for (mx = -20; mx < width+20; mx += 37) {
                memcpy(current,p,sizeof(p));
                assert(dock_layout(&s,count,width,height,mx,height-20,1,p));
                check_plate(&s,p,count,width,height);
                dock_animation_reset(&a);
                for (step = 0; step < 4; ++step) {
                    dock_animate_step(&s,&a,current,p,count);
                    check_plate(&s,current,count,width,height);
                }
                if (count+1 < DOCK_MAX_ITEMS &&
                    dock_edge_gap_layout(&s,DOCK_EDGE_BOTTOM,count,width,height,count/2,mx,height-20,1,current,&gap))
                    check_plate(&s,current,count,width,height);
            }
        }
    }
    /* Hover gave the same answers for any row, even one off the surface. */
    p[0] = (DockPlacement){-500,10,40,0}; p[1] = (DockPlacement){400,10,40,0};
    for (count = 1; count <= 2; ++count) {
        int x, y;
        for (y = -5; y < 125; y += 3) for (x = -5; x < 325; ++x)
            assert(dock_hover_contains(&s,p+2-count,count,320,120,x,y) ==
                reference_hover(&s,p+2-count,count,320,120,x,y));
    }
}

int main(void)
{
    test_click_and_pairing(); test_reorder_and_outside();
    test_cancellation_and_excursion(); test_global_cancel_and_invalid_down(); test_geometry();
    test_plate_equivalence();
    puts("interaction tests passed"); return 0;
}
