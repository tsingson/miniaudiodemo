#include "play_eq_runtime.h"

#include "play_dsp_common.h"

#include <stddef.h>

static void eq_pipeline_copy_from_state(play_dsp_state* state,
                                        play_linear_eq_stage* linearStage,
                                        play_dynamic_eq_stage* dynamicStage,
                                        play_normal_eq_stage* normalStage)
{
    for (int i = 0; i < PLAY_EQ_LINEAR_MAX_TAPS; ++i)
    {
        linearStage->linear.coeff[i] = state->linearFirCoeff[i];
        for (int ch = 0; ch < MAX_CHANNELS; ++ch)
        {
            linearStage->linear.hist[i][ch] = state->linearFirHist[i][ch];
        }
    }
    linearStage->linear.firEnabled = state->linearFirEnabled;
    linearStage->linear.userEnabled = state->linearFirUserEnabled;
    linearStage->linear.taps = state->linearFirTaps;
    for (int ch = 0; ch < MAX_CHANNELS; ++ch)
    {
        linearStage->linear.writeIndex[ch] = state->linearFirWriteIndex[ch];
        linearStage->linear.transientEnv[ch] = state->linearFirTransientEnv[ch];
        linearStage->linear.lastDelayed[ch] = state->linearFirLastDelayed[ch];
    }

    for (int band = 0; band < EQ_BANDS; ++band)
    {
        dynamicStage->dynamicReductionDB[band] = state->dynamicReductionDB[band];
        for (int ch = 0; ch < MAX_CHANNELS; ++ch)
        {
            dynamicStage->dynamicEnv[band][ch] = state->dynamicEnv[band][ch];
            dynamicStage->svfIc1eq[band][ch] = state->svfIc1eq[band][ch];
            dynamicStage->svfIc2eq[band][ch] = state->svfIc2eq[band][ch];
            dynamicStage->x1[band][ch] = state->x1[band][ch];
            dynamicStage->x2[band][ch] = state->x2[band][ch];
            dynamicStage->y1[band][ch] = state->y1[band][ch];
            dynamicStage->y2[band][ch] = state->y2[band][ch];

            normalStage->svfIc1eq[band][ch] = state->svfIc1eq[band][ch];
            normalStage->svfIc2eq[band][ch] = state->svfIc2eq[band][ch];
            normalStage->x1[band][ch] = state->x1[band][ch];
            normalStage->x2[band][ch] = state->x2[band][ch];
            normalStage->y1[band][ch] = state->y1[band][ch];
            normalStage->y2[band][ch] = state->y2[band][ch];
        }
    }

    play_dynamic_eq_stage_set_params(dynamicStage,
                                     state->dynamicAttack,
                                     state->dynamicRelease,
                                     state->dynamicThreshold,
                                     state->dynamicMaxReductionDB,
                                     state->dynamicStrengthDB);
}

static void eq_pipeline_copy_to_state(play_dsp_state* state,
                                      const play_linear_eq_stage* linearStage,
                                      const play_dynamic_eq_stage* dynamicStage,
                                      const play_normal_eq_stage* normalStage)
{
    for (int i = 0; i < PLAY_EQ_LINEAR_MAX_TAPS; ++i)
    {
        state->linearFirCoeff[i] = linearStage->linear.coeff[i];
        for (int ch = 0; ch < MAX_CHANNELS; ++ch)
        {
            state->linearFirHist[i][ch] = linearStage->linear.hist[i][ch];
        }
    }
    state->linearFirEnabled = linearStage->linear.firEnabled;
    state->linearFirUserEnabled = linearStage->linear.userEnabled;
    state->linearFirTaps = linearStage->linear.taps;
    for (int ch = 0; ch < MAX_CHANNELS; ++ch)
    {
        state->linearFirWriteIndex[ch] = linearStage->linear.writeIndex[ch];
        state->linearFirTransientEnv[ch] = linearStage->linear.transientEnv[ch];
        state->linearFirLastDelayed[ch] = linearStage->linear.lastDelayed[ch];
    }

    for (int band = 0; band < EQ_BANDS; ++band)
    {
        state->dynamicReductionDB[band] = dynamicStage->dynamicReductionDB[band];
        for (int ch = 0; ch < MAX_CHANNELS; ++ch)
        {
            state->dynamicEnv[band][ch] = dynamicStage->dynamicEnv[band][ch];
        }

        if (state->bandModes[band] == (uint8_t)PLAY_EQ_MODE_DYNAMIC)
        {
            for (int ch = 0; ch < MAX_CHANNELS; ++ch)
            {
                state->svfIc1eq[band][ch] = dynamicStage->svfIc1eq[band][ch];
                state->svfIc2eq[band][ch] = dynamicStage->svfIc2eq[band][ch];
                state->x1[band][ch] = dynamicStage->x1[band][ch];
                state->x2[band][ch] = dynamicStage->x2[band][ch];
                state->y1[band][ch] = dynamicStage->y1[band][ch];
                state->y2[band][ch] = dynamicStage->y2[band][ch];
            }
        }
        else if (state->bandModes[band] == (uint8_t)PLAY_EQ_MODE_NORMAL)
        {
            for (int ch = 0; ch < MAX_CHANNELS; ++ch)
            {
                state->svfIc1eq[band][ch] = normalStage->svfIc1eq[band][ch];
                state->svfIc2eq[band][ch] = normalStage->svfIc2eq[band][ch];
                state->x1[band][ch] = normalStage->x1[band][ch];
                state->x2[band][ch] = normalStage->x2[band][ch];
                state->y1[band][ch] = normalStage->y1[band][ch];
                state->y2[band][ch] = normalStage->y2[band][ch];
            }
        }
    }
}

void play_eq_runtime_prepare(play_eq_runtime* runtime, play_dsp_state* state, uint32_t channels)
{
    if (runtime == NULL || state == NULL)
    {
        return;
    }

    play_eq_stage_profile_init(&runtime->profile,
                               state->sampleRate,
                               channels,
                               state->bandFreqs,
                               state->gainsDB,
                               state->qValues,
                               state->bandModes,
                               EQ_BANDS);

    play_linear_eq_stage_init(&runtime->linearStage,
                              &runtime->profile,
                              state->linearFirUserEnabled ? 1 : 0,
                              (int)state->linearFirTaps,
                              (uint8_t)PLAY_EQ_MODE_LINEAR);
    play_dynamic_eq_stage_init(&runtime->dynamicStage,
                               state->sampleRate,
                               channels,
                               state->bandFreqs,
                               state->gainsDB,
                               state->qValues,
                               state->bandModes,
                               EQ_BANDS,
                               (uint8_t)PLAY_EQ_MODE_LINEAR,
                               (uint8_t)PLAY_EQ_MODE_DYNAMIC);
    play_normal_eq_stage_init(&runtime->normalStage,
                              state->sampleRate,
                              channels,
                              state->bandFreqs,
                              state->gainsDB,
                              state->qValues,
                              state->bandModes,
                              EQ_BANDS,
                              (uint8_t)PLAY_EQ_MODE_LINEAR,
                              (uint8_t)PLAY_EQ_MODE_NORMAL);

    eq_pipeline_copy_from_state(state, &runtime->linearStage, &runtime->dynamicStage, &runtime->normalStage);
}

void play_eq_runtime_process(play_eq_runtime* runtime, play_frame_block* block, int bypassEnabled, uint8_t channelMask)
{
    uint8_t prevMask;

    if (runtime == NULL || block == NULL)
    {
        return;
    }

    prevMask = block->channelMask;
    block->channelMask = channelMask;

    play_pipeline_init(&runtime->pipeline);
    (void)play_pipeline_add_stage(&runtime->pipeline,
                                  "linear-eq",
                                  bypassEnabled ? 0 : 1,
                                  &runtime->linearStage,
                                  play_linear_eq_stage_process);
    (void)play_pipeline_add_stage(&runtime->pipeline,
                                  "dynamic-eq",
                                  bypassEnabled ? 0 : 1,
                                  &runtime->dynamicStage,
                                  play_dynamic_eq_stage_process);
    (void)play_pipeline_add_stage(&runtime->pipeline,
                                  "normal-eq",
                                  bypassEnabled ? 0 : 1,
                                  &runtime->normalStage,
                                  play_normal_eq_stage_process);
    (void)play_pipeline_run(&runtime->pipeline, block);

    block->channelMask = prevMask;
}

void play_eq_runtime_commit(play_eq_runtime* runtime, play_dsp_state* state)
{
    if (runtime == NULL || state == NULL)
    {
        return;
    }

    eq_pipeline_copy_to_state(state, &runtime->linearStage, &runtime->dynamicStage, &runtime->normalStage);
}
