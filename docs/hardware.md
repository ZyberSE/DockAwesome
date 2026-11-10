# Hardware notes

## Builds

`make` builds DockAwesome for the Vampire 68080 with AMMX (`-m68080`,
`DOCK_AMMX`). There is no build for other CPUs, so every backend uses the
AMMX kernels; `--ammx` is still accepted. The dock does not refuse to start
when exec `AttnFlags` bit 10 (`AFF_68080`) is clear: ApolloOS sets it, but
AmigaOS 3.2 on a Vampire need not. `--caps` shows the flags. See the
[build instructions](../README.md#build-and-test).

The build links with `-noixemul`. The app and larger probes request a
32 KiB stack with `__stack` and link with `-Wl,-u,___stkinit`.
Check `___stkinit`, `___stkexit`, and `___stkswap` in the binary.

## AMMX kernels

The kernels work in app-owned RAM. The half blend uses LOAD, PAVGB, and STORE
for two RGBA pixels. PAVGB calculates `(src + dst + 1) >> 1` for each byte.
It matches the C blend and accepts exact aliasing.

The RGB24 encoder packs four RGBA pixels with VPERM selectors `$01245689`
and `$00002456`. The C encoder uses endian-aware words and `memcpy` for
unaligned buffers. Both handle short tails.

RGB565 is big-endian. Decode repeats color bits and encode truncates them.
The key kernel uses a separate coverage byte for each pixel. See [PiP](pip-backend.md).

The Apollo 68080 Programmer's Reference Manual lists the opcodes. The
binutils disassembler can miss AMMX mnemonics. Check the raw words and
run `make ammx-probe` on Vampire. The probe checks all 65,536 byte pairs
and RGB565 colors, source and destination alignment, tails, and key collisions.

### Kernel timings

These are warm-cache runs at 709,379 EClock Hz on core 12515. They measure
the kernels in RAM.

| Kernel | C ticks | AMMX ticks | Work |
| --- | ---: | ---: | --- |
| Half blend | 4,686,175 | 194,196 | 16,384,000 pixels |
| RGB24 encode | 1,151,032 | 181,799 | 16,384,000 pixels |
| RGB565 decode | 452,158 | 60,775 | 4,096,000 pixels |
| RGB565 encode | 361,954 | 37,840 | 4,096,000 pixels |
| Byte comparison | 490,806 | 65,783 | 16,384,000 bytes |
| Key transport, clear | — | 79,191 | 4,096,000 pixels |
| Key transport, covered | — | 79,364 | 4,096,000 pixels |

## SAGA registers

The register map comes from these local ApolloCrossDev files.

- `../ApolloCrossDev/Projects/SAGAPiP/SAGAPiP.c`
- `../ApolloCrossDev/Projects/ApolloLib/ApolloCrossDev_Defines.h`

PiP2 uses `DFF3B0` through `DFF3C2` on core 11900 or newer.
A 352 px row worked with zero modulo. A 336 px row showed stripes.
Keep buffers and rows aligned to 64 bytes and widths in 32 px steps.
Test each new mode on the display.

The example uses offsets +16 and -1 in a different mode. The tested
Workbench mode uses +17 and -1. The alignment probe checks physical output.
The source probe passed 200 submissions and 199 known source updates and
left the Workbench bitmap intact.

The buffer retirement check reads `GfxBase->VBCounter` after the pointer
write. If the counter stays the same, wait for VBlank before reusing the
buffer. ApolloOS increments the counter before signaling waiters.

## ApolloOS notes

- The ApolloOS sources in `../ApolloOS` are older than R9.5: Intuition LVOs
  differ by one slot. Check OS behavior with a probe on the target.
- Intuition on R9.5 sends window open and close notifications but no
  Workbench close notification. Workbench resets come from
  `screennotify.library` (`AddWorkbenchClient`, message type 3).
- Outline fonts take seconds to open through diskfont after a reboot
  (about 11 s for four Vera Sans sizes). The About text is prerendered.

## ApolloOS text ABI

The classic NDK `Text()` inline declares the D0 count as `UWORD`.
ApolloOS reads `ULONG`. A word write left stale upper bits and turned a
short string into a huge count. Pass the full-width count to `Text()`
and `TextLength()`. Inspect register writes when porting to ApolloOS.
