#include "miniaudio.h"
#include "play_dsp_common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define HTTP_PORT 8080

typedef struct
{
    ma_decoder decoder;
} wav_input_source;

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
    pthread_mutex_t dspMutex;
} app_audio_state;

typedef struct
{
    app_audio_state* app;
    atomic_bool running;
    int serverFd;
    pthread_t thread;
} http_server_state;

static int send_all(int fd, const char* data, size_t len)
{
    size_t sent = 0;

    while (sent < len)
    {
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n > 0)
        {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        return -1;
    }

    return 0;
}

static ma_result wav_read_frames(void* userData, float* out, ma_uint64 frameCount, ma_uint64* framesRead)
{
    wav_input_source* source = (wav_input_source*)userData;
    return ma_decoder_read_pcm_frames(&source->decoder, out, frameCount, framesRead);
}

static ma_result wav_rewind(void* userData)
{
    wav_input_source* source = (wav_input_source*)userData;
    return ma_decoder_seek_to_pcm_frame(&source->decoder, 0);
}

static void audio_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
    app_audio_state* app = (app_audio_state*)pDevice->pUserData;
    float* out = (float*)pOutput;
    uint32_t pipelineChannels;
    float planarStorage[MAX_CHANNELS][frameCount > 0u ? frameCount : 1u];
    float* planarPtrs[MAX_CHANNELS] = {0};
    ma_uint64 totalRead = 0;
    int rewindAttempts = 0;

    (void)pInput;

    while (totalRead < frameCount)
    {
        ma_uint64 chunkRead = 0;
        ma_result readResult = app->input.read_frames(app->input.userData, out + totalRead * app->channels,
                                                      frameCount - totalRead, &chunkRead);

        if (readResult != MA_SUCCESS || chunkRead == 0)
        {
            if (rewindAttempts >= 3 || app->input.rewind(app->input.userData) != MA_SUCCESS)
            {
                memset(out + totalRead * app->channels, 0,
                       (size_t)((frameCount - totalRead) * app->channels * sizeof(float)));
                break;
            }
            rewindAttempts += 1;
            continue;
        }

        totalRead += chunkRead;
        rewindAttempts = 0;
    }

    pipelineChannels = (app->channels > MAX_CHANNELS) ? MAX_CHANNELS : app->channels;
    for (uint32_t ch = 0; ch < pipelineChannels; ++ch)
    {
        planarPtrs[ch] = planarStorage[ch];
    }

    for (uint32_t i = 0; i < frameCount; ++i)
    {
        for (uint32_t ch = 0; ch < pipelineChannels; ++ch)
        {
            planarStorage[ch][i] = out[i * app->channels + ch];
        }
    }

    pthread_mutex_lock(&app->dspMutex);
    play_dsp_process_planar(&app->dsp, planarPtrs, frameCount, pipelineChannels);
    pthread_mutex_unlock(&app->dspMutex);

    for (uint32_t i = 0; i < frameCount; ++i)
    {
        for (uint32_t ch = 0; ch < pipelineChannels; ++ch)
        {
            out[i * app->channels + ch] = planarStorage[ch][i];
        }
    }
}

static void send_http_response(int clientFd, const char* contentType, const char* body)
{
    char header[256];
    int bodyLen = (int)strlen(body);
    int n = snprintf(header,
                     sizeof(header),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %d\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     contentType,
                     bodyLen);

    if (n > 0)
    {
        (void)send_all(clientFd, header, (size_t)n);
        (void)send_all(clientFd, body, (size_t)bodyLen);
    }
}

static void send_not_found(int clientFd)
{
    const char* body = "Not Found";
    const char* header =
        "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 9\r\nConnection: close\r\n\r\n";
    (void)send_all(clientFd, header, strlen(header));
    (void)send_all(clientFd, body, strlen(body));
}

static int parse_float_array_from_json(const char* body, const char* key, float* out, int maxCount)
{
    const char* p = strstr(body, key);
    int count = 0;

    if (p == NULL)
    {
        return 0;
    }

    p = strchr(p, '[');
    if (p == NULL)
    {
        return 0;
    }
    ++p;

    while (*p != '\0' && *p != ']' && count < maxCount)
    {
        char* endPtr;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')
        {
            ++p;
        }
        if (*p == ']' || *p == '\0')
        {
            break;
        }

        out[count] = strtof(p, &endPtr);
        if (endPtr == p)
        {
            break;
        }

        ++count;
        p = endPtr;
    }

    return count;
}

static bool parse_float_value_from_json(const char* body, const char* key, float* out)
{
    const char* p = strstr(body, key);
    char* endPtr;

    if (p == NULL)
    {
        return false;
    }

    p = strchr(p, ':');
    if (p == NULL)
    {
        return false;
    }
    ++p;

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
    {
        ++p;
    }

    *out = strtof(p, &endPtr);
    if (endPtr == p)
    {
        return false;
    }

    return true;
}

static void handle_http_request(http_server_state* http, int clientFd)
{
    char req[2048];
    ssize_t n = recv(clientFd, req, sizeof(req) - 1, 0);
    if (n <= 0)
    {
        return;
    }

    req[n] = '\0';
    if (strncmp(req, "GET /spectrum", 13) == 0)
    {
        char body[16384];
        float preBins[ANALYZER_BINS];
        float postBins[ANALYZER_BINS];
        float phaseDeltaDeg[ANALYZER_BINS];
        float groupDelayMs[ANALYZER_BINS];
        uint32_t pluginMask;
        uint32_t sampleRate;
        int offset = 0;

        pthread_mutex_lock(&http->app->dspMutex);
        play_dsp_copy_spectrum(&http->app->dsp, preBins, postBins, ANALYZER_BINS);
        play_dsp_copy_phase_metrics(&http->app->dsp, phaseDeltaDeg, groupDelayMs, ANALYZER_BINS);
        pluginMask = play_dsp_get_plugin_mask(&http->app->dsp);
        sampleRate = http->app->dsp.sampleRate;
        pthread_mutex_unlock(&http->app->dspMutex);

        offset += snprintf(body + offset,
                           sizeof(body) - (size_t)offset,
                           "{\"sampleRate\":%u,\"phasePluginEnabled\":%s,\"preBins\":[",
                           sampleRate,
                           ((pluginMask & PLAY_DSP_PLUGIN_PHASE_ANALYZER) != 0u) ? "true" : "false");
        for (int i = 0; i < ANALYZER_BINS; ++i)
        {
            float bin = preBins[i];
            if (!isfinite(bin))
            {
                bin = -80.0f;
            }
            offset += snprintf(body + offset,
                               sizeof(body) - (size_t)offset,
                               "%s%.2f",
                               (i == 0) ? "" : ",",
                               bin);
            if (offset >= (int)sizeof(body) - 16)
            {
                break;
            }
        }

        offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "],\"postBins\":[");
        for (int i = 0; i < ANALYZER_BINS; ++i)
        {
            float bin = postBins[i];
            if (!isfinite(bin))
            {
                bin = -80.0f;
            }
            offset += snprintf(body + offset,
                               sizeof(body) - (size_t)offset,
                               "%s%.2f",
                               (i == 0) ? "" : ",",
                               bin);
            if (offset >= (int)sizeof(body) - 16)
            {
                break;
            }
        }

        offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "],\"phaseDeltaDeg\":[");
        for (int i = 0; i < ANALYZER_BINS; ++i)
        {
            float v = phaseDeltaDeg[i];
            if (!isfinite(v))
            {
                v = 0.0f;
            }
            offset += snprintf(body + offset,
                               sizeof(body) - (size_t)offset,
                               "%s%.2f",
                               (i == 0) ? "" : ",",
                               v);
            if (offset >= (int)sizeof(body) - 16)
            {
                break;
            }
        }

        offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "],\"groupDelayMs\":[");
        for (int i = 0; i < ANALYZER_BINS; ++i)
        {
            float v = groupDelayMs[i];
            if (!isfinite(v))
            {
                v = 0.0f;
            }
            offset += snprintf(body + offset,
                               sizeof(body) - (size_t)offset,
                               "%s%.4f",
                               (i == 0) ? "" : ",",
                               v);
            if (offset >= (int)sizeof(body) - 16)
            {
                break;
            }
        }
        snprintf(body + offset, sizeof(body) - (size_t)offset, "]}");
        send_http_response(clientFd, "application/json", body);
        return;
    }

    if (strncmp(req, "GET /eq", 7) == 0)
    {
        char body[8192];
        float freqs[EQ_BANDS];
        float gains[EQ_BANDS];
        float qs[EQ_BANDS];
        uint8_t modes[EQ_BANDS];
        float dynamicReductionDB[EQ_BANDS];
        float dynamicAttack;
        float dynamicRelease;
        float dynamicThreshold;
        float dynamicMaxReductionDB;
        float dynamicStrengthDB;
        int linearFirEnabled;
        int linearFirTaps;
        int linearFirDelaySamples;
        float linearFirDelayMs;
        int mbDynEnabled;
        uint8_t mbDynPosition;
        float mbDynThreshold;
        float mbDynAttack;
        float mbDynRelease;
        float mbDynStrength;
        float mbDynBandAmountDb[PLAY_MB_DYN_BANDS];
        float mbDynBandAppliedDb[PLAY_MB_DYN_BANDS];
        float mbDynBandLowHz[PLAY_MB_DYN_BANDS];
        float mbDynBandHighHz[PLAY_MB_DYN_BANDS];
        int bypassEnabled;
        int lowCrossfeedEnabled;
        uint8_t lowCrossfeedPosition;
        uint32_t pluginMask;
        uint32_t sampleRate;
        int offset = 0;

        pthread_mutex_lock(&http->app->dspMutex);
        play_dsp_copy_eq(&http->app->dsp, freqs, gains, qs, EQ_BANDS);
        play_dsp_copy_dynamic_curve(&http->app->dsp, modes, dynamicReductionDB, EQ_BANDS);
        play_dsp_get_dynamic_params(&http->app->dsp,
                                    &dynamicAttack,
                                    &dynamicRelease,
                                    &dynamicThreshold,
                                    &dynamicMaxReductionDB,
                                    &dynamicStrengthDB);
        play_dsp_get_linear_fir_config(&http->app->dsp,
                           &linearFirEnabled,
                           &linearFirTaps,
                           &linearFirDelaySamples,
                           &linearFirDelayMs);
        play_dsp_get_multiband_dynamics_config(&http->app->dsp,
                               &mbDynEnabled,
                               &mbDynPosition,
                               &mbDynThreshold,
                               &mbDynAttack,
                               &mbDynRelease,
                               &mbDynStrength);
        play_dsp_copy_multiband_dynamics_amounts(&http->app->dsp, mbDynBandAmountDb, PLAY_MB_DYN_BANDS);
        play_dsp_copy_multiband_dynamics_applied_db(&http->app->dsp, mbDynBandAppliedDb, PLAY_MB_DYN_BANDS);
        play_dsp_copy_multiband_dynamics_bands(&http->app->dsp,
                               mbDynBandLowHz,
                               mbDynBandHighHz,
                               PLAY_MB_DYN_BANDS);
        bypassEnabled = play_dsp_get_bypass(&http->app->dsp);
        play_dsp_get_low_crossfeed(&http->app->dsp, &lowCrossfeedEnabled, &lowCrossfeedPosition);
        pluginMask = play_dsp_get_plugin_mask(&http->app->dsp);
        sampleRate = http->app->dsp.sampleRate;
        pthread_mutex_unlock(&http->app->dspMutex);

        offset += snprintf(body + offset,
                           sizeof(body) - (size_t)offset,
                           "{\"minGain\":%.1f,\"maxGain\":%.1f,\"minQ\":%.2f,\"maxQ\":%.2f,\"sampleRate\":%u,\"phasePluginEnabled\":%s,\"bypassEnabled\":%s,\"lowCrossfeedEnabled\":%s,\"lowCrossfeedPosition\":%u,\"lowCrossfeedCutoffHz\":%.1f,\"lowCrossfeedRatio\":%.2f,\"dynamicAttack\":%.4f,\"dynamicRelease\":%.4f,\"dynamicThreshold\":%.4f,\"dynamicMaxReductionDB\":%.2f,\"dynamicStrengthDB\":%.2f,\"dynamicAttackMin\":%.3f,\"dynamicAttackMax\":%.3f,\"dynamicReleaseMin\":%.3f,\"dynamicReleaseMax\":%.3f,\"dynamicThresholdMin\":%.3f,\"dynamicThresholdMax\":%.3f,\"dynamicMaxReductionDBMin\":%.1f,\"dynamicMaxReductionDBMax\":%.1f,\"dynamicStrengthDBMin\":%.1f,\"dynamicStrengthDBMax\":%.1f,\"linearFirEnabled\":%s,\"linearFirTaps\":%d,\"linearFirDelaySamples\":%d,\"linearFirDelayMs\":%.4f,\"linearFirSupportedTaps\":[17,25,33],\"mbDynEnabled\":%s,\"mbDynPosition\":%u,\"mbDynThreshold\":%.4f,\"mbDynAttack\":%.4f,\"mbDynRelease\":%.4f,\"mbDynStrength\":%.3f,\"mbDynBandMinDB\":%.1f,\"mbDynBandMaxDB\":%.1f,\"mbDynThresholdMin\":%.3f,\"mbDynThresholdMax\":%.3f,\"mbDynAttackMin\":%.4f,\"mbDynAttackMax\":%.3f,\"mbDynReleaseMin\":%.3f,\"mbDynReleaseMax\":%.3f,\"mbDynStrengthMin\":%.2f,\"mbDynStrengthMax\":%.1f,\"freqs\":[",
                           EQ_MIN_GAIN_DB,
                           EQ_MAX_GAIN_DB,
                           EQ_MIN_Q,
                           EQ_MAX_Q,
                           sampleRate,
                           ((pluginMask & PLAY_DSP_PLUGIN_PHASE_ANALYZER) != 0u) ? "true" : "false",
                           bypassEnabled ? "true" : "false",
                           lowCrossfeedEnabled ? "true" : "false",
                           (unsigned)lowCrossfeedPosition,
                           PLAY_LOW_CROSSFEED_CUTOFF_HZ,
                           PLAY_LOW_CROSSFEED_RATIO,
                           dynamicAttack,
                           dynamicRelease,
                           dynamicThreshold,
                           dynamicMaxReductionDB,
                           dynamicStrengthDB,
                           DYNAMIC_EQ_MIN_ATTACK,
                           DYNAMIC_EQ_MAX_ATTACK,
                           DYNAMIC_EQ_MIN_RELEASE,
                           DYNAMIC_EQ_MAX_RELEASE,
                           DYNAMIC_EQ_MIN_THRESHOLD,
                           DYNAMIC_EQ_MAX_THRESHOLD,
                           DYNAMIC_EQ_MIN_MAX_REDUCTION_DB,
                           DYNAMIC_EQ_MAX_MAX_REDUCTION_DB,
                           DYNAMIC_EQ_MIN_STRENGTH_DB,
                           DYNAMIC_EQ_MAX_STRENGTH_DB,
                           linearFirEnabled ? "true" : "false",
                           linearFirTaps,
                           linearFirDelaySamples,
                           linearFirDelayMs,
                           mbDynEnabled ? "true" : "false",
                           (unsigned)mbDynPosition,
                           mbDynThreshold,
                           mbDynAttack,
                           mbDynRelease,
                           mbDynStrength,
                           PLAY_MB_DYN_MIN_DB,
                           PLAY_MB_DYN_MAX_DB,
                           PLAY_MB_DYN_MIN_THRESHOLD,
                           PLAY_MB_DYN_MAX_THRESHOLD,
                           PLAY_MB_DYN_MIN_ATTACK,
                           PLAY_MB_DYN_MAX_ATTACK,
                           PLAY_MB_DYN_MIN_RELEASE,
                           PLAY_MB_DYN_MAX_RELEASE,
                           PLAY_MB_DYN_MIN_STRENGTH,
                           PLAY_MB_DYN_MAX_STRENGTH);
        for (int i = 0; i < EQ_BANDS; ++i)
        {
            offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "%s%.0f", (i == 0) ? "" : ",", freqs[i]);
        }
        offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "],\"gains\":[");
        for (int i = 0; i < EQ_BANDS; ++i)
        {
            offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "%s%.2f", (i == 0) ? "" : ",", gains[i]);
        }
        offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "],\"qs\":[");
        for (int i = 0; i < EQ_BANDS; ++i)
        {
            offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "%s%.2f", (i == 0) ? "" : ",", qs[i]);
        }
        offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "],\"modes\":[");
        for (int i = 0; i < EQ_BANDS; ++i)
        {
            offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "%s%u", (i == 0) ? "" : ",",
                               (unsigned)modes[i]);
        }
        offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "],\"dynamicReductionDB\":[");
        for (int i = 0; i < EQ_BANDS; ++i)
        {
            offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "%s%.2f", (i == 0) ? "" : ",",
                               dynamicReductionDB[i]);
        }
        offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "],\"mbDynBandAmountDB\":[");
        for (int i = 0; i < PLAY_MB_DYN_BANDS; ++i)
        {
            offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "%s%.2f", (i == 0) ? "" : ",",
                               mbDynBandAmountDb[i]);
        }
        offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "],\"mbDynBandAppliedDB\":[");
        for (int i = 0; i < PLAY_MB_DYN_BANDS; ++i)
        {
            offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "%s%.2f", (i == 0) ? "" : ",",
                               mbDynBandAppliedDb[i]);
        }
        offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "],\"mbDynBandLowHz\":[");
        for (int i = 0; i < PLAY_MB_DYN_BANDS; ++i)
        {
            offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "%s%.0f", (i == 0) ? "" : ",",
                               mbDynBandLowHz[i]);
        }
        offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "],\"mbDynBandHighHz\":[");
        for (int i = 0; i < PLAY_MB_DYN_BANDS; ++i)
        {
            offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "%s%.0f", (i == 0) ? "" : ",",
                               mbDynBandHighHz[i]);
        }
        snprintf(body + offset, sizeof(body) - (size_t)offset, "]}");
        send_http_response(clientFd, "application/json", body);
        return;
    }

    if (strncmp(req, "POST /eq", 8) == 0)
    {
        const char* bodyStart = strstr(req, "\r\n\r\n");
        float gains[EQ_BANDS];
        float qs[EQ_BANDS];
        float dynamicAttack;
        float dynamicRelease;
        float dynamicThreshold;
        float dynamicMaxReductionDB;
        float dynamicStrengthDB;
        float phasePluginEnabledValue;
        float bypassEnabledValue;
        float lowCrossfeedEnabledValue;
        float lowCrossfeedPositionValue;
        float mbDynEnabledValue;
        float mbDynPositionValue;
        float mbDynThresholdValue;
        float mbDynAttackValue;
        float mbDynReleaseValue;
        float mbDynStrengthValue;
        float linearFirEnabledValue;
        float linearFirTapsValue;
        float resetAllValue;
        float mbDynBandAmountDb[PLAY_MB_DYN_BANDS];
        int gainCount;
        int qCount;
        int mbDynBandCount;
        bool hasDynamicAttack;
        bool hasDynamicRelease;
        bool hasDynamicThreshold;
        bool hasDynamicMaxReductionDB;
        bool hasDynamicStrengthDB;
        bool hasPhasePluginEnabled;
        bool hasBypassEnabled;
        bool hasLowCrossfeedEnabled;
        bool hasLowCrossfeedPosition;
        bool hasMbDynEnabled;
        bool hasMbDynPosition;
        bool hasMbDynThreshold;
        bool hasMbDynAttack;
        bool hasMbDynRelease;
        bool hasMbDynStrength;
        bool hasLinearFirEnabled;
        bool hasLinearFirTaps;
        bool hasResetAll;
        bool hasAnyMbDyn;
        bool hasAnyDynamic;

        if (bodyStart == NULL)
        {
            send_http_response(clientFd, "application/json", "{\"ok\":false,\"reason\":\"missing body\"}");
            return;
        }

        bodyStart += 4;
        gainCount = parse_float_array_from_json(bodyStart, "\"gains\"", gains, EQ_BANDS);
        qCount = parse_float_array_from_json(bodyStart, "\"qs\"", qs, EQ_BANDS);
        hasDynamicAttack = parse_float_value_from_json(bodyStart, "\"dynamicAttack\"", &dynamicAttack);
        hasDynamicRelease = parse_float_value_from_json(bodyStart, "\"dynamicRelease\"", &dynamicRelease);
        hasDynamicThreshold = parse_float_value_from_json(bodyStart, "\"dynamicThreshold\"", &dynamicThreshold);
        hasDynamicMaxReductionDB = parse_float_value_from_json(bodyStart, "\"dynamicMaxReductionDB\"",
                                                               &dynamicMaxReductionDB);
        hasDynamicStrengthDB = parse_float_value_from_json(bodyStart, "\"dynamicStrengthDB\"", &dynamicStrengthDB);
        hasPhasePluginEnabled = parse_float_value_from_json(bodyStart, "\"phasePluginEnabled\"",
                                                            &phasePluginEnabledValue);
        hasBypassEnabled = parse_float_value_from_json(bodyStart, "\"bypassEnabled\"", &bypassEnabledValue);
        hasLowCrossfeedEnabled = parse_float_value_from_json(bodyStart, "\"lowCrossfeedEnabled\"",
                                                             &lowCrossfeedEnabledValue);
        hasLowCrossfeedPosition = parse_float_value_from_json(bodyStart, "\"lowCrossfeedPosition\"",
                                                              &lowCrossfeedPositionValue);
        hasMbDynEnabled = parse_float_value_from_json(bodyStart, "\"mbDynEnabled\"", &mbDynEnabledValue);
        hasMbDynPosition = parse_float_value_from_json(bodyStart, "\"mbDynPosition\"", &mbDynPositionValue);
        hasMbDynThreshold = parse_float_value_from_json(bodyStart, "\"mbDynThreshold\"", &mbDynThresholdValue);
        hasMbDynAttack = parse_float_value_from_json(bodyStart, "\"mbDynAttack\"", &mbDynAttackValue);
        hasMbDynRelease = parse_float_value_from_json(bodyStart, "\"mbDynRelease\"", &mbDynReleaseValue);
        hasMbDynStrength = parse_float_value_from_json(bodyStart, "\"mbDynStrength\"", &mbDynStrengthValue);
        hasLinearFirEnabled = parse_float_value_from_json(bodyStart, "\"linearFirEnabled\"", &linearFirEnabledValue);
        hasLinearFirTaps = parse_float_value_from_json(bodyStart, "\"linearFirTaps\"", &linearFirTapsValue);
        hasResetAll = parse_float_value_from_json(bodyStart, "\"resetAll\"", &resetAllValue);
        mbDynBandCount = parse_float_array_from_json(bodyStart, "\"mbDynBandAmountDB\"", mbDynBandAmountDb,
                                                     PLAY_MB_DYN_BANDS);
        hasAnyMbDyn = hasMbDynEnabled || hasMbDynPosition || hasMbDynThreshold || hasMbDynAttack || hasMbDynRelease ||
            hasMbDynStrength || mbDynBandCount > 0;
        hasAnyDynamic = hasDynamicAttack || hasDynamicRelease || hasDynamicThreshold || hasDynamicMaxReductionDB ||
            hasDynamicStrengthDB;

        if (gainCount <= 0 && qCount <= 0 && !hasAnyDynamic && !hasPhasePluginEnabled && !hasBypassEnabled &&
            !hasLowCrossfeedEnabled && !hasLowCrossfeedPosition && !hasAnyMbDyn && !hasLinearFirEnabled &&
            !hasLinearFirTaps && !hasResetAll)
        {
            send_http_response(clientFd, "application/json", "{\"ok\":false,\"reason\":\"invalid eq payload\"}");
            return;
        }

        pthread_mutex_lock(&http->app->dspMutex);
        if (hasResetAll && resetAllValue >= 0.5f)
        {
            float resetGains[EQ_BANDS];
            float resetQs[EQ_BANDS];
            float resetMbAmounts[PLAY_MB_DYN_BANDS];

            for (int i = 0; i < EQ_BANDS; ++i)
            {
                resetGains[i] = 0.0f;
                resetQs[i] = 0.9f;
            }
            for (int i = 0; i < PLAY_MB_DYN_BANDS; ++i)
            {
                resetMbAmounts[i] = 0.0f;
            }

            play_dsp_set_eq_params(&http->app->dsp, resetGains, EQ_BANDS, resetQs, EQ_BANDS);
            play_dsp_set_dynamic_params(&http->app->dsp,
                                        DYNAMIC_EQ_MIN_ATTACK,
                                        DYNAMIC_EQ_MIN_RELEASE,
                                        DYNAMIC_EQ_MIN_THRESHOLD,
                                        0.0f,
                                        DYNAMIC_EQ_MIN_STRENGTH_DB);
            play_dsp_set_multiband_dynamics_config(&http->app->dsp,
                                                   0,
                                                   (uint8_t)PLAY_CROSSFEED_PRE_EQ,
                                                   PLAY_MB_DYN_MIN_THRESHOLD,
                                                   PLAY_MB_DYN_MIN_ATTACK,
                                                   PLAY_MB_DYN_MIN_RELEASE,
                                                   PLAY_MB_DYN_MIN_STRENGTH);
            play_dsp_set_multiband_dynamics_amounts(&http->app->dsp, resetMbAmounts, PLAY_MB_DYN_BANDS);
        }

        if (gainCount > 0 || qCount > 0)
        {
            play_dsp_set_eq_params(&http->app->dsp, gains, gainCount, qs, qCount);
        }

        if (hasAnyDynamic)
        {
            float currentAttack;
            float currentRelease;
            float currentThreshold;
            float currentMaxReductionDB;
            float currentStrengthDB;

            play_dsp_get_dynamic_params(&http->app->dsp,
                                        &currentAttack,
                                        &currentRelease,
                                        &currentThreshold,
                                        &currentMaxReductionDB,
                                        &currentStrengthDB);

            if (hasDynamicAttack)
            {
                currentAttack = dynamicAttack;
            }
            if (hasDynamicRelease)
            {
                currentRelease = dynamicRelease;
            }
            if (hasDynamicThreshold)
            {
                currentThreshold = dynamicThreshold;
            }
            if (hasDynamicMaxReductionDB)
            {
                currentMaxReductionDB = dynamicMaxReductionDB;
            }
            if (hasDynamicStrengthDB)
            {
                currentStrengthDB = dynamicStrengthDB;
            }

            play_dsp_set_dynamic_params(&http->app->dsp,
                                        currentAttack,
                                        currentRelease,
                                        currentThreshold,
                                        currentMaxReductionDB,
                                        currentStrengthDB);
        }

        if (hasPhasePluginEnabled)
        {
            play_dsp_set_plugin_enabled(&http->app->dsp,
                                        PLAY_DSP_PLUGIN_PHASE_ANALYZER,
                                        (phasePluginEnabledValue >= 0.5f) ? 1 : 0);
        }

        if (hasBypassEnabled)
        {
            play_dsp_set_bypass(&http->app->dsp, (bypassEnabledValue >= 0.5f) ? 1 : 0);
        }

        if (hasLowCrossfeedEnabled || hasLowCrossfeedPosition)
        {
            int currentEnabled = 0;
            uint8_t currentPosition = (uint8_t)PLAY_CROSSFEED_PRE_EQ;

            play_dsp_get_low_crossfeed(&http->app->dsp, &currentEnabled, &currentPosition);
            if (hasLowCrossfeedEnabled)
            {
                currentEnabled = (lowCrossfeedEnabledValue >= 0.5f) ? 1 : 0;
            }
            if (hasLowCrossfeedPosition)
            {
                currentPosition = (lowCrossfeedPositionValue >= 0.5f)
                                      ? (uint8_t)PLAY_CROSSFEED_POST_EQ
                                      : (uint8_t)PLAY_CROSSFEED_PRE_EQ;
            }

            play_dsp_set_low_crossfeed(&http->app->dsp, currentEnabled, currentPosition);
        }

        if (hasLinearFirEnabled || hasLinearFirTaps)
        {
            int currentEnabled = 1;
            int currentTaps = PLAY_LINEAR_FIR_DEFAULT_TAPS;
            play_dsp_get_linear_fir_config(&http->app->dsp, &currentEnabled, &currentTaps, NULL, NULL);
            if (hasLinearFirEnabled)
            {
                currentEnabled = (linearFirEnabledValue >= 0.5f) ? 1 : 0;
            }
            if (hasLinearFirTaps)
            {
                currentTaps = (int)(linearFirTapsValue + 0.5f);
            }
            play_dsp_set_linear_fir_config(&http->app->dsp, currentEnabled, currentTaps);
        }

        if (hasAnyMbDyn)
        {
            int currentEnabled = 0;
            uint8_t currentPosition = (uint8_t)PLAY_CROSSFEED_PRE_EQ;
            float currentThreshold = PLAY_MB_DYN_DEFAULT_THRESHOLD;
            float currentAttack = PLAY_MB_DYN_DEFAULT_ATTACK;
            float currentRelease = PLAY_MB_DYN_DEFAULT_RELEASE;
            float currentStrength = PLAY_MB_DYN_DEFAULT_STRENGTH;

            play_dsp_get_multiband_dynamics_config(&http->app->dsp,
                                                   &currentEnabled,
                                                   &currentPosition,
                                                   &currentThreshold,
                                                   &currentAttack,
                                                   &currentRelease,
                                                   &currentStrength);

            if (hasMbDynEnabled)
            {
                currentEnabled = (mbDynEnabledValue >= 0.5f) ? 1 : 0;
            }
            if (hasMbDynPosition)
            {
                currentPosition = (mbDynPositionValue >= 0.5f) ? (uint8_t)PLAY_CROSSFEED_POST_EQ
                                                               : (uint8_t)PLAY_CROSSFEED_PRE_EQ;
            }
            if (hasMbDynThreshold)
            {
                currentThreshold = mbDynThresholdValue;
            }
            if (hasMbDynAttack)
            {
                currentAttack = mbDynAttackValue;
            }
            if (hasMbDynRelease)
            {
                currentRelease = mbDynReleaseValue;
            }
            if (hasMbDynStrength)
            {
                currentStrength = mbDynStrengthValue;
            }

            play_dsp_set_multiband_dynamics_config(&http->app->dsp,
                                                   currentEnabled,
                                                   currentPosition,
                                                   currentThreshold,
                                                   currentAttack,
                                                   currentRelease,
                                                   currentStrength);
            if (mbDynBandCount > 0)
            {
                play_dsp_set_multiband_dynamics_amounts(&http->app->dsp, mbDynBandAmountDb, mbDynBandCount);
            }
        }
        pthread_mutex_unlock(&http->app->dspMutex);

        send_http_response(clientFd, "application/json", "{\"ok\":true}");
        return;
    }

    if (strncmp(req, "GET / ", 6) == 0 || strncmp(req, "GET /HTTP", 9) == 0)
    {
        const char* page =
            "<!doctype html><html><head><meta charset='utf-8'><title>EQ + Spectrum</title>"
            "<style>body{margin:0;background:radial-gradient(circle at 20% 0%,#132238,#060b14 55%);color:#dbeafe;font-family:'Avenir Next',Helvetica,sans-serif;}"
            ".wrap{padding:20px;max-width:1100px;margin:0 auto;}h1{font-size:24px;margin:0 0 8px;}"
            ".sub{opacity:.8;margin-bottom:12px}.panel{background:rgba(15,23,42,.66);border:1px solid #243449;border-radius:14px;padding:12px;}"
            "canvas{width:100%;height:400px;display:block;border-radius:10px;background:linear-gradient(180deg,#0b1220,#050910);}"
            ".dyn{margin-top:12px;display:grid;grid-template-columns:repeat(5,minmax(0,1fr));gap:10px}"
            ".sliders{margin-top:14px;display:block}.sliderGroup{margin-bottom:12px}.sliderGroupTitle{font-size:12px;letter-spacing:.6px;color:#93c5fd;margin:0 0 8px;text-transform:uppercase}.sliderGroupGrid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:10px}.cell{background:#0b1424;border:1px solid #243449;border-radius:10px;padding:8px;}"
            "label{display:block;font-size:12px;opacity:.85;margin-bottom:6px}input[type=range]{width:100%}.val{font-size:12px;color:#93c5fd}"
            ".mode{font-size:10px;padding:1px 6px;border-radius:999px;background:#1f2937;color:#93c5fd;margin-left:6px;letter-spacing:.2px;}"
            ".qval{font-size:12px;color:#fca5a5}.row{display:flex;justify-content:space-between;align-items:center}"
            "@media(max-width:900px){.dyn{grid-template-columns:repeat(2,minmax(0,1fr));}.sliderGroupGrid{grid-template-columns:repeat(2,minmax(0,1fr));}}</style></head>"
            "<body><div class='wrap'><h1>实时频谱 + 多段EQ + 多段压缩</h1><div class='sub'>拖拽橙色控制点或下方滑块，实时改变播放 EQ</div>"
            "<div class='panel'><canvas id='c' width='1000' height='400'></canvas><div id='dyn' class='dyn'></div><div id='sliders' class='sliders'></div></div></div>"
            "<script>const c=document.getElementById('c');const g=c.getContext('2d');const slidersEl=document.getElementById('sliders');"
            "const dynEl=document.getElementById('dyn');let eqDynPane=null,compDynPane=null,eqTabPanel=null,compTabPanel=null,tabEqBtn=null,tabCompBtn=null,mbCurveWrap=null,mbCurveCanvas=null,mbCurveCtx=null,mbCurveDrag=-1;"
            "let specPre=[];let specPost=[];let phaseDelta=[];let groupDelay=[];let phaseEnabled=false;let bypassEnabled=false;let lowCrossfeedEnabled=false;let lowCrossfeedPosition=0;let freqs=[];let gains=[];let qs=[];let modes=[];let dynReduction=[];let minGain=-12,maxGain=12,minQ=.3,maxQ=4,sampleRate=48000;"
            "let dyn={attack:.12,release:.02,threshold:.18,maxReductionDB:12,strengthDB:18};"
            "let dynRange={attackMin:.01,attackMax:.5,releaseMin:.005,releaseMax:.3,thresholdMin:.02,thresholdMax:1,maxReductionDBMin:0,maxReductionDBMax:24,strengthDBMin:1,strengthDBMax:36};"
            "let mbDynEnabled=false,mbDynPosition=0;let mbDyn={threshold:.16,attack:.02,release:.06,strength:1.2};let linearFirEnabled=true,linearFirTaps=25;"
            "let mbDynRange={thresholdMin:.01,thresholdMax:1,attackMin:.002,attackMax:.2,releaseMin:.005,releaseMax:.5,strengthMin:.1,strengthMax:8};"
            "let mbDynBandAmountDB=[0,0,0,0,0,0,0,0],mbDynBandAppliedDB=[0,0,0,0,0,0,0,0],mbDynBandLowHz=[20,80,150,300,700,1500,4000,10000],mbDynBandHighHz=[80,150,300,700,1500,4000,10000,16000];"
            "let drag=-1;let lastPost=0;let lastSpectrumFetch=0;let spectrumPending=false;const spectrumFetchIntervalMs=25;"
            "const specMinDb=-36,specMaxDb=36;"
            "const fMin=20,fMax=22000;const displayFreqs=[20,30,40,50,60,70,80,90,100,160,280,300,400,500,600,1000,1250,1500,1600,1700,1800,2000,2500,3000,4000,5000,6000,7000,8000,10000,12000,14000,15000,16000,17000,18000,20000,22000];function lx(f){return (Math.log(f)-Math.log(fMin))/(Math.log(fMax)-Math.log(fMin));}"
            "function xOfF(f){return 55+lx(f)*(c.width-100);}function yOfDb(db){const d=Math.max(specMinDb,Math.min(specMaxDb,db));return 30+(specMaxDb-d)/(specMaxDb-specMinDb)*(c.height-70);}"
            "function dbOfY(y){const t=(y-30)/(c.height-70);return specMaxDb-t*(specMaxDb-specMinDb);}"
            "function eqModeOfFreq(f){if(f<=220)return 'Linear';if(f>16000)return 'Normal';return 'Dynamic';}"
            "function modeText(m,f){if(m===0)return 'Linear';if(m===1)return 'Dynamic';if(m===2)return 'Normal';return eqModeOfFreq(f);}"
            "function fmtFreq(f){if(f>=1000){const k=f/1000;const s=(Math.abs(k-Math.round(k))<1e-6)?String(Math.round(k)):k.toFixed(k<2?2:1).replace(/\\.0$/,'');return s+'k';}return String(f);}"
            "function spectrumDbAt(freq,bins){if(!bins||bins.length===0)return specMinDb;if(bins.length===1)return bins[0];const lf=Math.log(Math.max(fMin,Math.min(fMax,freq)));const t=(lf-Math.log(fMin))/(Math.log(fMax)-Math.log(fMin));const p=Math.max(0,Math.min(1,t))*(bins.length-1);const i0=Math.floor(p);const i1=Math.min(bins.length-1,i0+1);const f0=i0/(bins.length-1);const f1=i1/(bins.length-1);const l0=Math.log(fMin)+f0*(Math.log(fMax)-Math.log(fMin));const l1=Math.log(fMin)+f1*(Math.log(fMax)-Math.log(fMin));const x=(lf-l0)/(l1-l0+1e-12);const v=bins[i0]+(bins[i1]-bins[i0])*Math.max(0,Math.min(1,x));return Math.max(specMinDb,Math.min(specMaxDb,v));}"
            "function harmanTargetDbAt(freq){const fp=[20,60,100,200,500,1000,2000,3000,5000,8000,10000,12000,16000,20000];const dp=[6.0,5.2,4.2,2.6,1.0,0.0,-0.7,-1.4,-2.0,-2.6,-3.0,-3.4,-4.0,-4.8];if(freq<=fp[0])return dp[0];const n=fp.length-1;if(freq>=fp[n])return dp[n];for(let i=0;i<n;i++){const f0=fp[i],f1=fp[i+1];if(freq>=f0&&freq<=f1){const t=(Math.log(freq)-Math.log(f0))/(Math.log(f1)-Math.log(f0)+1e-12);return dp[i]+(dp[i+1]-dp[i])*t;}}return 0;}"
            "function mbDynCurveDbAt(freq,vals){if(!vals||!vals.length||!mbDynBandLowHz.length||!mbDynBandHighHz.length)return 0;const n=Math.min(vals.length,mbDynBandLowHz.length,mbDynBandHighHz.length);if(n<=0)return 0;const c=[];for(let i=0;i<n;i++){c.push(Math.sqrt(Math.max(20,mbDynBandLowHz[i])*Math.max(20,mbDynBandHighHz[i])));}const map=i=>-(vals[i]||0);if(freq<=c[0])return map(0);if(freq>=c[n-1])return map(n-1);for(let i=0;i<n-1;i++){if(freq>=c[i]&&freq<=c[i+1]){const t=(Math.log(freq)-Math.log(c[i]))/(Math.log(c[i+1])-Math.log(c[i])+1e-12);return map(i)+(map(i+1)-map(i))*t;}}return map(n-1);}"
            "function dynamicReductionAt(freq){if(!freqs.length||!modes.length||!dynReduction.length)return 0;const pts=[];for(let i=0;i<freqs.length;i++){if(modes[i]===1){pts.push({f:freqs[i],r:Math.max(0,dynReduction[i]||0)});}}if(!pts.length)return 0;if(freq<=220||freq>16000)return 0;if(freq<=pts[0].f)return pts[0].r;const last=pts.length-1;if(freq>=pts[last].f)return pts[last].r;for(let i=0;i<pts.length-1;i++){const p0=pts[i],p1=pts[i+1];if(freq>=p0.f&&freq<=p1.f){const t=(Math.log(freq)-Math.log(p0.f))/(Math.log(p1.f)-Math.log(p0.f)+1e-12);return p0.r+(p1.r-p0.r)*t;}}return 0;}"
            "function biquadMagAt(freq,f0,gainDb,q){const A=Math.pow(10,gainDb/40);const w0=2*Math.PI*f0/sampleRate;const alpha=Math.sin(w0)/(2*q);"
            "const b0=1+alpha*A,b1=-2*Math.cos(w0),b2=1-alpha*A,a0=1+alpha/A,a1=-2*Math.cos(w0),a2=1-alpha/A;"
            "const w=2*Math.PI*freq/sampleRate,c1=Math.cos(w),s1=Math.sin(w),c2=Math.cos(2*w),s2=Math.sin(2*w);"
            "const nr=b0+b1*c1+b2*c2,ni=-(b1*s1+b2*s2),dr=a0+a1*c1+a2*c2,di=-(a1*s1+a2*s2);"
            "return Math.sqrt((nr*nr+ni*ni)/(dr*dr+di*di+1e-12));}"
            "function lowpassMagAt(freq,fc,q){const w0=2*Math.PI*fc/sampleRate;const c0=Math.cos(w0),s0=Math.sin(w0);const alpha=s0/(2*q);const b0=(1-c0)/2,b1=1-c0,b2=(1-c0)/2,a0=1+alpha,a1=-2*c0,a2=1-alpha;const w=2*Math.PI*freq/sampleRate,c1=Math.cos(w),s1=Math.sin(w),c2=Math.cos(2*w),s2=Math.sin(2*w);const nr=b0+b1*c1+b2*c2,ni=-(b1*s1+b2*s2),dr=a0+a1*c1+a2*c2,di=-(a1*s1+a2*s2);return Math.sqrt((nr*nr+ni*ni)/(dr*dr+di*di+1e-12));}"
            "function eqResponseDb(freq){let m=1;for(let i=0;i<freqs.length;i++){m*=biquadMagAt(freq,freqs[i],gains[i],qs[i]);}return 20*Math.log10(m+1e-9);}"
            "function splinePath(points,color,width){if(points.length<2)return;g.strokeStyle=color;g.lineWidth=width;g.beginPath();g.moveTo(points[0].x,points[0].y);"
            "for(let i=0;i<points.length-1;i++){const p0=points[i],p1=points[i+1];const cx=(p0.x+p1.x)/2;g.quadraticCurveTo(p0.x,p0.y,cx,(p0.y+p1.y)/2);}"
            "const l=points.length-1;g.lineTo(points[l].x,points[l].y);g.stroke();}"
            "function drawGrid(){g.fillStyle='#0b1220';g.fillRect(0,0,c.width,c.height);g.strokeStyle='rgba(148,163,184,.22)';g.lineWidth=1;"
            "let lastLabelX=-1e9;displayFreqs.forEach(f=>{const x=xOfF(f);g.beginPath();g.moveTo(x,25);g.lineTo(x,c.height-40);g.stroke();if(x-lastLabelX>22){g.fillStyle='rgba(203,213,225,.82)';g.font='11px Avenir Next';g.fillText(fmtFreq(f),x-14,c.height-18);lastLabelX=x;}});"
            "for(let d=specMinDb;d<=specMaxDb;d+=3){const y=yOfDb(d);g.beginPath();g.moveTo(48,y);g.lineTo(c.width-42,y);g.stroke();g.fillStyle='rgba(148,163,184,.8)';g.fillText(d+'dB',8,y+4);}"
            "g.fillStyle='#94a3b8';g.fillText('Pre-EQ',c.width-690,20);g.fillStyle='#22d3ee';g.fillText('Post-EQ',c.width-630,20);g.fillStyle='#facc15';g.fillText('Harman Ref',c.width-565,20);g.fillStyle='#60a5fa';g.fillText('MB Target',c.width-485,20);g.fillStyle='#06b6d4';g.fillText('MB Applied',c.width-405,20);g.fillStyle='#34d399';g.fillText('Dynamic',c.width-325,20);g.fillStyle='#fb923c';g.fillText('EQ points',c.width-260,20);g.fillStyle='#e879f9';g.fillText('Estimated EQ',c.width-190,20);}"
            "function draw(){drawGrid();const hp=[];for(let i=0;i<displayFreqs.length;i++){const f=displayFreqs[i];hp.push({x:xOfF(f),y:yOfDb(harmanTargetDbAt(f))});}splinePath(hp,'#facc15',1.8);const hasMbTarget=(mbDynBandAmountDB||[]).some(v=>Math.abs(v||0)>0.01)||mbDynEnabled;const hasMbApplied=(mbDynBandAppliedDB||[]).some(v=>Math.abs(v||0)>0.01)||mbDynEnabled;if(hasMbTarget){const mp=[];for(let i=0;i<180;i++){const t=i/179;const f=fMin*Math.pow(fMax/fMin,t);const db=mbDynCurveDbAt(f,mbDynBandAmountDB);mp.push({x:xOfF(f),y:yOfDb(db)});}splinePath(mp,'#60a5fa',2.0);}if(hasMbApplied){const ma=[];for(let i=0;i<180;i++){const t=i/179;const f=fMin*Math.pow(fMax/fMin,t);const db=mbDynCurveDbAt(f,mbDynBandAppliedDB);ma.push({x:xOfF(f),y:yOfDb(db)});}splinePath(ma,'#06b6d4',2.0);}if(specPre.length){const sp=[];for(let i=0;i<displayFreqs.length;i++){const f=displayFreqs[i];const db=spectrumDbAt(f,specPre);sp.push({x:xOfF(f),y:yOfDb(db)});}splinePath(sp,'#94a3b8',1.8);}if(specPost.length){const sp=[];for(let i=0;i<displayFreqs.length;i++){const f=displayFreqs[i];const db=spectrumDbAt(f,specPost);sp.push({x:xOfF(f),y:yOfDb(db)});}splinePath(sp,'#22d3ee',2.4);}"
            "if(freqs.length&&modes.length&&dynReduction.length){const dp=[];for(let i=0;i<180;i++){const t=i/179;const f=fMin*Math.pow(fMax/fMin,t);const red=dynamicReductionAt(f);dp.push({x:xOfF(f),y:yOfDb(-red)});}splinePath(dp,'#34d399',2.0);}"
            "if(freqs.length&&gains.length&&qs.length){const rp=[];for(let i=0;i<140;i++){const t=i/139;const f=fMin*Math.pow(fMax/fMin,t);const db=Math.max(minGain,Math.min(maxGain,eqResponseDb(f)));rp.push({x:xOfF(f),y:yOfDb(db)});}splinePath(rp,'#e879f9',2.6);}"
            "if(freqs.length&&gains.length){const ep=freqs.map((f,i)=>({x:xOfF(f),y:yOfDb(gains[i]),f}));splinePath(ep,'#fb923c',3);"
            "ep.forEach(p=>{g.fillStyle='#f59e0b';g.beginPath();g.arc(p.x,p.y,6,0,Math.PI*2);g.fill();g.strokeStyle='#fed7aa';g.lineWidth=1;g.stroke();g.fillStyle='#ffe7cf';g.fillText((p.f>=1000?(p.f/1000).toFixed(p.f%1000?1:0)+'k':p.f)+'Hz',p.x-14,p.y-10);});}}"
            "function schedulePost(){const now=Date.now();if(now-lastPost<30)return;lastPost=now;fetch('/eq',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({gains,qs,dynamicAttack:dyn.attack,dynamicRelease:dyn.release,dynamicThreshold:dyn.threshold,dynamicMaxReductionDB:dyn.maxReductionDB,dynamicStrengthDB:dyn.strengthDB,phasePluginEnabled:phaseEnabled?1:0,bypassEnabled:bypassEnabled?1:0,lowCrossfeedEnabled:lowCrossfeedEnabled?1:0,lowCrossfeedPosition:lowCrossfeedPosition,linearFirEnabled:linearFirEnabled?1:0,linearFirTaps:linearFirTaps,mbDynEnabled:mbDynEnabled?1:0,mbDynPosition:mbDynPosition,mbDynThreshold:mbDyn.threshold,mbDynAttack:mbDyn.attack,mbDynRelease:mbDyn.release,mbDynStrength:mbDyn.strength,mbDynBandAmountDB:mbDynBandAmountDB})}).catch(()=>{});}"
            "function syncBypassUi(){const b=document.getElementById('bp');const v=document.getElementById('bpv');if(b)b.checked=!!bypassEnabled;if(v)v.textContent=bypassEnabled?'ON':'OFF';}"
            "function bindBypassUi(){const b=document.getElementById('bp');if(!b)return;b.addEventListener('change',()=>{bypassEnabled=!!b.checked;syncBypassUi();schedulePost();});syncBypassUi();}"
            "function bindResetUi(){const b=document.getElementById('resetAll');if(!b)return;b.addEventListener('click',async()=>{try{await fetch('/eq',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({resetAll:1})});}catch(e){}window.location.reload();});}"
            "function setupTabs(){const panel=c.parentElement;const toolbar=document.createElement('div');toolbar.style.margin='12px 0 10px';toolbar.style.display='flex';toolbar.style.justifyContent='space-between';toolbar.style.alignItems='center';toolbar.style.gap='10px';toolbar.style.flexWrap='wrap';toolbar.innerHTML='<div><label style=\"display:inline-block\"><input id=\"bp\" type=\"checkbox\"> Bypass DSP</label> <span id=\"bpv\" class=\"val\">OFF</span> <button id=\"resetAll\" type=\"button\" style=\"margin-left:10px;background:#7f1d1d;color:#fecaca;border:1px solid #b91c1c;border-radius:10px;padding:5px 10px;cursor:pointer\">Reset</button></div><div><button id=\"tabEq\" type=\"button\" style=\"background:#1e293b;color:#e2e8f0;border:1px solid #475569;border-radius:10px;padding:6px 12px;cursor:pointer\">多段 EQ</button> <button id=\"tabComp\" type=\"button\" style=\"background:#0f172a;color:#94a3b8;border:1px solid #334155;border-radius:10px;padding:6px 12px;cursor:pointer\">多段压缩</button></div>';panel.insertBefore(toolbar,dynEl);eqTabPanel=document.createElement('div');compTabPanel=document.createElement('div');eqTabPanel.style.marginTop='8px';compTabPanel.style.marginTop='8px';compTabPanel.style.display='none';eqDynPane=document.createElement('div');eqDynPane.className='dyn';compDynPane=document.createElement('div');compDynPane.className='dyn';eqTabPanel.appendChild(eqDynPane);eqTabPanel.appendChild(slidersEl);compTabPanel.appendChild(compDynPane);panel.insertBefore(eqTabPanel,dynEl);panel.insertBefore(compTabPanel,dynEl);dynEl.style.display='none';tabEqBtn=document.getElementById('tabEq');tabCompBtn=document.getElementById('tabComp');const setTab=(name)=>{const eqOn=name!=='comp';eqTabPanel.style.display=eqOn?'block':'none';compTabPanel.style.display=eqOn?'none':'block';if(tabEqBtn){tabEqBtn.style.background=eqOn?'#1e293b':'#0f172a';tabEqBtn.style.color=eqOn?'#e2e8f0':'#94a3b8';}if(tabCompBtn){tabCompBtn.style.background=eqOn?'#0f172a':'#1e293b';tabCompBtn.style.color=eqOn?'#94a3b8':'#e2e8f0';}};if(tabEqBtn)tabEqBtn.addEventListener('click',()=>setTab('eq'));if(tabCompBtn)tabCompBtn.addEventListener('click',()=>setTab('comp'));setTab('eq');}"
            "function splitDynControlsByTab(){if(!eqDynPane||!compDynPane)return;eqDynPane.innerHTML='';compDynPane.innerHTML='';const cells=Array.from(dynEl.children);cells.forEach((cell,i)=>{if(i<8){eqDynPane.appendChild(cell);}else{compDynPane.appendChild(cell);}});}"
            "function ensureMbCurveUi(){if(!compTabPanel||mbCurveCanvas)return;mbCurveWrap=document.createElement('div');mbCurveWrap.className='cell';mbCurveWrap.style.marginTop='10px';mbCurveWrap.innerHTML='<div class=row><label>压缩曲线示意（每段贝赛）</label><span class=val id=mbCurveHint>实线=Target 虚线=Applied，拖动节点可调</span></div><canvas id=mbCurve width=960 height=210 style=\"width:100%;height:210px;display:block;border-radius:8px;background:linear-gradient(180deg,#08111d,#050910);border:1px solid #213145\"></canvas>';mbCurveCanvas=mbCurveWrap.querySelector('#mbCurve');mbCurveCtx=mbCurveCanvas.getContext('2d');compTabPanel.insertBefore(mbCurveWrap,compDynPane);const xPad=28,yPad=20,w=()=>mbCurveCanvas.width-2*xPad,h=()=>mbCurveCanvas.height-2*yPad;const xAt=f=>xPad+((Math.log(Math.max(20,Math.min(20000,f)))-Math.log(20))/(Math.log(20000)-Math.log(20)))*w();const yAt=v=>yPad+((6-Math.max(-6,Math.min(6,v)))/12)*h();const vAt=y=>6-((Math.max(yPad,Math.min(yPad+h(),y))-yPad)/h())*12;const centers=()=>mbDynBandLowHz.map((lo,i)=>Math.sqrt(Math.max(20,lo)*Math.max(20,mbDynBandHighHz[i]||lo)));const findBand=x=>{const c=centers();let bi=0,bd=1e9;for(let i=0;i<c.length;i++){const d=Math.abs(xAt(c[i])-x);if(d<bd){bd=d;bi=i;}}return bd<34?bi:-1;};const syncBand=i=>{const s=document.getElementById('mbas'+i);const v=document.getElementById('mbav'+i);if(s)s.value=(mbDynBandAmountDB[i]||0).toFixed(2);if(v)v.textContent=(mbDynBandAmountDB[i]||0).toFixed(1);};const blendBand=(idx,target,follow)=>{if(idx<0||idx>=mbDynBandAmountDB.length)return;const cur=mbDynBandAmountDB[idx]||0;const clamped=Math.max(-6,Math.min(6,target));mbDynBandAmountDB[idx]=cur+(clamped-cur)*follow;syncBand(idx);};const smoothSet=(i,target)=>{const center=Math.max(-6,Math.min(6,target));blendBand(i,center,0.45);blendBand(i-1,center*0.72,0.22);blendBand(i+1,center*0.72,0.22);blendBand(i-2,center*0.45,0.12);blendBand(i+2,center*0.45,0.12);};mbCurveCanvas.addEventListener('mousedown',e=>{const r=mbCurveCanvas.getBoundingClientRect();const x=(e.clientX-r.left)*mbCurveCanvas.width/r.width;mbCurveDrag=findBand(x);});window.addEventListener('mousemove',e=>{if(mbCurveDrag<0)return;const r=mbCurveCanvas.getBoundingClientRect();const y=(e.clientY-r.top)*mbCurveCanvas.height/r.height;smoothSet(mbCurveDrag,vAt(y));drawMbCurve();schedulePost();});window.addEventListener('mouseup',()=>{mbCurveDrag=-1;});}"
            "function updateMbAppliedLabels(){for(let i=0;i<8;i++){const a=document.getElementById('mbap'+i);if(a)a.textContent=((mbDynBandAppliedDB[i]||0).toFixed(2)+' dB');}}"
            "function drawMbCurve(){if(!mbCurveCtx||!mbCurveCanvas)return;const g2=mbCurveCtx,w=mbCurveCanvas.width,h=mbCurveCanvas.height,xPad=28,yPad=20,iw=w-2*xPad,ih=h-2*yPad;const xAt=f=>xPad+((Math.log(Math.max(20,Math.min(20000,f)))-Math.log(20))/(Math.log(20000)-Math.log(20)))*iw;const yAt=v=>yPad+((6-Math.max(-6,Math.min(6,v)))/12)*ih;g2.fillStyle='#07101b';g2.fillRect(0,0,w,h);g2.strokeStyle='rgba(148,163,184,.26)';g2.lineWidth=1;for(let d=-6;d<=6;d+=3){const y=yAt(d);g2.beginPath();g2.moveTo(xPad,y);g2.lineTo(w-xPad,y);g2.stroke();}const edges=[mbDynBandLowHz[0]||20].concat(mbDynBandHighHz);g2.strokeStyle='rgba(125,211,252,.35)';edges.forEach(f=>{const x=xAt(f);g2.beginPath();g2.moveTo(x,yPad);g2.lineTo(x,h-yPad);g2.stroke();});const c=mbDynBandLowHz.map((lo,i)=>Math.sqrt(Math.max(20,lo)*Math.max(20,mbDynBandHighHz[i]||lo)));const pts=c.map((f,i)=>({x:xAt(f),y:yAt(mbDynBandAmountDB[i]||0),v:(mbDynBandAmountDB[i]||0)}));const ap=pts.map((p,i)=>({x:p.x,y:yAt(-(mbDynBandAppliedDB[i]||0)),v:-(mbDynBandAppliedDB[i]||0)}));g2.strokeStyle='#38bdf8';g2.lineWidth=2.3;for(let i=0;i<pts.length-1;i++){const p0=pts[i],p1=pts[i+1],cx=(p0.x+p1.x)/2,cy=(p0.y+p1.y)/2-(p1.v-p0.v)*3.0;g2.beginPath();g2.moveTo(p0.x,p0.y);g2.quadraticCurveTo(cx,cy,p1.x,p1.y);g2.stroke();}g2.save();g2.setLineDash([6,5]);g2.strokeStyle='rgba(6,182,212,.95)';g2.lineWidth=1.8;for(let i=0;i<ap.length-1;i++){const p0=ap[i],p1=ap[i+1],cx=(p0.x+p1.x)/2,cy=(p0.y+p1.y)/2-(p1.v-p0.v)*2.2;g2.beginPath();g2.moveTo(p0.x,p0.y);g2.quadraticCurveTo(cx,cy,p1.x,p1.y);g2.stroke();}g2.restore();pts.forEach((p,i)=>{g2.fillStyle=i===mbCurveDrag?'#f59e0b':'#22d3ee';g2.beginPath();g2.arc(p.x,p.y,5,0,Math.PI*2);g2.fill();});g2.fillStyle='rgba(191,219,254,.9)';g2.font='11px Avenir Next';for(let i=0;i<pts.length;i++){g2.fillText((mbDynBandAmountDB[i]||0).toFixed(1),pts[i].x-10,pts[i].y-9);}g2.fillStyle='rgba(148,163,184,.95)';g2.fillText('Target',xPad+6,yPad+12);g2.fillStyle='rgba(6,182,212,.95)';g2.fillText('Applied',xPad+62,yPad+12);}"
            "function buildDynamic(){const items=[{key:'attack',label:'Dyn Attack',min:dynRange.attackMin,max:dynRange.attackMax,step:0.001,digits:3},{key:'release',label:'Dyn Release',min:dynRange.releaseMin,max:dynRange.releaseMax,step:0.001,digits:3},{key:'threshold',label:'Threshold',min:dynRange.thresholdMin,max:dynRange.thresholdMax,step:0.005,digits:3},{key:'maxReductionDB',label:'Max Red (dB)',min:dynRange.maxReductionDBMin,max:dynRange.maxReductionDBMax,step:0.1,digits:1},{key:'strengthDB',label:'Strength (dB)',min:dynRange.strengthDBMin,max:dynRange.strengthDBMax,step:0.1,digits:1}];dynEl.innerHTML='';const phaseCell=document.createElement('div');phaseCell.className='cell';phaseCell.innerHTML='<div class=row><label>Phase Plugin</label><span class=val id=pv>'+(phaseEnabled?'ON':'OFF')+'</span></div><input id=pe type=checkbox '+(phaseEnabled?'checked':'')+'>';dynEl.appendChild(phaseCell);const pe=phaseCell.querySelector('#pe');const pv=phaseCell.querySelector('#pv');const upPhase=()=>{phaseEnabled=!!pe.checked;pv.textContent=phaseEnabled?'ON':'OFF';schedulePost();};pe.addEventListener('change',upPhase);const crossCell=document.createElement('div');crossCell.className='cell';crossCell.innerHTML='<div class=row><label>Low Crossfeed</label><span class=val id=cv>'+(lowCrossfeedEnabled?'ON':'OFF')+'</span></div><input id=ce type=checkbox '+(lowCrossfeedEnabled?'checked':'')+'><div class=row><label>Position</label><span class=val id=cpv>'+(lowCrossfeedPosition? 'Post EQ':'Pre EQ')+'</span></div><select id=cp><option value=0'+(lowCrossfeedPosition===0?' selected':'')+'>Pre EQ</option><option value=1'+(lowCrossfeedPosition===1?' selected':'')+'>Post EQ</option></select>';dynEl.appendChild(crossCell);const ce=crossCell.querySelector('#ce');const cv=crossCell.querySelector('#cv');const cp=crossCell.querySelector('#cp');const cpv=crossCell.querySelector('#cpv');const upCross=()=>{lowCrossfeedEnabled=!!ce.checked;lowCrossfeedPosition=parseInt(cp.value,10)||0;cv.textContent=lowCrossfeedEnabled?'ON':'OFF';cpv.textContent=lowCrossfeedPosition?'Post EQ':'Pre EQ';schedulePost();};ce.addEventListener('change',upCross);cp.addEventListener('change',upCross);const firCell=document.createElement('div');firCell.className='cell';const firDelayMs=((Math.max(17,Math.min(33,linearFirTaps))-1)*0.5)*1000/Math.max(1,sampleRate);firCell.innerHTML='<div class=row><label>Linear FIR</label><span class=val id=lfv>'+(linearFirEnabled?'ON':'OFF')+'</span></div><input id=lfe type=checkbox '+(linearFirEnabled?'checked':'')+'><div class=row><label>FIR Taps</label><span class=val id=lfd>'+firDelayMs.toFixed(3)+' ms</span></div><select id=lft><option value=17'+(linearFirTaps===17?' selected':'')+'>17</option><option value=25'+(linearFirTaps===25?' selected':'')+'>25</option><option value=33'+(linearFirTaps===33?' selected':'')+'>33</option></select>';dynEl.appendChild(firCell);const lfe=firCell.querySelector('#lfe');const lfv=firCell.querySelector('#lfv');const lft=firCell.querySelector('#lft');const lfd=firCell.querySelector('#lfd');const upFir=()=>{linearFirEnabled=!!lfe.checked;linearFirTaps=parseInt(lft.value,10)||25;const d=((linearFirTaps-1)*0.5)*1000/Math.max(1,sampleRate);lfv.textContent=linearFirEnabled?'ON':'OFF';lfd.textContent=d.toFixed(3)+' ms';schedulePost();};lfe.addEventListener('change',upFir);lft.addEventListener('change',upFir);items.forEach(it=>{const cell=document.createElement('div');cell.className='cell';cell.innerHTML='<div class=row><label>'+it.label+'</label><span class=val id=dv_'+it.key+'></span></div><input id=ds_'+it.key+' type=range min='+it.min+' max='+it.max+' step='+it.step+' value='+dyn[it.key]+'>';dynEl.appendChild(cell);const s=cell.querySelector('#ds_'+it.key);const v=cell.querySelector('#dv_'+it.key);const upd=()=>{dyn[it.key]=parseFloat(s.value);v.textContent=dyn[it.key].toFixed(it.digits);schedulePost();};s.addEventListener('input',upd);upd();});}"
            "function buildMbDyn(){const names=['20-80','80-150','150-300','300-700','700-1.5k','1.5k-4k','4k-10k','10k-16k'];const head=document.createElement('div');head.className='cell';head.innerHTML='<div class=row><label>MB Dyn</label><span class=val id=mbv>'+(mbDynEnabled?'ON':'OFF')+'</span></div><input id=mbe type=checkbox '+(mbDynEnabled?'checked':'')+'><div class=row><label>Position</label><span class=val id=mbpv>'+(mbDynPosition?'Post EQ':'Pre EQ')+'</span></div><select id=mbp><option value=0'+(mbDynPosition===0?' selected':'')+'>Pre EQ</option><option value=1'+(mbDynPosition===1?' selected':'')+'>Post EQ</option></select>';dynEl.appendChild(head);const mbe=head.querySelector('#mbe');const mbv=head.querySelector('#mbv');const mbp=head.querySelector('#mbp');const mbpv=head.querySelector('#mbpv');const upSwitch=()=>{mbDynEnabled=!!mbe.checked;mbDynPosition=parseInt(mbp.value,10)||0;mbv.textContent=mbDynEnabled?'ON':'OFF';mbpv.textContent=mbDynPosition?'Post EQ':'Pre EQ';drawMbCurve();schedulePost();};mbe.addEventListener('change',upSwitch);mbp.addEventListener('change',upSwitch);const gitems=[{k:'threshold',label:'MB Thres',min:mbDynRange.thresholdMin,max:mbDynRange.thresholdMax,step:0.005,d:3},{k:'attack',label:'MB Attack',min:mbDynRange.attackMin,max:mbDynRange.attackMax,step:0.001,d:3},{k:'release',label:'MB Release',min:mbDynRange.releaseMin,max:mbDynRange.releaseMax,step:0.001,d:3},{k:'strength',label:'MB Strength',min:mbDynRange.strengthMin,max:mbDynRange.strengthMax,step:0.05,d:2}];gitems.forEach(it=>{const cell=document.createElement('div');cell.className='cell';cell.innerHTML='<div class=row><label>'+it.label+'</label><span class=val id=mbgv_'+it.k+'></span></div><input id=mbgs_'+it.k+' type=range min='+it.min+' max='+it.max+' step='+it.step+' value='+mbDyn[it.k]+'>';dynEl.appendChild(cell);const s=cell.querySelector('#mbgs_'+it.k);const v=cell.querySelector('#mbgv_'+it.k);const upd=()=>{mbDyn[it.k]=parseFloat(s.value);v.textContent=mbDyn[it.k].toFixed(it.d);drawMbCurve();schedulePost();};s.addEventListener('input',upd);upd();});for(let i=0;i<names.length;i++){const cell=document.createElement('div');cell.className='cell';cell.innerHTML='<div class=row><label>MB '+names[i]+' dB</label><span class=val id=mbav'+i+'></span></div><input id=mbas'+i+' type=range min=-6 max=6 step=0.1 value='+(mbDynBandAmountDB[i]||0)+'><div class=row><label>Applied</label><span class=val id=mbap'+i+'></span></div>';dynEl.appendChild(cell);const s=cell.querySelector('#mbas'+i);const v=cell.querySelector('#mbav'+i);const a=cell.querySelector('#mbap'+i);const upd=()=>{mbDynBandAmountDB[i]=parseFloat(s.value);v.textContent=mbDynBandAmountDB[i].toFixed(1);a.textContent=(mbDynBandAppliedDB[i]||0).toFixed(2)+' dB';drawMbCurve();schedulePost();};s.addEventListener('input',upd);upd();}}"
            "function buildSliders(){slidersEl.innerHTML='';const groups=[{name:'低频',from:0,to:220},{name:'中频',from:221,to:16000},{name:'高频',from:16001,to:50000}];groups.forEach(gr=>{const wrap=document.createElement('div');wrap.className='sliderGroup';const title=document.createElement('div');title.className='sliderGroupTitle';title.textContent=gr.name;const grid=document.createElement('div');grid.className='sliderGroupGrid';freqs.forEach((f,i)=>{if(!(f>=gr.from&&f<=gr.to))return;const cell=document.createElement('div');cell.className='cell';cell.innerHTML='<div class=row><label>'+(f>=1000?(f/1000).toFixed(f%1000?1:0)+'k':f)+'Hz <span class=mode>'+modeText(modes[i],f)+'</span></label><span class=val id=v'+i+'></span></div><input id=sg'+i+' type=range min='+minGain+' max='+maxGain+' step=0.1 value='+gains[i]+'><div class=row><label>Q</label><span class=qval id=qv'+i+'></span></div><input id=sq'+i+' type=range min='+minQ+' max='+maxQ+' step=0.01 value='+qs[i]+'>';grid.appendChild(cell);const sg=cell.querySelector('#sg'+i);const sq=cell.querySelector('#sq'+i);const v=cell.querySelector('#v'+i);const qv=cell.querySelector('#qv'+i);const upd=()=>{gains[i]=parseFloat(sg.value);qs[i]=parseFloat(sq.value);v.textContent=gains[i].toFixed(1)+' dB';qv.textContent=qs[i].toFixed(2);draw();schedulePost();};sg.addEventListener('input',upd);sq.addEventListener('input',upd);upd();});wrap.appendChild(title);wrap.appendChild(grid);slidersEl.appendChild(wrap);});}"
            "c.addEventListener('mousedown',e=>{if(!freqs.length)return;const r=c.getBoundingClientRect();const x=(e.clientX-r.left)*c.width/r.width;const y=(e.clientY-r.top)*c.height/r.height;let best=-1,bestD=1e9;freqs.forEach((f,i)=>{const dx=x-xOfF(f),dy=y-yOfDb(gains[i]);const d=dx*dx+dy*dy;if(d<bestD){bestD=d;best=i;}});if(bestD<400)drag=best;if(drag>=0){gains[drag]=Math.max(minGain,Math.min(maxGain,dbOfY(y)));const s=document.getElementById('sg'+drag);if(s)s.value=gains[drag];const v=document.getElementById('v'+drag);if(v)v.textContent=gains[drag].toFixed(1)+' dB';draw();schedulePost();}});"
            "window.addEventListener('mousemove',e=>{if(drag<0)return;const r=c.getBoundingClientRect();const y=(e.clientY-r.top)*c.height/r.height;gains[drag]=Math.max(minGain,Math.min(maxGain,dbOfY(y)));const s=document.getElementById('sg'+drag);if(s)s.value=gains[drag];const v=document.getElementById('v'+drag);if(v)v.textContent=gains[drag].toFixed(1)+' dB';draw();schedulePost();});"
            "window.addEventListener('mouseup',()=>{drag=-1;});"
            "async function pollSpectrum(){const now=performance.now();if(!spectrumPending&&(now-lastSpectrumFetch)>=spectrumFetchIntervalMs){spectrumPending=true;lastSpectrumFetch=now;fetch('/spectrum').then(r=>r.ok?r.json():null).then(j=>{if(!j)return;if(Array.isArray(j.preBins))specPre=j.preBins;if(Array.isArray(j.postBins))specPost=j.postBins;if(Array.isArray(j.phaseDeltaDeg))phaseDelta=j.phaseDeltaDeg;if(Array.isArray(j.groupDelayMs))groupDelay=j.groupDelayMs;if(typeof j.phasePluginEnabled==='boolean')phaseEnabled=j.phasePluginEnabled;if(Array.isArray(j.bins)&&!specPost.length)specPost=j.bins;}).catch(()=>{}).finally(()=>{spectrumPending=false;});}requestAnimationFrame(pollSpectrum);}"
            "async function init(){setupTabs();bindBypassUi();bindResetUi();try{const r=await fetch('/eq');if(r.ok){const j=await r.json();if(Array.isArray(j.freqs)&&Array.isArray(j.gains)&&Array.isArray(j.qs)){freqs=j.freqs;gains=j.gains;qs=j.qs;modes=Array.isArray(j.modes)?j.modes:freqs.map(f=>f<=220?0:(f>16000?2:1));dynReduction=Array.isArray(j.dynamicReductionDB)?j.dynamicReductionDB:freqs.map(()=>0);minGain=j.minGain;maxGain=j.maxGain;minQ=j.minQ;maxQ=j.maxQ;sampleRate=j.sampleRate||sampleRate;if(typeof j.phasePluginEnabled==='boolean')phaseEnabled=j.phasePluginEnabled;if(typeof j.bypassEnabled==='boolean')bypassEnabled=j.bypassEnabled;if(typeof j.lowCrossfeedEnabled==='boolean')lowCrossfeedEnabled=j.lowCrossfeedEnabled;if(typeof j.lowCrossfeedPosition==='number')lowCrossfeedPosition=(j.lowCrossfeedPosition>=0.5?1:0);if(typeof j.linearFirEnabled==='boolean')linearFirEnabled=j.linearFirEnabled;if(typeof j.linearFirTaps==='number')linearFirTaps=(j.linearFirTaps<=17?17:(j.linearFirTaps<=25?25:33));if(typeof j.dynamicAttack==='number')dyn.attack=j.dynamicAttack;if(typeof j.dynamicRelease==='number')dyn.release=j.dynamicRelease;if(typeof j.dynamicThreshold==='number')dyn.threshold=j.dynamicThreshold;if(typeof j.dynamicMaxReductionDB==='number')dyn.maxReductionDB=j.dynamicMaxReductionDB;if(typeof j.dynamicStrengthDB==='number')dyn.strengthDB=j.dynamicStrengthDB;if(typeof j.dynamicAttackMin==='number')dynRange.attackMin=j.dynamicAttackMin;if(typeof j.dynamicAttackMax==='number')dynRange.attackMax=j.dynamicAttackMax;if(typeof j.dynamicReleaseMin==='number')dynRange.releaseMin=j.dynamicReleaseMin;if(typeof j.dynamicReleaseMax==='number')dynRange.releaseMax=j.dynamicReleaseMax;if(typeof j.dynamicThresholdMin==='number')dynRange.thresholdMin=j.dynamicThresholdMin;if(typeof j.dynamicThresholdMax==='number')dynRange.thresholdMax=j.dynamicThresholdMax;if(typeof j.dynamicMaxReductionDBMin==='number')dynRange.maxReductionDBMin=j.dynamicMaxReductionDBMin;if(typeof j.dynamicMaxReductionDBMax==='number')dynRange.maxReductionDBMax=j.dynamicMaxReductionDBMax;if(typeof j.dynamicStrengthDBMin==='number')dynRange.strengthDBMin=j.dynamicStrengthDBMin;if(typeof j.dynamicStrengthDBMax==='number')dynRange.strengthDBMax=j.dynamicStrengthDBMax;if(typeof j.mbDynEnabled==='boolean')mbDynEnabled=j.mbDynEnabled;if(typeof j.mbDynPosition==='number')mbDynPosition=(j.mbDynPosition>=0.5?1:0);if(typeof j.mbDynThreshold==='number')mbDyn.threshold=j.mbDynThreshold;if(typeof j.mbDynAttack==='number')mbDyn.attack=j.mbDynAttack;if(typeof j.mbDynRelease==='number')mbDyn.release=j.mbDynRelease;if(typeof j.mbDynStrength==='number')mbDyn.strength=j.mbDynStrength;if(Array.isArray(j.mbDynBandAmountDB))mbDynBandAmountDB=j.mbDynBandAmountDB.slice(0,8);if(Array.isArray(j.mbDynBandAppliedDB))mbDynBandAppliedDB=j.mbDynBandAppliedDB.slice(0,8);if(Array.isArray(j.mbDynBandLowHz))mbDynBandLowHz=j.mbDynBandLowHz.slice(0,8);if(Array.isArray(j.mbDynBandHighHz))mbDynBandHighHz=j.mbDynBandHighHz.slice(0,8);if(typeof j.mbDynThresholdMin==='number')mbDynRange.thresholdMin=j.mbDynThresholdMin;if(typeof j.mbDynThresholdMax==='number')mbDynRange.thresholdMax=j.mbDynThresholdMax;if(typeof j.mbDynAttackMin==='number')mbDynRange.attackMin=j.mbDynAttackMin;if(typeof j.mbDynAttackMax==='number')mbDynRange.attackMax=j.mbDynAttackMax;if(typeof j.mbDynReleaseMin==='number')mbDynRange.releaseMin=j.mbDynReleaseMin;if(typeof j.mbDynReleaseMax==='number')mbDynRange.releaseMax=j.mbDynReleaseMax;if(typeof j.mbDynStrengthMin==='number')mbDynRange.strengthMin=j.mbDynStrengthMin;if(typeof j.mbDynStrengthMax==='number')mbDynRange.strengthMax=j.mbDynStrengthMax;buildSliders();buildDynamic();buildMbDyn();splitDynControlsByTab();ensureMbCurveUi();drawMbCurve();updateMbAppliedLabels();}}}catch(e){}if(!dynEl.children.length){buildDynamic();buildMbDyn();splitDynControlsByTab();}ensureMbCurveUi();drawMbCurve();updateMbAppliedLabels();syncBypassUi();draw();pollSpectrum();setInterval(async()=>{try{const r=await fetch('/eq');if(r.ok){const j=await r.json();if(Array.isArray(j.dynamicReductionDB))dynReduction=j.dynamicReductionDB;if(Array.isArray(j.modes))modes=j.modes;if(typeof j.phasePluginEnabled==='boolean')phaseEnabled=j.phasePluginEnabled;if(typeof j.bypassEnabled==='boolean')bypassEnabled=j.bypassEnabled;if(typeof j.lowCrossfeedEnabled==='boolean')lowCrossfeedEnabled=j.lowCrossfeedEnabled;if(typeof j.lowCrossfeedPosition==='number')lowCrossfeedPosition=(j.lowCrossfeedPosition>=0.5?1:0);if(typeof j.linearFirEnabled==='boolean')linearFirEnabled=j.linearFirEnabled;if(typeof j.linearFirTaps==='number')linearFirTaps=(j.linearFirTaps<=17?17:(j.linearFirTaps<=25?25:33));if(typeof j.mbDynEnabled==='boolean')mbDynEnabled=j.mbDynEnabled;if(typeof j.mbDynPosition==='number')mbDynPosition=(j.mbDynPosition>=0.5?1:0);if(Array.isArray(j.mbDynBandAppliedDB))mbDynBandAppliedDB=j.mbDynBandAppliedDB.slice(0,8);const lfe=document.getElementById('lfe');const lft=document.getElementById('lft');const lfv=document.getElementById('lfv');const lfd=document.getElementById('lfd');if(lfe)lfe.checked=!!linearFirEnabled;if(lft)lft.value=String(linearFirTaps);if(lfv)lfv.textContent=linearFirEnabled?'ON':'OFF';if(lfd){const d=((linearFirTaps-1)*0.5)*1000/Math.max(1,sampleRate);lfd.textContent=d.toFixed(3)+' ms';}syncBypassUi();updateMbAppliedLabels();drawMbCurve();if(compDynPane&&compDynPane.childElementCount){buildMbDyn();splitDynControlsByTab();ensureMbCurveUi();drawMbCurve();updateMbAppliedLabels();}}}catch(e){}draw();},100);}init();"
            "</script></body></html>";
        send_http_response(clientFd, "text/html; charset=utf-8", page);
        return;
    }

    send_not_found(clientFd);
}

static void* http_server_thread_main(void* userData)
{
    http_server_state* http = (http_server_state*)userData;
    struct sockaddr_in addr;

    http->serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (http->serverFd < 0)
    {
        perror("socket");
        return NULL;
    }

    {
        int enable = 1;
        setsockopt(http->serverFd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(HTTP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(http->serverFd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(http->serverFd);
        http->serverFd = -1;
        return NULL;
    }

    if (listen(http->serverFd, 8) < 0)
    {
        perror("listen");
        close(http->serverFd);
        http->serverFd = -1;
        return NULL;
    }

    while (atomic_load_explicit(&http->running, memory_order_acquire))
    {
        int clientFd = accept(http->serverFd, NULL, NULL);
        if (clientFd < 0)
        {
            continue;
        }
        handle_http_request(http, clientFd);
        close(clientFd);
    }

    if (http->serverFd >= 0)
    {
        close(http->serverFd);
        http->serverFd = -1;
    }

    return NULL;
}

const char* input_audio_file_path = "3.wav";

int main(void)
{
    ma_result result;
    ma_device device;
    ma_device_config deviceConfig;
    ma_decoder_config decoderConfig;
    wav_input_source wavSource;
    app_audio_state app;
    http_server_state http;

    memset(&app, 0, sizeof(app));
    memset(&http, 0, sizeof(http));
    memset(&wavSource, 0, sizeof(wavSource));
    http.serverFd = -1;

    deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format = ma_format_f32;
    deviceConfig.playback.channels = 2;
    deviceConfig.sampleRate = 48000;
    deviceConfig.dataCallback = audio_data_callback;
    deviceConfig.pUserData = &app;

    result = ma_device_init(NULL, &deviceConfig, &device);
    if (result != MA_SUCCESS)
    {
        printf("初始化播放设备失败。\n");
        return -1;
    }

    decoderConfig = ma_decoder_config_init(ma_format_f32, device.playback.channels, device.sampleRate);

    result = ma_decoder_init_file(input_audio_file_path, &decoderConfig, &wavSource.decoder);
    if (result != MA_SUCCESS)
    {
        printf("加载 wav 失败。\n");
        ma_device_uninit(&device);
        return -1;
    }

    app.channels = device.playback.channels;
    app.input.userData = &wavSource;
    app.input.read_frames = wav_read_frames;
    app.input.rewind = wav_rewind;
    if (pthread_mutex_init(&app.dspMutex, NULL) != 0)
    {
        printf("初始化互斥锁失败。\n");
        ma_device_uninit(&device);
        ma_decoder_uninit(&wavSource.decoder);
        return -1;
    }
    if (play_dsp_init(&app.dsp, device.sampleRate, app.channels) != 0)
    {
        printf("初始化 DSP 失败。\n");
        pthread_mutex_destroy(&app.dspMutex);
        ma_device_uninit(&device);
        ma_decoder_uninit(&wavSource.decoder);
        return -1;
    }

    http.app = &app;
    atomic_init(&http.running, true);
    if (pthread_create(&http.thread, NULL, http_server_thread_main, &http) != 0)
    {
        printf("启动 HTTP 线程失败。\n");
        pthread_mutex_destroy(&app.dspMutex);
        ma_device_uninit(&device);
        ma_decoder_uninit(&wavSource.decoder);
        return -1;
    }

    result = ma_device_start(&device);
    if (result != MA_SUCCESS)
    {
        printf("启动播放设备失败。\n");
        atomic_store_explicit(&http.running, false, memory_order_release);
        if (http.serverFd >= 0)
        {
            shutdown(http.serverFd, SHUT_RDWR);
        }
        pthread_join(http.thread, NULL);
        pthread_mutex_destroy(&app.dspMutex);
        ma_device_uninit(&device);
        ma_decoder_uninit(&wavSource.decoder);
        return -1;
    }

    printf("正在循环播放 test.wav（20-300Hz +3dB EQ）。\n");
    printf("频谱可视化: http://127.0.0.1:%d\n", HTTP_PORT);
    printf("按回车键退出。\n");
    getchar();

    atomic_store_explicit(&http.running, false, memory_order_release);
    if (http.serverFd >= 0)
    {
        shutdown(http.serverFd, SHUT_RDWR);
    }
    pthread_join(http.thread, NULL);

    ma_device_uninit(&device);
    ma_decoder_uninit(&wavSource.decoder);
    pthread_mutex_destroy(&app.dspMutex);

    return 0;
}
