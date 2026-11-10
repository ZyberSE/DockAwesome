# Project lessons

## Communication

- A local build is "build N", not a version. Versions exist only for
  release packages (for example "1.0 beta 1").
- Reply to the user in Swedish, in plain words. Present comparisons as
  "icon one, two, three steps from the pointer" with sizes in pixels, not
  internal terms such as falloff, tail or neighbour index.

## Build and test

- Build successfully before uploading. An old binary can survive a failed build.
- Wait for the native window or gadget before sending input or reading saved
  preferences. A visible window alone does not prove the dock responds.
- Redirect the profiled command's output from an AmigaDOS script.
- Test each native path on its target OS. Measure full frames and individual
  stages. Elapsed time and competing-task throughput measure different costs.
- Clip image bounds before negating or subtracting coordinates. Test extremes
  under a sanitizer.
- Use the observed video for motion. Use physical PiP output for scanout.
  Screenshots and exported buffers can omit or miss display issues.

- Drive the dock and Workbench windows with relative motion
  (amiga_drag/amiga_click relative:true). Absolute warps did not move a
  window and missed the dock's hover and drag paths.
- Test the popup on the plate strip under the icons. A right-click on a pin
  without menu actions opens nothing by design, and zoom moves the icons, so
  a fixed point can hit a pin in one run and the plate in the next.
- Measure backdrop changes by running the dock from an Execute script with
  --profile, dragging a drawer window across the dock, then Break. Compare
  render_us_avg and the Damage line against the same scenario on the old
  build.
- Parallel worktree agents can start from an older commit: tell each to reset
  to the integration commit. Merge one branch at a time and run
  --damage-check on the Vampire. Bisect a regression by building each branch
  head on its own.

## Motion and appearance

- Zoom inward from a fixed baseline. Keep plate thickness fixed and let its
  ends follow the icon row. Middle hovers can center. End hovers can shift.
- Measure the zoom ratio, wave, and activation area. The reference is about
  2× and grows the icons 1, 2 and 3 pitches away by 0.86, 0.48 and 0.04 of
  the zoom. Stop the timer when the row settles.
- Keep the wave's weights summing to a constant at every pointer phase and
  floor the phase once for all icons. Otherwise the row's total growth
  breathes and icons outside the wave move by a pixel.
- Keep hover continuous across enlarged icons. Use icon alpha for button hits.
- Map the pointer between centers. Separate icon and gap mapping caused a
  7 px reversal in one five-pin test.
- Keep Q8 target centers separate from rounded cache widths. Round each icon
  on its own so distant icons stay steady.
- Compare current and target placements to decide when to stop ticking. Draw
  only on raster changes. Use a one-pixel minimum correction capped by the
  remaining distance to avoid a long fractional tail.
- Keep level 0 when Scale is 1.00×. Identical cache images need no animation.
- Measure gaps at the artwork, including transparent margins. When tighter
  zoom gaps are requested, keep rest spacing and compact as neighbors grow.
- Check every corner, the thin rim, icon centering, and the screen gap.
  Preserve corner samples when adjusting the straight rim.
- Keep the right PiP transport padding separate from the 3 px screen gap.
- Compare original PNG alpha with the decoded icon before changing scaling.
- Use a Vampire screenshot as the baseline for Workbench icon comparisons.
- Keep running dots small and sharp, near the screen-side icon baseline.

## PiP and Intuition

- Keep layout, pixel kernels, and screen output separate. Vampire and SAGA
  are hardware targets. Snapshot and opaque modes support other window stacks.
- Probe native alpha support before relying on it. Simple Refresh does not
  retain hidden pixels. Keep the input window clear of visible PiP artwork.
- Wait for `FirstScreen` after `ScreenToFront`. Check known source colors.
- Screen movement can require a new PiP frame even when source bytes match.
- Coalesce capture deadlines. Measure native refresh and shape updates
  separately from pixel composition.
- Inspect the locked layer chain for depth. `Screen.FirstWindow` is creation
  order on ApolloOS.
- Validate source and physical alignment with window edges crossing the overlay.
  Check calibrated registers if the rim is missing on the display.
- Keep DMA rows aligned to 64 bytes, in 32-pixel width steps, with zero modulo.
  A 336 px test showed stripes. Retest physical output after geometry changes.
- Pass a full `ULONG` count to ApolloOS text calls. A classic `UWORD` inline
  can leave stale upper bits. Check generated register writes.
- If a task stays in a library call after Ctrl-C, use a user-approved restart.

## Workbench input and drops

- Prepare the temporary receiver before Workbench locks its layers. Capture
  the clean desktop first and backfill newly exposed regions from that image.
  Skip normal layer and bitmap reads during the gesture.
- Check Workbench port ownership and client bounds. Read selection bounds
  between gestures. The selection hook can block on an icon press.
- Test fast empty-desktop clicks and stationary holds. Keep the idle receiver
  shape empty and avoid a visible marker.
- Preserve the sampled release position. USB events can combine motion with
  a press or release, unlike basic automation.
- Keep the insertion gap until the AppMessage arrives. Verify the original
  desktop icon and saved pin order through an add-and-remove test.
- Check for duplicates and enough zoom space before reusing the live surface.
  Keep the surface when the row fits to avoid a visible restart.
- Keep internal reorder zoom active. Incoming drops use the resting sizes.
- Verify the full right-button pair, including a held press on an icon or
  outside an open popup. Preserve independent ownership for the popup.
- Test the click while Workbench has focus and while the dock has focus.

## Settings and menus

- Use the click to place the context menu. The PiP rectangle includes clear
  padding. Check the visible artwork over the menu's actual span.
- Sample the desktop for menu glass and mask the overlay inside its shape.
  Mask owned dialogs as they move and refresh the old area.
- Test Settings on the machine. Keep the window and controls compact.
  Center button labels by glyph bounds and use equal opposite padding.
- For GadTools MX labels, set `PLACETEXT_RIGHT` and set the heading
  separately with `GTMX_TitlePlace`.
- Use real relative motion when automation cannot move a slider knob.
  Verify the value and saved file. Persist the scale factor separately.
- Keep decorative native windows untitled. Patched Workbench can show titles
  for borderless windows.
- Keep the one configured Folder Stack separate from regular drawer pins.
  Check its icon at display size and test navigation and saving.
- Test folder glass over a window edge during motion and after close.
  Sample the backdrop at the destination. Simple Refresh can repaint on close.
- Use JSON for preferences. Preserve the user's file during a format change.

## Open items

- Track the object that stays open. Shell can outlive its Workbench launcher.
  Drawers and volumes use Workbench's open drawer list.
- Match a document separately from its default tool. Check that closing the
  actual item clears the dot. Test unpinned app discovery separately.
- Check Shell close dispatch separately from its indicator. ApolloOS can
  route raw console close through the active window.

## Transfers and benchmarks

- After `gh pr merge`, fetch before fast-forwarding and building main: a
  stale origin/main built the old code once.

- Transfer files with ApolloExplorer `acp`, not base64. Start
  `C:ApolloExplorerSrv` on the Amiga first. If `acp` cannot load Qt, run a
  copy next to a `Frameworks` link to the app bundle and set
  `DYLD_FRAMEWORK_PATH`. Uploads to `host:RAM:` work; downloads keep only the
  text before the second colon, so fetch a volume root.
- Benchmark cache builds with the real decoded pins and live settings. A
  synthetic icon overstated translucent pixels and hid where time went.
- 64-bit math in per-tap code becomes library calls on the 68k.
- When moving code to another file, diff the m68k disassembly per function.
  GCC had inferred that a same-file callee was pure; across files it
  reloaded fields after the call until the declaration said so.
- Diff `gcc -S` output, not objdump: Amiga objects hold several code hunks
  with overlapping addresses, so objdump assigns code to the wrong symbol.
  In the single file GCC put draw, the event loop and live_capture in
  .text.unlikely (optimized for size); moved behind a function table they
  compile for speed. Separate that from real changes with a diagnostic
  build that adds `__attribute__((cold))` and `-fno-inline`.
- Give delegated native fixes an independent review before deploying.
  Fixes for blocking and bounded waits can move the risk to exit time:
  never exit while the system can still signal the task or call our code.

## Icons

- Match the pinned icons: 48x48 PNG icons whose artwork fills about 44x44,
  with a selected image (ApolloOS darkens it and adds an active detail).
- Show distinct design options side by side before installing an icon.
  Use Workbench colors; neon colors read as non-Amiga.
- At 48 px, use one bold symbol with a strong silhouette that fills the
  canvas. Small scenes (plates, rows, shelves) read as clutter.
- After a rejected design, offer several distinct concepts at real size on
  the real backdrop instead of iterating on the same idea.
- Base the program icon on the actual dock. Keep the Amiga reference small.
- A WBStartup program that keeps running needs the DONOTWAIT tooltype,
  or Workbench stalls startup with "has not yet returned" and the later
  WBStartup programs (AmiMCP included) never start. Check it with
  GetDiskObject/FindToolType on the installed icon.
- Time outline (diskfont) fonts cold, right after a reboot: a warm cache
  hid an 11 s open. Prerender static UI text instead of opening outline
  fonts on demand.
- Probes that draw text must use the ULONG Text/TextLength wrappers from
  amiga_text.h; the NDK UWORD inlines hung a probe on ApolloOS.
- The ApolloOS source in ~/GitRepos/ApolloOS is older than R9.5 (Intuition
  LVOs differ by one slot). Verify OS behavior with a probe on the target.
- Workbench reset notifications on ApolloOS come from screennotify.library
  (AddWorkbenchClient, type 3), not Intuition's SNOTIFY_BEFORE_CLOSEWB.
- A framebench win on malloc buffers does not carry over to the PiP buffer:
  measure conversion changes in the real dock with --profile before keeping
  them (a fused kernel 30% faster in the bench was 9% slower live).
- Workbench-started programs do not appear in Status. Before blaming a stale
  PiP2 lease, check for a dock started from WBStartup.

- Installing a dock build on the Amiga: quit the dock, then Rename the WBStartup binary to Disabled/ BEFORE copying (Workbench keeps a WBRun binary locked: Copy fails with "object is in use"). Run install steps one call at a time and check each result before deleting the uploaded copy or starting WBRun; a parallel batch restarted the old build and deleted the new upload (2026-11-09).
- Never run `Avail FLUSH` through the Amiga agent on the Vampire: on
  ApolloOS with 1.5 GB it did not return, Break did not stop it, and
  afterwards neither the agent nor new programs started. Read free memory
  from the Workbench title bar instead (2026-11-09).
- Read ../amiga-refs (RKM Libraries/Devices 3rd ed., HRM, Apollo docs)
  before building on OS behaviour, not only third-party source: its
  Workbench chapter showed that Workbench runs a project with the project
  icon's stack and TOOLPRI, which the first launcher had wrong. The NDK 3.2
  R4 autodocs and release notes are now in ../amiga-refs/amigaos-3.2; the
  wb release notes showed $STACK: and the CLI/REXX tool types, which I had
  wrongly called absent from Workbench 3.x (2026-11-09).
- Screenshots do not show the SAGA PiP layer. The installed dock (PiP, from
  WBStartup) is invisible in them, and its hover labels ("Music",
  "Internet") look like stray text. Put a test dock on another edge, or quit
  the installed one, before clicking: test clicks at the bottom started
  AmigaAMP through the installed dock (2026-11-09).
- Before blaming the dock for a leaked lock (or other resource), repeat the
  launch with a control: the same tool through Workbench, and a tool with no
  C startup. A drawer "leak" here came from the libnix test tool on
  ApolloOS; Workbench left the same lock (2026-11-09).
- RX treats a quoted first argument as ARexx source, not a file name. Run
  a script by path with RX "call '<path>'" (2026-11-09).
- AmigaOS 3.9 Intuition holds the screen's LayerInfo lock through window
  drags. A poll that gives up on AttemptSemaphore stalls for the whole
  drag; skip only the part that needs the lock (Coffin PiP probe,
  2026-11-09).
- PiP and other overlays are invisible to screenshots: ask the user what
  the monitor shows, and test the hardware path alone (a bare register
  probe) before debugging the dock around it. The Coffin PiP "failure"
  was the dock's own visible-width mask, not the hardware (2026-11-09).
- Test Amiga interactions myself (amiga_drag, amiga_click, --profile
  counters) before asking the user; ask only for what the agent cannot see
  (the PiP layer on the monitor, a physical mouse, reboots). The user had
  to say so after a run of "please try" requests (2026-11-09).
- AmigaOS 3.9 Workbench cancels an icon drag if windows are moved or
  restacked as it starts: a receiver that moves onto the dock at the press
  stalled input about a second and got no AppMessage. A drop target there
  must already be in place before the press (2026-11-09).
- After uploading a build, quit every running dock and check Status shows
  exactly one RAM:DockAwesome before asking anyone to look: a second PiP
  dock finds the PiP2 lease taken and quits silently under Run >NIL:, so
  the old build kept running and the user tested it (2026-11-09).

## Prove a visual cause before building its fix (2026-11-09)
- I blamed PiP1 on/off toggling for a doubled dock image seen in the user's video and built "PiP1 stays on". The next video showed the same doubling, so the cause was a guess.
- What was ruled out on Coffin: compose/convert mismatch (--damage-check: 0/0), a dock image in the captured backdrop (--pip-dump source is clean).
- Rule: when only the user can see the symptom, first confirm with them what exactly they mean (send a still from their own video), then change code.
- Go through the user's video frame by frame (ffmpeg crop + tile at full 60 fps) before guessing; the 3 fps sheet hid detail.

## SAGA overlay order: PiP2 lies over PiP1 (2026-11-09)
- On Coffin (core 11900+) PiP2's opaque pixels cover PiP1. I read a blurry video frame the other way and moved the dragged icon to PiP1 only; the user saw it vanish behind the dock's plate.
- Rule: the dock (PiP2) draws whatever lies on its surface; PiP1 only adds what lies beyond it, as the same picture so the two meet without a seam.

## Cancelling a Workbench drag in tests on Coffin (2026-11-09)
- A right-click to cancel the drag opened a MagicMenu menu; Esc then closed the active drawer window. Reopen it with ARexx `WINDOW "<path>" OPEN`.
- Rule: end a test drag by releasing over the icon's own window (or its start point) instead of the right button.

## Workbench draws its drag image after the pointer moves (2026-11-10)
- A per-sample log on Coffin showed the pointer at its new place while the screen still held the drag image at the old one; it caught up a sample later. Shifting the reveal to the pointer therefore made it worse, and holes in PiP2 always lag the image.
- Rule: over the dock, paint the drag image's sampled pixels into the PiP buffer instead of revealing the screen; log per-sample data before choosing between such fixes.
- Check the test itself before reading its numbers: my press missed the moved icon and started a lasso, and I read the lasso's pixel count as a regression. Take a screenshot during each scripted drag.

## Probe a capability by using it (2026-11-10)
- On Coffin, rgb_rastport (graphics LVO table size) and alpha_text (cybergraphics opens as V51) both said yes, but RGB pens drew palette orange and BltTemplateAlpha drew nothing: an orange Folder Stack frame and an invisible menu text.
- Rule: a capability that draws is probed by drawing one pixel into a bitmap like the screen's and reading it back.

## Leave the user's dock running after a test (2026-11-10)
- After a profiled test run I sent Break and asked the user to film the dock; it was not running ("Ser inte dockan").
- Rule: every test that stops the dock ends with Run >NIL: of the normal command and a Status check, before any request to the user.

## Two SAGA overlays must not share rows with a wide one (2026-11-10)
- With the dock at the bottom (PiP2 992 px wide) on Coffin's 24-bit screen, a tooltip in PiP1 scrambled the whole dock; the PiP1 overlay was 96 rows tall and reached over the dock's rows. Beside a 96 px side dock the same overlay showed fine.
- Keeping PiP1 off the dock's rows (even 4 rows clear) did not help: every PiP1 label or drag still scrambled the 992 px dock. Measured fine: 128 + 96 px.
- Rule: use a second overlay only where its combination was seen working (PiP1 beside a narrow dock); test layout changes (edge, size) on the hardware, not only on the edge that was verified; after two failed hardware guesses, fall back to the known-good path instead of a third.

## Probes must tolerate the screen's precision (2026-11-10)
- The RGB pen probe compared the read-back pixel exactly; on ApolloOS's 16-bit screen the colour comes back with 5-6 bits per channel, so it said "no" and the UI lost its RGB colours. Compare at the screen's precision (RGB565 masks).
- Run --caps on every OS after changing a probe, not only on the OS the change was made for.

## A fix for "all OSes" covers every branch of the code (2026-11-10)
- The user said the drag-out edge fix applied to all versions and OSes. I fixed the PiP1 and window paths on Coffin, but the shaped (ApolloOS) branch of update_drag_preview kept its own rule: no window until Remove armed, so the dock's icon was cut at PiP2's invisible edge. My ApolloOS regression said "drag preview works" from screenshots, which do not show PiP.
- Rule: when a fix must hold on every OS, list every branch that selects behaviour by caps (shaped, PiP1, window) and change or rule out each one. A screenshot never verifies what PiP shows; ask the user for that part.

## Without shapes, the armed drop receiver must not cover windows (2026-11-10)
- Filmed on Coffin (IMG_2484): a stale band over a Workbench list window where it reached into the dock's box. The receiver arms with the buttons up (pointer on a Workbench icon, anywhere in a drawer window, and stays armed on the dock), went to the front over the whole box and backfilled from a snapshot; whatever changed under it (wheel scrolling, a move) showed stale through PiP's keyed headroom.
- A screenshot shows this (the receiver is in the screen bitmap): on AmigaOS 3.2.3 a window moved into the box was cut at the box's top edge.
- Rule: a helper window that waits on the dock's box stays behind the other windows (WindowToBack); check windows near the dock with the pointer inside a Workbench window after any change to arming.
- On AmigaOS 3.2.3 a Run process is not found by task name: use Status COMMAND=<path> and Break <n>.

## See the screen mid-drag before handing a drag fix over (2026-11-10)
- Twice the user saw the same drag fault after my fix: the drawn pin was allowed one try per gesture, so leaving the dock a second time in the same drag fell back to the window preview. Counters (opens, places) said "drawn" and I believed them.
- What showed it: holding the button with amiga_button, moving with relative moves (absolute warps did not register as a press on the left-edge dock), and a screenshot at each step; the window preview's grey Remove label gave it away.
- Rule: for any drag change, test out, back in and out again in one gesture with the button held, and look at each step. Count only work that is needed (a still pin was redrawn 1000 times).

## SAGA PiP rows wrap their last columns; composite before claiming a fix (2026-11-10)
- A left PiP dock showed pixels of a dragged pin at the screen's left edge and lost its own last columns: SAGA shows the last columns of a PiP row at the start of the next. The right dock's keyed tail had already been found; the left dock lacked it. Calibration (+17/-1) was right; my guess that it was wrong cost a round.
- What finally showed every fault: the button held, the pin stepped across the edge in small steps, and each screenshot composited with that frame's PiP front buffer (--pip-dump). Do this before handing over any change at the PiP/screen seam, and keep the steps small, as the user asked.
- Programs started with Run >file lose their stdout here: run them from an Execute script, and have probes also write results to a file of their own.

## Native menus: MagicMenu holds IntuitionBase and P96's bitmap lock (2026-11-10)
- On Coffin a desktop right-click (MagicMenu) hid the dock for the whole menu. Painting the menu over the dock instead (screen diff, as for Workbench drag images) blocked the dock: LockBitMap on the 24-bit screen waits until MagicMenu closes, and the status polls' LockIBase did too. MagicMenu's menus are not layers on the screen (the LayerInfo lock stayed free).
- Rule: read nothing from the screen and take no Intuition lock while a native menu is held. Decide once per menu from the pointer whether it can reach the dock (256 px), hide PiP only then, and let the loop wait for the menu.
- Testing menus with the agent: a held right button can leave MagicMenu in sticky mode with the pointer over items like Power > Reboot now. Move away and close with Esc; Ctrl-C to the dock is answered only after the menu closes (cleanup needs Intuition).

## SAGA PiP survives a reboot (2026-11-10)
- A "black box at ApolloOS boot" (twice) and, photographed on AmigaOS 3.2.3, Coffin's last dock frame at boot: PiP stays on through a reboot, showing the previous system's buffer (black once that memory is cleared). My first guess (window backfill) was reverted unmerged.
- Rule: ask what exactly is seen (shape, content, timing) before guessing at a boot-time artefact; a photo of the stale frame identified it at once.
- The dock turns both units off at start (unless leased), on Ctrl-Amiga-Amiga (keyboard reset handler), and --pip-off exists for S:User-Startup.

## Release notes list only what has run on hardware (2026-11-10)
"Works with Directory Opus 5 as the desktop" went into the 1.0b2 notes from a
commit title, while tasks/todo.md said "Not run: DOpus 5 listers; needs AmiKit".
- Before writing a release-notes line, find its verification in tasks/todo.md
  (or the commit body). "Built, not yet run on hardware" stays out.
- Commit titles describe intent; the verification notes describe what is true.
