# Verification

Run the portable tests and verify the two Amiga executables.

```sh
make host-test host-sanitize
make verify
make AMMX=1 verify
```

The geometry tests cover the 3 px desktop gap on all three edges and the
padded right-hand PiP surface. They also cover icon centering, zoom, hit
testing and drop placement.

The native target is a Vampire 68080 running ApolloOS R9.5 with a 1280×720
RGB565 SAGA Workbench. For an isolated native test, start DockAwesome080
with `--ammx --pip --config RAM:DockAwesome-test.json`. Verify hovering,
dragging, context menus and Settings on each edge. Check the glass while
moving a Workbench window behind it. Check the corners on the physical
display because screenshots of the native screen can omit PiP.
