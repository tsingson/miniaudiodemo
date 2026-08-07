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
        ma_result readResult = app->input.read_frames(app->input.userData, out + totalRead * app->channels, frameCount - totalRead, &chunkRead);

        if (readResult != MA_SUCCESS || chunkRead == 0)
        {
            if (app->input.rewind(app->input.userData) != MA_SUCCESS)
            {
                memset(out + totalRead * app->channels, 0, (size_t)((frameCount - totalRead) * app->channels * sizeof(float)));
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
    const char* header = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 9\r\nConnection: close\r\n\r\n";
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
        char body[8192];
        float preBins[ANALYZER_BINS];
        float postBins[ANALYZER_BINS];
        int offset = 0;

        pthread_mutex_lock(&http->app->dspMutex);
        play_dsp_copy_spectrum(&http->app->dsp, preBins, postBins, ANALYZER_BINS);
        offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "{\"sampleRate\":%u,\"preBins\":[", http->app->dsp.sampleRate);
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
        offset += snprintf(body + offset,
                           sizeof(body) - (size_t)offset,
                           "{\"minGain\":%.1f,\"maxGain\":%.1f,\"minQ\":%.2f,\"maxQ\":%.2f,\"sampleRate\":%u,\"dynamicAttack\":%.4f,\"dynamicRelease\":%.4f,\"dynamicThreshold\":%.4f,\"dynamicMaxReductionDB\":%.2f,\"dynamicStrengthDB\":%.2f,\"dynamicAttackMin\":%.3f,\"dynamicAttackMax\":%.3f,\"dynamicReleaseMin\":%.3f,\"dynamicReleaseMax\":%.3f,\"dynamicThresholdMin\":%.3f,\"dynamicThresholdMax\":%.3f,\"dynamicMaxReductionDBMin\":%.1f,\"dynamicMaxReductionDBMax\":%.1f,\"dynamicStrengthDBMin\":%.1f,\"dynamicStrengthDBMax\":%.1f,\"freqs\":[",
                           EQ_MIN_GAIN_DB,
                           EQ_MAX_GAIN_DB,
                           EQ_MIN_Q,
                           EQ_MAX_Q,
                           http->app->dsp.sampleRate,
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
            offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "%s%u", (i == 0) ? "" : ",", (unsigned)modes[i]);
        }
        offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "],\"dynamicReductionDB\":[");
        for (int i = 0; i < EQ_BANDS; ++i)
        {
            offset += snprintf(body + offset, sizeof(body) - (size_t)offset, "%s%.2f", (i == 0) ? "" : ",", dynamicReductionDB[i]);
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
        int gainCount;
        int qCount;
        bool hasDynamicAttack;
        bool hasDynamicRelease;
        bool hasDynamicThreshold;
        bool hasDynamicMaxReductionDB;
        bool hasDynamicStrengthDB;
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
        hasDynamicMaxReductionDB = parse_float_value_from_json(bodyStart, "\"dynamicMaxReductionDB\"", &dynamicMaxReductionDB);
        hasDynamicStrengthDB = parse_float_value_from_json(bodyStart, "\"dynamicStrengthDB\"", &dynamicStrengthDB);
        hasAnyDynamic = hasDynamicAttack || hasDynamicRelease || hasDynamicThreshold || hasDynamicMaxReductionDB || hasDynamicStrengthDB;

        if (gainCount <= 0 && qCount <= 0 && !hasAnyDynamic)
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
            "<body><div class='wrap'><h1>实时频谱 + 多段 EQ</h1><div class='sub'>拖拽橙色控制点或下方滑块，实时改变播放 EQ</div>"
            "<div class='panel'><canvas id='c' width='1000' height='400'></canvas><div id='dyn' class='dyn'></div><div id='sliders' class='sliders'></div></div></div>"
            "<script>const c=document.getElementById('c');const g=c.getContext('2d');const slidersEl=document.getElementById('sliders');"
            "const dynEl=document.getElementById('dyn');"
            "let specPre=[];let specPost=[];let freqs=[];let gains=[];let qs=[];let modes=[];let dynReduction=[];let minGain=-12,maxGain=12,minQ=.3,maxQ=4,sampleRate=48000;"
            "let dyn={attack:.12,release:.02,threshold:.18,maxReductionDB:12,strengthDB:18};"
            "let dynRange={attackMin:.01,attackMax:.5,releaseMin:.005,releaseMax:.3,thresholdMin:.02,thresholdMax:1,maxReductionDBMin:0,maxReductionDBMax:24,strengthDBMin:1,strengthDBMax:36};"
            "let drag=-1;let lastPost=0;"
            "const specMinDb=-36,specMaxDb=36;"
            "const fMin=20,fMax=20000;function lx(f){return (Math.log(f)-Math.log(fMin))/(Math.log(fMax)-Math.log(fMin));}"
            "function xOfF(f){return 55+lx(f)*(c.width-100);}function yOfDb(db){const d=Math.max(specMinDb,Math.min(specMaxDb,db));return 30+(specMaxDb-d)/(specMaxDb-specMinDb)*(c.height-70);}"
            "function dbOfY(y){const t=(y-30)/(c.height-70);return specMaxDb-t*(specMaxDb-specMinDb);}"
            "function eqModeOfFreq(f){if(f<=2000)return 'Linear';if(f>16000)return 'Normal';return 'Dynamic';}"
            "function modeText(m,f){if(m===0)return 'Linear';if(m===1)return 'Dynamic';if(m===2)return 'Normal';return eqModeOfFreq(f);}"
            "function biquadMagAt(freq,f0,gainDb,q){const A=Math.pow(10,gainDb/40);const w0=2*Math.PI*f0/sampleRate;const alpha=Math.sin(w0)/(2*q);"
            "const b0=1+alpha*A,b1=-2*Math.cos(w0),b2=1-alpha*A,a0=1+alpha/A,a1=-2*Math.cos(w0),a2=1-alpha/A;"
            "const w=2*Math.PI*freq/sampleRate,c1=Math.cos(w),s1=Math.sin(w),c2=Math.cos(2*w),s2=Math.sin(2*w);"
            "const nr=b0+b1*c1+b2*c2,ni=-(b1*s1+b2*s2),dr=a0+a1*c1+a2*c2,di=-(a1*s1+a2*s2);"
            "return Math.sqrt((nr*nr+ni*ni)/(dr*dr+di*di+1e-12));}"
            "function eqResponseDb(freq){let m=1;for(let i=0;i<freqs.length;i++){m*=biquadMagAt(freq,freqs[i],gains[i],qs[i]);}return 20*Math.log10(m+1e-9);}"
            "function splinePath(points,color,width){if(points.length<2)return;g.strokeStyle=color;g.lineWidth=width;g.beginPath();g.moveTo(points[0].x,points[0].y);"
            "for(let i=0;i<points.length-1;i++){const p0=points[i],p1=points[i+1];const cx=(p0.x+p1.x)/2;g.quadraticCurveTo(p0.x,p0.y,cx,(p0.y+p1.y)/2);}"
            "const l=points.length-1;g.lineTo(points[l].x,points[l].y);g.stroke();}"
            "function drawGrid(){g.fillStyle='#0b1220';g.fillRect(0,0,c.width,c.height);g.strokeStyle='rgba(148,163,184,.22)';g.lineWidth=1;"
            "const ticks=[20,50,100,200,500,1000,2000,5000,10000,20000];ticks.forEach(f=>{const x=xOfF(f);g.beginPath();g.moveTo(x,25);g.lineTo(x,c.height-40);g.stroke();"
            "g.fillStyle='rgba(203,213,225,.82)';g.font='11px Avenir Next';g.fillText((f>=1000?(f/1000).toFixed(f%1000?1:0)+'k':f)+'Hz',x-14,c.height-18);});"
            "for(let d=specMinDb;d<=specMaxDb;d+=3){const y=yOfDb(d);g.beginPath();g.moveTo(48,y);g.lineTo(c.width-42,y);g.stroke();g.fillStyle='rgba(148,163,184,.8)';g.fillText(d+'dB',8,y+4);}"
            "g.fillStyle='#94a3b8';g.fillText('Pre-EQ',c.width-330,20);g.fillStyle='#22d3ee';g.fillText('Post-EQ',c.width-275,20);g.fillStyle='#34d399';g.fillText('Dynamic',c.width-215,20);g.fillStyle='#fb923c';g.fillText('EQ points',c.width-150,20);g.fillStyle='#e879f9';g.fillText('Estimated EQ',c.width-80,20);}"
            "function draw(){drawGrid();if(specPre.length){const sp=[];for(let i=0;i<specPre.length;i++){const f=fMin*Math.pow(fMax/fMin,i/(specPre.length-1));const db=Math.max(specMinDb,Math.min(specMaxDb,specPre[i]));sp.push({x:xOfF(f),y:yOfDb(db)});}splinePath(sp,'#94a3b8',1.6);}if(specPost.length){const sp=[];for(let i=0;i<specPost.length;i++){const f=fMin*Math.pow(fMax/fMin,i/(specPost.length-1));const db=Math.max(specMinDb,Math.min(specMaxDb,specPost[i]));sp.push({x:xOfF(f),y:yOfDb(db)});}splinePath(sp,'#22d3ee',2.2);}"
            "if(freqs.length&&modes.length&&dynReduction.length){const dp=freqs.map((f,i)=>({x:xOfF(f),y:yOfDb(-(dynReduction[i]||0)),f,m:modes[i]}));splinePath(dp,'#34d399',2.0);}"
            "if(freqs.length&&gains.length&&qs.length){const rp=[];for(let i=0;i<140;i++){const t=i/139;const f=fMin*Math.pow(fMax/fMin,t);const db=Math.max(minGain,Math.min(maxGain,eqResponseDb(f)));rp.push({x:xOfF(f),y:yOfDb(db)});}splinePath(rp,'#e879f9',2.6);}"
            "if(freqs.length&&gains.length){const ep=freqs.map((f,i)=>({x:xOfF(f),y:yOfDb(gains[i]),f}));splinePath(ep,'#fb923c',3);"
            "ep.forEach(p=>{g.fillStyle='#f59e0b';g.beginPath();g.arc(p.x,p.y,6,0,Math.PI*2);g.fill();g.strokeStyle='#fed7aa';g.lineWidth=1;g.stroke();g.fillStyle='#ffe7cf';g.fillText((p.f>=1000?(p.f/1000).toFixed(p.f%1000?1:0)+'k':p.f)+'Hz',p.x-14,p.y-10);});}}"
            "function schedulePost(){const now=Date.now();if(now-lastPost<60)return;lastPost=now;fetch('/eq',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({gains,qs,dynamicAttack:dyn.attack,dynamicRelease:dyn.release,dynamicThreshold:dyn.threshold,dynamicMaxReductionDB:dyn.maxReductionDB,dynamicStrengthDB:dyn.strengthDB})}).catch(()=>{});}"
            "function buildDynamic(){const items=[{key:'attack',label:'Dyn Attack',min:dynRange.attackMin,max:dynRange.attackMax,step:0.001,digits:3},{key:'release',label:'Dyn Release',min:dynRange.releaseMin,max:dynRange.releaseMax,step:0.001,digits:3},{key:'threshold',label:'Threshold',min:dynRange.thresholdMin,max:dynRange.thresholdMax,step:0.005,digits:3},{key:'maxReductionDB',label:'Max Red (dB)',min:dynRange.maxReductionDBMin,max:dynRange.maxReductionDBMax,step:0.1,digits:1},{key:'strengthDB',label:'Strength (dB)',min:dynRange.strengthDBMin,max:dynRange.strengthDBMax,step:0.1,digits:1}];dynEl.innerHTML='';items.forEach(it=>{const cell=document.createElement('div');cell.className='cell';cell.innerHTML='<div class=row><label>'+it.label+'</label><span class=val id=dv_'+it.key+'></span></div><input id=ds_'+it.key+' type=range min='+it.min+' max='+it.max+' step='+it.step+' value='+dyn[it.key]+'>';dynEl.appendChild(cell);const s=cell.querySelector('#ds_'+it.key);const v=cell.querySelector('#dv_'+it.key);const upd=()=>{dyn[it.key]=parseFloat(s.value);v.textContent=dyn[it.key].toFixed(it.digits);schedulePost();};s.addEventListener('input',upd);upd();});}"
            "function buildSliders(){slidersEl.innerHTML='';freqs.forEach((f,i)=>{const cell=document.createElement('div');cell.className='cell';"
            "cell.innerHTML='<div class=row><label>'+ (f>=1000?(f/1000).toFixed(f%1000?1:0)+'k':f) +'Hz <span class=mode>'+modeText(modes[i],f)+'</span></label><span class=val id=v'+i+'></span></div><input id=sg'+i+' type=range min='+minGain+' max='+maxGain+' step=0.1 value='+gains[i]+'><div class=row><label>Q</label><span class=qval id=qv'+i+'></span></div><input id=sq'+i+' type=range min='+minQ+' max='+maxQ+' step=0.01 value='+qs[i]+'>';"
            "slidersEl.appendChild(cell);const sg=cell.querySelector('#sg'+i);const sq=cell.querySelector('#sq'+i);const v=cell.querySelector('#v'+i);const qv=cell.querySelector('#qv'+i);const upd=()=>{gains[i]=parseFloat(sg.value);qs[i]=parseFloat(sq.value);v.textContent=gains[i].toFixed(1)+' dB';qv.textContent=qs[i].toFixed(2);draw();schedulePost();};sg.addEventListener('input',upd);sq.addEventListener('input',upd);upd();});}"
            "c.addEventListener('mousedown',e=>{if(!freqs.length)return;const r=c.getBoundingClientRect();const x=(e.clientX-r.left)*c.width/r.width;const y=(e.clientY-r.top)*c.height/r.height;let best=-1,bestD=1e9;freqs.forEach((f,i)=>{const dx=x-xOfF(f),dy=y-yOfDb(gains[i]);const d=dx*dx+dy*dy;if(d<bestD){bestD=d;best=i;}});if(bestD<400)drag=best;if(drag>=0){gains[drag]=Math.max(minGain,Math.min(maxGain,dbOfY(y)));const s=document.getElementById('sg'+drag);if(s)s.value=gains[drag];const v=document.getElementById('v'+drag);if(v)v.textContent=gains[drag].toFixed(1)+' dB';draw();schedulePost();}});"
            "window.addEventListener('mousemove',e=>{if(drag<0)return;const r=c.getBoundingClientRect();const y=(e.clientY-r.top)*c.height/r.height;gains[drag]=Math.max(minGain,Math.min(maxGain,dbOfY(y)));const s=document.getElementById('sg'+drag);if(s)s.value=gains[drag];const v=document.getElementById('v'+drag);if(v)v.textContent=gains[drag].toFixed(1)+' dB';draw();schedulePost();});"
            "window.addEventListener('mouseup',()=>{drag=-1;});"
            "async function pollSpectrum(){try{const r=await fetch('/spectrum');if(r.ok){const j=await r.json();if(Array.isArray(j.preBins))specPre=j.preBins;if(Array.isArray(j.postBins))specPost=j.postBins;if(Array.isArray(j.bins)&&!specPost.length)specPost=j.bins;}}catch(e){}requestAnimationFrame(pollSpectrum);}"
            "async function init(){try{const r=await fetch('/eq');if(r.ok){const j=await r.json();if(Array.isArray(j.freqs)&&Array.isArray(j.gains)&&Array.isArray(j.qs)){freqs=j.freqs;gains=j.gains;qs=j.qs;modes=Array.isArray(j.modes)?j.modes:freqs.map(f=>f<=2000?0:(f>16000?2:1));dynReduction=Array.isArray(j.dynamicReductionDB)?j.dynamicReductionDB:freqs.map(()=>0);minGain=j.minGain;maxGain=j.maxGain;minQ=j.minQ;maxQ=j.maxQ;sampleRate=j.sampleRate||sampleRate;if(typeof j.dynamicAttack==='number')dyn.attack=j.dynamicAttack;if(typeof j.dynamicRelease==='number')dyn.release=j.dynamicRelease;if(typeof j.dynamicThreshold==='number')dyn.threshold=j.dynamicThreshold;if(typeof j.dynamicMaxReductionDB==='number')dyn.maxReductionDB=j.dynamicMaxReductionDB;if(typeof j.dynamicStrengthDB==='number')dyn.strengthDB=j.dynamicStrengthDB;if(typeof j.dynamicAttackMin==='number')dynRange.attackMin=j.dynamicAttackMin;if(typeof j.dynamicAttackMax==='number')dynRange.attackMax=j.dynamicAttackMax;if(typeof j.dynamicReleaseMin==='number')dynRange.releaseMin=j.dynamicReleaseMin;if(typeof j.dynamicReleaseMax==='number')dynRange.releaseMax=j.dynamicReleaseMax;if(typeof j.dynamicThresholdMin==='number')dynRange.thresholdMin=j.dynamicThresholdMin;if(typeof j.dynamicThresholdMax==='number')dynRange.thresholdMax=j.dynamicThresholdMax;if(typeof j.dynamicMaxReductionDBMin==='number')dynRange.maxReductionDBMin=j.dynamicMaxReductionDBMin;if(typeof j.dynamicMaxReductionDBMax==='number')dynRange.maxReductionDBMax=j.dynamicMaxReductionDBMax;if(typeof j.dynamicStrengthDBMin==='number')dynRange.strengthDBMin=j.dynamicStrengthDBMin;if(typeof j.dynamicStrengthDBMax==='number')dynRange.strengthDBMax=j.dynamicStrengthDBMax;buildSliders();buildDynamic();}}}catch(e){}if(!dynEl.children.length)buildDynamic();draw();pollSpectrum();setInterval(async()=>{try{const r=await fetch('/eq');if(r.ok){const j=await r.json();if(Array.isArray(j.dynamicReductionDB))dynReduction=j.dynamicReductionDB;if(Array.isArray(j.modes))modes=j.modes;}}catch(e){}draw();},100);}init();"
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
    result = ma_decoder_init_file("1.wav", &decoderConfig, &wavSource.decoder);
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
