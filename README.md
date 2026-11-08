# DockAwesome

A macOS-style dock for ApolloOS on the Vampire V4 (68080, AMMX and SAGA).

- Hover to magnify nearby icons and see their names.
- Drag icons to add, move, or remove pins. Original files stay in place.
- See open apps, drawers, and documents. Every program with a window on screen
  gets a dot under its pin, or a temporary icon. Pin those with **Keep in Dock**.
- Browse a drawer from the dock with **Folder Stack**.
- Use live transparency with SAGA PiP2, or use a static or solid background.

## Requirements

- A Vampire V4 with a 68080 CPU. DockAwesome is built only for the 68080 and
  uses AMMX. It does not run on other CPUs.
- ApolloOS R9.5, tested with core 12515 and a 1280 × 720 RGB565 SAGA Workbench.
- Live transparency (PiP) needs a 16-bit SAGA screen. On other screens the
  dock uses a static or solid background. Screens above 8 bits need
  `cybergraphics.library` V40 or newer.
- `screennotify.library`, included in ApolloOS, lets the dock close and
  reopen when the Workbench resolution changes.

| Icon format | Support |
| --- | --- |
| Classic planar `.info` | One image, screen palette, pen 0 is transparent |
| ColorIcon and GlowIcon | Indexed colors and transparency from `icon.library` |
| NewIcons | When supported by the installed `icon.library` |
| PNG and other extensions | ARGB or indexed data from `icon.library`. ARGB keeps partial alpha. |

Source icons can be up to 1024 × 1024 px. Workbench can supply a default icon.

## Run

Copy `DockAwesome` and `DockAwesome.info` to the same drawer. Open it from
Workbench or run it from Shell. To start it with Workbench, put both files in
`SYS:WBStartup`. The icon has the `DONOTWAIT` tooltype; keep it, or Workbench
waits for the dock during startup.

```text
DockAwesome SYS:Prefs SYS:System
DockAwesome --pip --size 32 --zoom 64
```

**About** shows the release version, or the build number for a local build,
and the build date. You can also run `DockAwesome --version` or
`Version DockAwesome FILE FULL`.

| Mode | Background |
| --- | --- |
| `--snapshot` (default) | Static capture. Press R to refresh it. |
| `--opaque` | Solid fill |
| `--pip` | Live transparency on SAGA PiP2. This mode enables AMMX. |

PiP needs Apollo core 11900 or newer, ApolloOS Intuition and Layers V50,
and a native RGB565 screen. Reserve PiP2 for the dock while it runs. A saved
PiP setting falls back to snapshot if needed. An explicit `--pip` requires PiP
support.

When the Workbench resolution changes, the dock closes before Workbench
resets and reopens on the new screen. It chooses PiP or snapshot for the new
mode and lowers **Size** if the icons no longer fit.

## Controls

| Action | Result |
| --- | --- |
| Click an icon | Open it through Workbench |
| Drag a pin along the dock | Reorder it |
| Drag a pin out and release at **Remove** | Remove the pin |
| Drop a Workbench icon on the dock | Add a pin. An empty dock also accepts drops. |
| Right-click the dock background | Open **Settings**, **About**, or **Quit** |
| Right-click an open item | **Quit** an app or **Close** a drawer or document, when supported |
| Right-click a temporary app | **Keep in Dock** |
| Right-click Shell | **New Shell**, **Show Shell**, or **Quit**, when available |
| Click Folder Stack | Browse the drawer or choose **Open in Workbench** |
| Delete over an icon | Remove the pin |
| Escape with dock focus, or Ctrl-C | Quit |

The Shell menu targets the active Shell. If another app has focus, it targets
the frontmost Shell on the dock's screen.

## Settings

- **Size** sets the resting icon size from 16 px up to the largest size that
  fits the chosen edge, scale, and Folder Stack (at most 72 px).
- **Scale** sets magnification from 1.00× to 2.50×, up to a 128 px zoom, the
  macOS limit (2.50× up to Size 51, 1.78× at 72 px). Use 1.00× to
  turn zoom off. A larger saved size or zoom stays selectable.
- **Left**, **Bottom**, and **Right** set the screen edge.
- **Folder Stack** holds one drawer. Use **Browse** to pick it or **Clear** to remove it.

Pins save automatically to `ENVARC:DockAwesome.json`. Use **Save** to apply
settings or **Cancel** to discard them. New sizes rebuild every icon, which
takes a few seconds; the Settings title shows the progress. Use `--config RAM:DockTest.json` to test
with a separate file.

CLI options include `--size 16..192`, `--zoom PIXELS`, `--levels 2..33`,
and `--profile`. Zoom must be at least the resting size and at most 192 px.
`--pip-dump PREFIX` writes source and composite PPM files on exit.
`--bench-sweep 1..64` sweeps a synthetic pointer across the dock at that
many pixels per frame, prints the profile and exits. `--damage-check`
compares every frame with a full redraw (see [Verification](docs/verification.md)).
Use `--pip-x-offset` and `--pip-y-offset` to set alignment from -128 to 128.
The verified offsets are +17 and -1.

## Build and test

The default toolchain is `~/ApolloCrossDevLocal/Compilers/GCC-6.50-Stable`.
Set `TOOLCHAIN` to override it. Python 3 numbers local builds from the
`.build-revision` counter ("Build 181" in About; `$VER` reads 0.181 so the
Version command can parse it). Only a release package has a version.

```sh
make host-test host-sanitize
make host-bench
make verify
make ammx-probe
make pip-probe pip-align-probe pip-coexist-probe
make package
```

`make` builds `build/DockAwesome` for the 68080 with AMMX. Packages go in
`build/dist/DockAwesome/`.
`make release RELEASE="1.0 beta 1"` builds with that version in About and
`$VER`, and stages the release in `build/release/DockAwesome`. Pack that
drawer with LhA on the Amiga: `LHA -r a DockAwesome-1.0b1.lha RAM:Release/ DockAwesome`.
With ApolloExplorer running on the Amiga, `make deploy AMIGA_DEST=amiga:RAM:`
copies both files with `acp`.
Run the AMMX probe on Vampire. `--profile` reports frame and upload times.

`tools/make-icon.py` packs the Workbench icon from `assets/DockAwesome-48.png`.
`tools/make-about-font.py` regenerates the About glyphs in `src/about_font.c`
from Bitstream Vera Sans; the font files are not part of the build.

See [Architecture](docs/architecture.md), [Hardware](docs/hardware.md),
[PiP](docs/pip-backend.md), [Verification](docs/verification.md), and the
[macOS reference](docs/macos-reference.md).
