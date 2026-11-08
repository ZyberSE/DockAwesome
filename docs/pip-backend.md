# SAGA PiP2

`DockAwesome --pip` uses AMMX and a small RGB565 overlay. Workbench keeps
its original bitmap and main scanout.

## Requirements

The tested setup is a Vampire 68080 with core 12515, ApolloOS R9.5,
Intuition 50.7, Layers 50.0, and a 1280 × 720 native RGB565 screen.

PiP requires core 11900 or newer, a native RGB565 screen, and the Intuition
V50 window shape API. Use snapshot or opaque mode on other window stacks.

The tested offsets are +17 on X and -1 on Y. Use `--pip-x-offset`,
`--pip-y-offset`, `pip_x_offset`, or `pip_y_offset` to set them.
The allowed range is -128 to 128.

## Rendering

Capture the dock area from the Workbench bitmap. Lock the bitmap for each
read and release it before drawing. Blend the glass and icons over that
capture. Keep a separate coverage byte per pixel.

| Coverage | RGB565 output |
| --- | --- |
| 0 | `F81F` to show live scanout |
| Nonzero, color `F81F` | `F81E` to preserve magenta artwork |
| Nonzero, other color | Keep the encoded color |

Covered pixels use the latest capture. Uncovered pixels show the desktop
directly. Check the actual display for alignment and scanout timing.
`--pip-dump PREFIX` writes source and composite PPM files on exit.

## Input and drops

The priority-60 `input.device` handler hit-tests the last presented icon
alpha. It reserves space for both button events before consuming a press.
It consumes the matching release even if the snapshot has changed.
Stable pin IDs survive reordering. The main task handles actions and
allocations. Ambiguous injected chains and presses on a full queue pass through.

The native AppWindow has an empty idle shape. A valid Workbench press
prepares a drop receiver before Workbench locks the layers. Use the public
port owner and client bounds to validate the press. When available, cache
desktop icon bounds between gestures.

Copy the clean desktop to a reusable bitmap before expanding the receiver.
Backfill the exposed regions from that bitmap. Skip normal layer and
background reads while the gesture is active. An unlayered read compares
the primary plane with the saved pixels. The source key then shows the
native drag image over the dock.

Keep the insertion gap through the AppMessage handoff. Save the release
coordinates before later motion can overwrite them. Restore the idle
shape after the message, cancellation, a known outside release, or a
300 ms grace period. The dock shares Workbench's priority on the tested
system and restores its own priority on exit.

An internal drag uses a small native preview outside the PiP area. Return
to the dock to cancel removal. Release outside to remove the pin.

## Updates and timing

Poll the background every 100 ms at rest and every 20 ms during motion or a
held button. Animation captures reset the background deadline. Matching
source bytes and geometry skip decode, draw, and upload. Reuse all buffers.

An earlier five-pin surface used 70,656 source bytes at 384 × 92 px.
Four measured transitions took 177 to 178 ms. The 34 frames averaged
6,357 µs each, with 801 µs for source capture and comparison and zero shape
updates. These are elapsed times. See [Hardware](hardware.md) for kernel timings.

## Hardware ownership

Suspend PiP when another screen is in front or an outside menu is active.
Key out owned dialogs and the rounded popup where they overlap the dock.
Screen movement forces a new presentation. A replaced or unsupported
bitmap stops the backend.

Check layer order with the layer lock. Use throttled `WindowToFront` calls
to keep the receiver visible. Skip promotion while dock UI is open.
`Screen.FirstWindow` lists creation order on ApolloOS.

The `DockAwesome.PiP2` semaphore blocks other cooperating instances.
PiP2 registers are write-only. Reserve PiP2 for the dock while it runs.
The backend uses base `DFF3B0`, RGB565 format 2, and source-key bit `0100`.
Keep the width a multiple of 32 pixels, the buffers and rows aligned to
64 bytes, and the modulo at zero. A 336 px test showed stripes.

Wait for VBlank before reusing a retired buffer. Pointer flips can still
tear mid-field. On exit, stop DMA, wait one field, clear the pointer, free
buffers, and release the lease. Keep window regions alive until close.

## Test tools

- `make ammx-probe` checks pixel kernels and measures RAM throughput.
- `make pip-probe` checks source integrity on a private screen.
- `make pip-align-probe` checks alignment. Arrows move by 1 px. Shift-arrow
  moves by 8 px. Escape or Return prints the offsets.
- `make pip-coexist-probe` checks the lease and screen changes with the dock running.

Stop the dock before running the standalone PiP and alignment probes.
