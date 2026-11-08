#include "dock_interaction.h"
#include "dock_geometry.h"
#include <string.h>

void dock_gesture_reset(DockGesture *g)
{
    if (g) memset(g, 0, sizeof(*g));
}

uint32_t dock_gesture_pressed(const DockGesture *g)
{
    return g && g->active && !g->cancelled ? g->pin_id : 0;
}

static int valid_input(const DockGestureInput *in)
{
    return (in->flags & DOCK_INPUT_VALID) == DOCK_INPUT_VALID && !(in->flags & DOCK_INPUT_CANCELLED);
}

DockGestureEffects dock_gesture_update(DockGesture *g, const DockGestureInput *in)
{
    DockGestureEffects effect = {0, 0, -1};
    int64_t dx, dy;
    int moved;
    if (!g || !in) return effect;
    if (in->kind == DOCK_GESTURE_DOWN) {
        if (g->active || !in->token || !in->pin_id || !valid_input(in)) return effect;
        dock_gesture_reset(g);
        g->active = 1; g->token = in->token; g->pin_id = in->pin_id;
        g->target_slot = -1;
        g->down_x = g->x = in->x; g->down_y = g->y = in->y;
        effect.flags = DOCK_EFFECT_REDRAW | DOCK_EFFECT_FOCUS; effect.pin_id = g->pin_id;
        return effect;
    }
    if (!g->active || (in->kind == DOCK_GESTURE_CANCEL ?
        (in->token && in->token != g->token) : in->token != g->token)) return effect;
    effect.pin_id = g->pin_id;
    if (in->kind == DOCK_GESTURE_CANCEL || !valid_input(in)) {
        if (!g->cancelled) effect.flags |= DOCK_EFFECT_REDRAW;
        g->cancelled = 1; g->remove_armed = 0;
    }
    if (in->kind == DOCK_GESTURE_CANCEL) return effect;
    if (in->kind != DOCK_GESTURE_MOVE && in->kind != DOCK_GESTURE_UP) return effect;
    moved = in->x != g->x || in->y != g->y;
    g->x = in->x; g->y = in->y;
    dx = (int64_t)in->x-g->down_x; dy = (int64_t)in->y-g->down_y;
    if (!g->dragged && (dx > 4 || dx < -4 || dy > 4 || dy < -4 || (in->flags & DOCK_INPUT_DRAGGED))) {
        g->dragged = 1;
        effect.flags |= DOCK_EFFECT_REDRAW;
    }
    if (!g->cancelled && g->dragged) {
        if (g->remove_armed != !!in->outside || (!in->outside && moved)) effect.flags |= DOCK_EFFECT_REDRAW;
        g->remove_armed = !!in->outside;
        if (!in->outside && in->target_slot >= 0) g->target_slot = in->target_slot;
    }
    if (in->kind == DOCK_GESTURE_UP) {
        effect.flags |= DOCK_EFFECT_REDRAW;
        if (!g->cancelled) {
            if (g->dragged) {
                if (in->outside) effect.flags |= DOCK_EFFECT_REMOVE_PIN;
                else if (g->target_slot >= 0) {
                    effect.flags |= DOCK_EFFECT_REORDER;
                    effect.target_slot = g->target_slot;
                }
            } else if (in->hit_id == g->pin_id) effect.flags |= DOCK_EFFECT_LAUNCH;
        }
        dock_gesture_reset(g);
    }
    return effect;
}

static int inside_margin(int x, int y, int64_t left, int top,
    int64_t right, int64_t bottom, int margin)
{
    return (int64_t)x >= left-margin && (int64_t)x < right+margin &&
        (int64_t)y >= (int64_t)top-margin && (int64_t)y < bottom+margin;
}

int dock_interaction_outside(const DockSettings *s, const DockPlacement *p,
    int count, int width, int height, int x, int y, int margin)
{
    int64_t left, right;
    int top, bottom, i;
    if (!dock_settings_valid(s) || count < 0 || count > DOCK_MAX_ITEMS ||
        (count && !p) || width < 1 || height < 1 || margin < 0) return 0;
    left = count ? (int64_t)p[0].x-s->padding : s->padding;
    right = count ? (int64_t)p[count-1].x+p[count-1].size+s->padding : width-s->padding;
    if (left < 0) left = 0;
    if (right > width) right = width;
    dock_panel_cross_bounds(s,height,&top,&bottom);
    if (inside_margin(x, y, left, top, right, bottom, margin)) return 0;
    for (i = 0; i < count; ++i)
        if (inside_margin(x, y, p[i].x, p[i].y,
                (int64_t)p[i].x+p[i].size, (int64_t)p[i].y+p[i].size, margin)) return 0;
    return 1;
}
