#include "dock_geometry.h"
#include "dock_interaction.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

enum { LONG = 640, CROSS = 96, N = 9 };

static void actual_point(DockEdge edge, int vx, int vy, int *x, int *y)
{
    if (edge == DOCK_EDGE_BOTTOM) { *x = vx; *y = vy; }
    else {
        *x = edge == DOCK_EDGE_LEFT ? CROSS-1-vy : vy;
        *y = vx;
    }
}

static void actual_boxes(DockEdge edge, const DockPlacement *v, DockPlacement *p, int count)
{
    int i;
    for (i = 0; i < count; ++i) {
        p[i] = v[i];
        if (edge != DOCK_EDGE_BOTTOM) {
            p[i].x = edge == DOCK_EDGE_LEFT ? CROSS-v[i].y-v[i].size : v[i].y;
            p[i].y = v[i].x;
        }
    }
}

static void dimensions(DockEdge edge, int *w, int *h)
{
    *w = edge == DOCK_EDGE_BOTTOM ? LONG : CROSS;
    *h = edge == DOCK_EDGE_BOTTOM ? CROSS : LONG;
}

static void test_layout_and_baseline(void)
{
    DockSettings s = dock_settings_default();
    DockPlacement rest[N], zoom[N], expected[N], p[N];
    DockRect panel;
    int edge, i, w, h, x, y, focus, baseline;
    assert(dock_layout(&s,N,LONG,CROSS,0,0,0,rest));
    focus = rest[4].x+s.base_size/2;
    assert(dock_layout(&s,N,LONG,CROSS,focus,70,1,zoom));
    assert(zoom[4].size == s.max_size);
    baseline = rest[0].y+rest[0].size;
    for (edge = DOCK_EDGE_BOTTOM; edge <= DOCK_EDGE_RIGHT; ++edge) {
        dimensions((DockEdge)edge,&w,&h);
        actual_point((DockEdge)edge,focus,70,&x,&y);
        assert(dock_edge_layout(&s,(DockEdge)edge,N,w,h,x,y,0,p));
        actual_boxes((DockEdge)edge,rest,expected,N);
        assert(!memcmp(p,expected,sizeof(p)));
        assert(dock_edge_panel_rect(&s,(DockEdge)edge,p,N,w,h,&panel));
        if (edge == DOCK_EDGE_BOTTOM) assert(2*p[0].y+p[0].size == panel.top+panel.bottom);
        else assert(2*p[0].x+p[0].size == panel.left+panel.right);
        assert(dock_edge_layout(&s,(DockEdge)edge,N,w,h,x,y,1,p));
        actual_boxes((DockEdge)edge,zoom,expected,N);
        assert(!memcmp(p,expected,sizeof(p)));
        for (i = 0; i < N; ++i) {
            assert(p[i].size == zoom[i].size && p[i].level == zoom[i].level);
            if (edge == DOCK_EDGE_LEFT) assert(p[i].x == CROSS-baseline);
            else if (edge == DOCK_EDGE_RIGHT) assert(p[i].x+p[i].size == baseline);
            else assert(p[i].y+p[i].size == baseline);
            assert(dock_hit_test(p,N,p[i].x+p[i].size/2,p[i].y+p[i].size/2) == i);
        }
    }
}

static void test_interaction(void)
{
    DockSettings s = dock_settings_default();
    DockPlacement v[N], p[N];
    int edge, vx, vy, x, y, w, h;
    assert(dock_layout(&s,N,LONG,CROSS,350,70,1,v));
    for (edge = DOCK_EDGE_BOTTOM; edge <= DOCK_EDGE_RIGHT; ++edge) {
        dimensions((DockEdge)edge,&w,&h);
        actual_boxes((DockEdge)edge,v,p,N);
        for (vx = -7; vx < LONG+8; vx += 11) for (vy = -7; vy < CROSS+8; vy += 7) {
            actual_point((DockEdge)edge,vx,vy,&x,&y);
            assert(dock_edge_hover(&s,(DockEdge)edge,p,N,w,h,x,y) ==
                dock_hover_contains(&s,v,N,LONG,CROSS,vx,vy));
            assert(dock_edge_outside(&s,(DockEdge)edge,p,N,w,h,x,y,5) ==
                dock_interaction_outside(&s,v,N,LONG,CROSS,vx,vy,5));
        }
        assert(dock_edge_outside(&s,(DockEdge)edge,p,N,w,h,INT_MIN,INT_MAX,0));
        assert(!dock_edge_hover(&s,(DockEdge)edge,p,N,w,h,INT_MIN,INT_MAX));
    }
}

static void test_panel_rect(void)
{
    DockSettings s = dock_settings_default();
    DockPlacement v[N], p[N];
    DockRect bottom, r;
    int edge, w, h;
    assert(dock_layout(&s,N,LONG,CROSS,350,70,1,v));
    assert(dock_edge_panel_rect(&s,DOCK_EDGE_BOTTOM,v,N,LONG,CROSS,&bottom));
    assert(bottom.left == v[0].x-s.padding);
    assert(bottom.right == v[N-1].x+v[N-1].size+s.padding);
    assert(bottom.bottom-bottom.top == 46);
    assert(CROSS-bottom.bottom == 3);
    for (edge = DOCK_EDGE_LEFT; edge <= DOCK_EDGE_RIGHT; ++edge) {
        dimensions((DockEdge)edge,&w,&h);
        actual_boxes((DockEdge)edge,v,p,N);
        assert(dock_edge_panel_rect(&s,(DockEdge)edge,p,N,w,h,&r));
        assert(r.top == bottom.left && r.bottom == bottom.right);
        assert(r.left == (edge == DOCK_EDGE_LEFT ? CROSS-bottom.bottom : bottom.top));
        assert(r.right == (edge == DOCK_EDGE_LEFT ? CROSS-bottom.top : bottom.bottom));
        assert(r.left >= 0 && r.right <= w && r.top >= 0 && r.bottom <= h);
        assert((edge == DOCK_EDGE_LEFT ? r.left : w-r.right) == 3);
        {
            int y = (r.top+r.bottom)/2;
            int outer = edge == DOCK_EDGE_LEFT ? r.left : r.right-1;
            int beyond = edge == DOCK_EDGE_LEFT ? r.left-1 : r.right;
            assert(dock_edge_hover(&s,(DockEdge)edge,p,N,w,h,outer,y));
            assert(!dock_edge_hover(&s,(DockEdge)edge,p,N,w,h,beyond,y));
        }
    }
    assert(dock_edge_panel_rect(&s,DOCK_EDGE_BOTTOM,NULL,0,1,1,&r));
    assert(r.left >= 0 && r.left <= r.right && r.right <= 1);
    assert(r.top >= 0 && r.top <= r.bottom && r.bottom <= 1);
    s.edge_padding = 5;
    assert(dock_edge_layout(&s,DOCK_EDGE_RIGHT,N,CROSS,LONG,0,0,0,p));
    assert(dock_edge_panel_rect(&s,DOCK_EDGE_RIGHT,p,N,CROSS,LONG,&r));
    assert(CROSS-r.right == 8);
    assert(CROSS-s.edge_padding-r.right == 3);
}

static void test_animation(void)
{
    DockSettings s = dock_settings_default();
    DockPlacement initial[N], reference[N], target[N], current[N], actual_target[N], expected[N];
    DockAnimation core, oriented;
    int edge, tick, w, h;
    assert(dock_layout(&s,N,LONG,CROSS,0,0,0,initial));
    for (edge = DOCK_EDGE_BOTTOM; edge <= DOCK_EDGE_RIGHT; ++edge) {
        dimensions((DockEdge)edge,&w,&h);
        memcpy(reference,initial,sizeof(initial));
        actual_boxes((DockEdge)edge,initial,current,N);
        dock_animation_reset(&core); dock_animation_reset(&oriented);
        for (tick = 0; tick < 140; ++tick) {
            int active = tick < 7 || (tick >= 11 && tick < 90);
            int focus = tick < 30 ? 350 : tick < 60 ? 230 : 450;
            int changed;
            assert(dock_layout(&s,N,LONG,CROSS,focus,70,active,target));
            actual_boxes((DockEdge)edge,target,actual_target,N);
            changed = dock_animate_step(&s,&core,reference,target,N);
            assert(dock_edge_animate(&s,(DockEdge)edge,&oriented,current,actual_target,N) == changed);
            actual_boxes((DockEdge)edge,reference,expected,N);
            assert(!memcmp(current,expected,sizeof(current)));
            assert(!memcmp(core.center,oriented.center,N*sizeof(*core.center)));
            if (edge == DOCK_EDGE_BOTTOM) assert(!memcmp(&core,&oriented,sizeof(core)));
        }
        assert(!memcmp(current,actual_target,sizeof(current)));
        assert(!dock_edge_animate(&s,(DockEdge)edge,&oriented,current,actual_target,N));
    }
}

static void test_counts_and_invalid(void)
{
    DockSettings s = dock_settings_default();
    struct { unsigned int before; DockPlacement p[DOCK_MAX_ITEMS]; unsigned int after; } guarded;
    DockPlacement target[DOCK_MAX_ITEMS];
    DockAnimation animation;
    DockRect r;
    int edge, count;
    guarded.before = 0x12345678U; guarded.after = 0x87654321U;
    for (edge = DOCK_EDGE_BOTTOM; edge <= DOCK_EDGE_RIGHT; ++edge) {
        for (count = 1; count <= DOCK_MAX_ITEMS; count += count == 1 ? 31 : 1) {
            int w = edge == DOCK_EDGE_BOTTOM ? 1400 : CROSS;
            int h = edge == DOCK_EDGE_BOTTOM ? CROSS : 1400;
            int step;
            assert(dock_edge_layout(&s,(DockEdge)edge,count,w,h,w/2,h/2,0,guarded.p));
            assert(dock_edge_layout(&s,(DockEdge)edge,count,w,h,w/2,h/2,1,target));
            dock_animation_reset(&animation);
            for (step = 0; step < 64 && memcmp(guarded.p,target,(size_t)count*sizeof(*target)); ++step)
                dock_edge_animate(&s,(DockEdge)edge,&animation,guarded.p,target,count);
            assert(!memcmp(guarded.p,target,(size_t)count*sizeof(*target)));
            assert(dock_edge_panel_rect(&s,(DockEdge)edge,guarded.p,count,w,h,&r));
        }
        assert(dock_edge_layout(&s,(DockEdge)edge,0,LONG,LONG,0,0,0,guarded.p));
    }
    assert(!dock_edge_layout(&s,(DockEdge)99,1,LONG,CROSS,20,20,1,guarded.p));
    assert(!dock_edge_layout(&s,DOCK_EDGE_LEFT,DOCK_MAX_ITEMS+1,CROSS,LONG,20,20,1,guarded.p));
    assert(!dock_edge_panel_rect(&s,(DockEdge)99,NULL,0,LONG,CROSS,&r));
    assert(guarded.before == 0x12345678U && guarded.after == 0x87654321U);
    s.max_size = s.base_size;
    assert(dock_edge_layout(&s,DOCK_EDGE_LEFT,1,CROSS,LONG,20,320,1,guarded.p));
    assert(dock_edge_panel_rect(&s,DOCK_EDGE_LEFT,guarded.p,1,CROSS,LONG,&r));
    assert(guarded.p[0].size == s.base_size);
    assert(2*guarded.p[0].x+guarded.p[0].size == r.left+r.right);
}

static void test_external_insertion(void)
{
    DockSettings s = dock_settings_default();
    DockPlacement rest[N], expanded[N+1], current[N], target[N];
    DockAnimation animation;
    int edge, w, h, slot, i, x, y, tick;
    for (edge = DOCK_EDGE_BOTTOM; edge <= DOCK_EDGE_RIGHT; ++edge) {
        dimensions((DockEdge)edge,&w,&h);
        assert(dock_edge_layout(&s,(DockEdge)edge,N,w,h,0,0,0,rest));
        assert(dock_edge_layout(&s,(DockEdge)edge,N+1,w,h,0,0,0,expanded));
        memcpy(current,rest,sizeof(current));
        dock_animation_reset(&animation);
        for (slot = 0; slot <= N; ++slot) {
            int center = (edge == DOCK_EDGE_BOTTOM ? expanded[slot].x : expanded[slot].y)+s.base_size/2;
            actual_point((DockEdge)edge,center,CROSS-s.padding-s.base_size/2,&x,&y);
            assert(dock_edge_insertion_slot(&s,(DockEdge)edge,N,w,h,x,y) == slot);
            assert(dock_edge_insertion_layout(&s,(DockEdge)edge,N,w,h,slot,target));
            for (i = 0; i < N; ++i)
                assert(!memcmp(&target[i],&expanded[i+(i >= slot)],sizeof(*target)));
            for (tick = 0; tick < 32 && memcmp(current,target,sizeof(current)); ++tick) {
                dock_edge_animate(&s,(DockEdge)edge,&animation,current,target,N);
                for (i = 1; i < N; ++i) {
                    int prev = edge == DOCK_EDGE_BOTTOM ? current[i-1].x : current[i-1].y;
                    int next = edge == DOCK_EDGE_BOTTOM ? current[i].x : current[i].y;
                    assert(next >= prev+current[i-1].size);
                }
            }
            assert(tick > 1 && !memcmp(current,target,sizeof(current)));
        }
        for (tick = 0; tick < 32 && memcmp(current,rest,sizeof(current)); ++tick)
            dock_edge_animate(&s,(DockEdge)edge,&animation,current,rest,N);
        assert(!memcmp(current,rest,sizeof(current)));
        actual_point((DockEdge)edge,LONG/2,0,&x,&y);
        assert(dock_edge_insertion_slot(&s,(DockEdge)edge,N,w,h,x,y) == -1);
        actual_point((DockEdge)edge,LONG/2,CROSS-s.padding-s.base_size/2,&x,&y);
        assert(dock_edge_insertion_slot(&s,(DockEdge)edge,0,w,h,x,y) == 0);
        assert(!dock_edge_insertion_layout(&s,(DockEdge)edge,DOCK_MAX_ITEMS,w,h,0,target));
    }
    assert(!dock_edge_insertion_layout(&s,DOCK_EDGE_BOTTOM,3,140,CROSS,2,target));
}

static void test_zoomed_reorder_gap(void)
{
    DockSettings s = dock_settings_default();
    DockPlacement full[N], neighbors[N-1], gap;
    int edge, w, h, x, y, slot, i;
    for (edge = DOCK_EDGE_BOTTOM; edge <= DOCK_EDGE_RIGHT; ++edge) {
        dimensions((DockEdge)edge,&w,&h);
        for (slot = 0; slot < N; ++slot) {
            int span = N*(s.base_size+s.gap)-s.gap;
            int center = (LONG-span)/2+slot*(s.base_size+s.gap)+s.base_size/2;
            actual_point((DockEdge)edge,center,CROSS-s.padding-s.base_size/2,&x,&y);
            assert(dock_edge_insertion_slot(&s,(DockEdge)edge,N-1,w,h,x,y) == slot);
            assert(dock_edge_layout(&s,(DockEdge)edge,N,w,h,x,y,1,full));
            assert(dock_edge_gap_layout(&s,(DockEdge)edge,N-1,w,h,slot,x,y,1,neighbors,&gap));
            assert(gap.level > 0 && !memcmp(&gap,&full[slot],sizeof(gap)));
            for (i = 0; i < N-1; ++i)
                assert(!memcmp(&neighbors[i],&full[i+(i >= slot)],sizeof(*neighbors)));
        }
    }
}

static void test_zoom_room_after_insertion(void)
{
    DockSettings s = dock_settings_default();
    DockPlacement small[8], grown[8];
    int i, small_peak = 0, grown_peak = 0;
    int old_length, reserved_length, needed_length;
    s.max_size = 70;
    old_length = dock_viewport_length(&s,5);
    reserved_length = dock_viewport_length(&s,7);
    needed_length = dock_viewport_length(&s,8);
    assert(old_length == 373 && needed_length == 484);
    assert(old_length < dock_viewport_length(&s,6));
    assert(reserved_length >= dock_viewport_length(&s,6) && reserved_length < needed_length);
    assert(dock_edge_layout(&s,DOCK_EDGE_RIGHT,8,128,old_length,110,old_length/2,0,small));
    assert(dock_edge_layout(&s,DOCK_EDGE_RIGHT,8,128,old_length,110,old_length/2,1,small));
    assert(dock_edge_layout(&s,DOCK_EDGE_RIGHT,8,128,needed_length,110,needed_length/2,1,grown));
    for (i = 0; i < 8; ++i) {
        if (small[i].size > small_peak) small_peak = small[i].size;
        if (grown[i].size > grown_peak) grown_peak = grown[i].size;
    }
    assert(small_peak == 45 && grown_peak >= s.max_size-2);
}

static void test_stack_gap(void)
{
    DockSettings s = dock_settings_default();
    DockPlacement rest[N], zoom[N], insert[N];
    int edge, w, h, x, y;
    s.end_gap = 12;
    assert(dock_row_length(&s,1) == s.base_size);
    for (edge = DOCK_EDGE_BOTTOM; edge <= DOCK_EDGE_RIGHT; ++edge) {
        int previous, stack;
        dimensions((DockEdge)edge,&w,&h);
        assert(dock_edge_layout(&s,edge,N,w,h,0,0,0,rest));
        previous = edge == DOCK_EDGE_BOTTOM ? rest[N-2].x : rest[N-2].y;
        stack = edge == DOCK_EDGE_BOTTOM ? rest[N-1].x : rest[N-1].y;
        assert(stack-previous-rest[N-2].size == s.gap+s.end_gap);
        actual_point(edge,stack+s.base_size/2,70,&x,&y);
        assert(dock_edge_layout(&s,edge,N,w,h,x,y,1,zoom));
        previous = edge == DOCK_EDGE_BOTTOM ? zoom[N-2].x : zoom[N-2].y;
        stack = edge == DOCK_EDGE_BOTTOM ? zoom[N-1].x : zoom[N-1].y;
        assert(zoom[N-1].size == s.max_size);
        assert(stack-previous-zoom[N-2].size >= s.end_gap);
        assert(dock_edge_insertion_layout(&s,edge,N,w,h,N-1,insert));
        previous = edge == DOCK_EDGE_BOTTOM ? insert[N-2].x : insert[N-2].y;
        stack = edge == DOCK_EDGE_BOTTOM ? insert[N-1].x : insert[N-1].y;
        assert(stack-previous == 2*(s.base_size+s.gap)+s.end_gap);
    }
}

int main(void)
{
    test_layout_and_baseline();
    test_interaction();
    test_panel_rect();
    test_animation();
    test_counts_and_invalid();
    test_external_insertion();
    test_zoomed_reorder_gap();
    test_zoom_room_after_insertion();
    test_stack_gap();
    puts("geometry: bottom/left/right layout, upright hit/slot/outside, half-open panel bounds and animation equivalence PASS");
    return 0;
}
