# Audio EQ Guide

## Overview

This project implements a hybrid 3-mode EQ pipeline in shared DSP code so both host prototype (play_macos) and MCU entry (play_mcu) use identical audio behavior.

The three mode rules are frequency based:

- Linear EQ: frequency <= 2 kHz
- Dynamic EQ: 2 kHz < frequency <= 16 kHz
- Normal EQ: frequency > 16 kHz

These thresholds are defined in `src/play_dsp_common.h`:

- `EQ_LINEAR_MAX_HZ` = 2000.0f
- `EQ_DYNAMIC_MAX_HZ` = 16000.0f

## Signal Path

For each frame and each channel, processing is done band-by-band in this order:

1. Biquad peaking EQ core (all bands)
2. Dynamic post-control (only dynamic bands)
3. Output write-back

The spectrum analyzer uses the post-EQ signal, so Web visualization reflects the final processed output.

## Mode Implementation Details

### 1) Linear EQ (<= 2 kHz)

Linear EQ still uses the same peaking biquad structure, but the user dB gain is remapped through a linear amplitude interpretation before filter coefficients are built.

Implementation idea:

- User gain in dB: `gainDB`
- Convert to a normalized linear amplitude ratio around unity:
  - `linearAmplitude = 1 + gainDB / EQ_MAX_GAIN_DB`
- Clamp to avoid invalid values:
  - minimum 0.05
- Convert back to dB for biquad coefficient generation:
  - `effectiveGainDB = 20 * log10(linearAmplitude)`

This makes low-mid adjustment feel smoother and more proportional for broad tonal shaping.

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

Public APIs:

- `play_dsp_set_eq_params(...)`
- `play_dsp_set_dynamic_params(...)`
- `play_dsp_get_dynamic_params(...)`

## Dynamic Parameter Ranges

Defined in `src/play_dsp_common.h`:

- Attack: `DYNAMIC_EQ_MIN_ATTACK` to `DYNAMIC_EQ_MAX_ATTACK` (0.01 to 0.50)
- Release: `DYNAMIC_EQ_MIN_RELEASE` to `DYNAMIC_EQ_MAX_RELEASE` (0.005 to 0.30)
- Threshold: `DYNAMIC_EQ_MIN_THRESHOLD` to `DYNAMIC_EQ_MAX_THRESHOLD` (0.02 to 1.00)
- Max Reduction: `DYNAMIC_EQ_MIN_MAX_REDUCTION_DB` to `DYNAMIC_EQ_MAX_MAX_REDUCTION_DB` (0 to 24 dB)
- Strength: `DYNAMIC_EQ_MIN_STRENGTH_DB` to `DYNAMIC_EQ_MAX_STRENGTH_DB` (1 to 36 dB)

`play_dsp_set_dynamic_params(...)` clamps all values to these ranges.

## Web API Contract

### GET /eq

Returns:

- static EQ limits and arrays (`minGain`, `maxGain`, `minQ`, `maxQ`, `freqs`, `gains`, `qs`)
- sample rate (`sampleRate`)
- dynamic parameter current values
- dynamic parameter min/max limits

### POST /eq

Accepts partial update payload. Any of the following fields may be present:

- `gains`: float array
- `qs`: float array
- `dynamicAttack`: float
- `dynamicRelease`: float
- `dynamicThreshold`: float
- `dynamicMaxReductionDB`: float
- `dynamicStrengthDB`: float

Updates are applied atomically under DSP mutex in `play_macos`.

## Frontend Interaction

The Web UI now has:

- Spectrum and EQ canvas
- Per-band gain and Q controls
- Per-band mode badge (`Linear`, `Dynamic`, `Normal`)
- Dynamic EQ control row with five sliders:
  - Dyn Attack
  - Dyn Release
  - Threshold
  - Max Red (dB)
  - Strength (dB)

Changing any dynamic slider sends real-time POST updates and immediately affects playback.

## Suggested Tuning Workflow

1. Shape low end and body using <= 2 kHz bands (Linear mode).
2. Add presence/definition in 2 kHz to 16 kHz bands (Dynamic mode).
3. Use > 16 kHz band for air, then reduce dynamic threshold only if upper-band harshness appears.
4. Start dynamic defaults from:
   - Attack: 0.12
   - Release: 0.02
   - Threshold: 0.18
   - Max Reduction: 12 dB
   - Strength: 18 dB

## MCU Notes

`play_mcu` uses the same shared DSP implementation, so these three modes and dynamic parameters are already available on MCU side.

To control parameters on device, implement `play_mcu_poll_eq(...)` and pass updated gains/Q values each block. If needed, add an MCU control path that calls `play_dsp_set_dynamic_params(...)` with host-provided values.

## Build and Validate

Build:

- `cmake --build build --target play_macos -j 4`
- `cmake --build build --target play_mcu -j 4`

Run host prototype (outside sandbox for audio + bind):

- `./build/play_macos`

Open Web UI:

- `http://127.0.0.1:8080`
