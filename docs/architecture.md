# Architecture

DockAwesome separates layout, rendering, and screen output. It targets
ApolloOS on the Vampire 68080 with AMMX kernels and SAGA PiP2. The portable
core also builds on the host for tests.

```text
Workbench icons -> RGBA -> cached icon sizes
                               |
Pointer -> layout -> renderer -> frame -> native window or PiP2
```

## Modules

| Module | Role |
| --- | --- |
| `dock_core.c` | Icon caches, layout, animation, blending, and pixel conversion |
| `dock_geometry.c` | Bottom, left, and right geometry and hit testing |
| `dock_interaction.c` | Button pairs, drag state, and pin actions |
| `dock_renderer.c` | Pixel backend selection, damage tracking, and damaged PiP conversion |
| `dock_prefs.c` | JSON preferences |
| `amiga_main.c` | Workbench integration, item state, input, and timers |
| `amiga_ui.c` | Menus, labels, Settings, About, and Folder Stack |
| `about_font.c` | Prerendered Vera Sans glyphs for About (generated) |
| `amiga_display.c` | Native capture and uploads for indexed and RTG screens |
| `amiga_pip.c` | PiP2 buffers, alignment, presentation, and lease |

`DockPixelBackend` selects C or AMMX kernels. Both use the same rounded
50% blend. Icons with partial alpha share the C compositor. The renderer
draws the overlays (running dots, the dragged icon) and the caller
presents the frame; screen memory stays in the native display layer.

## Layout and memory

The default icon cache holds 33 RGBA levels from 32 to 64 px. Each icon uses
316,096 bytes. The 2 px resting inset grows with the artwork, which ranges
from 28 to 56 px at default zoom. Keep the source aspect ratio and alpha.

Levels are resampled once per icon with a bicubic filter (a = -0.75) on
premultiplied alpha, widened by half the ratio when shrinking. Faint dark
pixels away from solid artwork are cleared first when they connect to the
crop border (detached drop shadows); enclosed dark glass is kept. The visible
artwork is then normalized into the box. Each level also stores its visible
runs per row, split into translucent and opaque runs (about 3% of the pixel
bytes). Drawing skips transparent pixels and copies opaque runs whole.

The cache budget is a quarter of the free memory at startup, at least
16 MiB. The dock holds up to 32 items, subject to screen space. Free source
images after caching. Reuse frame, backdrop, upload, and coverage buffers.

Keep the icon baseline and plate thickness fixed during zoom. Map the pointer
between icon centers using Q8 sizes and gaps. Round each icon after layout.
Side icons stay upright. Bounds use half-open rectangles.

The default plate is 46 px thick with an 11 px radius and a 3 px screen gap.
Up to Size 32 the margin around the artwork follows the reference (31 px per
96 px); above it the margin grows by an eighth of the artwork's growth, so a
Size 72 plate is 96 px thick (12 px above and below the icons).
It follows the icon row with 8 px end padding. Icon gaps shrink from 5 to
3 px during magnification. Rounding can change the drawn gap by one pixel.
See [the panel reference](macos-panel.md).

`DockPanelShape` uses 12,804 bytes for corner masks. Rebuild them when the
radius changes. The edge uses 8 × 8 coverage samples. The interior uses bulk
blending. Tests compare cached and uncached rendering.

The renderer keeps the plate interior (the plate color mixed 50% with the
backdrop) row by row between frames. The main program calls
`dock_renderer_backdrop_changed` whenever the backdrop changes (a PiP
capture, a drag reference, or a snapshot); the next frame mixes the rows
again. The backdrop is copied only outside the plate interior. On Vampire at
Size 72 with 13 icons this took composition from 40.6 to 24.1 ms per frame.

The renderer also keeps the inputs of its last draw and redraws only the
damage: a list of up to 80 rectangles that never intersect (overlapping
ones merge into their bounding box; overflow redraws everything). Changed
slots add their old and new image rectangles, moved plate ends a strip that
reaches the corner radius plus one pixel inward, the divider its old and new
span, and running dots and the dragged icon, which the renderer now draws as
overlays, their old and new rectangles. A different canvas, backdrop,
coverage buffer, edge, centered plate, setting, item count, corner radius or
plate thickness redraws everything, as does `dock_renderer_invalidate`. The
main program calls it after every cache free, replace, build or swap and
every reorder, because a rebuilt cache can reuse a freed address. Each
rectangle is drawn by `dock_render_oriented_clip`, which writes canvas and
coverage only inside it, with the bytes a full draw gives there.
`DockRenderer.damage` reports what the last draw wrote.

## Input and updates

The `input.device` bridge converts the Amiga register ABI to the GCC stack
ABI. It tracks owned button pairs and signals the main task. Keep rendering,
allocation, and Intuition calls out of the input handler.

The 20 ms animation timer runs only during motion or a pending hover label.
Entry and exit use an eight-tick cubic ease-out: a third of the change on the
first tick, all of it by the sixth. The transition ends when the row reaches
its target. Moving the pointer does not restart the curve. Schedule the next tick before uploading the current frame.
Compare current and target placements to detect completion. A subpixel step
may change no pixels.

The Workbench worker updates open items after window changes and reconciles
every eight seconds. An app is a process that owns a visible window (a
layer, not a backdrop, at least 16 × 16, with a title, close gadget, or drag
bar); background processes never appear. Each app belongs, in order, to its
earlier attribution, a pinned program with its path, the pin launched just
before it appeared (within 10 s), a temporary icon from its program file, or a
temporary default tool icon labeled with its task name. Clicking a running
icon focuses its attributed window. The main task waits for input, messages,
or timer signals.

As a `screennotify.library` Workbench client, the dock releases its windows,
UI, PiP, and screen lock before Workbench resets and reopens afterwards.

## Native output

Snapshot mode captures the desktop before opening the dock. Press R to
refresh the image. Opaque mode draws a solid background. Both use a native
window and its clipping rules.

PiP uses a small RGB565 overlay and an empty idle AppWindow shape. Pixels
outside the artwork and plate use SAGA's source key. The native display code
locks the source bitmap for each read. Equal source bytes skip decoding and
drawing. Poll at 100 ms when idle and 20 ms during motion. See [PiP](pip-backend.md).

Workbench drops use a temporary receiver. Capture a clean backdrop before
Workbench locks the layers. Reuse it until the drop or release is complete.
Window regions must outlive their windows. See [Hardware](hardware.md) for
the input and layer ABI notes.

Settings use GadTools. The Size slider stops at the largest size that fits
the draft's edge, scale, and Folder Stack, using the same check as Save.
Build candidate caches before swapping them in; the dialog shows progress and
disables its controls meanwhile. Restore the previous settings and caches if
saving or opening the surface fails. The JSON file stores the edge. A missing
edge defaults to bottom.

## References

- Local Amiga RKM Libraries and Devices manuals in `../amiga-refs/`
- Apollo 68080 Programmer's Reference Manual for LOAD, PAVGB, STORE, and VPERM
- Amiga NDK and CyberGraphX SDK headers
