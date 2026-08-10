#ifndef EQ_LOW_SEQ_H
#define EQ_LOW_SEQ_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * eq_low_seq
 * ----------
 * A low-frequency parametric EQ module focused on <= 500 Hz, with emphasis on
 * < 150 Hz bass bands.
 *
 * Design goals:
 * - Effective narrow-band gain at selected bass frequencies.
 * - Minimal algorithmic latency (IIR, effectively 0 ms algorithmic delay).
 * - Keep phase shift as small as practical by adaptive Q limiting modes.
 *
 * Notes:
 * - Any real-time narrow-band EQ must trade phase linearity for latency.
 * - This module uses minimum-phase peaking biquads for very low delay.
 */

#define EQ_LOW_SEQ_MAX_BANDS 8
#define EQ_LOW_SEQ_MAX_CHANNELS 2
#define EQ_LOW_SEQ_MAX_FREQ_HZ 500.0f

typedef enum eq_low_seq_mode
{
    /* Keep requested Q as much as possible; strongest narrow-band effect. */
    EQ_LOW_SEQ_MODE_GAIN_FOCUSED = 0,
    /* Practical default: balances gain effectiveness and phase movement. */
    EQ_LOW_SEQ_MODE_BALANCED = 1,
    /* Aggressively limits Q below 500 Hz to reduce phase movement. */
    EQ_LOW_SEQ_MODE_PHASE_FOCUSED = 2
} eq_low_seq_mode;

typedef struct eq_low_seq_band
{
    float freqHz;
    float gainDb;
    float q;
    uint8_t enabled;
} eq_low_seq_band;

typedef struct eq_low_seq_ctx
{
    uint32_t sampleRate;
    uint32_t channels;
    uint8_t bandCount;
    uint8_t bypass;
    uint8_t mode;

    eq_low_seq_band bands[EQ_LOW_SEQ_MAX_BANDS];

    /* biquad coeffs per band (a0 normalized) */
    double b0[EQ_LOW_SEQ_MAX_BANDS];
    double b1[EQ_LOW_SEQ_MAX_BANDS];
    double b2[EQ_LOW_SEQ_MAX_BANDS];
    double a1[EQ_LOW_SEQ_MAX_BANDS];
    double a2[EQ_LOW_SEQ_MAX_BANDS];

    /* direct-form states per band/channel */
    double x1[EQ_LOW_SEQ_MAX_BANDS][EQ_LOW_SEQ_MAX_CHANNELS];
    double x2[EQ_LOW_SEQ_MAX_BANDS][EQ_LOW_SEQ_MAX_CHANNELS];
    double y1[EQ_LOW_SEQ_MAX_BANDS][EQ_LOW_SEQ_MAX_CHANNELS];
    double y2[EQ_LOW_SEQ_MAX_BANDS][EQ_LOW_SEQ_MAX_CHANNELS];
} eq_low_seq_ctx;

void eq_low_seq_init(eq_low_seq_ctx* ctx, uint32_t sampleRate, uint32_t channels);

void eq_low_seq_set_bypass(eq_low_seq_ctx* ctx, int enabled);
void eq_low_seq_set_mode(eq_low_seq_ctx* ctx, eq_low_seq_mode mode);

/*
 * Set one band. freqHz is clamped to [20, 500]. q is mode-limited and clamped.
 * gainDb is clamped to [-18, +18].
 */
void eq_low_seq_set_band(eq_low_seq_ctx* ctx, int bandIndex, float freqHz, float gainDb, float q, int enabled);

/*
 * Bulk profile update for the first min(count, EQ_LOW_SEQ_MAX_BANDS) bands.
 * gainsDb/qs can be NULL to keep existing values.
 */
void eq_low_seq_set_profile(eq_low_seq_ctx* ctx,
                            const float* freqsHz,
                            const float* gainsDb,
                            const float* qs,
                            int count,
                            int enabled);

void eq_low_seq_reset_state(eq_low_seq_ctx* ctx);
void eq_low_seq_rebuild(eq_low_seq_ctx* ctx);

float eq_low_seq_process_sample(eq_low_seq_ctx* ctx, uint32_t ch, float in);
void eq_low_seq_process_planar(eq_low_seq_ctx* ctx,
                               float* const* planar,
                               uint32_t frameCount,
                               uint32_t channels);

/* Returns algorithmic delay (IIR path): effectively zero. */
void eq_low_seq_get_latency(const eq_low_seq_ctx* ctx, int* outDelaySamples, float* outDelayMs);

/*
 * Compute mode-dependent Q cap for given frequency.
 * Useful for UI previews and offline validation.
 */
float eq_low_seq_q_cap_for_freq(eq_low_seq_mode mode, float freqHz);

#ifdef __cplusplus
}
#endif

#endif
