# macOS plate and rim

The supplied 424 × 2306 video crop shows a rounded plate with a straight
inner edge. The middle reaches roughly x313 to x315. The rounded ends reach
about y240 and y2083. The crop cuts off the outer edge and screen gap.

The top-left curve fits a radius of about 30 video pixels. That is about
27% of the visible plate thickness. The threshold and compression make
this an estimate.

| Row | First visible plate pixel |
| ---: | ---: |
| 241 | 336 |
| 245 | 327 |
| 249 | 322 |
| 253 | 319 |
| 257 | 317 |
| 261 | 315 |
| 265 | 314 |

The rim follows the full shape. One sample across the straight edge reads
`0, 11, 47, 17, 15, 16, 16` in grayscale. The body is about 16 and the
rim softens over 1 to 2 video pixels. The black background hides the true
glass opacity.

Keep all four Amiga corners visible, preserve the 3 px screen gap, and
antialias the thin rim. Let icons grow past the plate without changing its
thickness. Check the result on the SAGA display.

See [macOS motion and spacing](macos-reference.md).
