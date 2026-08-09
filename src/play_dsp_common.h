#ifndef PLAY_DSP_COMMON_H
#define PLAY_DSP_COMMON_H

#include <stdint.h>

#ifdef USE_CMSIS_DSP
#include "arm_math.h"
#endif

#define ANALYZER_WINDOW 512
#define ANALYZER_BINS 48
#define MAX_CHANNELS 2
#define EQ_BANDS 13
#define EQ_MIN_GAIN_DB -36.0f
#define EQ_MAX_GAIN_DB 36.0f
#define EQ_MIN_Q 0.3f
#define EQ_MAX_Q 12.0f
#define EQ_LOW_SVF_MAX_HZ 1000.0f

#define EQ_LINEAR_MAX_HZ 2000.0f
#define EQ_DYNAMIC_MAX_HZ 17000.0f
#define DYNAMIC_EQ_MIN_ATTACK 0.01f
#define DYNAMIC_EQ_MAX_ATTACK 0.50f
#define DYNAMIC_EQ_MIN_RELEASE 0.005f
#define DYNAMIC_EQ_MAX_RELEASE 0.30f
#define DYNAMIC_EQ_MIN_THRESHOLD 0.01f
#define DYNAMIC_EQ_MAX_THRESHOLD 1.00f
#define DYNAMIC_EQ_MIN_MAX_REDUCTION_DB 0.0f
#define DYNAMIC_EQ_MAX_MAX_REDUCTION_DB 36.0f
#define DYNAMIC_EQ_MIN_STRENGTH_DB 1.0f
#define DYNAMIC_EQ_MAX_STRENGTH_DB 36.0f

#define PLAY_MB_DYN_BANDS 8
#define PLAY_MB_DYN_MIN_DB -6.0f
#define PLAY_MB_DYN_MAX_DB 6.0f
#define PLAY_MB_DYN_MIN_THRESHOLD 0.01f
#define PLAY_MB_DYN_MAX_THRESHOLD 1.00f
#define PLAY_MB_DYN_MIN_ATTACK 0.002f
#define PLAY_MB_DYN_MAX_ATTACK 0.20f
#define PLAY_MB_DYN_MIN_RELEASE 0.005f
#define PLAY_MB_DYN_MAX_RELEASE 0.50f
#define PLAY_MB_DYN_MIN_STRENGTH 0.1f
#define PLAY_MB_DYN_MAX_STRENGTH 8.0f
#define PLAY_MB_DYN_DEFAULT_THRESHOLD 0.16f
#define PLAY_MB_DYN_DEFAULT_ATTACK 0.02f
#define PLAY_MB_DYN_DEFAULT_RELEASE 0.06f
#define PLAY_MB_DYN_DEFAULT_STRENGTH 1.2f

#define PLAY_DSP_PLUGIN_PHASE_ANALYZER 0x01u
#define PLAY_LOW_CROSSFEED_CUTOFF_HZ 150.0f
#define PLAY_LOW_CROSSFEED_RATIO 0.40f

typedef enum
{
    PLAY_CROSSFEED_PRE_EQ = 0,
    PLAY_CROSSFEED_POST_EQ = 1
} play_crossfeed_position;

typedef enum
{
    PLAY_EQ_MODE_LINEAR = 0,
    PLAY_EQ_MODE_DYNAMIC = 1,
    PLAY_EQ_MODE_NORMAL = 2
} play_eq_mode;

typedef struct play_dsp_state
{
    uint32_t sampleRate;
    uint32_t channels;

    float bandFreqs[EQ_BANDS];
    float gainsDB[EQ_BANDS];
    float qValues[EQ_BANDS];
    uint8_t bandModes[EQ_BANDS];
    uint8_t bandUseSVF[EQ_BANDS];
    float dynamicEnv[EQ_BANDS][MAX_CHANNELS];
    float dynamicReductionDB[EQ_BANDS];
    float dynamicAttack;
    float dynamicRelease;
    float dynamicThreshold;
    float dynamicMaxReductionDB;
    float dynamicStrengthDB;
    uint32_t pluginMask;
    uint8_t bypassEnabled;
    uint8_t lowCrossfeedEnabled;
    uint8_t lowCrossfeedPosition;
    float lowCrossfeedLpA;
    float lowCrossfeedLpState[MAX_CHANNELS];
    uint8_t mbDynEnabled;
    uint8_t mbDynPosition;
    float mbDynThreshold;
    float mbDynAttack;
    float mbDynRelease;
    float mbDynStrength;
    float mbDynBandLowHz[PLAY_MB_DYN_BANDS];
    float mbDynBandHighHz[PLAY_MB_DYN_BANDS];
    float mbDynBandAmountDb[PLAY_MB_DYN_BANDS];
    float mbDynBandAppliedDb[PLAY_MB_DYN_BANDS];
    float mbDynEnv[PLAY_MB_DYN_BANDS][MAX_CHANNELS];
    float mbDynLpA[PLAY_MB_DYN_BANDS - 1];
    float mbDynLpState[PLAY_MB_DYN_BANDS - 1][MAX_CHANNELS];
    float phaseDeltaDeg[ANALYZER_BINS];
    float groupDelayMs[ANALYZER_BINS];

    double b0[EQ_BANDS];
    double b1[EQ_BANDS];
    double b2[EQ_BANDS];
    double a1[EQ_BANDS];
    double a2[EQ_BANDS];

    double svfG[EQ_BANDS];
    double svfK[EQ_BANDS];
    double svfA[EQ_BANDS];
    double svfH[EQ_BANDS];
    double svfIc1eq[EQ_BANDS][MAX_CHANNELS];
    double svfIc2eq[EQ_BANDS][MAX_CHANNELS];

    double x1[EQ_BANDS][MAX_CHANNELS];
    double x2[EQ_BANDS][MAX_CHANNELS];
    double y1[EQ_BANDS][MAX_CHANNELS];
    double y2[EQ_BANDS][MAX_CHANNELS];

    float preSamples[ANALYZER_WINDOW];
    float postSamples[ANALYZER_WINDOW];
    int writeIndex;
    float preBins[ANALYZER_BINS];
    float postBins[ANALYZER_BINS];
    int binIndex[ANALYZER_BINS];

#ifdef USE_CMSIS_DSP
    arm_rfft_fast_instance_f32 rfft;
    int rfftReady;
    float fftIn[ANALYZER_WINDOW];
    float fftOut[ANALYZER_WINDOW];
    float fftMag[ANALYZER_WINDOW / 2];
    float fftLog[ANALYZER_WINDOW / 2];
#endif
} play_dsp_state;

int play_dsp_init(play_dsp_state* state, uint32_t sampleRate, uint32_t channels);
void play_dsp_set_eq_params(play_dsp_state* state, const float* gains, int gainCount, const float* qs, int qCount);
void play_dsp_set_dynamic_params(play_dsp_state* state,
                                 float attack,
                                 float release,
                                 float threshold,
                                 float maxReductionDB,
                                 float strengthDB);
void play_dsp_get_dynamic_params(const play_dsp_state* state,
                                 float* outAttack,
                                 float* outRelease,
                                 float* outThreshold,
                                 float* outMaxReductionDB,
                                 float* outStrengthDB);
void play_dsp_set_bypass(play_dsp_state* state, int enabled);
int play_dsp_get_bypass(const play_dsp_state* state);
void play_dsp_set_low_crossfeed(play_dsp_state* state, int enabled, uint8_t position);
void play_dsp_get_low_crossfeed(const play_dsp_state* state, int* outEnabled, uint8_t* outPosition);
void play_dsp_set_multiband_dynamics_config(play_dsp_state* state,
                                            int enabled,
                                            uint8_t position,
                                            float threshold,
                                            float attack,
                                            float release,
                                            float strength);
void play_dsp_get_multiband_dynamics_config(const play_dsp_state* state,
                                            int* outEnabled,
                                            uint8_t* outPosition,
                                            float* outThreshold,
                                            float* outAttack,
                                            float* outRelease,
                                            float* outStrength);
void play_dsp_set_multiband_dynamics_amounts(play_dsp_state* state, const float* amountsDb, int count);
void play_dsp_copy_multiband_dynamics_amounts(const play_dsp_state* state, float* outAmountsDb, int maxCount);
void play_dsp_copy_multiband_dynamics_applied_db(const play_dsp_state* state, float* outAppliedDb, int maxCount);
void play_dsp_copy_multiband_dynamics_bands(const play_dsp_state* state,
                                            float* outBandLowHz,
                                            float* outBandHighHz,
                                            int maxCount);
void play_dsp_set_plugin_enabled(play_dsp_state* state, uint32_t pluginBit, int enabled);
uint32_t play_dsp_get_plugin_mask(const play_dsp_state* state);
void play_dsp_process(play_dsp_state* state, float* interleavedFrames, uint32_t frameCount, uint32_t channels);
void play_dsp_copy_bins(const play_dsp_state* state, float* outBins, int maxCount);
void play_dsp_copy_spectrum(const play_dsp_state* state, float* outPreBins, float* outPostBins, int maxCount);
void play_dsp_copy_dynamic_curve(const play_dsp_state* state, uint8_t* outModes, float* outReductionDB, int maxCount);
void play_dsp_copy_phase_metrics(const play_dsp_state* state, float* outPhaseDeltaDeg, float* outGroupDelayMs,
                                 int maxCount);
void play_dsp_copy_eq(const play_dsp_state* state, float* outFreqs, float* outGains, float* outQs, int maxCount);

#endif
