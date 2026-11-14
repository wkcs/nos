# NK60 lighting effects

User approved this design on 2026-11-14.

## Behavior

Keep IDs 0–3 (`off`, `static`, `breath`, `rainbow`) stable. Append six
effects: `cycle` (uniform spectrum cycle), `wave` (selected-color wave),
`comet` (selected-color head and fading tail), `gradient` (diagonal spectrum),
`ripple` (selected-color rings expanding from the center), and `twinkle`
(independent colored stars fading in and out).

Rainbow moves left to right through 16 mixed RGB color anchors, including
orange, amber, lime, mint, sky blue, indigo, violet and rose. Interpolate all
three channels using the full phase before rounding to 8-bit output.
One revolution takes 8192 phase units, approximately 0.96 seconds at speed
255 and 1.91 seconds at speed 128. Every column uses the same hue in every
row. Offset neighboring columns by 256 phase units, widening bands and halving
the maximum neighboring-column channel difference from 96 to 48. Keep
continuous wraparound, including at the 32-bit phase counter boundary.

The NLK Studio Web client at `/Volumes/nick-disk/projects/nk60/nk60_web/web`
must label `wave` as 单色流光 and `rainbow` as 彩虹波浪. The former uses
the selected color and the board's smooth brightness envelope. The latter
uses the same palette, timing and direction as the firmware. Keep labels
separate from the list of renderers supported by the Web preview.

The NK60 display binding advances phases on every display refresh using
elapsed milliseconds, retaining the existing speed + 1 phase units per
30 milliseconds. Carry fractional phase units between refreshes. Existing
wire structures remain unchanged. Breathing uses a 32768-unit period (within
0.4% of the original 32640), symmetric Q15 smoothstep and approximate gamma 2
brightness compensation. Evaluate the cubic before rounding to avoid
one-level reversals. Preserve full precision until scaling the selected RGB
color; use the identical arithmetic in the Web preview. All keys breathe in
sync, with gentle turnarounds at zero and peak brightness.

## Implementation boundaries

Keep integer-only, stateless per-pixel renderers in
`board/nk60-v2/nk60_light_effects.h`. Register effects in the existing table;
derive both zone masks from its enum count. Respect selected color for wave,
comet and ripple. Global brightness and layer composition stay in the display
binding. No allocations, new UAPI fields or changes to configuration format.

## Verification

Host tests cover old IDs, mixed-color samples, rightward propagation, period,
bounded color changes across frames and wraparound, spatial variation,
color scaling, repeatability, metadata lengths and zone masks. Display tests
cover fractional phase accumulation, speed endpoints and generation resets.
Run host regressions, Python tests, CMake checks, layer checks, all four board
builds and QEMU smoke. Physical LED appearance requires NK60 hardware.
For the follow-up palette and label correction, run the lighting/display
fixtures, NK60 build, full Web tests and production Web build, and inspect
the connected Web dropdown to verify exactly one 彩虹波浪 entry.
Breathing regressions cover every phase for monotonic rise/fall, mirrored
halves, color scaling, bounded frame changes, period and counter wraparound;
Web tests cover matching samples, synchronized keys, both effect aliases,
brightness and speed scaling.
