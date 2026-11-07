# DockAwesome

A macOS-style dock for AmigaOS. Built for Vampire and SAGA.

- Hover to magnify nearby icons and see their names.
- Drag icons to add, move, or remove pins. Original files stay in place.
- See open apps, drawers, and documents. Pin temporary apps with **Keep in Dock**.
- Browse a drawer from the dock with **Folder Stack**.
- Use live transparency with SAGA PiP2, or use a static or solid background.

## Requirements

- **DockAwesome** needs a 68020 or newer.
- **DockAwesome080** needs a Vampire 68080. `--ammx` enables AMMX.
- The dock needs a public screen with at least 8-bit color. RTG screens above
  8 bits need `cybergraphics.library` V40 or newer.
- The minimum library versions are Intuition, Graphics, and Layers V39,
  Icon and Workbench V44, and Utility V37.

| Icon format | Support |
| --- | --- |
| Classic planar `.info` | One image, screen palette, pen 0 is transparent |
| ColorIcon and GlowIcon | Indexed colors and transparency from `icon.library` |
| NewIcons | When supported by the installed `icon.library` |
| PNG and other extensions | ARGB or indexed data from `icon.library`. ARGB keeps partial alpha. |

Source icons can be up to 1024 × 1024 px. Workbench can supply a default icon.

## Run

Copy the binary to a drawer. Run it from Shell.

```text
DockAwesome SYS:Prefs SYS:System
DockAwesome080 --pip --size 32 --zoom 64
```

**About** shows the version and build date. You can also run
`DockAwesome080 --version` or `Version DockAwesome080 FILE FULL`.

| Mode | Background |
| --- | --- |
| `--snapshot` (default) | Static capture. Press R to refresh it. |
| `--opaque` | Solid fill |
| `--pip` | Live transparency on SAGA PiP2. This mode enables AMMX. |

PiP needs DockAwesome080, Apollo core 11900 or newer, ApolloOS/AROS
Intuition and Layers V50, and a native RGB565 screen. Reserve PiP2 for the
dock while it runs. A saved PiP setting falls back to snapshot if needed.
An explicit `--pip` requires PiP support.

On Coffin r65, use `DockAwesome080 --ammx --snapshot`. Workbench drawer
closing works there. App quitting and document closing need ApolloOS
Intuition extensions.

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

- **Size** sets the resting icon size. The slider normally runs from 16 to 96 px.
- **Scale** sets magnification from 1.00× to 2.00×. Use 1.00× to turn zoom off.
- **Left**, **Bottom**, and **Right** set the screen edge.
- **Folder Stack** holds one drawer. Use **Browse** to pick it or **Clear** to remove it.

Pins save automatically to `ENVARC:DockAwesome.json`. Use **Save** to apply
settings or **Cancel** to discard them. Use `--config RAM:DockTest.json` to test
with a separate file.

CLI options include `--size 16..192`, `--zoom PIXELS`, `--levels 2..33`,
and `--profile`. Zoom must be at least the resting size and at most 192 px.
`--pip-dump PREFIX` writes source and composite PPM files on exit.
Use `--pip-x-offset` and `--pip-y-offset` to set alignment from -128 to 128.
The verified offsets are +17 and -1.

## Build and test

The default toolchain is `~/ApolloCrossDevLocal/Compilers/GCC-6.50-Stable`.
Set `TOOLCHAIN` to override it. Python 3 generates the build version.
Both builds share the local `.build-revision` counter.

```sh
make host-test host-sanitize
make host-bench
make verify
make AMMX=1 verify
make ammx-probe
make pip-probe pip-align-probe pip-coexist-probe
make package
make AMMX=1 package
```

Builds go in `build/`. Packages go in `build/dist/DockAwesome/`.
Run the AMMX probe on Vampire. `--profile` reports frame and upload times.

See [Architecture](docs/architecture.md), [Hardware](docs/hardware.md),
[PiP](docs/pip-backend.md), [Verification](docs/verification.md), and the
[macOS reference](docs/macos-reference.md).
