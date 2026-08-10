/*
Minimal macOS-friendly offline skeleton:
- Input: stereo file
- Processing: LPF / (HPF->LPF for mid band) / HPF
- Output: 6-channel WAV: L_low, R_low, L_mid, R_mid, L_high, R_high

Build (macOS):
  clang -O2 -std=c99 examples/three_way_crossover_macos_offline.c -o three_way_macos -lm -lpthread

Run:
  ./three_way_macos input.wav output_6ch.wav [f1] [f2]
*/

#include "miniaudio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FRAME_BLOCK_SIZE
#define FRAME_BLOCK_SIZE 1024
#endif

// 分频点
static double parse_or_default(const char* s, double def)
{
    if (s == NULL)
    {
        return def;
    }

    char* end = NULL;
    double v = strtod(s, &end);
    if (end == s || !isfinite(v) || v <= 0.0)
    {
        return def;
    }

    return v;
}

int main(int argc, char** argv)
{
    ma_result result;
    ma_decoder decoder;
    ma_encoder encoder;
    ma_decoder_config decoderConfig;
    ma_encoder_config encoderConfig;
    ma_lpf lpf;
    ma_hpf midHpf;
    ma_lpf midLpf;
    ma_hpf hpf;
    ma_lpf_config lpfConfig;
    ma_hpf_config midHpfConfig;
    ma_lpf_config midLpfConfig;
    ma_hpf_config hpfConfig;

    float inFrames[FRAME_BLOCK_SIZE * 2];
    float lowFrames[FRAME_BLOCK_SIZE * 2];
    float midTempFrames[FRAME_BLOCK_SIZE * 2];
    float midFrames[FRAME_BLOCK_SIZE * 2];
    float highFrames[FRAME_BLOCK_SIZE * 2];
    float outFrames6[FRAME_BLOCK_SIZE * 6];

    ma_uint64 framesRead = 0;
    ma_uint32 i;

    /* Default crossover points: f1=200Hz, f2=2000Hz. */
    double f1;
    double f2;
    if (argc < 3)
    {
        printf("Usage: %s <input-audio> <output-6ch-wav> [f1] [f2]\n", argv[0]);
        return -1;
    }

    f1 = parse_or_default((argc > 3) ? argv[3] : NULL, 200.0);
    f2 = parse_or_default((argc > 4) ? argv[4] : NULL, 2000.0);

    if (f2 <= f1)
    {
        printf("Invalid crossover points: require f2 > f1.\n");
        return -2;
    }

    decoderConfig = ma_decoder_config_init(ma_format_f32, 2, 48000);
    result = ma_decoder_init_file(argv[1], &decoderConfig, &decoder);
    if (result != MA_SUCCESS)
    {
        printf("ma_decoder_init_file() failed: %d\n", result);
        return -3;
    }

    encoderConfig = ma_encoder_config_init(
        ma_encoding_format_wav,
        ma_format_f32,
        6,
        decoder.outputSampleRate
    );
    result = ma_encoder_init_file(argv[2], &encoderConfig, &encoder);
    if (result != MA_SUCCESS)
    {
        printf("ma_encoder_init_file() failed: %d\n", result);
        ma_decoder_uninit(&decoder);
        return -4;
    }

    lpfConfig = ma_lpf_config_init(ma_format_f32, 2, decoder.outputSampleRate, f1, 4);
    midHpfConfig = ma_hpf_config_init(ma_format_f32, 2, decoder.outputSampleRate, f1, 4);
    midLpfConfig = ma_lpf_config_init(ma_format_f32, 2, decoder.outputSampleRate, f2, 4);
    hpfConfig = ma_hpf_config_init(ma_format_f32, 2, decoder.outputSampleRate, f2, 4);

    result = ma_lpf_init(&lpfConfig, NULL, &lpf);
    if (result != MA_SUCCESS)
    {
        printf("ma_lpf_init() failed: %d\n", result);
        ma_encoder_uninit(&encoder);
        ma_decoder_uninit(&decoder);
        return -5;
    }

    result = ma_hpf_init(&midHpfConfig, NULL, &midHpf);
    if (result != MA_SUCCESS)
    {
        printf("ma_hpf_init(midHpf) failed: %d\n", result);
        ma_lpf_uninit(&lpf, NULL);
        ma_encoder_uninit(&encoder);
        ma_decoder_uninit(&decoder);
        return -6;
    }

    result = ma_lpf_init(&midLpfConfig, NULL, &midLpf);
    if (result != MA_SUCCESS)
    {
        printf("ma_lpf_init(midLpf) failed: %d\n", result);
        ma_hpf_uninit(&midHpf, NULL);
        ma_lpf_uninit(&lpf, NULL);
        ma_encoder_uninit(&encoder);
        ma_decoder_uninit(&decoder);
        return -7;
    }

    result = ma_hpf_init(&hpfConfig, NULL, &hpf);
    if (result != MA_SUCCESS)
    {
        printf("ma_hpf_init() failed: %d\n", result);
        ma_lpf_uninit(&midLpf, NULL);
        ma_hpf_uninit(&midHpf, NULL);
        ma_lpf_uninit(&lpf, NULL);
        ma_encoder_uninit(&encoder);
        ma_decoder_uninit(&decoder);
        return -8;
    }

    printf("Processing... sampleRate=%u, f1=%.2fHz, f2=%.2fHz\n",
           decoder.outputSampleRate, f1, f2);

    for (;;)
    {
        ma_uint64 framesWritten;

        result = ma_decoder_read_pcm_frames(&decoder, inFrames, FRAME_BLOCK_SIZE, &framesRead);
        if (result != MA_SUCCESS && result != MA_AT_END)
        {
            printf("ma_decoder_read_pcm_frames() failed: %d\n", result);
            break;
        }

        if (framesRead == 0)
        {
            break;
        }

        ma_lpf_process_pcm_frames(&lpf, lowFrames, inFrames, framesRead);
        ma_hpf_process_pcm_frames(&midHpf, midTempFrames, inFrames, framesRead);
        ma_lpf_process_pcm_frames(&midLpf, midFrames, midTempFrames, framesRead);
        ma_hpf_process_pcm_frames(&hpf, highFrames, inFrames, framesRead);

        for (i = 0; i < (ma_uint32)framesRead; ++i)
        {
            const ma_uint32 inBase2 = i * 2;
            const ma_uint32 outBase6 = i * 6;

            outFrames6[outBase6 + 0] = lowFrames[inBase2 + 0];
            outFrames6[outBase6 + 1] = lowFrames[inBase2 + 1];
            outFrames6[outBase6 + 2] = midFrames[inBase2 + 0];
            outFrames6[outBase6 + 3] = midFrames[inBase2 + 1];
            outFrames6[outBase6 + 4] = highFrames[inBase2 + 0];
            outFrames6[outBase6 + 5] = highFrames[inBase2 + 1];
        }

        result = ma_encoder_write_pcm_frames(&encoder, outFrames6, framesRead, &framesWritten);
        if (result != MA_SUCCESS || framesWritten != framesRead)
        {
            printf("ma_encoder_write_pcm_frames() failed: %d\n", result);
            break;
        }
    }

    ma_hpf_uninit(&hpf, NULL);
    ma_lpf_uninit(&midLpf, NULL);
    ma_hpf_uninit(&midHpf, NULL);
    ma_lpf_uninit(&lpf, NULL);
    ma_encoder_uninit(&encoder);
    ma_decoder_uninit(&decoder);

    printf("Done.\n");
    return 0;
}
