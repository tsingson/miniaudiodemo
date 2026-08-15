#ifndef DSP_HTTP_CTRL_H
#define DSP_HTTP_CTRL_H

#include "../miniaudio/miniaudio.h"
#include "play_dsp_common.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#define HTTP_PORT 8080

typedef struct
{
    void* userData;
    ma_result (*read_frames)(void* userData, float* out, ma_uint64 frameCount, ma_uint64* framesRead);
    ma_result (*rewind)(void* userData);
} pcm_input_stream;

typedef struct
{
    pcm_input_stream input;
    play_dsp_state dsp;
    ma_uint32 channels;
    uint8_t rmsLogEnabled;
    uint32_t rmsLogWindowFrames;
    uint32_t rmsLogFrameAccum;
    double rmsLogSumSq[MAX_CHANNELS];
    pthread_mutex_t dspMutex;
} app_audio_state;

typedef struct
{
    app_audio_state* app;
    atomic_bool running;
    int serverFd;
    pthread_t thread;
} http_server_state;

void* dsp_http_ctrl_thread_main(void* userData);

#endif
