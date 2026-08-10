#include "play_channel_plan.h"

#include <string.h>

static uint32_t channels_sanitize(uint32_t channels)
{
    if (channels == 0u)
    {
        return 0u;
    }
    if (channels > PLAY_CHANNEL_PLAN_MAX_CHANNELS)
    {
        return PLAY_CHANNEL_PLAN_MAX_CHANNELS;
    }
    return channels;
}

void play_channel_plan_init_identity(play_channel_plan* plan, uint32_t channels)
{
    uint32_t n;

    if (plan == NULL)
    {
        return;
    }

    memset(plan, 0, sizeof(*plan));
    n = channels_sanitize(channels);
    plan->inChannels = n;
    plan->outChannels = n;

    for (uint32_t ch = 0; ch < n; ++ch)
    {
        plan->matrix[ch][ch] = 1.0f;
    }
}

int play_channel_plan_set_matrix(play_channel_plan* plan,
                                 uint32_t inChannels,
                                 uint32_t outChannels,
                                 const float* matrixRowMajor,
                                 uint32_t matrixCount)
{
    uint32_t inN;
    uint32_t outN;
    uint32_t needed;

    if (plan == NULL || matrixRowMajor == NULL)
    {
        return -1;
    }

    inN = channels_sanitize(inChannels);
    outN = channels_sanitize(outChannels);
    if (inN == 0u || outN == 0u)
    {
        return -1;
    }

    needed = inN * outN;
    if (matrixCount < needed)
    {
        return -1;
    }

    memset(plan, 0, sizeof(*plan));
    plan->inChannels = inN;
    plan->outChannels = outN;

    for (uint32_t outCh = 0; outCh < outN; ++outCh)
    {
        for (uint32_t inCh = 0; inCh < inN; ++inCh)
        {
            plan->matrix[outCh][inCh] = matrixRowMajor[outCh * inN + inCh];
        }
    }

    return 0;
}

int play_channel_plan_prepare_stereo_to_5_1(play_channel_plan* plan)
{
    if (plan == NULL)
    {
        return -1;
    }

    memset(plan, 0, sizeof(*plan));
    plan->inChannels = 2u;
    plan->outChannels = 6u;

    /* Output order: L, R, C, LFE, Ls, Rs. */
    plan->matrix[0][0] = 1.0f; /* L  <- L */
    plan->matrix[1][1] = 1.0f; /* R  <- R */
    plan->matrix[2][0] = 0.7071068f; /* C  <- L */
    plan->matrix[2][1] = 0.7071068f; /* C  <- R */
    plan->matrix[3][0] = 0.0f; /* LFE left for dedicated stage */
    plan->matrix[3][1] = 0.0f;
    plan->matrix[4][0] = 0.7071068f; /* Ls <- L */
    plan->matrix[5][1] = 0.7071068f; /* Rs <- R */

    return 0;
}

int play_channel_plan_apply_planar(const play_channel_plan* plan,
                                   float* const* inPlanar,
                                   float* const* outPlanar,
                                   uint32_t frameCount)
{
    float mix[PLAY_CHANNEL_PLAN_MAX_CHANNELS];

    if (plan == NULL || inPlanar == NULL || outPlanar == NULL)
    {
        return -1;
    }
    if (plan->inChannels == 0u || plan->outChannels == 0u)
    {
        return -1;
    }

    for (uint32_t inCh = 0; inCh < plan->inChannels; ++inCh)
    {
        if (inPlanar[inCh] == NULL)
        {
            return -1;
        }
    }
    for (uint32_t outCh = 0; outCh < plan->outChannels; ++outCh)
    {
        if (outPlanar[outCh] == NULL)
        {
            return -1;
        }
    }

    for (uint32_t i = 0; i < frameCount; ++i)
    {
        for (uint32_t outCh = 0; outCh < plan->outChannels; ++outCh)
        {
            float sum = 0.0f;
            for (uint32_t inCh = 0; inCh < plan->inChannels; ++inCh)
            {
                sum += plan->matrix[outCh][inCh] * inPlanar[inCh][i];
            }
            mix[outCh] = sum;
        }

        for (uint32_t outCh = 0; outCh < plan->outChannels; ++outCh)
        {
            outPlanar[outCh][i] = mix[outCh];
        }
    }

    return 0;
}
