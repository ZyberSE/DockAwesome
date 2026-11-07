# Architecture

DockAwesome separates layout, rendering, and screen output. The same core
runs on ApolloOS and AmigaOS. Vampire can use AMMX kernels and SAGA PiP2.

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
| `dock_renderer.c` | Pixel backend and presenter selection |
| `dock_prefs.c` | JSON preferences |
| `amiga_main.c` | Workbench integration, item state, input, and timers |
| `amiga_ui.c` | Menus, labels, Settings, About, and Folder Stack |
| `amiga_display.c` | Native capture and uploads for indexed and RTG screens |
| `amiga_pip.c` | PiP2 buffers, alignment, presentation, and lease |

`DockPixelBackend` selects C or AMMX kernels. Both use the same rounded
50% blend. Icons with partial alpha share the C compositor.
`DockPresenter` accepts a frame and its context. Screen memory stays in the
native display layer.

## Layout and memory

The default icon cache holds 33 RGBA levels from 32 to 64 px. Each icon uses
316,096 bytes. The 2 px resting inset grows with the artwork, which ranges
from 28 to 56 px at default zoom. Keep the source aspect ratio and alpha.

The cache limit is 16 MiB. The dock holds up to 32 items, subject to screen
space. Free source images after caching. Reuse frame, backdrop, upload, and
coverage buffers.

Keep the icon baseline and plate thickness fixed during zoom. Map the pointer
between icon centers using Q8 sizes and gaps. Round each icon after layout.
Side icons stay upright. Bounds use half-open rectangles.

The default plate is 46 px thick with an 11 px radius and a 3 px screen gap.
It follows the icon row with 8 px end padding. Icon gaps shrink from 5 to
3 px during magnification. Rounding can change the drawn gap by one pixel.
See [the panel reference](macos-panel.md).

`DockPanelShape` uses 12,804 bytes for corner masks. Rebuild them when the
radius changes. The edge uses 8 × 8 coverage samples. The interior uses bulk
blending. Tests compare cached and uncached rendering.

## Input and updates

The `input.device` bridge converts the Amiga register ABI to the GCC stack
ABI. It tracks owned button pairs and signals the main task. Keep rendering,
allocation, and Intuition calls out of the input handler.

The 20 ms animation timer runs only during motion or a pending hover label.
Entry and exit use eight smoothstep ticks. Moving the pointer does not restart
the curve. Schedule the next tick before uploading the current frame.
Compare current and target placements to detect completion. A subpixel step
may change no pixels.

The Workbench worker updates open items after window changes and reconciles
every eight seconds. Classic Intuition uses window list checks. The main
task waits for input, messages, or timer signals.

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

Settings use GadTools. Build candidate caches before swapping them in.
Restore the previous settings and caches if saving or opening the surface
fails. The JSON file stores the edge. A missing edge defaults to bottom.

## References

- Local Amiga RKM Libraries and Devices manuals in `../amiga-refs/`
- Apollo 68080 Programmer's Reference Manual for LOAD, PAVGB, STORE, and VPERM
- Amiga NDK and CyberGraphX SDK headers
