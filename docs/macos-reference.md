# macOS Dock reference

The supplied recording, `Monosnap screencast 2026-11-06 19-11-35.mp4`, is
39.13 seconds at 60 fps. It shows a 424 × 2306 px crop of the right Dock.
The measurements below use video pixels.

## Shape and zoom

The plate keeps a straight inner edge and constant thickness. Nearby icons
grow toward the desktop. Distant icons keep their size and move along the row.
For a middle hover, both plate ends move about 159 px.

| State | Plate span at x410 | Height | Midpoint |
| --- | --- | ---: | ---: |
| Rest, 25.000 s | y245 to y2079 | 1,835 px | y1162.0 |
| Zoom, 0.000 s | y86 to y2237 | 2,152 px | y1161.5 |
| Zoom, 10.000 s | y86 to y2237 | 2,152 px | y1161.5 |

The Nova artwork grew from about 52 to 102 px wide. The Finder artwork stayed
58 px high in the same frames. In the zoom wave, Photos, Discord, Fusion,
Nova, Docker, and Kitty reached about 80, 112, 132, 132, 116, and 88 px.
Resting artwork measured about 66 px.

DockAwesome uses a smoothstep wave over four resting icon pitches on each
side. At a shared midpoint, the factors are about 1.04, 1.32, 1.68, 1.96,
1.96, 1.68, 1.32, and 1.04 before rounding.

## Timing and pointer placement

A clear entry from 0.750 to 0.917 s shows these plate heights:

```text
1835, 1839, 1863, 1910, 1970, 2003, 2089, 2112, 2143, 2152
```

A clear exit from 0.117 to 0.283 s shows these heights:

```text
2152, 2139, 2104, 2051, 1988, 1955, 1898, 1856, 1837, 1835
```

Other clear exits take 150 to 183 ms. Use about 160 ms and keep the transition
running as the pointer moves. Only the visible plate starts hover. Keep
hover active across the gaps as the icons grow.

The Finder hover at 4.25 s shifts the plate midpoint by about 63 px.
The pointer stays at nearly the same fraction of the icon width,
19/80 at rest and 36/152 at zoom. The ChatGPT hover near 18 s shows
similar motion. Middle hovers can stay centered. End hovers move the row.

## Spacing and rounding

The Finder and Launchpad screenshot suggests about 0.32 artwork widths for
end padding and 0.30 for icon gaps. DockAwesome uses 8 px of end padding,
a 5 px resting icon gap, and an 11 px default radius.

Visible video gaps grow with the icons. Fusion to Nova grows from about
22 to 36 px. Nova to Docker grows from 22 to 33 px. Finder to Launchpad
grows from 21 to 33 px. Shadows and antialiasing add about 1 to 2 px of
uncertainty.

A native five-pin capture measured artwork gaps of 9, 11, 12, and 11 px
at rest and 11, 17, 19, and 15 px at zoom before gap compaction. The current
layout narrows the box gap from 5 to 3 px as neighbors grow.

Mapping the gap separately caused a 7 px edge reversal at x208 to x213
on a 384 px dock. Map the pointer continuously between icon centers.
This reduced the reversal to 1 px of rounding. Use Q8 sizes and gaps, then
round each icon separately. This removed the measured distant-icon jumps.

## Folder Stack

The macOS 26.6.2 grid starts with a smaller scale and zero opacity. One
Core Animation transaction moves it into place. The normal duration is
250 ms. Shift uses a five-second slow-motion path. Reduce Motion skips
the transform. [Apple Dock guide](https://support.apple.com/guide/mac-help/mchl231f08fb/mac)
[Core Animation defaults](https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/CoreAnimation_guide/AnimatableProperties/AnimatableProperties.html)

The 3428 × 2914 capture shows a 246 px pitch, 174 px folder artwork, and
about 34 to 39 px of outer padding. Use one line per name and keep the end
when truncating. [Apple middle truncation](https://developer.apple.com/documentation/appkit/nslinebreakmode/bytruncatingmiddle)

DockAwesome uses the dock's resting icon size and starts the opening
animation at 87.5% scale. Anchor the animation at the popup tip.
[Core Animation timing](https://developer.apple.com/documentation/quartzcore/camediatimingfunctionname/default)
