#include "miniaudio/miniaudio.h"
#include "dsp_http_ctrl.h"
#include "play_dsp_common.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#define UAC2_SILENCE_FRAMES (48000U * 2U)

struct wav_source {
    ma_decoder decoder;
    ma_uint64 silence_frames;
};

static app_audio_state g_app;
static http_server_state g_http;
static struct wav_source *g_source;
static uint32_t g_callback_count;
static ma_uint64 g_output_frames;
static uint64_t g_last_rate_ms;

static ma_result wav_read(void *user_data, float *output, ma_uint64 frames, ma_uint64 *read)
{
    struct wav_source *source = user_data;
    return ma_decoder_read_pcm_frames(&source->decoder, output, frames, read);
}

static ma_result wav_rewind(void *user_data)
{
    struct wav_source *source = user_data;
    return ma_decoder_seek_to_pcm_frame(&source->decoder, 0);
}

static const ma_device_info *find_uac2_device(const ma_device_info *devices,
                                               ma_uint32 count,
                                               const char *requested_name)
{
    if (requested_name == NULL || requested_name[0] == '\0') {
        return NULL;
    }

    for (ma_uint32 i = 0U; i < count; ++i) {
        if (strstr(devices[i].name, requested_name) != NULL) {
            return &devices[i];
        }
    }

    return NULL;
}

static void audio_callback(ma_device *device, void *output, const void *input, ma_uint32 frame_count)
{
    app_audio_state *app = device->pUserData;
    struct wav_source *source = g_source;
    int16_t *output_pcm = output;
    float samples[frame_count > 0U ? frame_count * 2U : 2U];
    float planar_storage[MAX_CHANNELS][frame_count > 0U ? frame_count : 1U];
    float *planar[MAX_CHANNELS] = {0};
    ma_uint64 offset = 0U;

    (void)input;

    g_callback_count++;
    g_output_frames += frame_count;
    if ((g_callback_count % 1000U) == 0U) {
        struct timespec now;
        uint64_t now_ms;
        clock_gettime(CLOCK_MONOTONIC, &now);
        now_ms = (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
        if (g_last_rate_ms != 0U && now_ms > g_last_rate_ms) {
            fprintf(stderr, "CoreAudio UAC2 callbacks=%u frames=%u rate=%llu Hz\n",
                    g_callback_count, frame_count,
                    (unsigned long long)(g_output_frames * 1000U /
                                         (now_ms - g_last_rate_ms)));
        } else {
            fprintf(stderr, "CoreAudio UAC2 callbacks=%u frames=%u\n",
                    g_callback_count, frame_count);
        }
        g_output_frames = 0U;
        g_last_rate_ms = now_ms;
    }

    while (offset < frame_count) {
        if (source->silence_frames > 0U) {
            ma_uint64 count = frame_count - offset;
            if (count > source->silence_frames) {
                count = source->silence_frames;
            }
            memset(samples + offset * 2U, 0,
                   (size_t)count * 2U * sizeof(float));
            source->silence_frames -= count;
            offset += count;
            continue;
        }

        ma_uint64 read = 0U;
        ma_result result = wav_read(source, samples + offset * 2U,
                                    frame_count - offset, &read);

        if (result != MA_SUCCESS || read == 0U) {
            if (wav_rewind(source) != MA_SUCCESS) {
                memset(samples + offset * 2U, 0,
                       (frame_count - offset) * 2U * sizeof(float));
                return;
            }
            source->silence_frames = UAC2_SILENCE_FRAMES;
            continue;
        }

        offset += read;
    }

    for (uint32_t ch = 0U; ch < 2U; ++ch) {
        planar[ch] = planar_storage[ch];
    }
    for (ma_uint32 i = 0U; i < frame_count; ++i) {
        planar_storage[0][i] = samples[i * 2U];
        planar_storage[1][i] = samples[i * 2U + 1U];
    }
    pthread_mutex_lock(&app->dspMutex);
    play_dsp_process_planar(&app->dsp, planar, frame_count, 2U);
    pthread_mutex_unlock(&app->dspMutex);
    for (ma_uint32 i = 0U; i < frame_count; ++i) {
        float left = planar_storage[0][i];
        float right = planar_storage[1][i];
        if (left > 1.0f) left = 1.0f;
        if (left < -1.0f) left = -1.0f;
        if (right > 1.0f) right = 1.0f;
        if (right < -1.0f) right = -1.0f;
        output_pcm[i * 2U] = (int16_t)(left * 32767.0f);
        output_pcm[i * 2U + 1U] = (int16_t)(right * 32767.0f);
    }
}

int main(void)
{
    const char *device_name = getenv("UAC2_DEVICE_NAME");
    const char *input_path = "1.wav";
    ma_context context;
    ma_device_info *playback_devices;
    ma_uint32 playback_count;
    ma_device_config config;
    ma_device device;
    ma_decoder_config decoder_config;
    struct wav_source source = {0};
    const ma_device_info *selected;
    ma_result result;

    memset(&g_app, 0, sizeof(g_app));
    memset(&g_http, 0, sizeof(g_http));
    g_http.serverFd = -1;
    g_source = &source;

    if (device_name == NULL || device_name[0] == '\0') {
        fprintf(stderr, "Set UAC2_DEVICE_NAME to the STM32 USB Audio playback device name.\n");
        return 2;
    }

    result = ma_context_init(NULL, 0U, NULL, &context);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "ma_context_init failed: %d\n", result);
        return 1;
    }

    result = ma_context_get_devices(&context, &playback_devices, &playback_count,
                                    NULL, NULL);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "Could not enumerate playback devices: %d\n", result);
        ma_context_uninit(&context);
        return 1;
    }

    selected = find_uac2_device(playback_devices, playback_count, device_name);
    if (selected == NULL) {
        fprintf(stderr, "UAC2 playback device not found: %s\n", device_name);
        for (ma_uint32 i = 0U; i < playback_count; ++i) {
            fprintf(stderr, "  %s\n", playback_devices[i].name);
        }
        ma_context_uninit(&context);
        return 3;
    }

    printf("Using UAC2 playback device: %s\n", selected->name);

    config = ma_device_config_init(ma_device_type_playback);
    config.playback.pDeviceID = &selected->id;
    config.playback.format = ma_format_s16;
    config.playback.channels = 2U;
    config.sampleRate = 48000U;
    config.dataCallback = audio_callback;
    config.pUserData = &g_app;

    decoder_config = ma_decoder_config_init(ma_format_f32, 2U, 48000U);
    result = ma_decoder_init_file(input_path, &decoder_config, &source.decoder);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "Could not open WAV file %s: %d\n", input_path, result);
        ma_context_uninit(&context);
        return 1;
    }

    g_app.channels = 2U;
    if (pthread_mutex_init(&g_app.dspMutex, NULL) != 0 ||
        play_dsp_init(&g_app.dsp, 48000U, 2U) != 0) {
        fprintf(stderr, "Could not initialize DSP.\n");
        ma_decoder_uninit(&source.decoder);
        ma_context_uninit(&context);
        return 1;
    }
    g_http.app = &g_app;
    atomic_init(&g_http.running, true);
    if (pthread_create(&g_http.thread, NULL, dsp_http_ctrl_thread_main, &g_http) != 0) {
        fprintf(stderr, "Could not start DSP HTTP service.\n");
        pthread_mutex_destroy(&g_app.dspMutex);
        ma_decoder_uninit(&source.decoder);
        ma_context_uninit(&context);
        return 1;
    }

    result = ma_device_init(&context, &config, &device);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "Could not open selected UAC2 device: %d\n", result);
        ma_decoder_uninit(&source.decoder);
        atomic_store(&g_http.running, false);
        pthread_join(g_http.thread, NULL);
        pthread_mutex_destroy(&g_app.dspMutex);
        ma_context_uninit(&context);
        return 1;
    }

    result = ma_device_start(&device);
    if (result != MA_SUCCESS) {
        fprintf(stderr, "Could not start UAC2 playback: %d\n", result);
        ma_device_uninit(&device);
        ma_decoder_uninit(&source.decoder);
        atomic_store(&g_http.running, false);
        pthread_join(g_http.thread, NULL);
        pthread_mutex_destroy(&g_app.dspMutex);
        ma_context_uninit(&context);
        return 1;
    }

    printf("Connected to UAC2 playback device: %s\n", selected->name);
    printf("Streaming %s as stereo float32 at 48000 Hz.\n", input_path);
    printf("DSP HTTP service: http://127.0.0.1:%d\n", HTTP_PORT);
    printf("Press Enter to stop.\n");
    getchar();

    ma_device_uninit(&device);
    ma_decoder_uninit(&source.decoder);
    atomic_store(&g_http.running, false);
    if (g_http.serverFd >= 0) {
        shutdown(g_http.serverFd, SHUT_RDWR);
    }
    pthread_join(g_http.thread, NULL);
    pthread_mutex_destroy(&g_app.dspMutex);
    ma_context_uninit(&context);
    return 0;
}
