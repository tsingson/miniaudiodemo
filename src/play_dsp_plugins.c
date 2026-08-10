#include "play_dsp_plugins.h"

#include "play_dsp_common.h"

#include <stddef.h>
#include <string.h>
#include <time.h>

static double now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        return 0.0;
    }
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo)
    {
        return lo;
    }
    if (v > hi)
    {
        return hi;
    }
    return v;
}

static int plugin_id_valid(uint8_t pluginId)
{
    return pluginId < (uint8_t)PLAY_PLUGIN_SLOT_COUNT;
}

static uint8_t sanitize_channel_mask(uint8_t mask)
{
    uint8_t v = (uint8_t)(mask & PLAY_DSP_CHANNEL_BOTH);
    return (v == 0u) ? PLAY_DSP_CHANNEL_BOTH : v;
}

static int plugin_is_observer(uint8_t pluginId)
{
    return pluginId == (uint8_t)PLAY_PLUGIN_OBSERVER_INPUT || pluginId == (uint8_t)PLAY_PLUGIN_OBSERVER_EQ_IN ||
           pluginId == (uint8_t)PLAY_PLUGIN_OBSERVER_EQ_OUT || pluginId == (uint8_t)PLAY_PLUGIN_OBSERVER_OUTPUT;
}

static void plugin_default_slot(play_dsp_state* state, uint8_t pluginId)
{
    play_dsp_plugin_slot_config* s;

    if (state == NULL || !plugin_id_valid(pluginId))
    {
        return;
    }

    s = &state->pluginSlots[pluginId];
    memset(s, 0, sizeof(*s));
    s->enabled = 1u;
    s->bypass = 0u;
    s->channelMask = PLAY_DSP_CHANNEL_BOTH;
    s->freqMinHz = 20.0f;
    s->freqMaxHz = 22000.0f;

    if (pluginId == (uint8_t)PLAY_PLUGIN_LOW_SEQ)
    {
        s->freqMaxHz = EQ_LOW_SEQ_MAX_FREQ_HZ;
    }
    else if (pluginId == (uint8_t)PLAY_PLUGIN_CROSSFEED_PRE || pluginId == (uint8_t)PLAY_PLUGIN_CROSSFEED_POST)
    {
        s->freqMaxHz = PLAY_LOW_CROSSFEED_CUTOFF_HZ;
    }
    else if (pluginId == (uint8_t)PLAY_PLUGIN_MBDYN_PRE || pluginId == (uint8_t)PLAY_PLUGIN_MBDYN_POST)
    {
        s->freqMaxHz = 16000.0f;
    }
}

void play_dsp_plugins_registry_init(play_dsp_state* state)
{
    if (state == NULL)
    {
        return;
    }

    state->pluginOrderCount = (uint8_t)PLAY_PLUGIN_SLOT_COUNT;
    state->pluginPerChannelBypassMode = 0u;

    for (uint8_t i = 0; i < (uint8_t)PLAY_PLUGIN_SLOT_COUNT; ++i)
    {
        state->pluginOrder[i] = i;
        plugin_default_slot(state, i);
        for (uint32_t ch = 0; ch < MAX_CHANNELS; ++ch)
        {
            state->pluginBypassByChannel[ch][i] = 0u;
        }
    }
}

int play_dsp_plugins_set_order(play_dsp_state* state, const uint8_t* order, int count)
{
    uint8_t used[PLAY_PLUGIN_SLOT_COUNT];

    if (state == NULL || order == NULL || count <= 0 || count > PLAY_PLUGIN_SLOT_COUNT)
    {
        return -1;
    }

    memset(used, 0, sizeof(used));
    for (int i = 0; i < count; ++i)
    {
        uint8_t id = order[i];
        if (!plugin_id_valid(id) || used[id])
        {
            return -1;
        }
        used[id] = 1u;
    }

    for (int i = 0; i < count; ++i)
    {
        state->pluginOrder[i] = order[i];
    }
    state->pluginOrderCount = (uint8_t)count;
    return 0;
}

int play_dsp_plugins_set_bypass(play_dsp_state* state, uint8_t pluginId, int bypass)
{
    if (state == NULL || !plugin_id_valid(pluginId))
    {
        return -1;
    }

    state->pluginSlots[pluginId].bypass = bypass ? 1u : 0u;
    return 0;
}

int play_dsp_plugins_set_channel_mask(play_dsp_state* state, uint8_t pluginId, uint8_t channelMask)
{
    if (state == NULL || !plugin_id_valid(pluginId))
    {
        return -1;
    }

    state->pluginSlots[pluginId].channelMask = sanitize_channel_mask(channelMask);
    return 0;
}

int play_dsp_plugins_set_channel_bypass(play_dsp_state* state, uint32_t channel, uint8_t pluginId, int bypass)
{
    if (state == NULL || channel >= MAX_CHANNELS || !plugin_id_valid(pluginId))
    {
        return -1;
    }

    state->pluginBypassByChannel[channel][pluginId] = bypass ? 1u : 0u;
    state->pluginPerChannelBypassMode = 1u;
    return 0;
}

int play_dsp_plugins_validate(play_dsp_state* state)
{
    if (state == NULL)
    {
        return -1;
    }

    if (state->pluginOrderCount == 0u || state->pluginOrderCount > (uint8_t)PLAY_PLUGIN_SLOT_COUNT)
    {
        return -1;
    }

    for (uint8_t i = 0; i < state->pluginOrderCount; ++i)
    {
        uint8_t id = state->pluginOrder[i];
        play_dsp_plugin_slot_config* s;

        if (!plugin_id_valid(id))
        {
            return -1;
        }

        s = &state->pluginSlots[id];
        s->channelMask = sanitize_channel_mask(s->channelMask);
        s->freqMinHz = clampf(s->freqMinHz, 0.0f, 22000.0f);
        s->freqMaxHz = clampf(s->freqMaxHz, s->freqMinHz, 22000.0f);

        if ((id == (uint8_t)PLAY_PLUGIN_CROSSFEED_PRE || id == (uint8_t)PLAY_PLUGIN_CROSSFEED_POST) &&
            s->channelMask != PLAY_DSP_CHANNEL_BOTH)
        {
            s->channelMask = PLAY_DSP_CHANNEL_BOTH;
        }
    }

    return 0;
}

static uint8_t effective_channel_mask(const play_dsp_state* state, uint8_t pluginId, uint8_t baseMask)
{
    uint8_t m = sanitize_channel_mask(baseMask);

    if (state == NULL || !state->pluginPerChannelBypassMode)
    {
        return m;
    }

    for (uint32_t ch = 0; ch < MAX_CHANNELS; ++ch)
    {
        if (state->pluginBypassByChannel[ch][pluginId])
        {
            m = (uint8_t)(m & ~(1u << ch));
        }
    }

    return m;
}

static int process_low_seq(play_dsp_plugins_runtime* runtime, uint8_t mask)
{
    play_dsp_state* state;
    play_frame_block* block;

    if (runtime == NULL)
    {
        return -1;
    }

    state = runtime->state;
    block = runtime->block;
    if (state == NULL || block == NULL || block->planar == NULL || block->layout != PLAY_BUFFER_LAYOUT_PLANAR)
    {
        return -1;
    }

    for (uint32_t i = 0; i < block->frameCount; ++i)
    {
        for (uint32_t ch = 0; ch < block->channels && ch < MAX_CHANNELS; ++ch)
        {
            if ((mask & (1u << ch)) == 0u)
            {
                continue;
            }
            block->planar[ch][i] = eq_low_seq_process_sample(&state->lowSeq, ch, block->planar[ch][i]);
        }
    }

    return 0;
}

static int process_slot(play_dsp_plugins_runtime* runtime, uint8_t pluginId, uint8_t mask)
{
    play_frame_block* block;
    uint8_t prevMask;

    if (runtime == NULL || runtime->block == NULL)
    {
        return -1;
    }

    block = runtime->block;
    prevMask = block->channelMask;
    block->channelMask = mask;

    if (pluginId == (uint8_t)PLAY_PLUGIN_OBSERVER_INPUT)
    {
        (void)play_observer_stage_process(&runtime->observerRawIn, block);
    }
    else if (pluginId == (uint8_t)PLAY_PLUGIN_LOW_SEQ)
    {
        (void)process_low_seq(runtime, mask);
    }
    else if (pluginId == (uint8_t)PLAY_PLUGIN_CROSSFEED_PRE)
    {
        (void)play_crossfeed_stage_process(&runtime->preCrossfeed, block);
    }
    else if (pluginId == (uint8_t)PLAY_PLUGIN_MBDYN_PRE)
    {
        (void)play_mbdyn_stage_process(&runtime->preMbDyn, block);
    }
    else if (pluginId == (uint8_t)PLAY_PLUGIN_OBSERVER_EQ_IN)
    {
        (void)play_observer_stage_process(&runtime->observerEqIn, block);
    }
    else if (pluginId == (uint8_t)PLAY_PLUGIN_EQ_CORE)
    {
        play_eq_runtime_process(&runtime->eqRuntime, block, runtime->state->bypassEnabled ? 1 : 0, mask);
    }
    else if (pluginId == (uint8_t)PLAY_PLUGIN_OBSERVER_EQ_OUT)
    {
        (void)play_observer_stage_process(&runtime->observerEqOut, block);
    }
    else if (pluginId == (uint8_t)PLAY_PLUGIN_MBDYN_POST)
    {
        (void)play_mbdyn_stage_process(&runtime->postMbDyn, block);
    }
    else if (pluginId == (uint8_t)PLAY_PLUGIN_CROSSFEED_POST)
    {
        (void)play_crossfeed_stage_process(&runtime->postCrossfeed, block);
    }
    else if (pluginId == (uint8_t)PLAY_PLUGIN_OBSERVER_OUTPUT)
    {
        (void)play_observer_stage_process(&runtime->observerOut, block);
    }

    block->channelMask = prevMask;
    return 0;
}

void play_dsp_plugins_prepare(play_dsp_plugins_runtime* runtime,
                              play_dsp_state* state,
                              play_frame_block* block,
                              uint32_t channels,
                              play_observer_metrics* metrics,
                              void (*onWindowComplete)(play_dsp_state* state))
{
    if (runtime == NULL || state == NULL || metrics == NULL || block == NULL)
    {
        return;
    }

    (void)play_dsp_plugins_validate(state);

    memset(runtime, 0, sizeof(*runtime));
    runtime->state = state;
    runtime->block = block;

    play_observer_stage_init(&runtime->observerRawIn, state, metrics, PLAY_OBSERVER_INPUT_RAW, NULL);
    play_observer_stage_init(&runtime->observerEqIn, state, metrics, PLAY_OBSERVER_INPUT_EQ, NULL);
    play_observer_stage_init(&runtime->observerEqOut, state, metrics, PLAY_OBSERVER_OUTPUT_EQ, NULL);
    play_observer_stage_init(&runtime->observerOut, state, metrics, PLAY_OBSERVER_OUTPUT_FINAL, onWindowComplete);

    play_crossfeed_stage_init(&runtime->preCrossfeed,
                              state,
                              state->lowCrossfeedEnabled ? 1 : 0,
                              (uint8_t)PLAY_CROSSFEED_PRE_EQ,
                              PLAY_LOW_CROSSFEED_RATIO);
    play_crossfeed_stage_init(&runtime->postCrossfeed,
                              state,
                              state->lowCrossfeedEnabled ? 1 : 0,
                              (uint8_t)PLAY_CROSSFEED_POST_EQ,
                              PLAY_LOW_CROSSFEED_RATIO);

    play_mbdyn_stage_init(&runtime->preMbDyn,
                          state,
                          state->mbDynEnabled ? 1 : 0,
                          (uint8_t)PLAY_CROSSFEED_PRE_EQ,
                          state->mbDynThreshold,
                          state->mbDynAttack,
                          state->mbDynRelease,
                          state->mbDynStrength);
    play_mbdyn_stage_init(&runtime->postMbDyn,
                          state,
                          state->mbDynEnabled ? 1 : 0,
                          (uint8_t)PLAY_CROSSFEED_POST_EQ,
                          state->mbDynThreshold,
                          state->mbDynAttack,
                          state->mbDynRelease,
                          state->mbDynStrength);

    play_eq_runtime_prepare(&runtime->eqRuntime, state, channels);
}

void play_dsp_plugins_run(play_dsp_plugins_runtime* runtime)
{
    play_dsp_state* state;

    if (runtime == NULL || runtime->state == NULL || runtime->block == NULL)
    {
        return;
    }

    state = runtime->state;
    for (uint8_t i = 0; i < state->pluginOrderCount; ++i)
    {
        uint8_t id = state->pluginOrder[i];
        play_dsp_plugin_slot_config* slot;
        uint8_t mask;

        if (!plugin_id_valid(id))
        {
            continue;
        }

        slot = &state->pluginSlots[id];
        if (!slot->enabled)
        {
            continue;
        }

        mask = effective_channel_mask(state, id, slot->channelMask);
        if (mask == 0u)
        {
            continue;
        }
        if (slot->bypass && !plugin_is_observer(id))
        {
            continue;
        }
        if (state->bypassEnabled && !plugin_is_observer(id))
        {
            continue;
        }

        (void)process_slot(runtime, id, mask);
    }
}

void play_dsp_plugins_commit(play_dsp_plugins_runtime* runtime)
{
    if (runtime == NULL || runtime->state == NULL)
    {
        return;
    }

    play_eq_runtime_commit(&runtime->eqRuntime, runtime->state);
}

void play_dsp_plugins_timing_begin(play_dsp_state* state, uint32_t frameCount, uint32_t channels)
{
    play_dsp_timing_event event;

    if (state == NULL)
    {
        return;
    }

    state->dspProcessSeq += 1u;
    state->dspLastStartMs = now_ms();

    if (state->timingBefore != NULL)
    {
        memset(&event, 0, sizeof(event));
        event.phase = PLAY_TIMING_TRIGGER_BEFORE;
        event.sequence = state->dspProcessSeq;
        event.frameCount = frameCount;
        event.channels = channels;
        event.budgetMs = (state->sampleRate > 0u) ? ((double)frameCount * 1000.0 / (double)state->sampleRate) : 0.0;
        state->timingBefore(&event, state->timingHookUserData);
    }
}

void play_dsp_plugins_timing_end(play_dsp_state* state, uint32_t frameCount, uint32_t channels)
{
    play_dsp_timing_event event;
    double budgetMs;

    if (state == NULL)
    {
        return;
    }

    state->dspLastEndMs = now_ms();
    state->dspLastProcessMs = state->dspLastEndMs - state->dspLastStartMs;
    budgetMs = (state->sampleRate > 0u) ? ((double)frameCount * 1000.0 / (double)state->sampleRate) : 0.0;
    state->dspLastBudgetMs = budgetMs;
    state->dspLastOverrunMs = state->dspLastProcessMs - budgetMs;
    if (state->dspLastOverrunMs < 0.0)
    {
        state->dspLastOverrunMs = 0.0;
    }
    state->sourceThrottleHint = (state->dspLastOverrunMs > 0.0) ? 1u : 0u;

    if (state->timingAfter != NULL)
    {
        memset(&event, 0, sizeof(event));
        event.phase = PLAY_TIMING_TRIGGER_AFTER;
        event.sequence = state->dspProcessSeq;
        event.frameCount = frameCount;
        event.channels = channels;
        event.budgetMs = state->dspLastBudgetMs;
        event.processMs = state->dspLastProcessMs;
        event.overrunMs = state->dspLastOverrunMs;
        event.throttleHint = state->sourceThrottleHint ? 1 : 0;
        state->timingAfter(&event, state->timingHookUserData);
    }
}
