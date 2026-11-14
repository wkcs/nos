# NK60 Lighting Effects Implementation Plan

**Goal:** Add six selectable NK60 effects and make the rainbow faster,
rightward and smoother.

**Architecture:** Retain the board renderer table and current wire IDs.
Use integer hue/envelope helpers for new stateless effects. Preserve phase
units while accumulating fractional updates in the NK60 display binding.

**Tech Stack:** C99/GNU11, existing C host fixtures, Python, CMake/Ninja.

## Task 1: Rainbow and registration

- [x] Extend `tests/host/test_nk60_light_effects.c` with rightward propagation
  (`render(x + 1, phase + 256) == render(x, phase)`), a period of 8192 phase
  units, mixed-color samples, bounded per-frame channel differences and
  continuity at wraparound. Run `python3 tests/host/run_nk60_light_effects_test.py`
  and confirm an assertion failure before changing the renderer.
- [x] Replace the three-sector rainbow with 16 mixed RGB anchors and integer
  interpolation, an 8192-unit period and 256-unit column offsets. Run the same fixture.

## Task 2: Six additional effects

- [x] Extend the same fixture to require ten unique named renderers, stable
  IDs 0–3, zone masks `(1U << 10U) - 1U`, animated/spatial variation, selected
  color scaling, repeatability and smooth wraparound. Confirm test failure.
- [x] Append cycle, wave, comet, gradient, ripple and twinkle renderers and
  enum IDs in `board/nk60-v2/nk60_light_effects.h`. Derive supported masks in
  `board/nk60-v2/nk60_keyboard.c` from the effect count. Run the fixture.

## Task 3: Smooth phase updates

- [x] Add checks in `tests/host/test_display_layers.c` for phases 85, 170,
  256 after three 10 ms refreshes at speed 255, and for low speed and phase
  reset on generation changes. Run `python3 tests/host/run_display_layers_test.py`
  and confirm failure before changing the binding.
- [x] Update `app/display/nk60_display_server.c` to accumulate elapsed
  milliseconds and remainder per layer with a 30 ms divisor, resetting both
  on activation or generation changes. Run the display and lighting fixtures.

## Task 4: Integration verification

- [x] Run `python3 tests/run_host_regressions.py` and
  `python3 -m unittest discover -s tests -p 'test_*.py'`.
- [x] Run `cmake -P tests/test_arch_flags.cmake`,
  `cmake -P tests/test_soc_headers.cmake` and `python3 scripts/check_layer_deps.py`.
- [x] Configure and build nk60-v2, qemu-stm32, zj-v3 and wl-001 using their
  presets; run `python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf`.
- [x] Review `git diff --check` and the final diff. Report firmware location,
  checks and any hardware-only validation remaining.

## Verification results

- Both focused C fixtures failed on the new requirements before implementation,
  then passed after implementation. Display fixture runs under ASan/UBSan.
- Full host regressions passed; Python discovery passed all 19 tests.
- Architecture flags, SoC headers and layer dependency checks passed.
- All four board presets configured and built without compiler warnings.
- QEMU STM32 boot, help, log statistics and RX burst smoke passed.
- Final diff whitespace check passed. Physical appearance awaits NK60 hardware.

## Follow-up: Web labels and finer rainbow colors

- [x] Reproduce two 彩虹波浪 options in the connected Web dropdown. Trace
  `web/src/components/capabilities/keyboardLightingModel.ts` in
  `/Volumes/nick-disk/projects/nk60/nk60_web`: `wave` was incorrectly an alias
  of `rainbow` for its label, color controls and preview renderer.
- [x] Add failing C tests for mixed tones, more than 1536 distinct RGB outputs,
  256-unit rightward column propagation and bounded neighboring-key differences.
  Add failing tests in `web/src/components/capabilities/keyboardLighting.test.ts`
  for distinct labels, wave color controls/envelope and matching rainbow samples.
- [x] Implement 16-color interpolation and wider rainbow bands in the board
  renderer and Web preview. Keep the phase period, IDs and names stable.
  Correct all six new Web labels while retaining unsupported-preview detection.
- [x] Pass both C fixtures, NK60 configure/build, all 86 Web tests and the
  production Web build. Vite reports its existing large-chunk advisory.
- [x] Reconnect the already authorized USB device after Web hot reload and
  verify one 彩虹波浪 option, a separate 单色流光 option and visible wave
  color controls. Preserve device settings; the new firmware is built, not flashed.

## Follow-up: Uneven breathing

- [x] Trace the original `/ 64` staircase and linear triangle brightness curve.
  User confirmed uneven fade rate and visible stepping within each breath.
- [x] Add failing C and Web regressions for perceptual brightness samples,
  equal rise/fall times, full-cycle monotonicity, synchronized pixels and wraparound.
- [x] Replace the triangle with a Q15 smoothstep envelope, approximate gamma 2,
  final RGB rounding and a 32768-unit period. Keep the cubic at full precision
  before rounding: premature truncation caused a 2-to-1 brightness reversal at
  phase 2782 in the first implementation, caught by the full-phase test.
- [x] Synchronize Web arithmetic, preserve effect IDs and update composition tests.
- [x] Pass lighting/display C fixtures, NK60 configure/build without compiler
  warnings, all 87 Web tests, production Web build and diff whitespace checks.
  Physical appearance still requires upgrading the keyboard firmware.
