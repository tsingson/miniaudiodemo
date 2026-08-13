#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <input.wav> <output.c>\n", prog);
    fprintf(stderr, "Example: %s 3.wav ./src/wav_3.c\n", prog);
}

static uint16_t read_u16_le(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32_le(const unsigned char *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int is_valid_wav_header(const unsigned char *buf, size_t size)
{
    if (size < 12U) {
        return 0;
    }
    return memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WAVE", 4) == 0;
}

static int parse_wav(const char *input_path,
                    uint16_t *channels,
                    uint32_t *sample_rate,
                    uint16_t *bits_per_sample,
                    uint8_t **data,
                    size_t *data_len)
{
    FILE *fp = NULL;
    unsigned char header[12];
    unsigned char *wav = NULL;
    size_t wav_size = 0U;
    size_t pos = 12U;
    uint16_t audio_format = 0U;
    uint32_t data_chunk_size = 0U;
    size_t data_offset = 0U;
    int ok = 0;

    *channels = 0U;
    *sample_rate = 0U;
    *bits_per_sample = 0U;
    *data = NULL;
    *data_len = 0U;

    fp = fopen(input_path, "rb");
    if (!fp) {
        fprintf(stderr, "Failed to open input WAV '%s': %s\n", input_path, strerror(errno));
        return -1;
    }

    if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        fprintf(stderr, "Failed to read WAV header from '%s'\n", input_path);
        goto cleanup;
    }

    if (!is_valid_wav_header(header, sizeof(header))) {
        fprintf(stderr, "Input '%s' is not a RIFF/WAVE file\n", input_path);
        goto cleanup;
    }

    if (fseek(fp, 0L, SEEK_END) != 0) {
        fprintf(stderr, "Failed to seek '%s'\n", input_path);
        goto cleanup;
    }

    wav_size = (size_t)ftell(fp);
    if (wav_size == (size_t)-1L) {
        fprintf(stderr, "Failed to determine file size for '%s'\n", input_path);
        goto cleanup;
    }

    if (fseek(fp, 0L, SEEK_SET) != 0) {
        fprintf(stderr, "Failed to rewind '%s'\n", input_path);
        goto cleanup;
    }

    wav = (unsigned char *)malloc(wav_size);
    if (!wav) {
        fprintf(stderr, "Out of memory while reading '%s'\n", input_path);
        goto cleanup;
    }

    if (fread(wav, 1, wav_size, fp) != wav_size) {
        fprintf(stderr, "Failed to read entire WAV data from '%s'\n", input_path);
        goto cleanup;
    }

    while (pos + 8U <= wav_size) {
        unsigned char *chunk = wav + pos;
        uint32_t chunk_size = read_u32_le(chunk + 4U);
        const size_t chunk_data = pos + 8U;

        if (chunk_data + chunk_size > wav_size) {
            fprintf(stderr, "Corrupt WAV chunk at offset %zu\n", pos);
            goto cleanup;
        }

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_size < 16U) {
                fprintf(stderr, "fmt chunk too small in '%s'\n", input_path);
                goto cleanup;
            }
            audio_format = read_u16_le(wav + chunk_data + 0U);
            *channels = read_u16_le(wav + chunk_data + 2U);
            *sample_rate = read_u32_le(wav + chunk_data + 4U);
            /* byte_rate = read_u32_le(wav + chunk_data + 8U); */
            /* block_align = read_u16_le(wav + chunk_data + 12U); */
            *bits_per_sample = read_u16_le(wav + chunk_data + 14U);
        } else if (memcmp(chunk, "data", 4) == 0) {
            data_chunk_size = chunk_size;
            data_offset = chunk_data;
        }

        pos = chunk_data + chunk_size + ((chunk_size & 1U) ? 1U : 0U);
    }

    if (audio_format != 1U || *channels == 0U || *sample_rate == 0U || *bits_per_sample == 0U || data_offset == 0U) {
        fprintf(stderr, "Unsupported or malformed WAV format in '%s'\n", input_path);
        goto cleanup;
    }

    *data = (uint8_t *)malloc(data_chunk_size);
    if (!*data) {
        fprintf(stderr, "Out of memory for WAV PCM payload\n");
        goto cleanup;
    }
    memcpy(*data, wav + data_offset, data_chunk_size);
    *data_len = data_chunk_size;

    ok = 1;

cleanup:
    free(wav);
    if (fp) {
        fclose(fp);
    }
    if (!ok) {
        free(*data);
        *data = NULL;
        *data_len = 0U;
    }
    return ok ? 0 : -1;
}

static void sanitize_name(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0U;
    size_t j = 0U;

    if (dst_size == 0U) {
        return;
    }

    while (src[i] != '\0' && j + 1U < dst_size) {
        unsigned char c = (unsigned char)src[i++];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
            dst[j++] = (char)c;
        } else {
            dst[j++] = '_';
        }
    }
    dst[j] = '\0';
}

int main(int argc, char **argv)
{
    const char *input_path = NULL;
    const char *output_path = NULL;
    uint16_t channels = 0U;
    uint32_t sample_rate = 0U;
    uint16_t bits_per_sample = 0U;
    uint8_t *wav_data = NULL;
    size_t wav_data_len = 0U;
    FILE *out = NULL;
    int rc = 1;
    char var_name[128];
    char *slash = NULL;
    const char *stem = NULL;

    if (argc != 3) {
        print_usage(argv[0]);
        return 1;
    }

    input_path = argv[1];
    output_path = argv[2];

    if (parse_wav(input_path, &channels, &sample_rate, &bits_per_sample, &wav_data, &wav_data_len) != 0) {
        return 1;
    }

    out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "Failed to open output file '%s': %s\n", output_path, strerror(errno));
        free(wav_data);
        return 1;
    }

    slash = strrchr(output_path, '/');
    if (slash) {
        stem = slash + 1;
    } else {
        stem = output_path;
    }

    if (strchr(stem, '.')) {
        size_t len = strcspn(stem, ".");
        snprintf(var_name, sizeof(var_name), "%.*s", (int)len, stem);
    } else {
        snprintf(var_name, sizeof(var_name), "%s", stem);
    }
    sanitize_name(var_name, sizeof(var_name), var_name);
    if (var_name[0] == '\0') {
        snprintf(var_name, sizeof(var_name), "pcm_i2s");
    }

    fprintf(out, "/* Generated from %s */\n", input_path);
    fprintf(out, "/* PCM I2S interleaved 16-bit data */\n\n");
    fprintf(out, "#include <stdint.h>\n\n");
    fprintf(out, "const uint32_t %s_sample_rate = %uU;\n", var_name, sample_rate);
    fprintf(out, "const uint16_t %s_channels = %uU;\n", var_name, channels);
    fprintf(out, "const uint16_t %s_bits_per_sample = %uU;\n", var_name, bits_per_sample);
    fprintf(out, "const uint32_t %s_num_samples = %zuU;\n\n", var_name, wav_data_len / ((size_t)bits_per_sample / 8U));
    fprintf(out, "const int16_t %s_i2s[] = {\n", var_name);

    {
        size_t total_samples = wav_data_len / ((size_t)bits_per_sample / 8U);
        size_t i;
        for (i = 0U; i < total_samples; ++i) {
            int32_t sample_i32 = 0;
            int16_t sample_i16 = 0;
            const size_t byte_offset = i * ((size_t)bits_per_sample / 8U);

            if (bits_per_sample == 8U) {
                sample_i32 = (int32_t)((int8_t)wav_data[byte_offset]) * 256;
            } else if (bits_per_sample == 16U) {
                sample_i32 = (int32_t)(int16_t)(((uint16_t)wav_data[byte_offset] & 0xFFU) |
                                               ((uint16_t)wav_data[byte_offset + 1U] << 8));
            } else if (bits_per_sample == 24U) {
                sample_i32 = (int32_t)((uint32_t)wav_data[byte_offset] |
                                      ((uint32_t)wav_data[byte_offset + 1U] << 8) |
                                      ((uint32_t)wav_data[byte_offset + 2U] << 16));
                if (sample_i32 & 0x00800000U) {
                    sample_i32 |= ~0x00FFFFFFU;
                }
            } else if (bits_per_sample == 32U) {
                sample_i32 = (int32_t)(((uint32_t)wav_data[byte_offset] & 0xFFU) |
                                      ((uint32_t)wav_data[byte_offset + 1U] << 8) |
                                      ((uint32_t)wav_data[byte_offset + 2U] << 16) |
                                      ((uint32_t)wav_data[byte_offset + 3U] << 24));
            }

            if (bits_per_sample > 16U) {
                sample_i16 = (int16_t)(sample_i32 >> (bits_per_sample - 16U));
            } else if (bits_per_sample == 16U) {
                sample_i16 = (int16_t)sample_i32;
            } else {
                sample_i16 = (int16_t)(sample_i32 >> 8);
            }

            if (i % 12U == 0U) {
                if (i != 0U) {
                    fprintf(out, "\n");
                }
                fprintf(out, "    ");
            }
            fprintf(out, "%d, ", (int)sample_i16);
        }
    }

    fprintf(out, "\n};\n\n");
    fprintf(out, "const uint32_t %s_i2s_len = sizeof(%s_i2s) / sizeof(%s_i2s[0]);\n", var_name, var_name, var_name);
    fprintf(out, "const uint32_t %s_i2s_bytes = sizeof(%s_i2s);\n", var_name, var_name);

    rc = 0;

    free(wav_data);
    fclose(out);
    return rc;
}
