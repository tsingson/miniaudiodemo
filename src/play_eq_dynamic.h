#ifndef PLAY_EQ_DYNAMIC_H
#define PLAY_EQ_DYNAMIC_H

float play_eq_dynamic_update_env(float env, float absSample, float attack, float release);
float play_eq_dynamic_compute_reduction(float env,
                                        float threshold,
                                        float strengthDb,
                                        float maxReductionDb,
                                        int hasPositiveGain);

#endif
