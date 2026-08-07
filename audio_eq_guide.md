# Audio EQ Guide

## Overview

This project implements a hybrid 3-mode EQ pipeline in shared DSP code so both host prototype (play_macos) and MCU entry (play_mcu) use identical audio behavior.

It also supports optional DSP plugins. The phase analyzer plugin can be enabled or disabled at runtime.

It also supports optional low-frequency stereo crossfeed:

- below 150 Hz low-pass content
- add 30% of left low-band into right
- add 30% of right low-band into left
- processing position selectable: pre-EQ or post-EQ

The three mode rules are frequency based:

- Linear EQ: frequency <= 2 kHz
- Dynamic EQ: 2 kHz < frequency <= 16 kHz
- Normal EQ: frequency > 16 kHz

For low-frequency stability and musical behavior, the project also enables TPT-SVF for low EQ bands:

- TPT-SVF band processing: frequency <= 1 kHz

These thresholds are defined in `src/play_dsp_common.h`:

- `EQ_LINEAR_MAX_HZ` = 2000.0f
- `EQ_DYNAMIC_MAX_HZ` = 16000.0f
- `EQ_LOW_SVF_MAX_HZ` = 1000.0f

## Signal Path

For each frame and each channel, processing is done band-by-band in this order:

1. Filter stage (hybrid):
  - TPT-SVF bell path for low-frequency bands (<= 1 kHz)
  - Biquad peaking path for other bands (> 1 kHz)
2. Dynamic post-control (only dynamic-mode bands)
3. Output write-back

The spectrum analyzer uses the post-EQ signal, so Web visualization reflects the final processed output.

In current implementation, both pre-EQ and post-EQ analyzer curves are exported to Web UI.

## Mode Implementation Details

### 1) Linear EQ (<= 2 kHz)

Linear EQ uses linear gain interpretation before coefficient build, then goes through the active filter topology of that band:

- If band frequency <= 1 kHz: TPT-SVF bell section
- If band frequency > 1 kHz: peaking biquad section

Implementation idea:

- User gain in dB: `gainDB`
- Convert to a normalized linear amplitude ratio around unity:
  - `linearAmplitude = 1 + gainDB / EQ_MAX_GAIN_DB`
- Clamp to avoid invalid values:
  - minimum 0.05
- Convert back to dB for biquad coefficient generation:
  - `effectiveGainDB = 20 * log10(linearAmplitude)`

This makes low-mid adjustment feel smoother and more proportional for broad tonal shaping.

### 1.1) TPT-SVF for Low-Frequency EQ (<= 1 kHz)

Low-frequency bands are processed with a Topology Preserving Transform State Variable Filter (TPT-SVF), configured as a bell-style section.

Why TPT-SVF here:

- Better numerical behavior at low frequencies
- Smooth parameter response while adjusting gain/Q
- Good fit for future embedded/MCU portability

Core per-band parameters:

- `g = tan(pi * f0 / fs)`
- `k = 1 / Q`
- `A = 10^(gainDB / 40)`
- `h = 1 / (1 + g * (g + k))`

State variables (per band, per channel):

- `ic1eq`
- `ic2eq`

Sample processing shape:

- `v1 = h * (ic1eq + g * (x - ic2eq))`
- `v2 = ic2eq + g * v1`
- `bp = v1`
- `y = x + k * (A - 1) * bp`
- `ic1eq = 2 * v1 - ic1eq`
- `ic2eq = 2 * v2 - ic2eq`

In code, low-band selection is done by `bandFreq <= EQ_LOW_SVF_MAX_HZ`, with a per-band switch (`bandUseSVF`) that chooses SVF vs biquad at runtime.

### 2) Dynamic EQ (2 kHz to 16 kHz)

Dynamic EQ uses envelope tracking per band and per channel, then applies an adaptive attenuation when boosted bands exceed threshold.

Envelope tracking:

- `env += coeff * (abs(out) - env)`
- `coeff = attack` when signal rises
- `coeff = release` when signal falls

Reduction condition:

- only active when user gain for this band is positive (`gainsDB[band] > 0`)
- only active when `env > threshold`

Reduction amount:

- `over = env - threshold`
- `reductionDb = over * strengthDB`
- clamp by `maxReductionDB`

Apply to band output:

- `out *= 10^(-reductionDb / 20)`

This preserves clarity by controlling harshness in upper mids/highs while still allowing musical boost at lower levels.

### 3) Normal EQ (> 16 kHz)

Normal mode uses the standard peaking EQ biquad response with no dynamic post-control and no linear remap.

It is best suited to air and brilliance shaping where direct, predictable static EQ is preferred.

## Shared State and API

Core state: `play_dsp_state` in `src/play_dsp_common.h`

Dynamic parameters are stored in state:

- `dynamicAttack`
- `dynamicRelease`
- `dynamicThreshold`
- `dynamicMaxReductionDB`
- `dynamicStrengthDB`

TPT-SVF-related state is also stored per band/per channel:

- `bandUseSVF`
- `svfG`, `svfK`, `svfA`, `svfH`
- `svfIc1eq`, `svfIc2eq`

Public APIs:

- `play_dsp_set_eq_params(...)`
- `play_dsp_set_dynamic_params(...)`
- `play_dsp_get_dynamic_params(...)`
- `play_dsp_set_plugin_enabled(...)`
- `play_dsp_get_plugin_mask(...)`
- `play_dsp_copy_phase_metrics(...)`

Plugin bit definitions:

- `PLAY_DSP_PLUGIN_PHASE_ANALYZER` (phase/group-delay analysis)

Low-crossfeed control APIs:

- `play_dsp_set_low_crossfeed(...)`
- `play_dsp_get_low_crossfeed(...)`

## Dynamic Parameter Ranges

Defined in `src/play_dsp_common.h`:

- Attack: `DYNAMIC_EQ_MIN_ATTACK` to `DYNAMIC_EQ_MAX_ATTACK` (0.01 to 0.50)
- Release: `DYNAMIC_EQ_MIN_RELEASE` to `DYNAMIC_EQ_MAX_RELEASE` (0.005 to 0.30)
- Threshold: `DYNAMIC_EQ_MIN_THRESHOLD` to `DYNAMIC_EQ_MAX_THRESHOLD` (0.02 to 1.00)
- Max Reduction: `DYNAMIC_EQ_MIN_MAX_REDUCTION_DB` to `DYNAMIC_EQ_MAX_MAX_REDUCTION_DB` (0 to 24 dB)
- Strength: `DYNAMIC_EQ_MIN_STRENGTH_DB` to `DYNAMIC_EQ_MAX_STRENGTH_DB` (1 to 36 dB)

`play_dsp_set_dynamic_params(...)` clamps all values to these ranges.

## Web API Contract

### GET /spectrum

Returns:

- `sampleRate`
- `preBins` (EQ before)
- `postBins` (EQ after)
- `phasePluginEnabled` (boolean)
- `phaseDeltaDeg` (array, only meaningful when plugin is enabled)
- `groupDelayMs` (array, only meaningful when plugin is enabled)

### GET /eq

Returns:

- static EQ limits and arrays (`minGain`, `maxGain`, `minQ`, `maxQ`, `freqs`, `gains`, `qs`)
- sample rate (`sampleRate`)
- plugin state (`phasePluginEnabled`)
- low-crossfeed state (`lowCrossfeedEnabled`, `lowCrossfeedPosition`)
- low-crossfeed constants (`lowCrossfeedCutoffHz`, `lowCrossfeedRatio`)
- dynamic parameter current values
- dynamic parameter min/max limits
- dynamic curve metadata (`modes`, `dynamicReductionDB`)

### POST /eq

Accepts partial update payload. Any of the following fields may be present:

- `gains`: float array
- `qs`: float array
- `dynamicAttack`: float
- `dynamicRelease`: float
- `dynamicThreshold`: float
- `dynamicMaxReductionDB`: float
- `dynamicStrengthDB`: float
- `phasePluginEnabled`: float-like boolean (`0` or `1`)
- `lowCrossfeedEnabled`: float-like boolean (`0` or `1`)
- `lowCrossfeedPosition`: `0` for pre-EQ, `1` for post-EQ

Updates are applied atomically under DSP mutex in `play_macos`.

When `phasePluginEnabled` is `0`, phase metrics are not updated in DSP loop (lower overhead).

## Frontend Interaction

The Web UI now has:

- Spectrum and EQ canvas
- Pre-EQ and Post-EQ curves
- Dynamic reduction continuous interpolation curve
- Per-band gain and Q controls
- Per-band mode badge (`Linear`, `Dynamic`, `Normal`)
- Dynamic EQ control row with five sliders:
  - Dyn Attack
  - Dyn Release
  - Threshold
  - Max Red (dB)
  - Strength (dB)
- A plugin toggle control: `Phase Plugin` (`ON` / `OFF`)
- A low-crossfeed toggle control: `Low Crossfeed` (`ON` / `OFF`)
- A low-crossfeed placement selector: `Pre EQ` / `Post EQ`

Changing any dynamic slider sends real-time POST updates and immediately affects playback.

Changing Phase Plugin toggle sends real-time POST update and controls whether phase/group-delay analysis is running.

Changing Low Crossfeed toggle or placement sends real-time POST update and controls whether low-band stereo blending happens before or after EQ.

Note:

- The purple Estimated EQ curve is still a client-side approximation based on biquad equations.
- Because low-frequency bands use TPT-SVF and dynamic processing can attenuate output, estimated curve and measured post curve may differ.

## Suggested Tuning Workflow

1. Shape low end and body using <= 2 kHz bands (Linear mode).
  - Note: <= 1 kHz bands are internally processed with TPT-SVF.
2. Add presence/definition in 2 kHz to 16 kHz bands (Dynamic mode).
3. Use > 16 kHz band for air, then reduce dynamic threshold only if upper-band harshness appears.
4. Start dynamic defaults from:
   - Attack: 0.12
   - Release: 0.02
   - Threshold: 0.18
   - Max Reduction: 12 dB
   - Strength: 18 dB

## Phase Metrics Interpretation Guide

This section helps interpret `phaseDeltaDeg` and `groupDelayMs` returned by `GET /spectrum` when `Phase Plugin` is enabled.

### 1) What to focus on first

- Prefer `groupDelayMs` as the primary practical metric.
- Use `phaseDeltaDeg` as a secondary trend indicator.
- Judge by frequency region, not by one global number.

### 2) Practical risk levels (rule-of-thumb)

For low-frequency region (20 Hz to 200 Hz):

- Low risk: group delay <= 2 ms
- Medium risk: 2 ms to 6 ms
- High risk: > 6 ms

For low-mid region (200 Hz to 1 kHz):

- Low risk: group delay <= 1.5 ms
- Medium risk: 1.5 ms to 4 ms
- High risk: > 4 ms

For high region (> 1 kHz):

- group delay is usually less audible for weight/punch
- prioritize tonal quality and harshness control over strict delay minimization

### 3) Audible symptoms mapping

- Excessive low-frequency group delay:
  - bass feels soft or laggy
  - kick transient loses impact
- Strongly uneven phase trend around crossover-like areas:
  - image focus may feel less stable
  - transients can feel smeared

### 4) How to decide if it is a real problem

Use A/B steps:

1. Toggle `Phase Plugin` ON, observe `groupDelayMs` trend.
2. Keep same loudness, bypass EQ changes that create peaks in low-band delay.
3. If delay peaks drop and punch returns, treat as confirmed phase-related issue.

### 5) Fast mitigation actions

If low-end delay is too high:

- reduce low-band boost magnitude first
- lower Q on low-frequency bands (wider, gentler shaping)
- avoid stacking multiple adjacent low boosts

If dynamic section causes instability in upper range:

- increase dynamic threshold slightly
- reduce dynamic strength or max reduction
- slow attack and/or increase release moderately

### 6) Notes on this project implementation

- `phaseDeltaDeg` is computed as post-EQ minus pre-EQ phase at analyzer bins.
- `groupDelayMs` is numerically derived from phase slope versus frequency.
- Metrics are meaningful for trend comparison and tuning direction; do not treat them as laboratory-grade absolute measurements.

## MCU Notes

`play_mcu` uses the same shared DSP implementation, so these three modes and dynamic parameters are already available on MCU side.

The plugin framework is shared as well. MCU side can enable/disable plugins by calling `play_dsp_set_plugin_enabled(...)`.

To control parameters on device, implement `play_mcu_poll_eq(...)` and pass updated gains/Q values each block. If needed, add an MCU control path that calls `play_dsp_set_dynamic_params(...)` with host-provided values.

## Build and Validate

Build:

- `cmake --build build --target play_macos -j 4`
- `cmake --build build --target play_mcu -j 4`

Run host prototype (outside sandbox for audio + bind):

- `./build/play_macos`

Open Web UI:

- `http://127.0.0.1:8080`
