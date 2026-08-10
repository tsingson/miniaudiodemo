#ifndef PLAY_DSP_COMMON_H
#define PLAY_DSP_COMMON_H

#include <stdint.h>

#include "play_dsp_defs.h"

#ifdef USE_CMSIS_DSP
#include "arm_math.h"
#endif

#include "eq_low_seq.h"
#include "play_dsp_plugins.h"

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
    uint8_t linearFirEnabled;
    uint8_t linearFirUserEnabled;
    uint8_t linearFirTaps;
    float linearFirCoeff[PLAY_LINEAR_FIR_MAX_TAPS];
    float linearFirHist[PLAY_LINEAR_FIR_MAX_TAPS][MAX_CHANNELS];
    int linearFirWriteIndex[MAX_CHANNELS];
    float linearFirTransientEnv[MAX_CHANNELS];
    float linearFirLastDelayed[MAX_CHANNELS];
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
    uint8_t lowSeqEnabled;
    uint8_t lowSeqMode;
    eq_low_seq_ctx lowSeq;
    play_dsp_plugin_slot_config pluginSlots[PLAY_PLUGIN_SLOT_COUNT];
    uint8_t pluginOrder[PLAY_PLUGIN_SLOT_COUNT];
    uint8_t pluginOrderCount;
    uint8_t pluginPerChannelBypassMode;
    uint8_t pluginBypassByChannel[MAX_CHANNELS][PLAY_PLUGIN_SLOT_COUNT];
    double dspLastStartMs;
    double dspLastEndMs;
    double dspLastProcessMs;
    double dspLastBudgetMs;
    double dspLastOverrunMs;
    uint64_t dspProcessSeq;
    uint8_t sourceThrottleHint;
    play_dsp_timing_trigger_fn timingBefore;
    play_dsp_timing_trigger_fn timingAfter;
    void* timingHookUserData;
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
    float eqTapInSamples[ANALYZER_WINDOW];
    float eqTapOutSamples[ANALYZER_WINDOW];
    uint64_t eqTapSeq;
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
void play_dsp_set_linear_fir_config(play_dsp_state* state, int enabled, int taps);
void play_dsp_get_linear_fir_config(const play_dsp_state* state,
                                    int* outEnabled,
                                    int* outTaps,
                                    int* outDelaySamples,
                                    float* outDelayMs);
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
void play_dsp_set_low_seq_enabled(play_dsp_state* state, int enabled);
int play_dsp_get_low_seq_enabled(const play_dsp_state* state);
void play_dsp_set_low_seq_mode(play_dsp_state* state, eq_low_seq_mode mode);
void play_dsp_set_low_seq_band(play_dsp_state* state, int bandIndex, float freqHz, float gainDb, float q, int enabled);
void play_dsp_set_low_seq_profile(play_dsp_state* state,
                                  const float* freqsHz,
                                  const float* gainsDb,
                                  const float* qs,
                                  int count,
                                  int enabled);
void play_dsp_set_plugin_enabled(play_dsp_state* state, uint32_t pluginBit, int enabled);
uint32_t play_dsp_get_plugin_mask(const play_dsp_state* state);
int play_dsp_plugin_set_order(play_dsp_state* state, const uint8_t* order, int count);
int play_dsp_plugin_set_bypass(play_dsp_state* state, uint8_t pluginId, int bypass);
int play_dsp_plugin_set_channel_mask(play_dsp_state* state, uint8_t pluginId, uint8_t channelMask);
int play_dsp_plugin_set_channel_bypass(play_dsp_state* state, uint32_t channel, uint8_t pluginId, int bypass);
int play_dsp_plugin_validate(play_dsp_state* state);
void play_dsp_set_timing_triggers(play_dsp_state* state,
                                  play_dsp_timing_trigger_fn before,
                                  play_dsp_timing_trigger_fn after,
                                  void* userData);
void play_dsp_get_timing_status(const play_dsp_state* state,
                                uint64_t* outSequence,
                                double* outBudgetMs,
                                double* outProcessMs,
                                double* outOverrunMs,
                                int* outSourceThrottleHint);
void play_dsp_process(play_dsp_state* state,
                      float* interleavedFrames,
                      uint32_t frameCount,
                      uint32_t channels);
void play_dsp_process_planar(play_dsp_state* state,
                             float* const* planarFrames,
                             uint32_t frameCount,
                             uint32_t channels);
void play_dsp_copy_bins(const play_dsp_state* state, float* outBins, int maxCount);
void play_dsp_copy_spectrum(const play_dsp_state* state, float* outPreBins, float* outPostBins, int maxCount);
void play_dsp_copy_eq_taps(const play_dsp_state* state,
                           float* outEqIn,
                           float* outEqOut,
                           int maxCount,
                           int* outWriteIndex,
                           uint64_t* outSeq);
void play_dsp_copy_dynamic_curve(const play_dsp_state* state, uint8_t* outModes, float* outReductionDB, int maxCount);
void play_dsp_copy_phase_metrics(const play_dsp_state* state, float* outPhaseDeltaDeg, float* outGroupDelayMs,
                                 int maxCount);
void play_dsp_copy_eq(const play_dsp_state* state, float* outFreqs, float* outGains, float* outQs, int maxCount);

#endif
