#include "play_eq_normal.h"

#include "play_dsp_common.h"

#include <math.h>
#include <stddef.h>

void play_eq_normal_build_peaking(double gainDb,
                                  double q,
                                  double frequency,
                                  double sampleRate,
                                  double* outB0,
                                  double* outB1,
                                  double* outB2,
                                  double* outA1,
                                  double* outA2)
{
    double a = pow(10.0, gainDb / 40.0);
    double w0 = 2.0 * M_PI * frequency / sampleRate;
    double cosw0 = cos(w0);
    double sinw0 = sin(w0);
    double alpha = sinw0 / (2.0 * q);
    double b0 = 1.0 + alpha * a;
    double b1 = -2.0 * cosw0;
    double b2 = 1.0 - alpha * a;
    double a0 = 1.0 + alpha / a;
    double a1 = -2.0 * cosw0;
    double a2 = 1.0 - alpha / a;

    *outB0 = b0 / a0;
    *outB1 = b1 / a0;
    *outB2 = b2 / a0;
    *outA1 = a1 / a0;
    *outA2 = a2 / a0;
}

void play_eq_normal_build_lowpass_12db(double cutoffHz,
                                       double sampleRate,
                                       double q,
                                       double* outB0,
                                       double* outB1,
                                       double* outB2,
                                       double* outA1,
                                       double* outA2)
{
    double w0 = 2.0 * M_PI * cutoffHz / sampleRate;
    double cosw0 = cos(w0);
    double sinw0 = sin(w0);
    double alpha = sinw0 / (2.0 * q);
    double b0 = (1.0 - cosw0) * 0.5;
    double b1 = 1.0 - cosw0;
    double b2 = (1.0 - cosw0) * 0.5;
    double a0 = 1.0 + alpha;
    double a1 = -2.0 * cosw0;
    double a2 = 1.0 - alpha;

    *outB0 = b0 / a0;
    *outB1 = b1 / a0;
    *outB2 = b2 / a0;
    *outA1 = a1 / a0;
    *outA2 = a2 / a0;
}

void play_eq_normal_init_default_profile_arrays(float* outFreqs, float* outGains, float* outQs, int bandCount)
{
    const float freqs[EQ_BANDS] = {
        20.0f, 35.0f, 60.0f, 110.0f, 220.0f, 360.0f, 700.0f, 1600.0f, 3200.0f, 4800.0f, 7200.0f, 10000.0f,
        16000.0f, 18000.0f, 20000.0f, 22000.0f};
    const float gains[EQ_BANDS] = {
        3.0f, 3.0f, 3.0f, 3.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -3.0f, -24.0f, -24.0f};
    const float qs[EQ_BANDS] = {
        0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f};
    int count = (bandCount < EQ_BANDS) ? bandCount : EQ_BANDS;

    if (outFreqs == NULL || outGains == NULL || outQs == NULL || count <= 0)
    {
        return;
    }

    for (int i = 0; i < count; ++i)
    {
        outFreqs[i] = freqs[i];
        outGains[i] = gains[i];
        outQs[i] = qs[i];
    }
}

void play_eq_assign_band_modes_arrays(const float* freqs,
                                      uint8_t* outModes,
                                      int bandCount,
                                      float linearMaxHz,
                                      float dynamicMaxHz,
                                      uint8_t linearModeValue,
                                      uint8_t dynamicModeValue,
                                      uint8_t normalModeValue)
{
    if (freqs == NULL || outModes == NULL || bandCount <= 0)
    {
        return;
    }

    for (int i = 0; i < bandCount; ++i)
    {
        float freq = freqs[i];
        if (freq <= linearMaxHz)
        {
            outModes[i] = linearModeValue;
        }
        else if (freq <= dynamicMaxHz)
        {
            outModes[i] = dynamicModeValue;
        }
        else
        {
            outModes[i] = normalModeValue;
        }
    }
}

void play_eq_normal_rebuild_arrays(uint32_t sampleRate,
                                   const float* freqs,
                                   const float* gains,
                                   const float* qs,
                                   const uint8_t* modes,
                                   int bandCount,
                                   uint8_t linearModeValue,
                                   float lowSvfMaxHz,
                                   uint8_t* outUseSVF,
                                   double* outB0,
                                   double* outB1,
                                   double* outB2,
                                   double* outA1,
                                   double* outA2,
                                   double* outSvfG,
                                   double* outSvfK,
                                   double* outSvfA,
                                   double* outSvfH)
{
    if (sampleRate == 0u || freqs == NULL || gains == NULL || qs == NULL || modes == NULL || bandCount <= 0 ||
        outUseSVF == NULL || outB0 == NULL || outB1 == NULL || outB2 == NULL || outA1 == NULL || outA2 == NULL ||
        outSvfG == NULL || outSvfK == NULL || outSvfA == NULL || outSvfH == NULL)
    {
        return;
    }

    for (int i = 0; i < bandCount; ++i)
    {
        double gainDB = gains[i];
        double q = qs[i];
        double frequency = freqs[i];
        uint8_t mode = modes[i];

        if (mode == linearModeValue)
        {
            outUseSVF[i] = 0u;
            continue;
        }

        outUseSVF[i] = (frequency <= (double)lowSvfMaxHz) ? 1u : 0u;

        if (outUseSVF[i])
        {
            double g = tan(M_PI * frequency / (double)sampleRate);
            double k = 1.0 / q;
            double aBell = pow(10.0, gainDB / 40.0);
            double h = 1.0 / (1.0 + g * (g + k));

            outSvfG[i] = g;
            outSvfK[i] = k;
            outSvfA[i] = aBell;
            outSvfH[i] = h;
            continue;
        }

        play_eq_normal_build_peaking(gainDB,
                                     q,
                                     frequency,
                                     (double)sampleRate,
                                     &outB0[i],
                                     &outB1[i],
                                     &outB2[i],
                                     &outA1[i],
                                     &outA2[i]);
    }
}

float play_eq_normal_process_svf_sample(double g, double k, double aBell, double h, double* ioIc1eq, double* ioIc2eq,
                                        float in)
{
    double ic1eq;
    double ic2eq;
    double v1;
    double v2;
    double bp;
    double out;

    if (ioIc1eq == NULL || ioIc2eq == NULL)
    {
        return in;
    }

    ic1eq = *ioIc1eq;
    ic2eq = *ioIc2eq;

    v1 = h * (ic1eq + g * ((double)in - ic2eq));
    v2 = ic2eq + g * v1;
    bp = v1;
    out = (double)in + k * (aBell - 1.0) * bp;

    *ioIc1eq = 2.0 * v1 - ic1eq;
    *ioIc2eq = 2.0 * v2 - ic2eq;

    return (float)out;
}

void play_eq_normal_init_default_profile(play_dsp_state* state)
{
    if (state == NULL)
    {
        return;
    }

    play_eq_normal_init_default_profile_arrays(state->bandFreqs, state->gainsDB, state->qValues, EQ_BANDS);
}

void play_eq_assign_band_modes(play_dsp_state* state)
{
    if (state == NULL)
    {
        return;
    }

    play_eq_assign_band_modes_arrays(state->bandFreqs,
                                     state->bandModes,
                                     EQ_BANDS,
                                     EQ_LINEAR_MAX_HZ,
                                     EQ_DYNAMIC_MAX_HZ,
                                     (uint8_t)PLAY_EQ_MODE_LINEAR,
                                     (uint8_t)PLAY_EQ_MODE_DYNAMIC,
                                     (uint8_t)PLAY_EQ_MODE_NORMAL);
}

void play_eq_normal_rebuild(play_dsp_state* state)
{
    if (state == NULL)
    {
        return;
    }

    play_eq_normal_rebuild_arrays(state->sampleRate,
                                  state->bandFreqs,
                                  state->gainsDB,
                                  state->qValues,
                                  state->bandModes,
                                  EQ_BANDS,
                                  (uint8_t)PLAY_EQ_MODE_LINEAR,
                                  EQ_LOW_SVF_MAX_HZ,
                                  state->bandUseSVF,
                                  state->b0,
                                  state->b1,
                                  state->b2,
                                  state->a1,
                                  state->a2,
                                  state->svfG,
                                  state->svfK,
                                  state->svfA,
                                  state->svfH);
}

float play_eq_normal_process_svf_band(play_dsp_state* state, int band, uint32_t ch, float in)
{
    if (state == NULL || band < 0 || band >= EQ_BANDS || ch >= MAX_CHANNELS)
    {
        return in;
    }

    return play_eq_normal_process_svf_sample(state->svfG[band],
                                             state->svfK[band],
                                             state->svfA[band],
                                             state->svfH[band],
                                             &state->svfIc1eq[band][ch],
                                             &state->svfIc2eq[band][ch],
                                             in);
}
