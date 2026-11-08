# Verification

Run the portable tests and verify the Amiga executable.

```sh
make host-test host-sanitize
make verify
```

`make host-test` also runs the core tests with the big-endian conversion
path emulated on the host.

The geometry tests cover the 3 px desktop gap on all three edges and the
padded right-hand PiP surface. They also cover icon centering, zoom, hit
testing and drop placement.

The native target is a Vampire 68080 running ApolloOS R9.5 with a 1280×720
RGB565 SAGA Workbench. For an isolated native test, start DockAwesome
with `--pip --config RAM:DockAwesome-test.json`. Verify hovering,
dragging, context menus and Settings on each edge. Check the glass while
moving a Workbench window behind it. Check the corners on the physical
display because screenshots of the native screen can omit PiP.

The core tests compare render checksums of 384 scenarios against values
generated with the full-frame renderer, and replay 20,400 random frames
(pointer walks and jumps, pressed and centered plates, item counts, dots
and a dragged icon, backdrop changes, slot swaps) comparing the damaged
redraw with a full one byte for byte. A double-buffer test checks PiP
conversion with skipped and failed presents and changing masks.

On the Vampire, `--damage-check` renders every frame a second time in full,
converts it in full, and adds `Damage check: frames=N compose_mismatch=M
convert_mismatch=K` to the profile report; both counts must stay zero.
`--bench-sweep SPEED` (1-64 px per frame) moves a synthetic pointer along the
icon centre line from one plate end to the other and back, leaves the dock,
waits until the row settles, prints the profile and exits. Frames are paced
by `WaitTOF` and the backdrop is not polled. Compare `render_us_avg` between
builds with the same configuration, back to back. The `Damage:` line gives
the number of full frames and the share of the canvas composed and converted.
