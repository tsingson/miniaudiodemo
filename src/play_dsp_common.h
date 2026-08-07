#ifndef PLAY_DSP_COMMON_H
#define PLAY_DSP_COMMON_H

#include <stdint.h>

#ifdef USE_CMSIS_DSP
#include "arm_math.h"
#endif

#define ANALYZER_WINDOW 512
#define ANALYZER_BINS 48
#define MAX_CHANNELS 2
#define EQ_BANDS 8
#define EQ_MIN_GAIN_DB -12.0f
#define EQ_MAX_GAIN_DB 12.0f
#define EQ_MIN_Q 0.3f
#define EQ_MAX_Q 4.0f

typedef struct
{
    uint32_t sampleRate;
    uint32_t channels;

    float bandFreqs[EQ_BANDS];
    float gainsDB[EQ_BANDS];
    float qValues[EQ_BANDS];

    double b0[EQ_BANDS];
    double b1[EQ_BANDS];
    double b2[EQ_BANDS];
    double a1[EQ_BANDS];
    double a2[EQ_BANDS];

    double x1[EQ_BANDS][MAX_CHANNELS];
    double x2[EQ_BANDS][MAX_CHANNELS];
    double y1[EQ_BANDS][MAX_CHANNELS];
    double y2[EQ_BANDS][MAX_CHANNELS];

    float samples[ANALYZER_WINDOW];
    int writeIndex;
    float bins[ANALYZER_BINS];
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
void play_dsp_process(play_dsp_state* state, float* interleavedFrames, uint32_t frameCount, uint32_t channels);
void play_dsp_copy_bins(const play_dsp_state* state, float* outBins, int maxCount);
void play_dsp_copy_eq(const play_dsp_state* state, float* outFreqs, float* outGains, float* outQs, int maxCount);

#endif
