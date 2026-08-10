#ifndef PLAY_EQ_NORMAL_H
#define PLAY_EQ_NORMAL_H

#include <stdint.h>

typedef struct play_dsp_state play_dsp_state;

/* RBJ peaking biquad builder; output coefficients are a0-normalized. */
void play_eq_normal_build_peaking(double gainDb,
                                  double q,
                                  double frequency,
                                  double sampleRate,
                                  double* outB0,
                                  double* outB1,
                                  double* outB2,
                                  double* outA1,
                                  double* outA2);

/* 12 dB/oct lowpass biquad builder; output coefficients are a0-normalized. */
void play_eq_normal_build_lowpass_12db(double cutoffHz,
                                       double sampleRate,
                                       double q,
                                       double* outB0,
                                       double* outB1,
                                       double* outB2,
                                       double* outA1,
                                       double* outA2);

void play_eq_normal_init_default_profile_arrays(float* outFreqs, float* outGains, float* outQs, int bandCount);
void play_eq_assign_band_modes_arrays(const float* freqs,
                                      uint8_t* outModes,
                                      int bandCount,
                                      float linearMaxHz,
                                      float dynamicMaxHz,
                                      uint8_t linearModeValue,
                                      uint8_t dynamicModeValue,
                                      uint8_t normalModeValue);
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
                                   double* outSvfH);
float play_eq_normal_process_svf_sample(double g, double k, double aBell, double h, double* ioIc1eq, double* ioIc2eq,
                                        float in);

void play_eq_normal_init_default_profile(play_dsp_state* state);
void play_eq_assign_band_modes(play_dsp_state* state);
void play_eq_normal_rebuild(play_dsp_state* state);
float play_eq_normal_process_svf_band(play_dsp_state* state, int band, uint32_t ch, float in);

#endif
