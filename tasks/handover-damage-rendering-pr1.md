# Handover: damage-region rendering, PR 1

## Implemented scope

Plan tasks/plan-damage-rendering.md §3 D1-D16:
- D1 `DockDamage` (80 rects, merge on intersection, overflow to full) in
  `dock_renderer.h/.c`.
- D2-D3 `dock_render_oriented_clip`: every canvas and coverage write is
  clipped; `blend_runs`/`blend_mask` take a clip; `dock_render_oriented_mask`
  is a full-canvas wrapper without overlays.
- D4 overlays (`DockOverlay`, `DockRenderer.overlays`): dots in slot order,
  then the dragged icon, built by `draw`; `running_dot` returns the overlay.
- D5 `dock_render_panel`: the drawn plate rect and radius, shared by the
  core and the damage computation.
- D6-D7 `DockRenderState` and the damage rules in `dock_renderer_draw`.
- D8 `dock_renderer_invalidate`, implied by `backdrop_changed` and
  `release`; app calls after cache free/replace/build/swap and reorders.
- D9-D12 scratch coverage masks (`pip_mask`), `dock_convert565_damage`,
  per-buffer `pip_pending[2]`, `amiga_pip_back_index`, full conversion for
  masked frames.
- D13 `--damage-check`; D14 `--bench-sweep`; `Damage:` profile line.
- D16 native modes compose with damage; upload unchanged.

## Deviations

- D5: the plate helper is `dock_render_panel`, exported in
  `dock_renderer.h` and implemented in `dock_core.c`, not static: the damage
  computation in `dock_renderer.c` must call it, and a static function in
  `dock_core.c` cannot be called from there.
- D12: no bounds signature. Any active mask (drag reveal, popup, non-empty
  dialog clip) converts the whole frame, and `pip_masked[2]` records whether
  each buffer's last conversion was masked; such a buffer converts in full
  once more. A single "last converted frame" signature would let buffer A
  (masked at frame N) skip its full conversion at N+2 because N+1 (buffer B)
  already matched. Bounds never matter, since masked frames are always full.
  Sonnet and Opus both judged it at least as strict as D12.
- D7: a changed corner radius or pixel backend also forces a full draw
  (both are stored per D6; the radius decides every corner pixel).
- D13: `--damage-check` implies `--profile`, because its line is part of
  the profile report.
- D14: the bench resets the frame and damage counters after the first
  frame and draws only when an animation step moves pixels (as the event
  loop does), so both builds count the same 640/166/84/106 frames.
- Q3: the renderer always uses glass when a backdrop exists, so the
  no-glass path is checked by redrawing each frame's damage rects with
  `dock_render_oriented_clip` and no glass into a second canvas.
- Review step 5: the diff (1,934 lines) was passed to Fable as a file path
  inside the plan's prompt instead of pasted.

## Assumptions

- Plan A1-A7 unchanged. The test configs used the live ENVARC settings
  (11 paths plus the Folder Stack, 12 icons) with size 72 or 42.
- Q6 "Settings open over the dock": the window was dragged over the dock
  with relative pointer motion (warped drags do not move it).
- Q7: no person watched the display. The damage check compares the PiP back
  buffer with a full conversion before every present; physical scanout of
  the new build was not verified.

## Verification

- `make host-test host-sanitize verify`: green (also run by Codex).
- Goldens: 384 checksums generated at commit 34dae0a with the unmodified
  renderer; unchanged at every later commit; also reproduced with dots as
  renderer overlays (Q2).
- Damage fuzz (Q3): 20,400 frames, 2,025 full, 22% of pixels redrawn, 0
  mismatches, also in test_core_be and under ASan/UBSan. Mutation checks
  of the rules (strip width, old divider, old icon and overlay rects,
  pressed flag) fail the tests.
- Q4 double-buffer conversion test: green in test_core and test_core_be.
- Vampire damage check (Q6): sweep Size 72 speed 12
  `Damage check: frames=167 compose_mismatch=0 convert_mismatch=0`; manual
  bottom 246, left 64, right 51 frames, all 0/0 (drag reveal 8, popup 4 and
  dialog 7 frames in the bottom session).
- Matched bench (render_us_avg, us; baseline 34dae0a, alternated per speed):

| Size, speed | Before | After | Change |
| --- | --- | --- | --- |
| 72, 3 | 35,894 | 32,960 | -8% |
| 72, 12 | 35,688 | 27,513 | -23% |
| 72, 25 | 35,319 | 30,951 | -12% |
| 42, 12 | 15,521 | 12,154 | -22% |

  D15: all speeds faster, speed 12 at Size 72 -23% (above 20%), 0
  mismatches: met. 86-95% of icon pixels are redrawn per frame because the
  row shifts; see tasks/todo.md.

## Reviews

- Sonnet self-review (diff against §3-§4, plus 48,000 extra fuzz frames
  under sanitizers): no P0/P1/P2.
- Opus, fresh context, (a) D7 invariant per input, (b) D11/D12 ordering,
  (c) clip arithmetic and widening: clean in the first round; no second
  round needed.
- Codex (`codex exec review --base main`): no findings; it ran
  `make host-test host-sanitize verify`, green.
- Fable final pass: complete against §1-§4, nothing blocking. Notes: commit
  this handover (done); speed 3 at -8% is explained by the row shift and
  meets D15.

## PR

Pushed once after the review loop; no CI and no post-merge workflows
(F17). The PR URL, final state and merge SHA exist only after this file is
pushed; they are in the session's final report.

## Install

After the merge: merged main built, the previous
ApolloOS:WBStartup/DockAwesome(.info) moved to
ApolloOS:WBStartup/Disabled/DockAwesome-<old version>.old, new binary and
icon copied in with DONOTWAIT checked, started through WBRun with the live
ENVARC config, RAM: cleaned. Exact versions are in the final report.

FINAL PR — no further kickoff
