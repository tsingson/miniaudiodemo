#include "miniaudio.h"
#include "play_dsp_common.h"

#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
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
    volatile bool running;
    int serverFd;
    pthread_t thread;
} http_server_state;

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
    ma_uint64 totalRead = 0;

    (void)pInput;

    while (totalRead < frameCount)
    {
        ma_uint64 chunkRead = 0;
        ma_result readResult = app->input.read_frames(app->input.userData, out + totalRead * app->channels,
                                                      frameCount - totalRead, &chunkRead);

        if (readResult != MA_SUCCESS || chunkRead == 0)
        {
            if (app->input.rewind(app->input.userData) != MA_SUCCESS)
            {
                memset(out + totalRead * app->channels, 0,
                       (size_t)((frameCount - totalRead) * app->channels * sizeof(float)));
                break;
            }
            continue;
        }

        totalRead += chunkRead;
    }

    pthread_mutex_lock(&app->dspMutex);
    play_dsp_process(&app->dsp, out, frameCount, app->channels);
    pthread_mutex_unlock(&app->dspMutex);
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
        send(clientFd, header, (size_t)n, 0);
        send(clientFd, body, (size_t)bodyLen, 0);
    }
}

static void send_not_found(int clientFd)
{
    const char* body = "Not Found";
    const char* header =
        "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 9\r\nConnection: close\r\n\r\n";
    send(clientFd, header, strlen(header), 0);
    send(clientFd, body, strlen(body), 0);
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
        int offset = 0;

        pthread_mutex_lock(&http->app->dspMutex);
        play_dsp_copy_spectrum(&http->app->dsp, preBins, postBins, ANALYZER_BINS);
        play_dsp_copy_phase_metrics(&http->app->dsp, phaseDeltaDeg, groupDelayMs, ANALYZER_BINS);
        pluginMask = play_dsp_get_plugin_mask(&http->app->dsp);
        offset += snprintf(body + offset,
                           sizeof(body) - (size_t)offset,
                           "{\"sampleRate\":%u,\"phasePluginEnabled\":%s,\"preBins\":[",
                           http->app->dsp.sampleRate,
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
        pthread_mutex_unlock(&http->app->dspMutex);

        snprintf(body + offset, sizeof(body) - (size_t)offset, "]}");
        send_http_response(clientFd, "application/json", body);
        return;
    }

    if (strncmp(req, "GET /eq", 7) == 0)
    {
        char body[4096];
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
        int bypassEnabled;
        int lowCrossfeedEnabled;
        uint8_t lowCrossfeedPosition;
        uint32_t pluginMask;
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
        bypassEnabled = play_dsp_get_bypass(&http->app->dsp);
        play_dsp_get_low_crossfeed(&http->app->dsp, &lowCrossfeedEnabled, &lowCrossfeedPosition);
        pluginMask = play_dsp_get_plugin_mask(&http->app->dsp);
        offset += snprintf(body + offset,
                           sizeof(body) - (size_t)offset,
                           "{\"minGain\":%.1f,\"maxGain\":%.1f,\"minQ\":%.2f,\"maxQ\":%.2f,\"sampleRate\":%u,\"phasePluginEnabled\":%s,\"bypassEnabled\":%s,\"lowCrossfeedEnabled\":%s,\"lowCrossfeedPosition\":%u,\"lowCrossfeedCutoffHz\":%.1f,\"lowCrossfeedRatio\":%.2f,\"dynamicAttack\":%.4f,\"dynamicRelease\":%.4f,\"dynamicThreshold\":%.4f,\"dynamicMaxReductionDB\":%.2f,\"dynamicStrengthDB\":%.2f,\"dynamicAttackMin\":%.3f,\"dynamicAttackMax\":%.3f,\"dynamicReleaseMin\":%.3f,\"dynamicReleaseMax\":%.3f,\"dynamicThresholdMin\":%.3f,\"dynamicThresholdMax\":%.3f,\"dynamicMaxReductionDBMin\":%.1f,\"dynamicMaxReductionDBMax\":%.1f,\"dynamicStrengthDBMin\":%.1f,\"dynamicStrengthDBMax\":%.1f,\"freqs\":[",
                           EQ_MIN_GAIN_DB,
                           EQ_MAX_GAIN_DB,
                           EQ_MIN_Q,
                           EQ_MAX_Q,
                           http->app->dsp.sampleRate,
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
                           DYNAMIC_EQ_MAX_STRENGTH_DB);
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
        pthread_mutex_unlock(&http->app->dspMutex);

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
        int gainCount;
        int qCount;
        bool hasDynamicAttack;
        bool hasDynamicRelease;
        bool hasDynamicThreshold;
        bool hasDynamicMaxReductionDB;
        bool hasDynamicStrengthDB;
        bool hasPhasePluginEnabled;
        bool hasBypassEnabled;
        bool hasLowCrossfeedEnabled;
        bool hasLowCrossfeedPosition;
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
        hasAnyDynamic = hasDynamicAttack || hasDynamicRelease || hasDynamicThreshold || hasDynamicMaxReductionDB ||
            hasDynamicStrengthDB;

        if (gainCount <= 0 && qCount <= 0 && !hasAnyDynamic && !hasPhasePluginEnabled && !hasBypassEnabled && !
            hasLowCrossfeedEnabled
            && !hasLowCrossfeedPosition)
        {
            send_http_response(clientFd, "application/json", "{\"ok\":false,\"reason\":\"invalid eq payload\"}");
            return;
        }

        pthread_mutex_lock(&http->app->dspMutex);
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
            ".sliders{margin-top:14px;display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:10px}.cell{background:#0b1424;border:1px solid #243449;border-radius:10px;padding:8px;}"
            "label{display:block;font-size:12px;opacity:.85;margin-bottom:6px}input[type=range]{width:100%}.val{font-size:12px;color:#93c5fd}"
            ".mode{font-size:10px;padding:1px 6px;border-radius:999px;background:#1f2937;color:#93c5fd;margin-left:6px;letter-spacing:.2px;}"
            ".qval{font-size:12px;color:#fca5a5}.row{display:flex;justify-content:space-between;align-items:center}"
            "@media(max-width:900px){.dyn{grid-template-columns:repeat(2,minmax(0,1fr));}.sliders{grid-template-columns:repeat(2,minmax(0,1fr));}}</style></head>"
            "<body><div class='wrap'><h1>实时频谱 + 多段 EQ</h1><div class='sub'>拖拽橙色控制点或下方滑块，实时改变播放 EQ <label style='margin-left:12px;display:inline-block;'><input id='bp' type='checkbox'> Bypass DSP</label> <span id='bpv' class='val'>OFF</span></div>"
            "<div class='panel'><canvas id='c' width='1000' height='400'></canvas><div id='dyn' class='dyn'></div><div id='sliders' class='sliders'></div></div></div>"
            "<script>const c=document.getElementById('c');const g=c.getContext('2d');const slidersEl=document.getElementById('sliders');"
            "const dynEl=document.getElementById('dyn');"
            "let specPre=[];let specPost=[];let phaseDelta=[];let groupDelay=[];let phaseEnabled=false;let bypassEnabled=false;let lowCrossfeedEnabled=false;let lowCrossfeedPosition=0;let freqs=[];let gains=[];let qs=[];let modes=[];let dynReduction=[];let minGain=-12,maxGain=12,minQ=.3,maxQ=4,sampleRate=48000;"
            "let dyn={attack:.12,release:.02,threshold:.18,maxReductionDB:12,strengthDB:18};"
            "let dynRange={attackMin:.01,attackMax:.5,releaseMin:.005,releaseMax:.3,thresholdMin:.02,thresholdMax:1,maxReductionDBMin:0,maxReductionDBMax:24,strengthDBMin:1,strengthDBMax:36};"
            "let drag=-1;let lastPost=0;let lastSpectrumFetch=0;let spectrumPending=false;const spectrumFetchIntervalMs=25;"
            "const specMinDb=-36,specMaxDb=36;"
            "const fMin=20,fMax=20000;const displayFreqs=[20,30,40,50,60,70,80,90,100,160,280,300,400,500,600,1000,1250,1500,1600,1700,1800,2000,2500,3000,4000,5000,6000,7000,8000,10000,12000,14000,15000,16000,17000,18000,20000];function lx(f){return (Math.log(f)-Math.log(fMin))/(Math.log(fMax)-Math.log(fMin));}"
            "function xOfF(f){return 55+lx(f)*(c.width-100);}function yOfDb(db){const d=Math.max(specMinDb,Math.min(specMaxDb,db));return 30+(specMaxDb-d)/(specMaxDb-specMinDb)*(c.height-70);}"
            "function dbOfY(y){const t=(y-30)/(c.height-70);return specMaxDb-t*(specMaxDb-specMinDb);}"
            "function eqModeOfFreq(f){if(f<=2000)return 'Linear';if(f>17000)return 'Normal';return 'Dynamic';}"
            "function modeText(m,f){if(m===0)return 'Linear';if(m===1)return 'Dynamic';if(m===2)return 'Normal';return eqModeOfFreq(f);}"
            "function fmtFreq(f){if(f>=1000){const k=f/1000;const s=(Math.abs(k-Math.round(k))<1e-6)?String(Math.round(k)):k.toFixed(k<2?2:1).replace(/\\.0$/,'');return s+'k';}return String(f);}"
            "function spectrumDbAt(freq,bins){if(!bins||bins.length===0)return specMinDb;if(bins.length===1)return bins[0];const lf=Math.log(Math.max(fMin,Math.min(fMax,freq)));const t=(lf-Math.log(fMin))/(Math.log(fMax)-Math.log(fMin));const p=Math.max(0,Math.min(1,t))*(bins.length-1);const i0=Math.floor(p);const i1=Math.min(bins.length-1,i0+1);const f0=i0/(bins.length-1);const f1=i1/(bins.length-1);const l0=Math.log(fMin)+f0*(Math.log(fMax)-Math.log(fMin));const l1=Math.log(fMin)+f1*(Math.log(fMax)-Math.log(fMin));const x=(lf-l0)/(l1-l0+1e-12);const v=bins[i0]+(bins[i1]-bins[i0])*Math.max(0,Math.min(1,x));return Math.max(specMinDb,Math.min(specMaxDb,v));}"
            "function harmanTargetDbAt(freq){const fp=[20,60,100,200,500,1000,2000,3000,5000,8000,10000,12000,16000,20000];const dp=[6.0,5.2,4.2,2.6,1.0,0.0,-0.7,-1.4,-2.0,-2.6,-3.0,-3.4,-4.0,-4.8];if(freq<=fp[0])return dp[0];const n=fp.length-1;if(freq>=fp[n])return dp[n];for(let i=0;i<n;i++){const f0=fp[i],f1=fp[i+1];if(freq>=f0&&freq<=f1){const t=(Math.log(freq)-Math.log(f0))/(Math.log(f1)-Math.log(f0)+1e-12);return dp[i]+(dp[i+1]-dp[i])*t;}}return 0;}"
            "function dynamicReductionAt(freq){if(!freqs.length||!modes.length||!dynReduction.length)return 0;const pts=[];for(let i=0;i<freqs.length;i++){if(modes[i]===1){pts.push({f:freqs[i],r:Math.max(0,dynReduction[i]||0)});}}if(!pts.length)return 0;if(freq<=2000||freq>17000)return 0;if(freq<=pts[0].f)return pts[0].r;const last=pts.length-1;if(freq>=pts[last].f)return pts[last].r;for(let i=0;i<pts.length-1;i++){const p0=pts[i],p1=pts[i+1];if(freq>=p0.f&&freq<=p1.f){const t=(Math.log(freq)-Math.log(p0.f))/(Math.log(p1.f)-Math.log(p0.f)+1e-12);return p0.r+(p1.r-p0.r)*t;}}return 0;}"
            "function biquadMagAt(freq,f0,gainDb,q){const A=Math.pow(10,gainDb/40);const w0=2*Math.PI*f0/sampleRate;const alpha=Math.sin(w0)/(2*q);"
            "const b0=1+alpha*A,b1=-2*Math.cos(w0),b2=1-alpha*A,a0=1+alpha/A,a1=-2*Math.cos(w0),a2=1-alpha/A;"
            "const w=2*Math.PI*freq/sampleRate,c1=Math.cos(w),s1=Math.sin(w),c2=Math.cos(2*w),s2=Math.sin(2*w);"
            "const nr=b0+b1*c1+b2*c2,ni=-(b1*s1+b2*s2),dr=a0+a1*c1+a2*c2,di=-(a1*s1+a2*s2);"
            "return Math.sqrt((nr*nr+ni*ni)/(dr*dr+di*di+1e-12));}"
            "function lowpassMagAt(freq,fc,q){const w0=2*Math.PI*fc/sampleRate;const c0=Math.cos(w0),s0=Math.sin(w0);const alpha=s0/(2*q);const b0=(1-c0)/2,b1=1-c0,b2=(1-c0)/2,a0=1+alpha,a1=-2*c0,a2=1-alpha;const w=2*Math.PI*freq/sampleRate,c1=Math.cos(w),s1=Math.sin(w),c2=Math.cos(2*w),s2=Math.sin(2*w);const nr=b0+b1*c1+b2*c2,ni=-(b1*s1+b2*s2),dr=a0+a1*c1+a2*c2,di=-(a1*s1+a2*s2);return Math.sqrt((nr*nr+ni*ni)/(dr*dr+di*di+1e-12));}"
            "function eqResponseDb(freq){let m=1;for(let i=0;i<freqs.length;i++){if(freqs[i]>=18000){m*=lowpassMagAt(freq,freqs[i],0.70710678118);}else{m*=biquadMagAt(freq,freqs[i],gains[i],qs[i]);}}return 20*Math.log10(m+1e-9);}"
            "function splinePath(points,color,width){if(points.length<2)return;g.strokeStyle=color;g.lineWidth=width;g.beginPath();g.moveTo(points[0].x,points[0].y);"
            "for(let i=0;i<points.length-1;i++){const p0=points[i],p1=points[i+1];const cx=(p0.x+p1.x)/2;g.quadraticCurveTo(p0.x,p0.y,cx,(p0.y+p1.y)/2);}"
            "const l=points.length-1;g.lineTo(points[l].x,points[l].y);g.stroke();}"
            "function drawGrid(){g.fillStyle='#0b1220';g.fillRect(0,0,c.width,c.height);g.strokeStyle='rgba(148,163,184,.22)';g.lineWidth=1;"
            "let lastLabelX=-1e9;displayFreqs.forEach(f=>{const x=xOfF(f);g.beginPath();g.moveTo(x,25);g.lineTo(x,c.height-40);g.stroke();if(x-lastLabelX>22){g.fillStyle='rgba(203,213,225,.82)';g.font='11px Avenir Next';g.fillText(fmtFreq(f),x-14,c.height-18);lastLabelX=x;}});"
            "for(let d=specMinDb;d<=specMaxDb;d+=3){const y=yOfDb(d);g.beginPath();g.moveTo(48,y);g.lineTo(c.width-42,y);g.stroke();g.fillStyle='rgba(148,163,184,.8)';g.fillText(d+'dB',8,y+4);}"
            "g.fillStyle='#94a3b8';g.fillText('Pre-EQ',c.width-470,20);g.fillStyle='#22d3ee';g.fillText('Post-EQ',c.width-410,20);g.fillStyle='#facc15';g.fillText('Harman Ref',c.width-345,20);g.fillStyle='#34d399';g.fillText('Dynamic',c.width-265,20);g.fillStyle='#fb923c';g.fillText('EQ points',c.width-200,20);g.fillStyle='#e879f9';g.fillText('Estimated EQ',c.width-130,20);}"
            "function draw(){drawGrid();const hp=[];for(let i=0;i<displayFreqs.length;i++){const f=displayFreqs[i];hp.push({x:xOfF(f),y:yOfDb(harmanTargetDbAt(f))});}splinePath(hp,'#facc15',1.8);if(specPre.length){const sp=[];for(let i=0;i<displayFreqs.length;i++){const f=displayFreqs[i];const db=spectrumDbAt(f,specPre);sp.push({x:xOfF(f),y:yOfDb(db)});}splinePath(sp,'#94a3b8',1.8);}if(specPost.length){const sp=[];for(let i=0;i<displayFreqs.length;i++){const f=displayFreqs[i];const db=spectrumDbAt(f,specPost);sp.push({x:xOfF(f),y:yOfDb(db)});}splinePath(sp,'#22d3ee',2.4);}"
            "if(freqs.length&&modes.length&&dynReduction.length){const dp=[];for(let i=0;i<180;i++){const t=i/179;const f=fMin*Math.pow(fMax/fMin,t);const red=dynamicReductionAt(f);dp.push({x:xOfF(f),y:yOfDb(-red)});}splinePath(dp,'#34d399',2.0);}"
            "if(freqs.length&&gains.length&&qs.length){const rp=[];for(let i=0;i<140;i++){const t=i/139;const f=fMin*Math.pow(fMax/fMin,t);const db=Math.max(minGain,Math.min(maxGain,eqResponseDb(f)));rp.push({x:xOfF(f),y:yOfDb(db)});}splinePath(rp,'#e879f9',2.6);}"
            "if(freqs.length&&gains.length){const ep=freqs.map((f,i)=>({x:xOfF(f),y:yOfDb(gains[i]),f}));splinePath(ep,'#fb923c',3);"
            "ep.forEach(p=>{g.fillStyle='#f59e0b';g.beginPath();g.arc(p.x,p.y,6,0,Math.PI*2);g.fill();g.strokeStyle='#fed7aa';g.lineWidth=1;g.stroke();g.fillStyle='#ffe7cf';g.fillText((p.f>=1000?(p.f/1000).toFixed(p.f%1000?1:0)+'k':p.f)+'Hz',p.x-14,p.y-10);});}}"
            "function schedulePost(){const now=Date.now();if(now-lastPost<60)return;lastPost=now;fetch('/eq',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({gains,qs,dynamicAttack:dyn.attack,dynamicRelease:dyn.release,dynamicThreshold:dyn.threshold,dynamicMaxReductionDB:dyn.maxReductionDB,dynamicStrengthDB:dyn.strengthDB,phasePluginEnabled:phaseEnabled?1:0,bypassEnabled:bypassEnabled?1:0,lowCrossfeedEnabled:lowCrossfeedEnabled?1:0,lowCrossfeedPosition:lowCrossfeedPosition})}).catch(()=>{});}"
            "function syncBypassUi(){const b=document.getElementById('bp');const v=document.getElementById('bpv');if(b)b.checked=!!bypassEnabled;if(v)v.textContent=bypassEnabled?'ON':'OFF';}"
            "function bindBypassUi(){const b=document.getElementById('bp');if(!b)return;b.addEventListener('change',()=>{bypassEnabled=!!b.checked;syncBypassUi();schedulePost();});syncBypassUi();}"
            "function buildDynamic(){const items=[{key:'attack',label:'Dyn Attack',min:dynRange.attackMin,max:dynRange.attackMax,step:0.001,digits:3},{key:'release',label:'Dyn Release',min:dynRange.releaseMin,max:dynRange.releaseMax,step:0.001,digits:3},{key:'threshold',label:'Threshold',min:dynRange.thresholdMin,max:dynRange.thresholdMax,step:0.005,digits:3},{key:'maxReductionDB',label:'Max Red (dB)',min:dynRange.maxReductionDBMin,max:dynRange.maxReductionDBMax,step:0.1,digits:1},{key:'strengthDB',label:'Strength (dB)',min:dynRange.strengthDBMin,max:dynRange.strengthDBMax,step:0.1,digits:1}];dynEl.innerHTML='';const phaseCell=document.createElement('div');phaseCell.className='cell';phaseCell.innerHTML='<div class=row><label>Phase Plugin</label><span class=val id=pv>'+(phaseEnabled?'ON':'OFF')+'</span></div><input id=pe type=checkbox '+(phaseEnabled?'checked':'')+'>';dynEl.appendChild(phaseCell);const pe=phaseCell.querySelector('#pe');const pv=phaseCell.querySelector('#pv');const upPhase=()=>{phaseEnabled=!!pe.checked;pv.textContent=phaseEnabled?'ON':'OFF';schedulePost();};pe.addEventListener('change',upPhase);const crossCell=document.createElement('div');crossCell.className='cell';crossCell.innerHTML='<div class=row><label>Low Crossfeed</label><span class=val id=cv>'+(lowCrossfeedEnabled?'ON':'OFF')+'</span></div><input id=ce type=checkbox '+(lowCrossfeedEnabled?'checked':'')+'><div class=row><label>Position</label><span class=val id=cpv>'+(lowCrossfeedPosition? 'Post EQ':'Pre EQ')+'</span></div><select id=cp><option value=0'+(lowCrossfeedPosition===0?' selected':'')+'>Pre EQ</option><option value=1'+(lowCrossfeedPosition===1?' selected':'')+'>Post EQ</option></select>';dynEl.appendChild(crossCell);const ce=crossCell.querySelector('#ce');const cv=crossCell.querySelector('#cv');const cp=crossCell.querySelector('#cp');const cpv=crossCell.querySelector('#cpv');const upCross=()=>{lowCrossfeedEnabled=!!ce.checked;lowCrossfeedPosition=parseInt(cp.value,10)||0;cv.textContent=lowCrossfeedEnabled?'ON':'OFF';cpv.textContent=lowCrossfeedPosition?'Post EQ':'Pre EQ';schedulePost();};ce.addEventListener('change',upCross);cp.addEventListener('change',upCross);items.forEach(it=>{const cell=document.createElement('div');cell.className='cell';cell.innerHTML='<div class=row><label>'+it.label+'</label><span class=val id=dv_'+it.key+'></span></div><input id=ds_'+it.key+' type=range min='+it.min+' max='+it.max+' step='+it.step+' value='+dyn[it.key]+'>';dynEl.appendChild(cell);const s=cell.querySelector('#ds_'+it.key);const v=cell.querySelector('#dv_'+it.key);const upd=()=>{dyn[it.key]=parseFloat(s.value);v.textContent=dyn[it.key].toFixed(it.digits);schedulePost();};s.addEventListener('input',upd);upd();});}"
            "function buildSliders(){slidersEl.innerHTML='';freqs.forEach((f,i)=>{const cell=document.createElement('div');cell.className='cell';"
            "cell.innerHTML='<div class=row><label>'+ (f>=1000?(f/1000).toFixed(f%1000?1:0)+'k':f) +'Hz <span class=mode>'+modeText(modes[i],f)+'</span></label><span class=val id=v'+i+'></span></div><input id=sg'+i+' type=range min='+minGain+' max='+maxGain+' step=0.1 value='+gains[i]+'><div class=row><label>Q</label><span class=qval id=qv'+i+'></span></div><input id=sq'+i+' type=range min='+minQ+' max='+maxQ+' step=0.01 value='+qs[i]+'>';"
            "slidersEl.appendChild(cell);const sg=cell.querySelector('#sg'+i);const sq=cell.querySelector('#sq'+i);const v=cell.querySelector('#v'+i);const qv=cell.querySelector('#qv'+i);const upd=()=>{gains[i]=parseFloat(sg.value);qs[i]=parseFloat(sq.value);v.textContent=gains[i].toFixed(1)+' dB';qv.textContent=qs[i].toFixed(2);draw();schedulePost();};sg.addEventListener('input',upd);sq.addEventListener('input',upd);upd();});}"
            "c.addEventListener('mousedown',e=>{if(!freqs.length)return;const r=c.getBoundingClientRect();const x=(e.clientX-r.left)*c.width/r.width;const y=(e.clientY-r.top)*c.height/r.height;let best=-1,bestD=1e9;freqs.forEach((f,i)=>{const dx=x-xOfF(f),dy=y-yOfDb(gains[i]);const d=dx*dx+dy*dy;if(d<bestD){bestD=d;best=i;}});if(bestD<400)drag=best;if(drag>=0){gains[drag]=Math.max(minGain,Math.min(maxGain,dbOfY(y)));const s=document.getElementById('sg'+drag);if(s)s.value=gains[drag];const v=document.getElementById('v'+drag);if(v)v.textContent=gains[drag].toFixed(1)+' dB';draw();schedulePost();}});"
            "window.addEventListener('mousemove',e=>{if(drag<0)return;const r=c.getBoundingClientRect();const y=(e.clientY-r.top)*c.height/r.height;gains[drag]=Math.max(minGain,Math.min(maxGain,dbOfY(y)));const s=document.getElementById('sg'+drag);if(s)s.value=gains[drag];const v=document.getElementById('v'+drag);if(v)v.textContent=gains[drag].toFixed(1)+' dB';draw();schedulePost();});"
            "window.addEventListener('mouseup',()=>{drag=-1;});"
            "async function pollSpectrum(){const now=performance.now();if(!spectrumPending&&(now-lastSpectrumFetch)>=spectrumFetchIntervalMs){spectrumPending=true;lastSpectrumFetch=now;fetch('/spectrum').then(r=>r.ok?r.json():null).then(j=>{if(!j)return;if(Array.isArray(j.preBins))specPre=j.preBins;if(Array.isArray(j.postBins))specPost=j.postBins;if(Array.isArray(j.phaseDeltaDeg))phaseDelta=j.phaseDeltaDeg;if(Array.isArray(j.groupDelayMs))groupDelay=j.groupDelayMs;if(typeof j.phasePluginEnabled==='boolean')phaseEnabled=j.phasePluginEnabled;if(Array.isArray(j.bins)&&!specPost.length)specPost=j.bins;}).catch(()=>{}).finally(()=>{spectrumPending=false;});}requestAnimationFrame(pollSpectrum);}"
            "async function init(){bindBypassUi();try{const r=await fetch('/eq');if(r.ok){const j=await r.json();if(Array.isArray(j.freqs)&&Array.isArray(j.gains)&&Array.isArray(j.qs)){freqs=j.freqs;gains=j.gains;qs=j.qs;modes=Array.isArray(j.modes)?j.modes:freqs.map(f=>f<=2000?0:(f>17000?2:1));dynReduction=Array.isArray(j.dynamicReductionDB)?j.dynamicReductionDB:freqs.map(()=>0);minGain=j.minGain;maxGain=j.maxGain;minQ=j.minQ;maxQ=j.maxQ;sampleRate=j.sampleRate||sampleRate;if(typeof j.phasePluginEnabled==='boolean')phaseEnabled=j.phasePluginEnabled;if(typeof j.bypassEnabled==='boolean')bypassEnabled=j.bypassEnabled;if(typeof j.lowCrossfeedEnabled==='boolean')lowCrossfeedEnabled=j.lowCrossfeedEnabled;if(typeof j.lowCrossfeedPosition==='number')lowCrossfeedPosition=(j.lowCrossfeedPosition>=0.5?1:0);if(typeof j.dynamicAttack==='number')dyn.attack=j.dynamicAttack;if(typeof j.dynamicRelease==='number')dyn.release=j.dynamicRelease;if(typeof j.dynamicThreshold==='number')dyn.threshold=j.dynamicThreshold;if(typeof j.dynamicMaxReductionDB==='number')dyn.maxReductionDB=j.dynamicMaxReductionDB;if(typeof j.dynamicStrengthDB==='number')dyn.strengthDB=j.dynamicStrengthDB;if(typeof j.dynamicAttackMin==='number')dynRange.attackMin=j.dynamicAttackMin;if(typeof j.dynamicAttackMax==='number')dynRange.attackMax=j.dynamicAttackMax;if(typeof j.dynamicReleaseMin==='number')dynRange.releaseMin=j.dynamicReleaseMin;if(typeof j.dynamicReleaseMax==='number')dynRange.releaseMax=j.dynamicReleaseMax;if(typeof j.dynamicThresholdMin==='number')dynRange.thresholdMin=j.dynamicThresholdMin;if(typeof j.dynamicThresholdMax==='number')dynRange.thresholdMax=j.dynamicThresholdMax;if(typeof j.dynamicMaxReductionDBMin==='number')dynRange.maxReductionDBMin=j.dynamicMaxReductionDBMin;if(typeof j.dynamicMaxReductionDBMax==='number')dynRange.maxReductionDBMax=j.dynamicMaxReductionDBMax;if(typeof j.dynamicStrengthDBMin==='number')dynRange.strengthDBMin=j.dynamicStrengthDBMin;if(typeof j.dynamicStrengthDBMax==='number')dynRange.strengthDBMax=j.dynamicStrengthDBMax;syncBypassUi();buildSliders();buildDynamic();}}}catch(e){}if(!dynEl.children.length)buildDynamic();syncBypassUi();draw();pollSpectrum();setInterval(async()=>{try{const r=await fetch('/eq');if(r.ok){const j=await r.json();if(Array.isArray(j.dynamicReductionDB))dynReduction=j.dynamicReductionDB;if(Array.isArray(j.modes))modes=j.modes;if(typeof j.phasePluginEnabled==='boolean')phaseEnabled=j.phasePluginEnabled;if(typeof j.bypassEnabled==='boolean')bypassEnabled=j.bypassEnabled;if(typeof j.lowCrossfeedEnabled==='boolean')lowCrossfeedEnabled=j.lowCrossfeedEnabled;if(typeof j.lowCrossfeedPosition==='number')lowCrossfeedPosition=(j.lowCrossfeedPosition>=0.5?1:0);syncBypassUi();}}catch(e){}draw();},100);}init();"
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

    while (http->running)
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

const char* input_audio_file_path = "2.wav";

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
        printf("加载 test.wav 失败。\n");
        ma_device_uninit(&device);
        return -1;
    }

    app.channels = device.playback.channels;
    app.input.userData = &wavSource;
    app.input.read_frames = wav_read_frames;
    app.input.rewind = wav_rewind;
    pthread_mutex_init(&app.dspMutex, NULL);
    if (play_dsp_init(&app.dsp, device.sampleRate, app.channels) != 0)
    {
        printf("初始化 DSP 失败。\n");
        pthread_mutex_destroy(&app.dspMutex);
        ma_device_uninit(&device);
        ma_decoder_uninit(&wavSource.decoder);
        return -1;
    }

    http.app = &app;
    http.running = true;
    pthread_create(&http.thread, NULL, http_server_thread_main, &http);

    result = ma_device_start(&device);
    if (result != MA_SUCCESS)
    {
        printf("启动播放设备失败。\n");
        http.running = false;
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

    http.running = false;
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
