# DockAwesome rename and cleanup

- [x] Rename the application, build and package references, and set the dialog title to About.
- [x] Use a 3 px desktop gap on every edge and preserve the right PiP transport.
- [x] Trim redundant comments and archive old artifacts outside the checkout.
- [x] Verify both builds, portable tests and the Vampire configuration.
- [x] Rename the checkout and initialize Git without creating a commit.

## Review

Host tests and sanitizers passed. Both Amiga HUNK builds and the distribution
package built successfully. The Vampire ran DockAwesome080 0.115. The
original JSON was moved to the new default path with identical content.
Menus opened on all three edges and the About window reported the exact
title. Exported PiP frames measured a 3 px gap on all three edges.
Git reports an empty main branch with no staged files. The ignore rules
exclude builds, the local revision counter and runtime preferences.

## Trim standalone tools

- [x] Archive the four optional diagnostics and remove their build targets and documentation references.
- [x] Build both variants and the retained AMMX/PiP probes, run host tests and verify Git remains empty.

Both Amiga variants and all four retained probes built in an isolated copy.
Host tests passed, and Make rejected each removed target. Git has no staged
files or commits.

## Whole-repository architecture and performance review

- [x] Capture a baseline and inventory the portable core, native integrations, and build tooling.
- [x] Review lifecycle, concurrency, input, persistence, rendering, and shared code paths.
- [x] Fix confirmed defects, consolidate compatible logic, and measure optimizations.
- [x] Compare behavior against the baseline with portable tests, both Amiga builds, and targeted Vampire checks.
- [x] Record verified changes, measurements, and open findings.
- [x] Complete an interactive PiP smoke test after the Vampire is restarted.

### Review results

- Saved rows that exceed the screen or cache budget now fail before any
  preference write. If a saved icon cannot load, the reduced dock stays
  usable and preference writes report that the change could not be saved.
  On Vampire, early admission preserved all six paths in the isolated
  recovery backup and created no replacement JSON.
- Divider coordinates remain in 64-bit arithmetic until clipped. The
  original UBSan reproducer passed after the fix. Screen/cache admission
  and RGB24 decoding now each have one shared implementation.
- Both Amiga builds, the portable tests and sanitizers, and the native AMMX
  probe passed. A 360-case render comparison produced identical RGBA and
  coverage output. The PiP probes compiled.
- A synthetic EClock composition test on Vampire used 13 icons, 40 frames per
  pass and six passes per build and orientation. Median Bottom time fell
  from 17.70 to 17.32 ms per frame. Median Right time fell from 19.51 to
  19.18 ms. Row scanning and an alternate alpha calculation were slower and
  were discarded.
- The baseline recovery trial left an older Dock process waiting on
  Intuition with the PiP lease held. The live configuration was untouched.
  After the restart, 0.117 passed PiP hover tests on Bottom and Right and
  opened About. An isolated config saved an additional pin. The running
  dock retained all 12 original pins and the original preferences.
- End-to-end hover measurements did not establish a frame-time improvement.
  The missing-icon save guard was reviewed separately from the native
  over-budget recovery and successful-save cases.

## US English copy

- [x] Audit documentation, comments, and program messages.
- [x] Use short, plain US English and keep key technical details.
- [x] Verify both Amiga builds and check the text.

Both Amiga builds and the probes compiled. Host tests and packaged Markdown
links passed. C code matched the baseline after excluding comments and
string contents. Git has no staged files or commits.

## Sharper icon scaling trial

- [x] Build a sharper cache filter with the same icon sizes and alpha handling.
- [x] Compare both filters on Vampire at rest and at full zoom.
- [x] Check the original icon and saved preferences after the test.

Both Amiga builds, host tests, and sanitizers passed. Both filters ran in
PiP on Vampire with a separate config. Demos kept 32 x 28 px of artwork at
rest and 62 x 54 px at full zoom. The original icon and live config match
their pretest bytes.

## WBDock icon copy

- [x] Read the icon path from WBDock and copy that icon to RAM.
- [x] Compare the copy with the existing pin on Vampire.
- [x] Run the sharper build beside WBDock without changing live preferences.

DH11 is the Archives volume. The two icon paths and the RAM copy had
identical bytes. DockAwesome 0.119 produced identical full PiP frames
from either icon at rest and full zoom. The copy runs in a test dock on
the left. WBDock remains at the bottom.

## Faint icon shadows

- [x] Apply one rule to all icons with partial alpha.
- [x] Compare Demos on Vampire at rest and full zoom.
- [x] Verify that edge pixels, icon size, and the original file are preserved.

The icon cache clears faint dark pixels away from denser pixels. The tests
cover soft edges, colored pixels, and fully translucent icons. On Vampire,
0.123 removed the shadow under e, m, and o in both frames and kept the line
under D. The artwork bounds, original icon, and live preferences matched.

## Review fixes for icon scaling and shadows

- [x] Clear faint shadows once on a cropped source copy, before scaling.
- [x] Sharpen only axes that scale up. Keep linear weights when scaling down.
- [x] Keep `sharp_fraction` within 0..255.
- [x] Test upscaled soft edges and shadow clearing at every level.
- [x] Run host tests, sanitizers and both Amiga builds.
- [x] Review the whole codebase as a senior engineer and record findings.

## Bicubic icon scaling

- [x] Replace the 2x2 bilinear cache filter with separable bicubic resampling.
- [x] Compare kernels on the 11 decoded pins at rest (36) and full zoom (70).
- [x] Measure cache builds on Vampire with the real pins and configuration.
- [x] Run 0.126 on Vampire with a copy of the live configuration.
- [x] Decide between bicubic quality and build time after viewing PiP output. (Keep bicubic.)

Keys bicubic (a = -0.75) with half-ratio widening for reductions. It is exact
at 1:1, keeps flat areas flat, and averages fine stripes. Against the 0.123
smoothstep filter, mean edge contrast on the real pins rose 6% at rest and
10% at full zoom. Catmull-Rom (a = -0.5) gave -4% and +6%. Vampire cache
builds for the 11 pins (36 -> 70 px, 33 levels): 0.123 took 107 ms per icon,
bicubic 239 ms (setup 26, vertical 83, horizontal 130). Moving tap tables to
32-bit math saved 10%. Register accumulation, transparent-pixel skips and a
reciprocal color divide did not change the time. Host tests, sanitizers and
both Amiga builds passed. 0.126 runs from RAM: with RAM:DockAwesome-test.json;
0.123 remains at RAM:DockAwesome080 for rollback.

### Senior review findings (open)

High
- amiga_main.c:1600/1780: worker stack frames of ~17.5 KB each on a 32 KiB
  stack; the icon-discovery chain reaches ~22 KB before icon.library runs.

Medium
- amiga_main.c:3405/3211: fatal-error requesters run while PiP, the input
  handler and the drop receiver are still live.
- amiga_main.c:1845-1918: Intuition calls and a blocking DoPkt inside Forbid.
- amiga_display.c:216: ReadPixelArray8 uses the UWORD NDK inline.
- amiga_ui.c:1738: ASL folder requester blocks main without suspending PiP.
- amiga_display.c:69-83: 8-bit pen table (1M iterations) rebuilt per open.
- dock_core.c: cleared shadows still count toward artwork bounds, shifting
  icons up to 3 px (rest) and 7 px (zoom). Kept by design; needs a decision.
- dock_core.c: the shadow rule erases dark translucent art enclosed by solid
  pixels; a flood fill from the crop border would keep it.
- dock_core.c: plate glass is recomputed every frame though it is constant.

Low
- Wake-ups on every mouse event while another screen is in front; full-surface
  compare on every animated frame; blocking LockLayerInfo in snapshot hover;
  drops lost during rebuild; save leftovers; unbounded exit loop; JSON errors
  invisible from Workbench; folder scans without pr_WindowPtr; drag preview
  has a window title; folder icons build a redundant second level; animation
  push can leave the surface with levels=2.

Resolved by the bicubic filter: large reductions lost fine detail, and
resampled colors were rounded down.

## Fix all review findings

- [x] Core: visible-artwork bounds after shadow clearing; flood-fill shadow
      rule; animated rows kept inside the surface; 32-bit mix_half; uint32
      weight math; big-endian RGB24 test; presenter hook removed;
      single-level dock_icon_build for folder icons.
- [x] amiga_main.c: all 12 findings (worker stack, deferred fatal
      requesters, IBase revalidation, DoPkt on the worker, handler wake
      filter, partial compare, AttemptSemaphore hover, drops across rebuild,
      save cleanup, bounded exit loop, Workbench JSON error).
- [x] amiga_ui/display/folder/drag_preview: findings 1-8; ASL blocking hook
      wired in main.
- [x] Host tests, big-endian tests, sanitizers, fuzz harness, both Amiga
      builds (0.135), AMMX probe on Vampire.
- [x] Program icon: assets/DockAwesome.info (tools/make-icon.py), added to
      `make package`; icon.library decodes it byte-exact on Vampire.
- [x] Independent review of the native diff; its five findings fixed in 0.140:
      exit waits for EndDockStatusNotify instead of exiting while Intuition
      can still signal the task; kept backfill hooks point at persistent
      code (no-op or forwarder) instead of DockAwesome's segment; console
      windows are probed only while the pointer rests on a running Shell
      pin, not on every status poll; Quit Shell is offered only for targets
      probed in the last 2 s; drops queued across a surface rebuild are
      translated to the new surface.
- [ ] Visual and interactive check of 0.140 on the physical display.

On Vampire, ApolloOS console windows have no UserPort and each console has
its own handler task, so window ownership cannot be checked through the
UserPort. 0.135 exited cleanly on Ctrl-C; 0.140 runs from RAM: with
RAM:DockAwesome-test.json. 0.123 remains at RAM:DockAwesome080.

Results: the shadow and glass repros now keep 400/400 glass pixels and
equal margins; all 11 real pins render byte-identical to before. The
levels=2 overflow repro fell from 65 px to 0; the fuzz harness reports 0
target and 0 animated out-of-surface frames. The C half blend fell from
4,686,175 to 2,150,417 EClock ticks per 16.4M pixels (AMMX unchanged, all
probe checks pass). Worker stack frames fell from ~17.5 KB to 1.3 KB.

Deferred, with reasons:
- Tinted-plate cache (kept glass) and visible-span icon blending (runs) were
  implemented in c41bfa5; see "Large-icon zoom speed".
- Partial compare (main finding 6) is implemented (frame_check and the
  plate-only check in live_capture); its cost is capture_compare_us_avg in
  the --profile report.
- Folder-reveal hook: if another program chains our backfill hook and the
  original was the default, a pass-through stays installed while the dock
  runs. Closing it needs a permanent code stub.
- Refactors: split amiga_main.c, narrow App, one ApolloOS ABI header, merge
  the two layout mappings, collapse the render wrapper stack, validate
  settings once per swap.

## Install in WBStartup

- [x] Move WBDock and WBDock.info to ApolloOS:WBStartup/Disabled.
- [x] Copy DockAwesome080 0.140 as ApolloOS:WBStartup/DockAwesome with the
      build's .info (assets/DockAwesome-48.info: WBTOOL, 32 KB stack).
- [x] Start it through Workbench (WBRun) with the live ENVARC configuration.
- [x] Remove staging files from RAM:.

WBRun started the installed copy; FindTask reports DockAwesome running and
WBDock not running. Roll back by moving WBDock and WBDock.info back from
Disabled and removing DockAwesome and DockAwesome.info from WBStartup.

## Workbench-style program icon

- [x] Measure the previous icon: 48x48 canvas but 40x22 px of artwork, while
      the pinned icons fill about 44x44.
- [x] Icon concept: a magnifying glass over the dock (user's choice of three
      bold concepts after the scene-style drafts were rejected). The lens
      shows the dock plate, two glossy icons and a Boing ball enlarged; the
      metal rim and handle fill the 48x48 canvas.
- [x] Add a selected image like ApolloOS icons: darker, with the ball turned
      half a check.
- [x] tools/make-icon.py writes assets/DockAwesome-48.info (WBTOOL, 32 KB
      stack, frameless); installed as ApolloOS:WBStartup/DockAwesome.info.

The previous icon is saved outside the checkout in the session scratchpad.

Icon handed over to Codex (2026-11-08): it replaces assets/DockAwesome-48.info
and the WBStartup icon. Do not change the icon files from this session.

## Green CRT program icon

- [x] Package the approved 48x48 artwork with a selected image.
- [x] Verify both packages use the new icon.
- [x] Replace the Workbench icon on Vampire and verify the installed file.

Both builds package the same icon. The WBStartup file matches it byte for
byte, and a Vampire screenshot shows the new icon.

## Simplify without losing performance

- [x] Resample: drop the register path, transparent skip and reciprocal
      divide (measured no gain on Vampire); keep the 32-bit tap tables.
- [x] Remove the four unused render wrappers and dock_edge_slot.
- [x] Validate settings once per call chain instead of in every layer.
- [x] One shared ChangeWindowShape wrapper.
- [x] Group profiling counters in one struct.
- [x] Verify: host tests, sanitizers, byte-identical render and cache output
      on the real pins, both Amiga builds, Vampire cache benchmark.

WBStartup fix: after a reboot Workbench waited on DockAwesome ("has not yet
returned"), so AmiMCP never started. The icon lacked DONOTWAIT (WBDock has
it). tools/make-icon.py now adds it; icon.library reads it from the
installed ApolloOS:WBStartup/DockAwesome.info (type 3, stack 32768).

## Sharper green CRT icon

- [x] Sharpen the 48x48 source with its color tones and alpha intact.
- [x] Verify the rebuilt Workbench icon and package output.
- [x] Compare the original and sharpened icon in WBStartup on Vampire.

Both packages and WBStartup use the sharpened icon. The before and after
captures match their source images at 1:1. The DONOTWAIT tool type is intact.

Simplification results: the resample keeps only the unrolled four-tap sum
(a generic register loop was slower than streaming: 279 vs 245 ms) and drops
the transparent skip and reciprocal. Vampire cache builds for the 11 pins:
241 ms before, 217-220 ms after. Output: 3 of 68,156 cache pixels and 1 of
72,000 rendered pixels differ by one level (exact division again). Removed
four render wrappers, dock_edge_slot and dock_interaction_slot (tests only),
and the duplicate validation in dock_renderer_draw; other validations guard
array bounds at API boundaries and stay. ChangeWindowShape lives in
include/amiga_abi.h (LVO calls unchanged). 47 profiling counters moved into
ProfileStats. Host, big-endian and sanitizer tests and the fuzz harness pass.

## Temporary icon jump

- [x] Cause: a program that is not pinned (ApolloBoot's ReqList window) gets
      a temporary icon; when it quits, program_poll_result snapped the whole
      row to the new rest layout, so the centered dock jumped.
- [x] Fix: like drops and unpins, the new icon appears in its slot and the
      rest glide to the new row (0.148, installed in WBStartup).
- [ ] User check: open and close ApolloBoot's window.

The 0.140 binary was locked after the dock quit, so it was renamed to
WBStartup/Disabled/DockAwesome-0140.old (since deleted).

## Running indicators for scripts and detached programs

Probed on Vampire with ApolloBoot's requester and HippoPlayer open:
- ApolloBoot (project, default tool IconX): the window "  ApolloBoot  " is
  owned by the CLI process BOOT:C/ReqList-Short; cli_CommandFile is empty and
  Workbench lists only ApolloOS:C/IconX. It became a temporary ReqList icon.
- HippoPlayer detaches: its window owner is task "HippoPlayer" without a CLI
  and it is missing from Workbench's program list, so it was not discovered.

- [x] A window titled exactly with a document pin's file name (blanks
      trimmed) marks that pin running and adds no temporary icon.
- [x] A window owner without a CLI is discovered as its task name in its
      home directory (lock duplicated in Forbid after a liveness check).
- [x] 0.150 installed in WBStartup and started through Workbench.
- [ ] User check: dot under ApolloBoot with its window open and no extra
      icon; HippoPlayer appears as a temporary icon with a dot.

0148.old in WBStartup/Disabled was still locked by Workbench; delete it later.

## General running-program detection (no per-program rules)

Requirement: every program with a window on screen appears, background
processes do not, and nothing is special-cased per program.
- [x] An app is a process owning a visible window: a layer, not a backdrop,
      at least 16x16, with a title, close gadget or drag bar.
- [x] Attribution order: earlier attribution, pinned program by path, a new
      app that appears within 10 s of a dock launch (that pin), a temporary
      icon from its program file, else the default tool icon labeled with
      the task name.
- [x] Attributions persist while the task lives and still shows a window,
      in every poll, so document pins keep their dot.
- [x] Clicking a running icon focuses its attributed task's window.
- [x] Remove the title-name and sole-tool heuristics.
- [ ] Verify on Vampire: ApolloBoot from the dock (dot, no extra icon),
      HippoPlayer from Workbench (temporary icon), background tasks absent.

0.156 (installed in WBStartup) implements the above. Apps without a program
file cannot be kept as pins. Disabled/ holds locked old binaries to delete.

## Test release 1.0 beta 1

- [x] Release versions: RELEASE replaces the build counter in About and
      $VER ("DockAwesome 1.0 (08.11.2026) beta 1"); About credits Reine
      "ZyBeR" Delwall. 68080 only.
- [x] make release stages DockAwesome, its icon, ReadMe.txt and README.md.
- [x] Packed with LhA 2.15 on the Amiga; Lhasa tests it and every file
      matches the staged build. ~/Downloads/DockAwesome-1.0b1.lha
      (103,621 bytes, SHA-256 02412cd2...0d0cfdf).
- [x] 1.0 beta 1 installed as ApolloOS:WBStartup/DockAwesome (same bytes
      as the archive) and started through Workbench. Disabled/ holds the
      locked DockAwesome-0156.old; delete it after a reboot.

## About typography and speed

- [x] Vera Sans through diskfont made the first About after a reboot take
      ~11 s (measured: open fonts 11,243 ms, window 3 ms, measure 0 ms,
      draw 5 ms). Later opens were fast only while the fonts stayed cached.
- [x] tools/make-about-font.py renders Vera Sans (Bold 20, 12, 11, 13 px,
      ASCII 32-126) into src/about_font.c; About composes lines from it and
      blits them with BltTemplateAlpha. No font files are needed at run
      time; without cybergraphics alpha it falls back to the screen font.
- [x] Credit on two lines with spacing; the name is regular weight.
- [x] About redesigned and prerendered; installed for the user's check.

## Settings limits, Save feedback and Workbench resets

- [x] Size slider maximum follows the largest Size that fits the draft's
      edge, Scale and Folder Stack (main's fit hook, same dock_items_fit as
      Save); Scale stops at the 192 px zoom. The icon cache budget is a
      quarter of free memory (at least 16 MB) instead of a fixed 16 MB.
- [x] Save shows a busy pointer and progress in the Settings title. Measured
      for 11 pins: icon loading 267 ms, cache 36->70 2,250 ms, 64->125
      6,444 ms; the cache build is the cost.
- [x] Workbench resets (resolution changes): Intuition on ApolloOS R9.5 sent
      no BEFORE_CLOSEWB (probe: 0 messages), so the dock is a
      screennotify.library Workbench client (type 3; FALSE before close,
      TRUE after open, also after a failed close). It releases its windows,
      UI and screen lock, waits, then reopens with the backend chosen for
      the new screen and lowers Size if needed.
- [x] Vampire: CloseWorkbench() from a separate process succeeded, Workbench
      reopened, and the dock reopened on the new screen with no requester.
- [x] User check: a real resolution change in ScreenMode prefs works.

Release archive rebuilt: ~/Downloads/DockAwesome-1.0b1.lha (119,699 bytes,
SHA-256 111bc02f...8afed359), packed with LhA on the Amiga; every file
matches build/release/DockAwesome.

## Large-icon zoom speed and plate padding

Vampire, Size 72, 13 icons, 1280x168 canvas (framebench, compose only):
58.7 ms before.
- [x] Plate glass: the plate color mixed with the backdrop is kept per row
      and mixed again only after the backdrop changes (PiP capture, drag
      reference, snapshot). 58.7 -> 46.3 ms; a backdrop that changes every
      frame costs +3.4 ms over before.
- [x] Icon blend without a divide, two channels per multiply, and
      precomputed per-row runs (transparent skipped, opaque copied) built
      with the cache (~3% of pixel bytes). -> 28.5 ms.
- [x] Backdrop copied only outside the plate interior. -> 24.5 ms.
- [x] Output byte-identical to HEAD: 300 render scenarios (3 edges, flat and
      captured, coverage, pressed, centered, divider), with and without
      glass; 200,000 random blends; new unit tests for glass and runs.
- [x] Real dock with --pip --profile: render 52.9 -> 40.8 ms (compose 40.6
      -> 28.5) before the backdrop-span change.
- [x] Real dock after all compose changes: compose 24.1 ms, conversion
      12.3 ms (render about 36 ms, from 52.9).
- [x] Rejected: a fused AMMX encode+key kernel. Validated bit-exact on the
      Vampire and 13% faster cache-hot, 8.1 vs 11.7 ms in framebench, but in
      the real dock writing the PiP buffer it was slower in every form: row
      spans 15.3-16.4 ms, full rows 13.4 ms, via a fast-RAM row and CopyMem
      14.7 ms, against 12.3 ms for the separate passes. Code removed.
- [x] Plate thickness: margin grows by an eighth of the artwork above Size 32
      (Size 72: 20 -> 12 px per side; user asked for slightly less than 14). Unchanged at Size 32 and below.
- [x] User check of the new padding on the Vampire (asked for slightly
      less than 14 px; 12 px approved).
- [x] Installed 1.0 beta 1 in ApolloOS:WBStartup (old binaries are in
      WBStartup/Disabled as DockAwesome-*.old; Workbench holds locks on
      them until a reboot, then delete them). RAM: cleaned.

Release archive rebuilt as 1.0 beta 1 (beta 1 was never published):
~/Downloads/DockAwesome-1.0b1.lha (122,219 bytes, SHA-256
75693347...ffb8eefe), packed with LhA on the Amiga; every file matches
build/release/DockAwesome.

## Damage-region rendering (plan-damage-rendering.md, PR 1)

Vampire, 12 icons, `--pip --bench-sweep`, baseline 34dae0a against the
damage build, run alternately per speed in one batch. Times in ms.

| Size, speed | Render before | Render after | Compose | Conversion | Damage (compose / conversion) |
| --- | --- | --- | --- | --- | --- |
| 72, 3 | 35.9 | 33.0 (-8%) | 24.1 -> 23.3 | 11.8 -> 9.6 | 52% / 59% |
| 72, 12 | 35.7 | 27.5 (-23%) | 23.8 -> 19.2 | 11.8 -> 8.2 | 52% / 57% |
| 72, 25 | 35.3 | 31.0 (-12%) | 23.5 -> 21.5 | 11.7 -> 9.4 | 59% / 66% |
| 42, 12 | 15.5 | 12.2 (-22%) | 10.0 -> 8.5 | 5.5 -> 3.6 | 42% / 49% |

- [x] Goldens (384 scenarios) unchanged; damage fuzz 20,400 frames; PiP
      double-buffer test; host-test and host-sanitize green.
- [x] `--damage-check`: sweep at Size 72 speed 12, 167 frames, 0 compose
      and 0 conversion mismatches; manual sessions on all three edges
      (hover, Workbench drag and drop, reorder, popup, Settings over the
      dock) 246 + 64 + 51 frames, 0 mismatches.
- [ ] Physical scanout: not observed by a person; the damage check compares
      the PiP back buffer with a full conversion before every present.
- Why the gain is smaller than the damaged area: a host replay of the same
  sweep shows 86-95% of icon pixels redrawn per frame, because moving the
  pointer shifts the whole row by a pixel or more. Icon blending dominates
  compose, so damage saves mostly plate and background. Speed 3 also has
  about twice the rectangles per frame (9 vs 4.5), which adds per-row
  call overhead in conversion. Next lever: keep distant icons still during
  the wave (layout), then merge row spans in the conversion.

## Next: zoom wave matching macOS (requested 2026-11-08)

Measured from the user's macOS screenshot (right edge, 2x pixels, rest
96 px, hovered 187 px) against `dock_layout` (Size 48, 195%, pointer on an
icon centre). Share of the full zoom per icon from the pointer:

| Steps from the pointer | macOS | DockAwesome |
| --- | --- | --- |
| 0 | 1.00 (1.95x) | 1.00 (1.96x) |
| 1 | 0.84-0.87 | 0.83 |
| 2 | 0.47-0.49 | 0.50 |
| 3 | ~0.04 | 0.15 |
| 4 | 0 | 0 |

- [x] Make the third icon from the pointer grow ~0.04 of the zoom instead
      of 0.15, keeping steps 0-2; add a geometry test for the profile.
      Catmull-Rom wave; at Size 42, 195%: 1.00/0.88/0.45/0.03/0.
- [x] Vampire, 0.178 against 0.180 (alternated, `--bench-sweep`, 12 icons):
      render Size 72 speed 3 33.1 -> 22.9 ms, speed 12 27.6 -> 23.9, speed 25
      31.2 -> 25.8; Size 42 speed 12 12.2 -> 10.3. Damage share at Size 72
      speed 3 52% -> 38% (distant icons no longer move). Entry settle at Size
      42 171 -> 136 ms, Size 72 329 -> 274 ms. `--damage-check` sweep: 166
      frames, 0 mismatches. 0.180 installed for the user to try (0.178 in
      WBStartup/Disabled). The profile itself is verified on the host; PiP
      does not show in screenshots, so the visual match is the user's check.
- [x] Entry: cubic ease-out over the same 8 ticks (user: macOS feels
      snappier). Note: the macOS video in docs/macos-reference.md shows an
      S-curve of about 150 ms, which the old smoothstep matched at Size 42
      (158 ms measured); the ease-out is faster at the start than that.
- Also reported: the entry feels less snappy than macOS. At Size 72 the
  8-tick entry took 332 ms before damage rendering (36 ms frames on a
  20 ms tick); re-measure after damage rendering before changing the curve.

## Smoother zoom and new size limits (2026-11-08)

- [x] Settings: Size up to 72 px, Scale up to 2.50x, zoom at most 128 px
      (the macOS limit). Measured at Size 42 x 2.50 (105 px): 13.0 ms per
      frame, entry 148 ms; Size 72 x 1.78 (128 px): 23.1 ms.
- [x] Display refresh measured: 50.00 Hz (150 WaitTOF in 2.9997 s), so 50
      frames per second is the ceiling; the dock already reached it.
- [x] Animation ticks synced to the vertical blank. Same pointer motion,
      two runs each: stalled frames (2+ fields) 40 of 250 with the timer,
      10 of 181 synced. Remaining stalls are frames over 20 ms (backdrop
      re-capture when the hover label opens, first ease-out frame).
- [x] Review fixes: sync only while fields measure 19-21 ms (60 Hz modes
      keep the timer), timer fallback when fields stop. Final build 184,
      same motion: 131 even gaps, 3 stalls (2.2%).
- [x] Build 184 installed in WBStartup (build 180 in
      WBStartup/Disabled/DockAwesome-build180.old). The live config keeps
      Scale 1.95x; raise it in Settings to try 2.50x.

## Round hover labels (2026-11-08)

- [x] Labels are pills (semicircular ends) with antialiased rim and tip,
      instead of a radius-7 circle on a 17 px body that showed as an
      octagon with stepped edges.
- [x] Text centred on its ink: on the Vampire (build 186, "Files") the rim
      runs x 12-60 and rows 8-24, the ink x 22-50 and capitals rows 13-19:
      the same centre column and row, 9 px of surface each side (was 1.5 px
      left and 1 px high).
- [x] Codex: tall fonts (ttCourier 25) clipped an underscore; the padding
      now grows equally above and below for accents, descenders and the
      label's own ink. Build 186 installed (build 184 in
      WBStartup/Disabled/DockAwesome-build184.old).

## Folder Stack motion and close button (2026-11-08)

- [x] Open in Workbench glyph drawn with 4x4 antialiasing.
- [x] The stack grows from its pin with a strip reveal (open 160 ms, close
      130 ms, cubic ease-out); measured on the Vampire at open 176 ms in 8
      frames and close 140 ms in 6, about 1 ms per frame.
- [x] While open, the pin morphs into a flat close button with a chevron
      toward the screen edge, and back when it closes.
- [x] Codex review fixes: no clicks in the unrevealed part, the close button
      consumes its press in PiP mode, closes (not reopens) in native modes,
      and keeps its release across shutdown. Final Codex review clean.
- [x] Build 207 installed in WBStartup (build 186 in
      WBStartup/Disabled/DockAwesome-build186.old); click on the pin opens,
      click on the close button closes and stays closed. Old rollbacks
      0.178, build180 and build184 are locked by Workbench until a reboot.

## Glass close button and faster Folder Stack start (2026-11-09)

- [x] Close button: smoked glass (surface alpha 176, rim alpha 220) at 90%
      of an icon's extent via dock_cache_build_scaled (the cache fits any
      artwork to the box, so an inset in the source was undone), thin
      chevron (2.6% stroke) in soft white (212).
- [x] The pin morphs as far as the box is out (dock_ui_folder_progress),
      opening and closing; opening uses the closing motion reversed, 130 ms.
- [x] First frame after the click on the Vampire (299x315 box): 89-95 ms
      before, 60 ms now. Glass over the backdrop is finished per revealed
      strip (15-20 ms moved into the frames, about 2.7 ms each), the glass
      tint writes whole words (16-24 ms with a memcpy per pixel, 8 ms now)
      and runs after the motion. Remaining stages (ms): read 12.5, layer 9,
      labels 8.6, buffers 6, capture 4.6, first present 4.8, window 3.8.
- [x] Build 215 installed in WBStartup (build 207 in
      WBStartup/Disabled/DockAwesome-build207.old). The 90% button and the
      tint guard came after it, from the Codex review.

## /simplify across the codebase (2026-11-09)

- [x] Makefile: pattern rules for host tests, sanitize and probes (183 to
      139 lines); `make -n -B` gives the same commands.
- [x] Core: dock_convert565_damage returns the pixels converted (replacing
      the profile copy), dock_image_opaque_at shared by both input hit
      tests, DOCK_DRAG_THRESHOLD, the plate rect from dock_edge_panel_rect,
      the empty plate normalized once, a derivable renderer field removed.
- [x] Amiga: one backend selection and snapshot fallback, full_frame,
      buttons_engaged, zoom_levels, ticks_us, the v50 Intuition LVOs in
      amiga_abi.h, popup shape via make_folder_shape, a shared coverage
      blend, never-written profile counters and write-only fields removed.
- [x] Host tests, sanitizer and the 68080 build pass with no warnings.
- [ ] Not run on the Vampire yet: needs a real-machine check of PiP,
      snapshot fallback, popups and drag before installing.

## Simplify follow-ups and platform architecture (2026-11-09)

Plan: ~/.claude/plans/rosy-frolicking-lynx.md. Vampire stays the target;
the architecture must allow other platforms later at no performance cost
(per-frame or startup dispatch only, measured against the baseline).

Baseline, build 219 (simplify commit), Size 36, 12 pins, 800x96 PiP canvas:
- PiP sweep 8 px: render 7.8-7.9 ms (compose 5.6, conversion 2.2-2.3).
- Snapshot sweep 8 px: render 13.8 ms (compose 4.6, conversion 8.3 at
  100% of the canvas, portable kernels: AMMX is PiP-only today).
- Window moved behind the PiP dock: 21 changes, all full frames,
  render 24.6 ms (compose 16.3, conversion 8.3), decode 2.1, compare 1.3.
- --damage-check: 0 compose and 0 convert mismatches.

- [x] 1. Backdrop change damages only the changed box (PiP hot path).
      Window behind the dock: render 24.6 -> 13.6 ms, 1 of 22 frames full,
      changed box 42% of the canvas; --damage-check 0.
- [x] 2. Snapshot/opaque present only damage; AMMX kernels in every mode.
      Snapshot sweep: render 13.8 -> 5.9 ms, conversion 8.3 -> 0.64 ms;
      a window passing over the dock repaints fully; --damage-check 0.
- [x] 3. Cache stamps replace address-based invalidation; one morph buffer.
      Folder Stack open/close and reorder: --damage-check 0 over 204 frames.
- [x] 4. Folder Stack close button rebuilt only when its inputs change.
- [x] 5. Bugs: stale dock_screen after Workbench reopen; slider vs Save rules.
      Slider checked on the Vampire; a screen mode change is not yet tested.
- [ ] 6. Compositor in dock_compose.c; shared clamp helpers
- [x] 7. Hover label, Size slider, popup rows, drag mask (labels, popup row
      hover and the slider checked on the Vampire). Coverage copy: pending.
- [ ] 8. PlatformCaps probe, pure profile choice, surface backend interface
- [ ] 9. Install, clean RAM:, review and lessons
