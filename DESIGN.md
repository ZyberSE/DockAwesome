# DockAwesome visual design

Use the recorded [macOS Dock](docs/macos-reference.md) as the motion reference.
Keep the original Workbench artwork, aspect ratio, and alpha.

## Layout and motion

- Float the plate 3 px from any screen edge. Use four rounded corners, a thin
  rim, and 8 px of end padding.
- Use 32 px resting icon boxes and up to 64 px at the default zoom. Center the
  resting boxes on the plate. Zoom toward the desktop from a fixed baseline.
- Keep the plate thickness fixed during zoom. Follow the icon row at both ends.
- Use a 5 px resting gap. Narrow it by up to 2 px as neighbors grow.
- Track the pointer across icon centers and gaps. Round each box separately so
  distant icons stay steady.
- Use a 160 ms entry and exit at 50 Hz. Moving the pointer should not restart
  the transition. Stop the animation timer when the layout settles.

## Interaction

- Darken a pressed icon. Open it on a valid release.
- Show an insertion gap for reordering and Workbench drops.
- Show **Remove** beyond the drag boundary. Dropping there removes only the pin.
- Put the hover label on the desktop side of the icon.
- Use a compact dark context menu. Match the dock's glass and thin rim.
- Keep **Size**, **Scale**, position, and **Folder Stack** in a compact native
  window. **Save** applies the draft. **Cancel** discards it.
- Keep side icons upright. Use the same zoom and input rules on all edges.
