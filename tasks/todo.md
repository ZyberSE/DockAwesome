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
