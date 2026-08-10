#ifndef PLAY_DSP_PLUGINS_H
#define PLAY_DSP_PLUGINS_H

#include <stdint.h>

#include "play_eq_runtime.h"
#include "play_pipeline_eq_stages.h"

typedef struct play_dsp_state play_dsp_state;

#define PLAY_DSP_CHANNEL_LEFT 0x01u
#define PLAY_DSP_CHANNEL_RIGHT 0x02u
#define PLAY_DSP_CHANNEL_BOTH (PLAY_DSP_CHANNEL_LEFT | PLAY_DSP_CHANNEL_RIGHT)

typedef enum
{
    PLAY_PLUGIN_OBSERVER_INPUT = 0,
    PLAY_PLUGIN_LOW_SEQ = 1,
    PLAY_PLUGIN_CROSSFEED_PRE = 2,
    PLAY_PLUGIN_MBDYN_PRE = 3,
    PLAY_PLUGIN_OBSERVER_EQ_IN = 4,
    PLAY_PLUGIN_EQ_CORE = 5,
    PLAY_PLUGIN_OBSERVER_EQ_OUT = 6,
    PLAY_PLUGIN_MBDYN_POST = 7,
    PLAY_PLUGIN_CROSSFEED_POST = 8,
    PLAY_PLUGIN_OBSERVER_OUTPUT = 9,
    PLAY_PLUGIN_SLOT_COUNT = 10
} play_dsp_plugin_id;

typedef struct
{
    uint8_t enabled;
    uint8_t bypass;
    uint8_t channelMask;
    float freqMinHz;
    float freqMaxHz;
} play_dsp_plugin_slot_config;

typedef enum
{
    PLAY_TIMING_TRIGGER_BEFORE = 0,
    PLAY_TIMING_TRIGGER_AFTER = 1
} play_dsp_timing_trigger_phase;

typedef struct
{
    play_dsp_timing_trigger_phase phase;
    uint64_t sequence;
    uint32_t frameCount;
    uint32_t channels;
    double budgetMs;
    double processMs;
    double overrunMs;
    int throttleHint;
} play_dsp_timing_event;

typedef void (*play_dsp_timing_trigger_fn)(const play_dsp_timing_event* event, void* userData);

typedef struct
{
    play_dsp_state* state;
    play_observer_stage observerRawIn;
    play_observer_stage observerEqIn;
    play_observer_stage observerEqOut;
    play_observer_stage observerOut;
    play_eq_runtime eqRuntime;
    play_frame_block* block;
    play_crossfeed_stage preCrossfeed;
    play_crossfeed_stage postCrossfeed;
    play_mbdyn_stage preMbDyn;
    play_mbdyn_stage postMbDyn;
} play_dsp_plugins_runtime;

void play_dsp_plugins_registry_init(play_dsp_state* state);
int play_dsp_plugins_set_order(play_dsp_state* state, const uint8_t* order, int count);
int play_dsp_plugins_set_bypass(play_dsp_state* state, uint8_t pluginId, int bypass);
int play_dsp_plugins_set_channel_mask(play_dsp_state* state, uint8_t pluginId, uint8_t channelMask);
int play_dsp_plugins_set_channel_bypass(play_dsp_state* state, uint32_t channel, uint8_t pluginId, int bypass);
int play_dsp_plugins_validate(play_dsp_state* state);

void play_dsp_plugins_prepare(play_dsp_plugins_runtime* runtime,
                              play_dsp_state* state,
                              play_frame_block* block,
                              uint32_t channels,
                              play_observer_metrics* metrics,
                              void (*onWindowComplete)(play_dsp_state* state));
void play_dsp_plugins_run(play_dsp_plugins_runtime* runtime);
void play_dsp_plugins_commit(play_dsp_plugins_runtime* runtime);

void play_dsp_plugins_timing_begin(play_dsp_state* state, uint32_t frameCount, uint32_t channels);
void play_dsp_plugins_timing_end(play_dsp_state* state, uint32_t frameCount, uint32_t channels);

#endif