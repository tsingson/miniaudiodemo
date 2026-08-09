#include "play_eq_normal.h"

#include <math.h>

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
