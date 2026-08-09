#include "play_eq_dynamic.h"

float play_eq_dynamic_update_env(float env, float absSample, float attack, float release)
{
    float coeff = (absSample > env) ? attack : release;
    return env + coeff * (absSample - env);
}

float play_eq_dynamic_compute_reduction(float env,
                                        float threshold,
                                        float strengthDb,
                                        float maxReductionDb,
                                        int hasPositiveGain)
{
    float reductionDb = 0.0f;

    if (!hasPositiveGain || env <= threshold)
    {
        return 0.0f;
    }

    reductionDb = (env - threshold) * strengthDb;
    if (reductionDb > maxReductionDb)
    {
        reductionDb = maxReductionDb;
    }

    return reductionDb;
}
