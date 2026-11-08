# Plan: damage-region rendering for zoom animation

Goal: per frame, recompose and reconvert only the canvas regions whose inputs
changed, instead of the whole surface. Output stays byte-identical to the
current full-frame renderer.

Measured cost being attacked (Vampire V4, Size 72, scale 195%, 13 icons,
1280x168 canvas, `--pip --profile`, tasks/todo.md "Large-icon zoom speed"):
compose 24.1 ms + PiP conversion 12.3 ms, about 36 ms per frame. Every frame
recomposes and reconverts all 215,040 pixels.

Expected saving (host simulation in this preparation, real `dock_layout` and
`dock_animate_step`, pointer sweeps of 1-25 px per 20 ms tick): the union of
changed item, plate-end and overlay rectangles covers 30-52% of the canvas at
Size 72 and 24-40% at the live Size 42. A single bounding band would cover
57-93%, so damage is a rectangle list, not one band. PiP conversion must also
cover the previous frame's damage (double buffer). Plausible result: render
about 36 -> 20-24 ms in sweeps, with larger gains in slow motion and settle
tails. 20 ms is the aspiration, not a gate (see §3 D14).

## 1. Delivery split

| PR | Scope | Must not touch | Done when | Model | Review |
| --- | --- | --- | --- | --- | --- |
| 1 `perf-damage-rendering` | Golden checksums and `--bench-sweep` first (baseline), then renderer damage, clipped primitives, renderer-owned overlays, PiP per-buffer damage conversion, `--damage-check`, docs | Layout, animation, geometry, input, caches, AMMX kernels, Settings/UI, native (non-PiP) display upload | Host tests and sanitizers green, goldens unchanged, damage fuzz byte-exact, Vampire `--damage-check` 0 mismatches, matched bench before/after shows reduced `render_us_avg` at every speed | `opus` | Heavy |

One PR: the renderer API is not useful without the PiP integration, and a
core-only PR would be a scaffold. Heavy because the core claim is a pixel-exact
invariant (every changed input pixel is inside the damage) plus double-buffer
ordering on hardware.

## 2. Verified current-state facts

Rendering
- F1. `dock_render_oriented_mask` (`src/dock_core.c:1121`) writes every canvas
  pixel and every coverage byte each call: background rows outside the plate
  (`background_span`, `:1103`), the plate rows with corner coverage
  (`panel_corner`/`panel_edge`), kept glass rows or the fill-and-mix, the
  rim row `cy == 0`, the divider (`draw_divider`, `:1079`, only when
  `s->end_gap && count > 1`), then icons via `blend_runs` (`:890`) or
  `blend_mask` (`:846`).
- F2. `center_panel` adjusts the panel rect inside the renderer
  (`:1140-1149`); `dock_edge_panel_rect` alone does not give the drawn rect.
  The corner radius is `thickness/4` capped by `DOCK_PANEL_MAX_RADIUS`
  (`:1164-1169`).
- F3. Icons are clipped only to the canvas (`blend_runs`, `blend_mask`). The
  `pressed` flag darkens one item (`pressed_item`, one-based).
- F4. `DockRenderer` (`include/dock_renderer.h`) is zero-initialized by the
  caller; `dock_renderer_draw` (`src/dock_renderer.c:10`) allocates the glass
  and calls the core. `dock_renderer_backdrop_changed` (`:31`) clears glass
  `ready`; `dock_renderer_release` (`:36`) frees glass. Kept glass and the
  per-frame mix give identical bytes (`tests/test_core.c:905-914`).
- F5. Overlays are drawn by the caller after the renderer: running dots
  (`draw_running_dot`, `src/amiga_main.c:3027`, via
  `dock_blend_masked`) for every non-NULL icon whose item is `running`, then
  the dragged icon (`draw`, `src/amiga_main.c:3048`, lines 3066-3073), both
  setting coverage. Nothing else writes `app->canvas` (grep: only
  `dock_renderer_draw`, the two `dock_blend_masked` calls, alloc/free, and the
  read-only `--pip-dump`).
- F6. `DockSettings` has a `size_t` after five `int`s, so it has padding on
  64-bit hosts; do not `memcmp` it.

PiP output
- F7. `present_frame` (`src/amiga_main.c:2946`) converts the WHOLE frame every
  call: per row it mutates `app->coverage` in place (drag mask reveal,
  popup span or `dialog_clip`, `visible_width` tail), then `encode565` and
  `key565` into the back buffer. It returns early without converting when
  `pip_blocked_by_screen_or_menu(app)` is true.
- F8. `amiga_pip_back_buffer` (`src/amiga_pip.c:166`) returns
  `pixels[front^1]` (two buffers, `MEMF_CLEAR`, 64-byte aligned rows);
  `amiga_pip_present` (`:183`) flips `front` only on success and returns 0
  without flipping when suspended. A back buffer therefore holds the frame
  from two presents ago.
- F9. AMMX `encode565` works in 4-pixel quads, `key565` in 8-pixel octets,
  both with scalar tails (`src/ammx.s:149`, `:262`). PiP width is a multiple
  of 32 on every edge (`open_surface`, `src/amiga_main.c:1216-1219`).
- F10. Backdrop changes: `open_surface` (`:1267`), drag reference
  (`:2653`), and live capture decode (`:2816-2820`) each call
  `dock_renderer_backdrop_changed`.
- F11. Surface lifecycle: `close_surface` (`:1151`) frees canvas, coverage,
  PiP and calls `dock_renderer_release`; `open_surface` (`:1193`) reallocates.
- F12. Item caches change behind stable `Item` addresses: free (`:317`),
  replace (`:363`), build (`:965`), Settings swap (`:3569`, `:3588`). Reorder
  permutes `app->items` (`:3191`, `:3348`). `icons[i]` is
  `&app->items[row_item(app,i)].cache` (`draw`).

Measurement
- F13. `--profile` reports `render_us_avg`, `compose_us_avg` (render minus
  present), `conversion_us_avg` (includes the `amiga_pip_back_buffer` wait),
  and PiP `capture_compare_us_avg` (`profile_report`, `:2852`; counters in
  `ProfileStats`, `:191`). The "framebench" used for 58.7 -> 24.5 ms was never
  committed; there is no reproducible in-dock benchmark.
- F14. The pointer reaches hover via `pointer_inside` (`:1290`) and
  `update_target` (`:1393`), reading `app->screen->MouseX/Y`. Animation
  ticks run in `animation_step` (`:1530`); `event_loop` is at `:3739`.
- F15. Live config on the Vampire (`ENVARC:DockAwesome.json`): size 42,
  scale_percent 195, levels 33, bottom, pip, 11 paths plus the Folder Stack
  `Personal:Downloads`. Size 72 at 195% gives max 140 and a 1280x168 canvas.
- F16. tasks/todo.md "Deferred, with reasons" is stale: the tinted-plate cache
  (kept glass) and visible-span icon blending (runs) were implemented in
  c41bfa5. Partial compare (`frame_check`, plate-only `check` in
  `live_capture`) is implemented; its cost appears as
  `capture_compare_us_avg`.
- F17. No CI: no `.github/` directory. Local checks are the full gate:
  `make host-test host-sanitize verify`.

## 3. Locked implementation decisions

All PR 1.

- D1. Damage type in `include/dock_renderer.h`:
  ```c
  #define DOCK_DAMAGE_MAX 80
  typedef struct { int full, count; DockRect rect[DOCK_DAMAGE_MAX]; } DockDamage;
  void dock_damage_clear(DockDamage *d);
  void dock_damage_set_full(DockDamage *d);
  /* Clip to [0,width)x[0,height); ignore empty; merge with every rect it
     intersects into their bounding box, repeated until no two rects
     intersect; overflow sets full. */
  void dock_damage_add(DockDamage *d, DockRect r, int width, int height);
  void dock_damage_merge(DockDamage *dst, const DockDamage *src, int width, int height);
  ```
  Implemented in `src/dock_renderer.c`.
- D2. Clipped core: add `dock_render_oriented_clip(...same params as
  dock_render_oriented_mask..., const DockOverlay *overlays, int overlay_count,
  const DockRect *clip)`. It writes canvas and coverage only inside `clip`,
  with the same bytes a full render produces there. Every write is clipped:
  background spans, coverage memsets, glass copies, corner and rim pixels,
  the `cy == 0` rim loop, the divider, icons, overlays. Glass rows are still
  rebuilt for whole rows when `!ready[y]` (writes only the glass cache).
  `dock_render_oriented_mask` stays as a wrapper with no overlays and the full
  canvas clip, so existing tests compile unchanged.
- D3. `blend_runs` and `blend_mask` take a clip rect (destination
  coordinates) in place of the implicit canvas clip; `dock_blend`/
  `dock_blend_masked` pass the full canvas.
- D4. Overlays move into the renderer:
  ```c
  typedef struct { const DockImage *image; int x, y; } DockOverlay;
  #define DOCK_MAX_OVERLAYS (DOCK_MAX_ITEMS+1)
  ```
  `DockRenderer` gains `const DockOverlay *overlays; int overlay_count;` set
  by the caller before `dock_renderer_draw`, like `pressed_item`. Overlays
  blend after icons, in array order, with coverage, inside each clip.
  `draw` builds the list: dots in slot order, then the dragged icon.
  `draw_running_dot` becomes a function that returns the dot overlay.
- D5. Effective panel rect: one static helper in `dock_core.c` returns the
  drawn rect (with the `center_panel` mirror) and radius; the renderer body
  and the damage computation both use it.
- D6. `DockRenderer` keeps the previous frame: `valid`, canvas
  pixels/width/height, backdrop pixels and `captured`, coverage pointer,
  edge, `center_panel`, settings fields (compared field by field, F6),
  count, per slot the placement, the drawn image's `pixels` pointer (NULL
  when `icons[i]` is NULL or the level is invalid), the pressed flag, the
  effective panel rect and radius, and the overlay list with image `pixels`
  pointers. `DockDamage damage;` holds the damage of the last draw and is
  readable by the caller.
- D7. Damage rules in `dock_renderer_draw`:
  - full: `!valid`; canvas, backdrop, `captured`, coverage pointer, edge,
    `center_panel`, settings or count changed; panel cross-axis bounds
    changed; overlay count above `DOCK_MAX_OVERLAYS`.
  - per slot: placement, image pixels pointer or pressed flag differs ->
    add the old rect and the new rect.
  - panel ends: for each long-axis end that moved, add the strip from
    min(old,new) to max(old,new) extended `radius+1` inward, across the
    panel's full cross extent.
  - divider (when drawn): if slot `count-2` or `count-1` changed or the panel
    changed, add the box from the start of the earlier icon to the end of
    the later one (old and new), across the panel's cross extent.
  - overlays: for each index in the longer list, if missing in one list or
    the image pointer, image pixels pointer, x or y differs -> add the old
    and new image rects.
  - Then render each damage rect with `dock_render_oriented_clip` (one rect
    = the full canvas when `full`). Store the new state; `valid = 1`.
  The invariant reviewers must re-derive: a pixel outside the damage has the
  same backdrop, panel geometry, glass row, covering items with the same
  image and pressed state, divider and overlays as last frame, so its canvas
  and coverage bytes are unchanged.
- D8. `void dock_renderer_invalidate(DockRenderer *r);` forces the next draw
  to be full. `dock_renderer_backdrop_changed` and `dock_renderer_release`
  imply it. The app calls it after every cache free, replace, build or swap
  site in F12 and after any change to item order or count (pointer diffs
  alone are not trusted: a freed and rebuilt cache can reuse an address).
- D9. Coverage is renderer-only output. `present_frame` stops mutating
  `app->coverage`: masks (drag reveal, popup or dialog clip, `visible_width`)
  are applied to a per-row scratch copy of the coverage span before keying.
  The profile counters (`revealed`, `ui_revealed`) are computed on the
  scratch.
- D10. Portable damaged conversion in `src/dock_renderer.c`:
  ```c
  typedef void (*DockCoverageMask)(void *context, int y, int left, int right,
                                   unsigned char *coverage_span);
  void dock_convert565_damage(unsigned char *buffer, int stride,
      const DockImage *frame, const unsigned char *coverage,
      const DockDamage *damage, unsigned char *scratch,
      DockConvertRGB565 encode, DockKeyRGB565 key,
      DockCoverageMask mask, void *context);
  ```
  For each rect (or the whole frame when `full`), per row: widen the span to
  multiples of 8 pixels (start down, end up, clamped to the width; F9),
  copy the coverage span to `scratch`, apply `mask`, `encode`, then `key`
  with the scratch. Overlapping spans are harmless: encode plus key is
  idempotent per pixel.
- D11. PiP per-buffer damage in the app: `DockDamage pip_pending[2]`, both
  set full by `open_surface`. Each `draw` merges `renderer.damage` into both
  before the `pip_blocked_by_screen_or_menu` check, so skipped presents
  accumulate. `amiga_pip_back_index(const AmigaPip *)` (new,
  `src/amiga_pip.c`) returns `front^1`. `present_frame` converts
  `pip_pending[back]` and then clears it, whatever `amiga_pip_present`
  returns (F8: a failed present leaves the same back buffer, now current).
- D12. Mask changes force full conversion: `present_frame` builds a mask
  signature (drag reveal live, popup open plus bounds, `dialog_clip`). If
  any mask is active, or the signature differs from the last converted
  frame's, it converts the full frame. `visible_width` is constant per
  surface and is applied through the scratch every frame.
- D13. `--damage-check` (debug flag; needs `--pip` or works for compose
  only in native modes): allocates a reference canvas, coverage and 565
  buffer. After each damaged draw it renders a full frame with a second
  `DockRenderer` invalidated every frame, compares canvas and coverage, and
  (PiP) converts the full frame with the same mask callback into the
  reference 565 buffer and compares it with the back buffer before
  presenting. The profile report prints
  `Damage check: frames=N compose_mismatch=M convert_mismatch=K`. Without
  the flag nothing is allocated.
- D14. `--bench-sweep SPEED` (1-64 px per tick; implies `--profile`): after
  the first frame, drive a synthetic pointer through `pointer_inside` (an
  override read only while the bench runs): enter at the left plate end on
  the icon centre line, sweep to the right end and back at SPEED px per
  frame, leave the dock, run until the animation settles, print the profile
  report and exit. Each frame is paced by `WaitTOF()` before `draw` so the
  back-buffer retirement wait is never counted. Live capture is not polled
  during the bench (static backdrop). The profile report also prints
  `Damage: frames=N full=F compose_pixels_avg=P% conversion_pixels_avg=Q%`
  (both in normal `--profile` runs too).
- D15. Ship gate: merge only when every bench speed (3, 12, 25) shows lower
  `render_us_avg` than the matched baseline and both damage-check runs report
  0 mismatches. If the speed-12 reduction at Size 72 is below 20%, use the
  `Damage:` line (full-frame rate, pixel shares) to find the cause and fix it
  within this scope before delivering. If the reduction stays below 10%,
  open the PR without merging and report the numbers.
- D16. Native (snapshot, opaque) modes use damage composition too; their
  upload stays full-frame (`amiga_display_present` unchanged).

## 4. Quality obligations

- Q1. Goldens before code: the FIRST commit adds `test_render_goldens` to
  `tests/test_core.c`: FNV-1a checksums of canvas plus coverage for a fixed
  scenario set rendered with the CURRENT code: 3 edges x flat/captured x
  glass on/off x pressed none/one x `center_panel` 0/1 x `end_gap` 0/6 x
  pointer at rest, left end, middle, right end, plus running dots drawn the
  old way (`dock_blend_masked` after `dock_renderer_draw`). The table is
  generated by running the test once against unmodified rendering code and
  committed; later commits must reproduce it unchanged.
- Q2. Overlay equivalence: the same scenarios with dots passed as renderer
  overlays must reproduce the Q1 checksums.
- Q3. Damage fuzz, at least 20,000 frames across all 3 edges, captured and
  flat, with and without glass: random pointer walks (steps 0-40 px and
  jumps), enter/leave, pressed changes, `center_panel` toggles, count
  changes, overlays appearing, moving and disappearing, backdrop changes
  (with `dock_renderer_backdrop_changed`), icon slot swaps (with
  `dock_renderer_invalidate`, D8). Every frame: incremental canvas plus
  coverage equals a fresh full render byte for byte.
- Q4. Conversion double-buffer test with the portable kernels: two buffers,
  per-buffer pending damage, random skipped presents and failed presents;
  after each conversion the back buffer equals a full conversion of the
  current frame; also with a simple mask callback. Run in `test_core_be`
  too.
- Q5. `dock_damage_add` unit tests: clipping, empty rects, transitive
  merging, overflow to full, no two output rects intersect.
- Q6. On the Vampire: `--damage-check` runs with 0 mismatches for a
  `--bench-sweep 12` at Size 72 and a manual session covering hover, a
  Workbench drag over the dock, an internal reorder drag, the context popup,
  Settings open over the dock, and the three edges.
- Q7. Physical PiP check (lessons: screenshots can omit PiP): after the
  damage changes, the user-visible dock on the real display shows no stale
  trails, at the end of every sweep and after the popup closes. Record what
  was checked and how (amiga_screenshot cannot see PiP; use `--pip-dump`
  and the damage check, and state that physical scanout was not verified if
  no observer is available).

## 5. Review procedure (Heavy)

Severity: P0 catastrophic, security, data loss or corruption; P1
merge-blocking correctness defect, regression, broken contract or unmet core
requirement; P2 concrete non-trivial defect, missed requirement, edge-case
failure or missing test that could let a real defect through. Style,
naming, preference, optional refactors and speculative robustness are not
findings.

1. One `sonnet` self-review of the diff against §3 and §4; fix valid
   findings; do not loop.
2. Full verification (§6 step list) so review starts green.
3. Fresh-context Opus review, spawned as
   `Agent({ subagent_type: "general-purpose", model: "opus", prompt })`,
   never a fork. Re-derive from scratch only: (a) the D7 invariant for every
   input of `dock_render_oriented_clip` (glass, corners, rim row, divider,
   center_panel, overlays, pressed); (b) D11/D12 ordering: skipped
   presents, failed presents, buffer reopen, mask transitions; (c) clip
   arithmetic in `blend_runs`, `blend_mask` and the 8-pixel widening (no
   out-of-bounds, incl. side edges). Report only P0/P1/P2 with file:line and
   a concrete failure scenario, at most about 300 words plus tables. Fix,
   commit, re-run a fresh Opus review; stop after one clean round.
4. Codex: `codex exec review --base main --dangerously-bypass-approvals-and-sandbox`
   (installed: codex-cli 0.167.0). Fix valid P0/P1/P2; re-run only if code
   changed. If tokens are unavailable, skip and note it.
5. One fresh-context Fable final pass,
   `Agent({ subagent_type: "general-purpose", model: "fable", prompt })`,
   with exactly this prompt (fill the bracketed parts):
   > Read tasks/plan-damage-rendering.md §1-§4: those are the requirements
   > for PR 1. The complete PR diff is below [paste `git diff main...HEAD`,
   > or if too large: the changed-file list plus every hunk in src/,
   > include/ and tests/]. Opus findings and resolutions: [list]. Codex
   > findings and resolutions or skip reason: [list]. Vampire results:
   > [bench before/after table, damage-check lines]. Is this implementation
   > complete against the original requirements, and is there anything
   > blocking merge? Answer in at most 300 words.
   If Fable reports a blocker: fix, re-verify, run one new Fable pass.

Never run two heavy local jobs at once: review agents, Codex, the Amiga
build and Vampire runs go strictly one after another. Every wait (a reviewer,
a Vampire command, the PR state) is a blocking foreground call, never a
background wait that assumes it will be resumed.

## 6. Delivery procedure

1. `git fetch`; confirm local `main` has no unpushed commits; fast-forward
   `main` to `origin/main`; `git switch -c perf-damage-rendering`. Never
   work on `main`. This plan file is untracked on `main`; commit it on the
   branch.
2. Commit order: (1) Q1 goldens and D14 bench plus the `Damage:`
   profile line (counting full-canvas pixels at this point); (2) damage
   type, clipped core and overlays (D1-D8) with Q2-Q5; (3) PiP conversion
   (D9-D12) and `--damage-check` (D13); (4) docs. Commit after every review
   round; push nothing until the review loop is clean.
3. Baseline: build commit (1) with `make amiga`, measure it on the Vampire
   (§7 Vampire protocol), keep the binary in the scratch area as
   `DockAwesome-baseline`. After the final commit, measure the new build
   with the identical protocol, back to back, with the same programs
   running.
4. Gate: `make host-test host-sanitize verify` green, Q6 zero mismatches,
   D15 met.
5. Push once and open a ready PR (no CI, so no draft step). Title, for
   example "Render only changed dock regions during zoom". The body lists
   the before/after table, the damage-check lines, assumptions and
   deviations. Push and PR creation are pre-authorized.
6. Merge: AUTHORIZED by the user on 2026-11-08, only after the full review
   loop is clean and D15 is met, with `gh pr merge <n> --merge`. Afterwards
   verify `gh pr view <n> --json state,mergedAt` shows MERGED and that
   `git ls-remote origin main` equals the merge commit's SHA. There are no
   post-merge workflows (F17); state that in the handover.
7. Install (AUTHORIZED 2026-11-08): after the merge, build the merged
   `main`, move the current `ApolloOS:WBStartup/DockAwesome` and `.info` to
   `ApolloOS:WBStartup/Disabled/DockAwesome-<old version>.old`, copy the new
   binary and icon in (keep DONOTWAIT in the icon; check with
   GetDiskObject/FindToolType as lessons say), start it through Workbench
   (WBRun) with the live ENVARC config, and verify it runs. Then delete
   `RAM:` test files and configs from this work.

## 7. Standing constraints

- Language: code, docs, commit messages and PR text in English, in the
  existing terse doc style (docs/*.md). Replies to the user in Swedish.
- Shared Amiga: another Claude session also uses the Vampire. Before every
  Vampire batch, run `ListAgents` and message other local sessions with
  `SendMessage` that you need the machine (stopping the dock, PiP2 lease,
  installs), and wait for their reply or a clear idle state by blocking.
  Never stop, kill or restart a dock or program another session started
  without that exchange. Keep each Vampire batch short and finish it by
  leaving the machine as you found it (installed dock running).
- Vampire protocol: stop the installed dock with its own Quit (right-click
  the dock background -> Quit) or Escape with dock focus; it holds the PiP2
  lease. Start tests from `RAM:` with `--pip --config RAM:DockAwesome-test.json`
  (a copy of ENVARC:DockAwesome.json with `"size": 72`; record the item
  count). Redirect profile output from an AmigaDOS script (lessons). Bench
  runs: `--bench-sweep 3`, `12`, `25` at Size 72, plus `12` at the live Size
  42, for baseline and new build. After the batch, restart the installed
  dock through WBRun.
- File transfer: ApolloExplorer `acp`, not base64 (start
  `C:ApolloExplorerSrv` first; see memory note on the Qt workaround and the
  download path bug). Wrap `acp` in a bounded wait.
- Build before uploading; an old binary survives a failed build.
- Keep RAM: and WBStartup clean: rollback copies only in
  `WBStartup/Disabled` (one per replaced version), test files deleted when
  done.
- Docs in the same PR: docs/architecture.md (damage rendering in "Layout and
  memory"), docs/pip-backend.md (per-buffer damage conversion in "Updates
  and timing"), docs/verification.md (`--damage-check` and `--bench-sweep`),
  README.md options list and `usage()` text, tasks/todo.md (new section
  with results; correct the stale "Deferred" bullets per F16),
  tasks/lessons.md only for genuinely new lessons.
- Performance rules (AGENTS.md): measure before claiming, keep matched
  comparisons, distinguish implemented, built, verified and installed.

## 8. Assumptions (owner may veto asynchronously)

- A1. One PR rather than core-then-app: a core-only PR would be unused code.
- A2. Damage is a rectangle list merged on intersection, capped at 80;
  overflow renders full. Simulation shows a single band would lose most of
  the gain.
- A3. Any active drag reveal, popup or dialog mask converts the full frame;
  these states are rare and short.
- A4. Backdrop changes stay full-frame. Row-precise capture damage is a
  follow-up if the `Damage:` line shows many full frames in normal use.
- A5. The bench uses a static backdrop and `WaitTOF` pacing; it measures
  frame cost, not capture cost. `capture_compare_us_avg` is reported from a
  normal `--profile` hover session for the partial-compare item.
- A6. Conversion spans widen to 8 pixels to keep AMMX kernels on aligned
  quads and octets instead of probing unaligned LOAD/STORE behavior.
- A7. The installed build after merge is a local-counter build, not a new
  release name; "1.0 beta 1" stays the last release.

## 9. Handover contract

`tasks/handover-damage-rendering-pr1.md`, containing only:
- implemented scope;
- deviations from this plan, with evidence and rationale;
- assumptions made during implementation;
- verification commands and results (host tests, sanitizers, goldens,
  fuzz frame count, Vampire damage-check lines, bench table before/after for
  each speed and size);
- Opus, Codex and Fable findings with resolutions; Codex skip reason if any;
- PR URL, final PR state and merge SHA;
- installed version, rollback file location, RAM: cleanup done;
- "FINAL PR — no further kickoff".
