#ifndef PLAY_EQ_NORMAL_H
#define PLAY_EQ_NORMAL_H

void play_eq_normal_build_peaking(double gainDb,
                                  double q,
                                  double frequency,
                                  double sampleRate,
                                  double* outB0,
                                  double* outB1,
                                  double* outB2,
                                  double* outA1,
                                  double* outA2);

void play_eq_normal_build_lowpass_12db(double cutoffHz,
                                       double sampleRate,
                                       double q,
                                       double* outB0,
                                       double* outB1,
                                       double* outB2,
                                       double* outA1,
                                       double* outA2);

#endif
