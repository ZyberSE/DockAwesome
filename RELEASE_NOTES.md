# Release notes

## 1.0 beta 2

Changes since 1.0 beta 1.

**More systems**
- Runs on AmigaOS 3.2.3 and AmigaOS 3.9 on the Vampire, with live PiP
  transparency, as well as on ApolloOS.
- When another desktop replaces Workbench, the dock starts pins itself the
  way Workbench does: Start from Shell and ARexx icons, `$STACK` and
  `TOOLPRI`.

**Look and feel**
- A zoom wave like the macOS Dock, paced by the display. Size goes up to
  72 px, Scale up to 2.50× and the zoom up to 128 px.
- Round, antialiased name labels.
- Folder Stack grows out of its pin and fades in, with round glass buttons
  and default icons for files without their own.
- Folder Stack icons load faster: the first opening went from 0.9 s to
  0.13 s, and later openings take about 0.01 s.

**Fixes**
- Dragging a pin out of the dock is smooth on every system, without
  tearing or a box around the pin.
- Right-clicking a pin with nothing to offer shows no menu.
- The dock steps aside only for a menu that could reach it.
- No old dock picture after a reboot or a switch to another OS.
- Less drawing work: only the changed parts of the dock are redrawn and
  uploaded.

**New**
- About links to the releases page. A click opens it in the browser when
  OpenURL is installed.
- `DockAwesome --caps` lists what the dock found on your machine. Add it to
  bug reports.
