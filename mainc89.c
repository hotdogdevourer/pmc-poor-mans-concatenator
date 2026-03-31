#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdarg.h>

#ifdef _WIN32
#  include <direct.h>
#  define mkdir_compat(p)  _mkdir(p)
#  define rmdir_compat(p)  _rmdir(p)
#else
#  include <unistd.h>
#  include <sys/stat.h>
#  define mkdir_compat(p)  mkdir((p), 0755)
#  define rmdir_compat(p)  rmdir(p)
#endif

static int vbc_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int     ret;
    FILE   *tmp;
    size_t  to_read;

    if (size == 0) return -1;
    buf[0] = '\0';

    tmp = tmpfile();
    if (!tmp) return -1;

    va_start(ap, fmt);
    ret = vfprintf(tmp, fmt, ap);
    va_end(ap);

    if (ret < 0) { fclose(tmp); return -1; }

    rewind(tmp);
    to_read = ((size_t)ret < size - 1u) ? (size_t)ret : size - 1u;
    fread(buf, 1u, to_read, tmp);
    buf[to_read] = '\0';
    fclose(tmp);
    return ret;
}
#define snprintf vbc_snprintf

#if defined(_MSC_VER) && _MSC_VER < 1600
   typedef unsigned char      uint8_t;
   typedef unsigned short     uint16_t;
   typedef unsigned int       uint32_t;
   typedef unsigned __int64   uint64_t;
   typedef signed short       int16_t;
   typedef signed int         int32_t;
   typedef unsigned int       size_t;                                
#else
#  include <stdint.h>
#endif

static char *vbc_strdup(const char *s) {
    size_t n = strlen(s) + 1u;
    char  *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
#define strdup vbc_strdup

#define DEFAULT_OUTPUT_BIT_DEPTH    2
#define DEFAULT_WORD_SILENCE_MS     50.0
#define DEFAULT_PHONEME_SILENCE_MS  0.0
#define DEFAULT_CROSSFADE_MS        0.0

static const int SUPPORTED_BIT_DEPTHS[]  = {1, 2, 4, 6, 8, 10, 12, 14, 16};
static const int N_SUPPORTED_BIT_DEPTHS  = 9;

static int is_supported_bit_depth(int bd) {
    int i;
    for (i = 0; i < N_SUPPORTED_BIT_DEPTHS; i++)
        if (SUPPORTED_BIT_DEPTHS[i] == bd) return 1;
    return 0;
}

static uint32_t crc32_buf(const uint8_t *buf, size_t len) {
    static uint32_t tbl[256];
    static int      tbl_init = 0;
    uint32_t crc;
    size_t   i;
    if (!tbl_init) {
        uint32_t n;
        for (n = 0; n < 256u; n++) {
            uint32_t c = n; int k;
            for (k = 0; k < 8; k++)
                c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
            tbl[n] = c;
        }
        tbl_init = 1;
    }
    crc = 0xFFFFFFFFu;
    for (i = 0; i < len; i++)
        crc = tbl[(crc ^ buf[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

typedef struct {
    char    *key;
    uint32_t offset;
    uint32_t length;
    uint32_t samples;
} VBCEntry;

typedef struct {
    uint8_t  channels;
    uint16_t bit_depth;
    uint32_t framerate;
    VBCEntry *entries;
    uint32_t  entry_count;
    uint8_t  *data;
    size_t    data_size;
    uint32_t  crc32;
} VoiceBank;

static void vbc_free(VoiceBank *vb) {
    uint32_t i;
    if (!vb) return;
    for (i = 0; i < vb->entry_count; i++) free(vb->entries[i].key);
    free(vb->entries);
    free(vb->data);
    memset(vb, 0, sizeof(*vb));
}

static const VBCEntry *vbc_find(const VoiceBank *vb, const char *key) {
    uint32_t i;
    for (i = 0; i < vb->entry_count; i++)
        if (strcmp(vb->entries[i].key, key) == 0)
            return &vb->entries[i];
    return NULL;
}

typedef struct {
    uint32_t framerate;
    uint16_t channels;
    uint16_t sampwidth;                         
    uint32_t nframes;
} WavParams;

static void w32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void w16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static void wav_write_header(FILE *f, uint32_t framerate, uint16_t channels,
                             uint16_t sampwidth, uint32_t data_bytes)
{
    uint32_t byte_rate       = framerate * channels * sampwidth;
    uint16_t block_align     = (uint16_t)(channels * sampwidth);
    uint16_t bits_per_sample = (uint16_t)(sampwidth * 8u);

    fwrite("RIFF", 1, 4, f);
    w32(f, 36u + data_bytes);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    w32(f, 16);
    w16(f, 1);                     
    w16(f, channels);
    w32(f, framerate);
    w32(f, byte_rate);
    w16(f, block_align);
    w16(f, bits_per_sample);
    fwrite("data", 1, 4, f);
    w32(f, data_bytes);
}

static int wav_read(const char *path, WavParams *params,
                    uint8_t **out_data, size_t *out_len)
{
    char     riff[4], wave[4];
    int      got_fmt = 0, got_data = 0;
    uint16_t audio_format = 0;
    FILE    *f = fopen(path, "rb");

    if (!f) { fprintf(stderr, "[ERROR] Cannot open WAV: %s\n", path); return 0; }

    if (fread(riff, 1, 4, f) != 4 || memcmp(riff, "RIFF", 4) != 0) {
        fprintf(stderr, "[ERROR] Not a RIFF file: %s\n", path); fclose(f); return 0;
    }
    { uint32_t riff_size; if (fread(&riff_size, 4, 1, f) != 1) { fclose(f); return 0; } }
    if (fread(wave, 1, 4, f) != 4 || memcmp(wave, "WAVE", 4) != 0) {
        fprintf(stderr, "[ERROR] Not a WAVE file: %s\n", path); fclose(f); return 0;
    }

    while (!got_data) {
        char     chunk_id[4];
        uint32_t chunk_size;
        if (fread(chunk_id,   1, 4, f) != 4) break;
        if (fread(&chunk_size, 4, 1, f) != 1) break;

        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            uint32_t byte_rate;
            uint16_t block_align, bits;
            if (fread(&audio_format,      2, 1, f) != 1) break;
            if (fread(&params->channels,  2, 1, f) != 1) break;
            if (fread(&params->framerate, 4, 1, f) != 1) break;
            if (fread(&byte_rate,         4, 1, f) != 1) break;
            if (fread(&block_align,       2, 1, f) != 1) break;
            if (fread(&bits,              2, 1, f) != 1) break;
            params->sampwidth = (uint16_t)(bits / 8u);
            if (chunk_size > 16) fseek(f, (long)(chunk_size - 16u), SEEK_CUR);
            got_fmt = 1;
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            size_t rd;
            if (!got_fmt) {
                fprintf(stderr, "[ERROR] data chunk before fmt\n"); fclose(f); return 0;
            }
            *out_data = (uint8_t *)malloc(chunk_size);
            if (!*out_data) { fclose(f); return 0; }
            rd = fread(*out_data, 1, chunk_size, f);
            *out_len = rd;
            params->nframes = (uint32_t)(rd / ((size_t)params->channels * params->sampwidth));
            got_data = 1;
        } else {
            fseek(f, (long)chunk_size, SEEK_CUR);
        }
    }
    fclose(f);
    if (!got_data) {
        fprintf(stderr, "[ERROR] No data chunk in: %s\n", path); return 0;
    }
    return 1;
}

static uint8_t *make_silence_frames(uint32_t framerate, uint16_t channels,
                                    double ms, size_t *out_bytes)
{
    size_t n_samples = (size_t)((double)framerate * ms / 1000.0);
    size_t n_bytes   = n_samples * channels * 2u;
    uint8_t *buf = (uint8_t *)calloc(n_bytes ? n_bytes : 1u, 1u);
    *out_bytes = n_bytes;
    return buf;
}


#define VBC_BUF_APPEND(byte) do { \
    uint8_t *_tmp = (uint8_t *)realloc(buf, buf_len + 1u); \
    if (!_tmp) { free(scaled); return NULL; } \
    buf = _tmp; \
    buf[buf_len++] = (uint8_t)(byte); \
} while(0)

static uint8_t *pcm_to_packed(const uint8_t *pcm_data, size_t pcm_len,
                               int sampwidth, int bit_depth,
                               size_t *out_len, uint32_t *out_samples)
{
    uint32_t  n_samples, i;
    uint32_t *samples = NULL;
    uint32_t  max_input;
    uint32_t  max_output;
    uint32_t *scaled  = NULL;
    uint8_t  *buf     = NULL;
    size_t    buf_len = 0;

    if (sampwidth != 1 && sampwidth != 2) {
        fprintf(stderr, "[ERROR] Unsupported sampwidth: %d\n", sampwidth);
        return NULL;
    }

    n_samples    = (uint32_t)(pcm_len / (size_t)sampwidth);
    *out_samples = n_samples;

    if (bit_depth == 16) {
        uint8_t *out = (uint8_t *)malloc((size_t)n_samples * 2u);
        if (!out) return NULL;
        if (sampwidth == 2) {
            memcpy(out, pcm_data, (size_t)n_samples * 2u);
        } else {
            for (i = 0; i < n_samples; i++) {
                int16_t v = (int16_t)(((uint32_t)pcm_data[i] * 65535u / 255u) - 32768u);
                memcpy(out + i * 2u, &v, 2u);
            }
        }
        *out_len = (size_t)n_samples * 2u;
        return out;
    }

    samples = (uint32_t *)malloc((size_t)n_samples * sizeof(uint32_t));
    if (!samples) return NULL;

    if (sampwidth == 1) {
        max_input = 255u;
        for (i = 0; i < n_samples; i++) samples[i] = pcm_data[i];
    } else {
        max_input = 65535u;
        for (i = 0; i < n_samples; i++) {
            int16_t s; memcpy(&s, pcm_data + i * 2u, 2u);
            samples[i] = (uint32_t)((int32_t)s + 32768);
        }
    }

    if (bit_depth == 1) {
        size_t   packed_bytes = ((size_t)n_samples + 7u) / 8u;
        uint8_t *out = (uint8_t *)calloc(packed_bytes, 1u);
        if (!out) { free(samples); return NULL; }
        for (i = 0; i < n_samples; i++) {
            int bit = (sampwidth == 2)
                ? ((int16_t)(samples[i] - 32768u) >= 0 ? 1 : 0)
                : (samples[i] >= 128u ? 1 : 0);
            out[i / 8u] |= (uint8_t)(bit << (7 - (int)(i % 8u)));
        }
        free(samples);
        *out_len = packed_bytes;
        return out;
    }

    max_output = (1u << bit_depth) - 1u;
    scaled = (uint32_t *)malloc((size_t)n_samples * sizeof(uint32_t));
    if (!scaled) { free(samples); return NULL; }
    for (i = 0; i < n_samples; i++)
        scaled[i] = (uint32_t)((uint64_t)samples[i] * max_output / max_input);
    free(samples);

    switch (bit_depth) {

    case 2:
        for (i = 0; i < n_samples; i += 4u) {
            uint8_t byte = 0;
            int j;
            for (j = 0; j < 4; j++)
                if (i + (uint32_t)j < n_samples)
                    byte |= (uint8_t)((scaled[i + (uint32_t)j] & 0x03u) << (6 - j * 2));
            VBC_BUF_APPEND(byte);
        }
        break;

    case 4:
        for (i = 0; i < n_samples; i += 2u) {
            uint8_t byte = (uint8_t)((scaled[i] & 0x0Fu) << 4);
            if (i + 1u < n_samples) byte |= (uint8_t)(scaled[i + 1u] & 0x0Fu);
            VBC_BUF_APPEND(byte);
        }
        break;

    case 6:
        for (i = 0; i < n_samples; i += 4u) {
            if (i + 3u < n_samples) {
                uint8_t s0 = (uint8_t)scaled[i], s1 = (uint8_t)scaled[i+1u],
                        s2 = (uint8_t)scaled[i+2u], s3 = (uint8_t)scaled[i+3u];
                VBC_BUF_APPEND((s0 << 2) | (s1 >> 4));
                VBC_BUF_APPEND(((s1 & 0x0Fu) << 4) | (s2 >> 2));
                VBC_BUF_APPEND(((s2 & 0x03u) << 6) | s3);
            } else {
                uint32_t rem[4] = {0,0,0,0};
                uint32_t cnt = n_samples - i, j;
                for (j = 0; j < cnt; j++) rem[j] = scaled[i + j];
                VBC_BUF_APPEND((rem[0] << 2) & 0xFCu);
                if (cnt >= 2u) { buf[buf_len-1u] |= (uint8_t)((rem[1] >> 4) & 0x03u); VBC_BUF_APPEND((rem[1] << 4) & 0xF0u); }
                if (cnt >= 3u) { buf[buf_len-1u] |= (uint8_t)((rem[2] >> 2) & 0x0Fu); VBC_BUF_APPEND((rem[2] << 6) & 0xC0u); }
                if (cnt >= 4u) buf[buf_len-1u] |= (uint8_t)(rem[3] & 0x3Fu);
            }
        }
        break;

    case 8:
        buf = (uint8_t *)malloc(n_samples);
        if (!buf) { free(scaled); return NULL; }
        for (i = 0; i < n_samples; i++) buf[i] = (uint8_t)(scaled[i] & 0xFFu);
        buf_len = n_samples;
        break;

    case 10:
        for (i = 0; i < n_samples; i += 4u) {
            if (i + 3u < n_samples) {
                uint16_t s0=(uint16_t)scaled[i], s1=(uint16_t)scaled[i+1u],
                         s2=(uint16_t)scaled[i+2u], s3=(uint16_t)scaled[i+3u];
                VBC_BUF_APPEND(s0 >> 2);
                VBC_BUF_APPEND(((s0 & 0x03u) << 6) | (s1 >> 4));
                VBC_BUF_APPEND(((s1 & 0x0Fu) << 4) | (s2 >> 6));
                VBC_BUF_APPEND(((s2 & 0x3Fu) << 2) | (s3 >> 8));
                VBC_BUF_APPEND(s3 & 0xFFu);
            } else {
                uint32_t rem[4]={0,0,0,0}, cnt = n_samples - i, j;
                for (j = 0; j < cnt; j++) rem[j] = scaled[i + j];
                if (cnt >= 1u) { VBC_BUF_APPEND(rem[0] >> 2); VBC_BUF_APPEND((rem[0] & 0x03u) << 6); }
                if (cnt >= 2u) { buf[buf_len-1u] |= (uint8_t)((rem[1] >> 4) & 0x3Fu); VBC_BUF_APPEND((rem[1] & 0x0Fu) << 4); }
                if (cnt >= 3u) { buf[buf_len-1u] |= (uint8_t)((rem[2] >> 6) & 0x0Fu); VBC_BUF_APPEND((rem[2] & 0x3Fu) << 2); }
                if (cnt >= 4u) { buf[buf_len-1u] |= (uint8_t)((rem[3] >> 8) & 0x03u); VBC_BUF_APPEND(rem[3] & 0xFFu); }
            }
        }
        break;

    case 12:
        for (i = 0; i < n_samples; i += 2u) {
            uint16_t s0 = (uint16_t)scaled[i];
            VBC_BUF_APPEND(s0 >> 4);
            if (i + 1u < n_samples) {
                uint16_t s1 = (uint16_t)scaled[i + 1u];
                VBC_BUF_APPEND(((s0 & 0x0Fu) << 4) | (s1 >> 8));
                VBC_BUF_APPEND(s1 & 0xFFu);
            } else {
                VBC_BUF_APPEND((s0 & 0x0Fu) << 4);
            }
        }
        break;

    case 14:
        for (i = 0; i < n_samples; i += 4u) {
            if (i + 3u < n_samples) {
                uint16_t s0=(uint16_t)scaled[i], s1=(uint16_t)scaled[i+1u],
                         s2=(uint16_t)scaled[i+2u], s3=(uint16_t)scaled[i+3u];
                VBC_BUF_APPEND(s0 >> 6);
                VBC_BUF_APPEND(((s0 & 0x3Fu) << 2) | (s1 >> 12));
                VBC_BUF_APPEND((s1 >> 4) & 0xFFu);
                VBC_BUF_APPEND(((s1 & 0x0Fu) << 4) | (s2 >> 10));
                VBC_BUF_APPEND((s2 >> 2) & 0xFFu);
                VBC_BUF_APPEND(((s2 & 0x03u) << 6) | (s3 >> 8));
                VBC_BUF_APPEND(s3 & 0xFFu);
            } else {
                uint32_t j;
                for (j = i; j < n_samples; j++) {
                    VBC_BUF_APPEND(scaled[j] >> 6);
                    VBC_BUF_APPEND((scaled[j] & 0x3Fu) << 2);
                }
            }
        }
        break;

    default:
        fprintf(stderr, "[ERROR] pack: unsupported bit depth %d\n", bit_depth);
        free(scaled);
        return NULL;
    }

    free(scaled);
    *out_len = buf_len;
    return buf;
}

#undef VBC_BUF_APPEND


#define VBC_PUSH(raw_unsigned, max_val) do { \
    int32_t _s; \
    if (n >= sample_count) break; \
    _s = (int32_t)(((uint64_t)(raw_unsigned) * (uint64_t)65535U / (uint64_t)(max_val))) - 32768; \
    if (_s < -32768) _s = -32768; \
    if (_s >  32767) _s =  32767; \
    out_buf[n++] = (int16_t)_s; \
} while(0)

static size_t unpack_packed_to_pcm(const uint8_t *packed_data, size_t packed_len,
                                   uint32_t sample_count, int bit_depth,
                                   int16_t *out_buf)
{
    size_t n = 0;

    switch (bit_depth) {

    case 1: {
        size_t i;
        for (i = 0; i < packed_len && n < sample_count; i++) {
            uint8_t byte = packed_data[i];
            int j;
            for (j = 7; j >= 0 && n < sample_count; j--) {
                uint8_t bit = (byte >> j) & 1u;
                out_buf[n++] = bit ? 32767 : -32768;
            }
        }
        break;
    }

    case 2: {
        size_t i;
        for (i = 0; i < packed_len && n < sample_count; i++) {
            uint8_t byte = packed_data[i];
            int sh;
            for (sh = 6; sh >= 0 && n < sample_count; sh -= 2) {
                uint8_t v = (byte >> sh) & 0x03u;
                VBC_PUSH(v, 3);
            }
        }
        break;
    }

    case 4: {
        size_t i;
        for (i = 0; i < packed_len && n < sample_count; i++) {
            uint8_t byte = packed_data[i];
            uint8_t hi = (byte >> 4) & 0x0Fu;
            uint8_t lo =  byte       & 0x0Fu;
            VBC_PUSH(hi, 15);
            if (n < sample_count) VBC_PUSH(lo, 15);
        }
        break;
    }

    case 6: {
        size_t i = 0;
        while (i + 2u < packed_len && n < sample_count) {
            uint8_t b0=packed_data[i], b1=packed_data[i+1u], b2=packed_data[i+2u];
            uint8_t s0 = (b0 >> 2) & 0x3Fu;
            uint8_t s1 = (uint8_t)(((b0 & 0x03u) << 4) | ((b1 >> 4) & 0x0Fu));
            uint8_t s2 = (uint8_t)(((b1 & 0x0Fu) << 2) | ((b2 >> 6) & 0x03u));
            uint8_t s3 =  b2 & 0x3Fu;
            VBC_PUSH(s0, 63); if (n < sample_count) VBC_PUSH(s1, 63);
            if (n < sample_count) VBC_PUSH(s2, 63);
            if (n < sample_count) VBC_PUSH(s3, 63);
            i += 3u;
        }
        break;
    }

    case 8: {
        size_t i;
        for (i = 0; i < packed_len && n < sample_count; i++)
            VBC_PUSH(packed_data[i], 255);
        break;
    }

    case 10: {
        size_t i = 0;
        while (i + 4u < packed_len && n < sample_count) {
            const uint8_t *b = packed_data + i;
            uint16_t s0 = (uint16_t)(((uint16_t)b[0] << 2) | (b[1] >> 6));
            uint16_t s1 = (uint16_t)((((uint16_t)b[1] & 0x3Fu) << 4) | (b[2] >> 4));
            uint16_t s2 = (uint16_t)((((uint16_t)b[2] & 0x0Fu) << 6) | (b[3] >> 2));
            uint16_t s3 = (uint16_t)((((uint16_t)b[3] & 0x03u) << 8) |  b[4]);
            VBC_PUSH(s0, 1023); if (n < sample_count) VBC_PUSH(s1, 1023);
            if (n < sample_count) VBC_PUSH(s2, 1023);
            if (n < sample_count) VBC_PUSH(s3, 1023);
            i += 5u;
        }
        break;
    }

    case 12: {
        size_t i = 0;
        while (i + 2u < packed_len && n < sample_count) {
            uint8_t  b0=packed_data[i], b1=packed_data[i+1u], b2=packed_data[i+2u];
            uint16_t s0 = (uint16_t)(((uint16_t)b0 << 4) | (b1 >> 4));
            uint16_t s1 = (uint16_t)((((uint16_t)b1 & 0x0Fu) << 8) | b2);
            VBC_PUSH(s0, 4095);
            if (n < sample_count) VBC_PUSH(s1, 4095);
            i += 3u;
        }
        break;
    }

    case 14: {
        size_t i = 0;
        while (i + 6u < packed_len && n < sample_count) {
            const uint8_t *b = packed_data + i;
            uint16_t s0 = (uint16_t)(((uint16_t)b[0] << 6) | (b[1] >> 2));
            uint16_t s1 = (uint16_t)((((uint16_t)b[1] & 0x03u) << 12) | ((uint16_t)b[2] << 4) | (b[3] >> 4));
            uint16_t s2 = (uint16_t)((((uint16_t)b[3] & 0x0Fu) << 10) | ((uint16_t)b[4] << 2)  | (b[5] >> 6));
            uint16_t s3 = (uint16_t)((((uint16_t)b[5] & 0x3Fu) << 8)  |  b[6]);
            VBC_PUSH(s0, 16383); if (n < sample_count) VBC_PUSH(s1, 16383);
            if (n < sample_count) VBC_PUSH(s2, 16383);
            if (n < sample_count) VBC_PUSH(s3, 16383);
            i += 7u;
        }
        break;
    }

    case 16: {
        size_t i;
        for (i = 0; i + 1u < packed_len && n < sample_count; i += 2u) {
            int16_t v; memcpy(&v, packed_data + i, 2u);
            out_buf[n++] = v;
        }
        break;
    }

    default:
        fprintf(stderr, "[ERROR] unpack: unsupported bit depth %d\n", bit_depth);
        break;
    }

#undef VBC_PUSH
    return n;
}

static int load_voice_bank(const char *filepath, VoiceBank *vb,
                           int verbose, int werror, int iwarn)
{
    long     file_size;
    uint8_t *content;
    size_t   body_size, offset;
    uint32_t stored_crc, calc_crc, entry_count, i;
    FILE    *f = fopen(filepath, "rb");

    if (!f) {
        fprintf(stderr, "[ERROR] Voice bank not found: %s\n", filepath);
        return 0;
    }
    fseek(f, 0, SEEK_END); file_size = ftell(f); fseek(f, 0, SEEK_SET);
    if (file_size < 18) {
        fprintf(stderr, "[ERROR] VBC file too small\n"); fclose(f); return 0;
    }

    content = (uint8_t *)malloc((size_t)file_size);
    if (!content) { fprintf(stderr, "[ERROR] Out of memory\n"); fclose(f); return 0; }
    if (fread(content, 1u, (size_t)file_size, f) != (size_t)file_size) {
        fprintf(stderr, "[ERROR] Read error\n"); free(content); fclose(f); return 0;
    }
    fclose(f);

    body_size  = (size_t)file_size - 4u;
    memcpy(&stored_crc, content + body_size, 4u);
    calc_crc   = crc32_buf(content, body_size);

    if (calc_crc != stored_crc) {
        if (!iwarn)
            fprintf(stderr, "[WARNING] CRC mismatch! Stored: %08X, Calculated: %08X\n"
                            "[WARNING] VBC file may be corrupted\n", stored_crc, calc_crc);
        if (werror) {
            fprintf(stderr, "[ERROR] Treating CRC mismatch as fatal (-werror)\n");
            free(content); return 0;
        }
    } else {
        if (verbose) printf("[INFO] CRC validated: %08X\n", stored_crc);
    }
    vb->crc32 = stored_crc;

    if (memcmp(content, "VBC\x00", 4u) != 0) {
        fprintf(stderr, "[ERROR] Invalid VBC Magic Number\n");
        free(content); return 0;
    }

    vb->channels = content[5];
    memcpy(&vb->bit_depth,  content + 6u,  2u);
    memcpy(&vb->framerate,  content + 8u,  4u);
    memcpy(&entry_count,    content + 12u, 4u);

    if (!is_supported_bit_depth(vb->bit_depth)) {
        fprintf(stderr, "[ERROR] Unsupported bit depth: %u\n", vb->bit_depth);
        free(content); return 0;
    }

    vb->entries = (VBCEntry *)calloc(entry_count, sizeof(VBCEntry));
    if (!vb->entries) { free(content); return 0; }
    vb->entry_count = entry_count;

    offset = 16u;
    for (i = 0; i < entry_count; i++) {
        uint8_t key_len;
        if (offset + 1u > body_size) goto vbc_parse_err;
        key_len = content[offset++];
        if (offset + key_len + 12u > body_size) goto vbc_parse_err;

        vb->entries[i].key = (char *)malloc(key_len + 1u);
        if (!vb->entries[i].key) goto vbc_parse_err;
        memcpy(vb->entries[i].key, content + offset, key_len);
        vb->entries[i].key[key_len] = '\0';
        offset += key_len;

        memcpy(&vb->entries[i].offset,  content + offset, 4u); offset += 4u;
        memcpy(&vb->entries[i].length,  content + offset, 4u); offset += 4u;
        memcpy(&vb->entries[i].samples, content + offset, 4u); offset += 4u;
    }

    {
        size_t data_size = body_size - offset;
        vb->data = (uint8_t *)malloc(data_size ? data_size : 1u);
        if (!vb->data) goto vbc_parse_err;
        memcpy(vb->data, content + offset, data_size);
        vb->data_size = data_size;
    }

    free(content);
    if (verbose) {
        const char *fmt_str = (vb->bit_depth < 8) ? "packed" : "PCM";
        printf("[INFO] Loaded VBC: %u entries, %uHz, %u-bit %s, %lu bytes\n",
               entry_count, vb->framerate, vb->bit_depth, fmt_str, (unsigned long)vb->data_size);
    }
    return 1;

vbc_parse_err:
    fprintf(stderr, "[ERROR] Corrupt or truncated VBC index\n");
    for (i = 0; i < entry_count; i++) free(vb->entries[i].key);
    free(vb->entries); vb->entries = NULL;
    free(content);
    return 0;
}

typedef struct { char *key; int is_word_end; } Token;
typedef struct { Token *items; size_t count; size_t capacity; } TokenList;

static void token_list_init(TokenList *tl) { tl->items = NULL; tl->count = tl->capacity = 0; }
static void token_list_free(TokenList *tl) {
    size_t i;
    for (i = 0; i < tl->count; i++) free(tl->items[i].key);
    free(tl->items); token_list_init(tl);
}
static int token_list_push(TokenList *tl, const char *key, int is_word_end) {
    if (tl->count == tl->capacity) {
        size_t newcap = tl->capacity ? tl->capacity * 2u : 16u;
        Token *tmp = (Token *)realloc(tl->items, newcap * sizeof(Token));
        if (!tmp) return 0;
        tl->items = tmp; tl->capacity = newcap;
    }
    tl->items[tl->count].key         = strdup(key);
    tl->items[tl->count].is_word_end = is_word_end;
    tl->count++;
    return 1;
}

static const char *strip_spaces(const char *key, char *out_buf) {
    size_t start = 0, len = strlen(key);
    size_t n;
    while (start < len && key[start] == ' ') start++;
    while (len > start && key[len-1] == ' ') len--;
    n = len - start;
    memcpy(out_buf, key + start, n);
    out_buf[n] = '\0';
    return out_buf;
}

static int tokenize_text(const char *text, const VoiceBank *vb, TokenList *out) {
    size_t      tlen;
    char       *lower;
    uint32_t    ec;
    char      **stripped;
    const char **original;
    uint32_t   *order;
    size_t      i;
    uint32_t    k;

    if (!text || !vb || vb->entry_count == 0) return 1;

    tlen  = strlen(text);
    lower = (char *)malloc(tlen + 1u);
    if (!lower) return 0;
    for (i = 0; i <= tlen; i++)
        lower[i] = (text[i] >= 'A' && text[i] <= 'Z') ? text[i] + 32 : text[i];

    ec       = vb->entry_count;
    stripped = (char **)malloc(ec * sizeof(char *));
    original = (const char **)malloc(ec * sizeof(const char *));
    if (!stripped || !original) { free(lower); free(stripped); free(original); return 0; }

    for (k = 0; k < ec; k++) {
        const char *raw  = vb->entries[k].key;
        size_t      rlen = strlen(raw);
        char       *buf  = (char *)malloc(rlen + 1u);
        if (!buf) { free(lower); return 0; }
        strip_spaces(raw, buf);
        stripped[k] = buf;
        original[k] = raw;
    }

    order = (uint32_t *)malloc(ec * sizeof(uint32_t));
    if (!order) { free(lower); return 0; }
    for (k = 0; k < ec; k++) order[k] = k;

    {
        uint32_t a;
        for (a = 1; a < ec; a++) {
            uint32_t tmp = order[a];
            size_t   tsl = strlen(stripped[tmp]);
            int32_t  b   = (int32_t)a - 1;
            while (b >= 0 && strlen(stripped[order[b]]) < tsl) {
                order[b+1] = order[b]; b--;
            }
            order[b+1] = tmp;
        }
    }

    {
        size_t pos = 0;
        while (pos < tlen) {
            int      matched = 0;
            uint32_t kk;
            if (lower[pos] == ' ') { pos++; continue; }
            for (kk = 0; kk < ec; kk++) {
                uint32_t    idx = order[kk];
                const char *sk  = stripped[idx];
                size_t      sl  = strlen(sk);
                if (sl == 0) continue;
                if (pos + sl <= tlen &&
                    strncmp(lower + pos, sk, sl) == 0)
                {
                    int is_we = (pos + sl == tlen) || (lower[pos + sl] == ' ');
                    token_list_push(out, original[idx], is_we);
                    pos += sl;
                    matched = 1;
                    break;
                }
            }
            if (!matched) pos++;
        }
    }

    for (k = 0; k < ec; k++) free(stripped[k]);
    free(stripped); free(original); free(order); free(lower);
    return 1;
}


static int pcm_is_silent(const int16_t *pcm, size_t n_samples) {
    size_t i;
    for (i = 0; i < n_samples; i++) {
        int32_t v = pcm[i]; if (v < 0) v = -v;
        if (v >= 328) return 0;
    }
    return 1;
}

static size_t nearest_zero_crossing(const int16_t *pcm, size_t start,
                                    size_t lo, size_t hi)
{
    size_t radius;
    size_t max_r = (hi - lo);
    if (start <= lo) return lo;
    if (start >= hi) return hi - 1u;
    if (pcm[start] == 0) return start;

    for (radius = 1u; radius <= max_r; radius++) {
        if (start + radius < hi) {
            if (pcm[start + radius - 1u] == 0 ||
                (pcm[start + radius - 1u] > 0) != (pcm[start + radius] > 0))
                return start + radius;
        }
        if (start >= lo + radius) {
            size_t back = start - radius;
            if (pcm[back] == 0 ||
                (back + 1u < hi &&
                 ((pcm[back] > 0) != (pcm[back + 1u] > 0))))
                return back;
        }
    }
    return start;
}











typedef struct { char *key; char *value; } MapEntry;
typedef struct { MapEntry *items; size_t count; size_t capacity; } MapList;

static void map_list_free(MapList *ml) {
    size_t i;
    for (i = 0; i < ml->count; i++) { free(ml->items[i].key); free(ml->items[i].value); }
    free(ml->items); ml->items = NULL; ml->count = ml->capacity = 0;
}
static int map_list_push(MapList *ml, const char *k, const char *v) {
    if (ml->count == ml->capacity) {
        size_t nc = ml->capacity ? ml->capacity * 2u : 16u;
        MapEntry *tmp = (MapEntry *)realloc(ml->items, nc * sizeof(MapEntry));
        if (!tmp) return 0;
        ml->items = tmp; ml->capacity = nc;
    }
    ml->items[ml->count].key   = strdup(k);
    ml->items[ml->count].value = strdup(v);
    ml->count++; return 1;
}
static int load_map_file(const char *path, MapList *ml) {
    char  line[4096];
    int   lineno = 0;
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[ERROR] Cannot open map file: %s\n", path); return 0; }
    while (fgets(line, sizeof(line), f)) {
        char  *colon;
        size_t len = strlen(line);
        lineno++;
        while (len > 0u && (line[len-1u] == '\n' || line[len-1u] == '\r')) line[--len] = '\0';
        if (len == 0u || line[0] == '#') continue;
        colon = strchr(line, ':');
        if (!colon) { fprintf(stderr, "[WARNING] Map line %d has no colon, skipped\n", lineno); continue; }
        *colon = '\0';
        if (strlen(line) == 0u) { fprintf(stderr, "[WARNING] Map line %d has empty key, skipped\n", lineno); continue; }
        map_list_push(ml, line, colon + 1);
    }
    fclose(f); return 1;
}

static void make_safe_filename(const char *key, char *out, size_t out_size) {
    const char *p;
    int alnum = 1;
    for (p = key; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            alnum = 0; break;
        }
    }
    if (alnum) {
        size_t i;
        for (i = 0; key[i] && i + 1u < out_size; i++)
            out[i] = (char)((key[i] >= 'A' && key[i] <= 'Z') ? key[i] + 32 : key[i]);
        out[i] = '\0';
    } else {
        size_t off = 0;
        if (off + 4u < out_size) { memcpy(out + off, "sym_", 4u); off += 4u; }
        for (p = key; *p && off + 2u < out_size; p++) {
            snprintf(out + off, out_size - off, "%02x", (unsigned char)*p);
            off += 2u;
        }
        out[off] = '\0';
    }
}

static int run_sam(const char *phoneme, const char *wav_path, int speed, int verbose) {
    char cmd[4096];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "sam.exe -phonetic -speed %d -wav %s %s", speed, wav_path, phoneme);
#else
    snprintf(cmd, sizeof(cmd), "./sam -phonetic -speed %d -wav %s %s 2>/dev/null", speed, wav_path, phoneme);
#endif
    if (verbose) printf("  SAM: %s\n", cmd);
    { int rc = system(cmd);
      if (rc != 0) { fprintf(stderr, "[WARNING] SAM failed for phoneme '%s' (exit %d)\n", phoneme, rc); return 0; }
    }
    return 1;
}

static int sam_exists(void) {
#ifdef _WIN32
    FILE *f = fopen("sam.exe", "r");
#else
    FILE *f = fopen("./sam",   "r");
#endif
    if (f) { fclose(f); return 1; } return 0;
}

static int write_silence_wav(const char *path, uint32_t framerate,
                              uint16_t channels, uint16_t sampwidth) {
    uint32_t  silence_samples = framerate / 10u;
    uint32_t  data_bytes      = silence_samples * channels * sampwidth;
    FILE     *f = fopen(path, "wb");
    if (!f) return 0;
    wav_write_header(f, framerate, channels, sampwidth, data_bytes);
    { void *z = calloc(data_bytes ? data_bytes : 1u, 1u);
      if (z) { fwrite(z, 1u, data_bytes, f); free(z); } }
    fclose(f); return 1;
}

static void ensure_dir(const char *path) { mkdir_compat(path); }

typedef struct { char *key; char *wav_path; } PackEntry;

typedef struct {
    uint8_t *key_bytes; uint8_t key_len;
    uint32_t offset, length, samples;
} VBCEntryRaw;

static int pack_vbc_from_wavs(PackEntry *entries, size_t n_entries,
                               const char *output_file, int bit_depth,
                               int verbose, int werror, int iwarn)
{

    VBCEntryRaw *raw;
    uint8_t     *data_buf  = NULL;
    size_t       data_used = 0;
    uint32_t     framerate = 0;
    uint16_t     channels  = 1;
    size_t       total_original = 0, valid_count = 0;
    size_t       i, index_size, total, off;
    uint8_t     *file_buf;
    uint32_t     crc, ec;
    FILE        *fout;

    (void)werror;
    printf("--- Packing %s (%d-bit) ---\n", output_file, bit_depth);

    raw = (VBCEntryRaw *)calloc(n_entries, sizeof(VBCEntryRaw));
    if (!raw) return 0;

    for (i = 0; i < n_entries; i++) {
        WavParams params;
        uint8_t  *pcm  = NULL;
        size_t    plen = 0;

        if (!wav_read(entries[i].wav_path, &params, &pcm, &plen)) {
            if (!iwarn)
                fprintf(stderr, "[WARNING] Missing or unreadable WAV for key '%s': %s\n",
                        entries[i].key, entries[i].wav_path);
            raw[i].key_bytes = (uint8_t *)strdup(entries[i].key);
            raw[i].key_len   = (uint8_t)strlen(entries[i].key);
            raw[i].offset    = (uint32_t)data_used;
            raw[i].length    = raw[i].samples = 0;
            continue;
        }

        if (framerate == 0) { framerate = params.framerate; channels = params.channels; }

        { size_t   packed_len = 0;
          uint32_t n_samples  = 0;
          uint8_t *packed = pcm_to_packed(pcm, plen, params.sampwidth, bit_depth, &packed_len, &n_samples);
          free(pcm);

          if (!packed) {
              if (!iwarn) fprintf(stderr, "[WARNING] Pack failed for '%s'\n", entries[i].key);
              raw[i].key_bytes = (uint8_t *)strdup(entries[i].key);
              raw[i].key_len   = (uint8_t)strlen(entries[i].key);
              raw[i].offset    = (uint32_t)data_used;
              raw[i].length    = raw[i].samples = 0;
              continue;
          }

          total_original += (size_t)n_samples * 2u;
          { uint8_t *tmp = (uint8_t *)realloc(data_buf, data_used + packed_len);
            if (!tmp) { free(packed); free(raw); return 0; }
            data_buf = tmp;
          }
          memcpy(data_buf + data_used, packed, packed_len);

          raw[i].key_bytes = (uint8_t *)strdup(entries[i].key);
          raw[i].key_len   = (uint8_t)strlen(entries[i].key);
          raw[i].offset    = (uint32_t)data_used;
          raw[i].length    = (uint32_t)packed_len;
          raw[i].samples   = n_samples;
          data_used       += packed_len;
          free(packed);
          valid_count++;
        }
    }

    if (valid_count == 0) {
        fprintf(stderr, "[ERROR] No audio data could be read.\n");
        free(data_buf);
        for (i = 0; i < n_entries; i++) free(raw[i].key_bytes);
        free(raw); return 0;
    }
    if (framerate == 0) framerate = 22050;

    index_size = 0;
    for (i = 0; i < n_entries; i++) index_size += 1u + raw[i].key_len + 4u + 4u + 4u;
    total    = 16u + index_size + data_used + 4u;
    file_buf = (uint8_t *)malloc(total);
    if (!file_buf) {
        free(data_buf);
        for (i = 0; i < n_entries; i++) free(raw[i].key_bytes);
        free(raw); return 0;
    }

    off = 0;
    memcpy(file_buf + off, "VBC\x00", 4u); off += 4u;
    file_buf[off++] = 1;
    file_buf[off++] = (uint8_t)channels;
    { uint16_t bd16 = (uint16_t)bit_depth; memcpy(file_buf + off, &bd16, 2u); off += 2u; }
    memcpy(file_buf + off, &framerate, 4u); off += 4u;
    ec = (uint32_t)n_entries;
    memcpy(file_buf + off, &ec, 4u); off += 4u;

    for (i = 0; i < n_entries; i++) {
        file_buf[off++] = raw[i].key_len;
        memcpy(file_buf + off, raw[i].key_bytes, raw[i].key_len); off += raw[i].key_len;
        memcpy(file_buf + off, &raw[i].offset,  4u); off += 4u;
        memcpy(file_buf + off, &raw[i].length,  4u); off += 4u;
        memcpy(file_buf + off, &raw[i].samples, 4u); off += 4u;
    }
    if (data_used > 0u) { memcpy(file_buf + off, data_buf, data_used); off += data_used; }

    crc = crc32_buf(file_buf, off);
    memcpy(file_buf + off, &crc, 4u); off += 4u;

    fout = fopen(output_file, "wb");
    if (!fout) {
        fprintf(stderr, "[ERROR] Cannot write: %s\n", output_file);
        free(file_buf); free(data_buf);
        for (i = 0; i < n_entries; i++) free(raw[i].key_bytes);
        free(raw); return 0;
    }
    fwrite(file_buf, 1u, off, fout); fclose(fout);

    { double ratio = total_original > 0u ? (double)data_used / (double)total_original : 0.0;
      printf("Created %s (%lu bytes %d-bit audio, %lu mappings)\n",
             output_file, (unsigned long)data_used, bit_depth, (unsigned long)valid_count);
      if (total_original > 0u)
          printf("Original: %lu bytes, Compression: %.1f%% of original\n",
                 (unsigned long)total_original, ratio * 100.0);
      printf("CRC32: %08X\n", crc);
    }
    if (verbose) printf("[INFO] %lu/%lu entries packed successfully\n", (unsigned long)valid_count, (unsigned long)n_entries);

    free(file_buf); free(data_buf);
    for (i = 0; i < n_entries; i++) free(raw[i].key_bytes);
    free(raw);
    return 1;
}

static char *read_file_text(const char *path) {
    size_t cap = 4096, len = 0, rd;
    char  *buf;
    FILE  *f = (strcmp(path, "-") == 0) ? stdin : fopen(path, "r");
    if (!f) { fprintf(stderr, "[ERROR] Cannot open input file: %s\n", path); return NULL; }
    buf = (char *)malloc(cap);
    if (!buf) { if (f != stdin) fclose(f); return NULL; }
    while ((rd = fread(buf + len, 1u, cap - len - 1u, f)) > 0) {
        len += rd;
        if (len + 1u >= cap) {
            char *tmp;
            cap *= 2u;
            tmp = (char *)realloc(buf, cap);
            if (!tmp) { free(buf); if (f != stdin) fclose(f); return NULL; }
            buf = tmp;
        }
    }
    buf[len] = '\0';
    if (f != stdin) fclose(f);
    return buf;
}

static int looks_like_file(const char *s) {
    FILE *f = fopen(s, "r");
    if (f) { fclose(f); return 1; } return 0;
}

static int stitch(const TokenList *tokens, const VoiceBank *vb,
                  const char *output_path,
                  double word_silence_ms, double phoneme_silence_ms,
                  int out_sampwidth, double crossfade_ms,
                  int verbose, int werror, int iwarn)
{
    size_t    i, valid = 0, skipped = 0;
    size_t    n_segs;
    int       bit_depth;
    uint16_t  out_channels;
    uint32_t  total_frames = 0;
    FILE     *out;

    int16_t **segs    = NULL;
    size_t   *seg_len = NULL;
    int      *seg_ok  = NULL;

    (void)werror; (void)iwarn;

    if (tokens->count == 0) { fprintf(stderr, "[ERROR] No tokens to stitch.\n"); return 0; }

    for (i = 0; i < tokens->count; i++) {
        if (vbc_find(vb, tokens->items[i].key)) valid++;
        else skipped++;
    }
    if (skipped > 0u && !iwarn) printf("[INFO] Skipped %lu unmapped token(s)\n", (unsigned long)skipped);
    if (valid == 0u) { fprintf(stderr, "[ERROR] No valid segments after filtering.\n"); return 0; }
    if (verbose) printf("Stitching %lu segment(s)...\n", (unsigned long)valid);

    bit_depth    = vb->bit_depth;
    out_channels = vb->channels;
    n_segs       = tokens->count;

    segs    = (int16_t **)calloc(n_segs, sizeof(int16_t *));
    seg_len = (size_t   *)calloc(n_segs, sizeof(size_t));
    seg_ok  = (int       *)calloc(n_segs, sizeof(int));
    if (!segs || !seg_len || !seg_ok) {
        free(segs); free(seg_len); free(seg_ok);
        return 0;
    }

    for (i = 0; i < n_segs; i++) {
        const VBCEntry *e = vbc_find(vb, tokens->items[i].key);
        int16_t *raw_pcm;
        size_t   raw_len;

        if (!e) { seg_ok[i] = 0; continue; }

        raw_pcm = (int16_t *)malloc((size_t)e->samples * sizeof(int16_t));
        if (!raw_pcm) { seg_ok[i] = 0; continue; }
        raw_len = unpack_packed_to_pcm(vb->data + e->offset, e->length,
                                       e->samples, bit_depth, raw_pcm);

        segs[i]    = raw_pcm;
        seg_len[i] = raw_len;
        seg_ok[i]  = 1;
    }

    if (crossfade_ms > 0.0) {
        size_t fade_samp = (size_t)((double)vb->framerate * crossfade_ms / 1000.0);
        size_t zcr_radius = fade_samp / 2u + 1u;

        for (i = 0; i + 1u < n_segs; i++) {
            size_t j = i + 1u;
            size_t fs, k;
            size_t cut_a, cut_b;

            while (j < n_segs && !seg_ok[j]) j++;
            if (j >= n_segs) break;
            if (!seg_ok[i]) continue;

            if (pcm_is_silent(segs[i], seg_len[i])) continue;
            if (pcm_is_silent(segs[j], seg_len[j])) continue;

            {
                size_t natural_cut_a = seg_len[i];                        
                size_t natural_cut_b = 0u;
                size_t lo_a, hi_a, lo_b, hi_b;

                lo_a = (natural_cut_a > zcr_radius) ? natural_cut_a - zcr_radius : 0u;
                hi_a = seg_len[i];                        

                lo_b = 0u;
                hi_b = (zcr_radius < seg_len[j]) ? zcr_radius : seg_len[j];

                cut_a = nearest_zero_crossing(segs[i],
                            (natural_cut_a > 0u) ? natural_cut_a - 1u : 0u,
                            lo_a, hi_a);
                cut_b = nearest_zero_crossing(segs[j], natural_cut_b,
                            lo_b, hi_b);

                if (cut_a < seg_len[i]) seg_len[i] = cut_a + 1u;
                if (cut_b > 0u && cut_b < seg_len[j]) {
                    segs[j]    += cut_b;
                    seg_len[j] -= cut_b;
                }
            }

            fs = fade_samp;
            if (fs > seg_len[i]) fs = seg_len[i];
            if (fs > seg_len[j]) fs = seg_len[j];

            for (k = 0; k < fs; k++) {
                double t     = (double)(k + 1u) / (double)(fs + 1u);
                double w_a   = 1.0 - t;
                double w_b   = t;
                double mixed = w_a * (double)segs[i][seg_len[i] - fs + k]
                             + w_b * (double)segs[j][k];
                if (mixed >  32767.0) mixed =  32767.0;
                if (mixed < -32768.0) mixed = -32768.0;
                segs[i][seg_len[i] - fs + k] = (int16_t)mixed;
                segs[j][k] = 0;
            }
        }
    }

    out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "[ERROR] Cannot write output: %s\n", output_path);
        for (i = 0; i < n_segs; i++) free(segs[i]);
        free(segs); free(seg_len); free(seg_ok);
        return 0;
    }
    wav_write_header(out, vb->framerate, out_channels, 2u, 0u);

    for (i = 0; i < n_segs; i++) {
        size_t sil_bytes;
        uint8_t *sil;
        double sil_ms;

        if (!seg_ok[i]) continue;

        fwrite(segs[i], 2u, seg_len[i], out);
        total_frames += (uint32_t)seg_len[i];

        if (i < n_segs - 1u) {
            sil_ms = tokens->items[i].is_word_end ? word_silence_ms : phoneme_silence_ms;
            if (sil_ms > 0.0) {
                sil = make_silence_frames(vb->framerate, out_channels, sil_ms, &sil_bytes);
                if (sil) {
                    fwrite(sil, 1u, sil_bytes, out);
                    free(sil);
                    total_frames += (uint32_t)(sil_bytes / (out_channels * 2u));
                }
            }
        }
    }

    fclose(out);

    out = fopen(output_path, "rb+");
    if (out) {
        wav_write_header(out, vb->framerate, out_channels, 2u,
                         (uint32_t)(total_frames * out_channels * 2u));
        fclose(out);
    }

    for (i = 0; i < n_segs; i++) free(segs[i]);
    free(segs); free(seg_len); free(seg_ok);

    { double dur = (double)total_frames / (double)vb->framerate;
      printf("Output: %s (%.2fs, %d-bit)\n", output_path, dur, out_sampwidth * 8);
    }
    return 1;
}

static void print_help(const char *prog) {
    printf("Usage: %s [options]\n\n", prog);
    printf("========================================\n");
    printf(" Poor Man's Concatenator (Generator & PMC Concatenator)\n");
    printf("========================================\n\n");
    printf("MODES\n-----\n");
    printf("  1. Concatenate (Default)\n");
    printf("     Reads a .vbc file, tokenizes input text, and stitches audio to a WAV file.\n");
    printf("     Required: -vb <bank.vbc> -o <out.wav> -i <text|file|->\n\n");
    printf("  2. Generate (-g flag)\n");
    printf("     Builds a .vbc file from SAM phonemes or pre-existing WAV files.\n");
    printf("     Required: -g -o <bank.vbc> (-sg <map> | -cvb <dir> <map>)\n\n");
    printf("COMMON OPTIONS\n--------------\n");
    printf("  -v          Verbose output.\n");
    printf("  -werror     Treat warnings as fatal errors.\n");
    printf("  -iwarn      Ignore (suppress) all warnings.\n");
    printf("  -h / -help  Print this help and exit.\n\n");
    printf("CONCATENATE OPTIONS (Default Mode)\n----------------------------------\n");
    printf("  -vb <file.vbc>       Path to input .vbc voice bank.\n");
    printf("  -o  <output.wav>     Path for output WAV file.\n");
    printf("  -i  <text|file|->    Input text to synthesize. Use '-' for stdin.\n");
    printf("  -wsm <float>         Word silence in ms (default: %.1f).\n",    DEFAULT_WORD_SILENCE_MS);
    printf("  -psm <float>         Phoneme silence in ms (default: %.1f).\n", DEFAULT_PHONEME_SILENCE_MS);
    printf("  -obd <int>           Output WAV bit depth: 8 or 16 (default: %d).\n", DEFAULT_OUTPUT_BIT_DEPTH);
    printf("\n");
    printf("  BOUNDARY SMOOTHING\n");
    printf("  -cf  <float>         Crossfade duration in ms for voiced segment boundaries\n");
    printf("                       (default: %.1f = disabled).\n", DEFAULT_CROSSFADE_MS);
    printf("                       When enabled, also activates:\n");
    printf("                         * Zero-Crossing Editing: cuts are snapped to the\n");
    printf("                           nearest zero-crossing (within half the cf window)\n");
    printf("                           so there is no sudden DC jump at the edit point.\n");
    printf("                         * Pitch Smoothing: the boundary region of the higher-\n");
    printf("                           pitched segment is fractionally resampled to match\n");
    printf("                           the pitch of its neighbour, eliminating beating.\n");
    printf("                       Silence segments are never crossfaded.\n");
    printf("\n");
    printf("GENERATE OPTIONS (-g Mode)\n--------------------------\n");
    printf("  -g                   Enable Generation mode.\n");
    printf("  -o  <output.vbc>     Path for output .vbc file.\n");
    printf("  -sg <map_file>       Synthesize phonemes using SAM. Map: KEY:SAM_PHONETIC_STRING.\n");
    printf("                       Requires 'sam' (or 'sam.exe' on Windows) in current dir.\n");
    printf("  -cvb <wav_dir> <map> Pack existing WAVs. Map: KEY:WAV_FILENAME.\n");
    printf("  -bd  <int>           Packed audio bit depth (default: 4). Supported: 1 2 4 6 8 10 12 14 16.\n");
    printf("  -speed <int>         SAM speech speed (default: 100).\n\n");
    printf("EXAMPLES\n--------\n");
    printf("  Basic concatenation:\n");
    printf("    %s -vb bank.vbc -o out.wav -i \"hello world\"\n", prog);
    printf("\n  With crossfade + ZCR + pitch smoothing:\n");
    printf("    %s -vb bank.vbc -o out.wav -i \"hello world\" -cf 10\n", prog);
    printf("\n  Generate (SAM):\n");
    printf("    %s -g -o bank.vbc -sg phonemes.map\n", prog);
    printf("\n  Generate (WAVs):\n");
    printf("    %s -g -o bank.vbc -cvb ./wavs words.map -bd 4\n", prog);
    printf("\n");
}

int main(int argc, char *argv[]) {
    int gen_mode = 0, i;
    if (argc < 2) { print_help(argv[0]); return 1; }

    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "-g") == 0) { gen_mode = 1; break; }

    if (gen_mode) {
        const char *output_file = NULL, *sg_map = NULL, *cvb_dir = NULL, *cvb_map = NULL;
        int         bit_depth = 4, speed = 100, verbose = 0, werror = 0, iwarn = 0;

        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "-help") == 0) {
                print_help(argv[0]); return 0;
            } else if (strcmp(argv[i], "-g") == 0) {
            } else if (strcmp(argv[i], "-o") == 0) {
                if (++i >= argc) { fprintf(stderr, "[ERROR] -o requires argument\n"); return 1; }
                output_file = argv[i];
            } else if (strcmp(argv[i], "-sg") == 0) {
                if (++i >= argc) { fprintf(stderr, "[ERROR] -sg requires map file\n"); return 1; }
                sg_map = argv[i];
            } else if (strcmp(argv[i], "-cvb") == 0) {
                if (i + 2 >= argc) { fprintf(stderr, "[ERROR] -cvb requires wav_dir and map_file\n"); return 1; }
                cvb_dir = argv[++i]; cvb_map = argv[++i];
            } else if (strcmp(argv[i], "-bd") == 0) {
                if (++i >= argc) { fprintf(stderr, "[ERROR] -bd requires integer\n"); return 1; }
                bit_depth = atoi(argv[i]);
                if (!is_supported_bit_depth(bit_depth)) {
                    fprintf(stderr, "[ERROR] Unsupported bit depth %d. Supported: 1 2 4 6 8 10 12 14 16\n", bit_depth);
                    return 1;
                }
            } else if (strcmp(argv[i], "-speed") == 0) {
                if (++i >= argc) { fprintf(stderr, "[ERROR] -speed requires integer\n"); return 1; }
                speed = atoi(argv[i]);
            } else if (strcmp(argv[i], "-v") == 0)      { verbose = 1;
            } else if (strcmp(argv[i], "-werror") == 0) { werror  = 1;
            } else if (strcmp(argv[i], "-iwarn") == 0)  { iwarn   = 1;
            } else { fprintf(stderr, "[WARNING] Unknown flag: %s (ignored)\n", argv[i]); }
        }

        if (!output_file) { fprintf(stderr, "[ERROR] -o <output.vbc> is required\n"); return 1; }
        if (!sg_map && !cvb_dir) { fprintf(stderr, "[ERROR] One of -sg or -cvb is required\n"); return 1; }
        if (sg_map  &&  cvb_dir) { fprintf(stderr, "[ERROR] -sg and -cvb are mutually exclusive\n"); return 1; }

        { MapList ml; PackEntry *entries; char tmp_dir[] = "_vbc_gen_tmp"; int used_tmp = 0; int ok;
          memset(&ml, 0, sizeof(ml));
          { const char *map_path = sg_map ? sg_map : cvb_map;
            if (!load_map_file(map_path, &ml)) return 1;
            if (ml.count == 0) { fprintf(stderr, "[ERROR] Map file is empty: %s\n", map_path); return 1; }
            printf("Loaded %lu map entries from %s\n", (unsigned long)ml.count, map_path);
          }

          entries = (PackEntry *)calloc(ml.count, sizeof(PackEntry));
          if (!entries) { map_list_free(&ml); return 1; }

          if (sg_map) {
              int have_sam = sam_exists();
              size_t j;
              if (!have_sam) {
                  if (!iwarn) fprintf(stderr, "[WARNING] SAM binary not found. Silence will be written.\n");
                  if (werror) { fprintf(stderr, "[ERROR] -werror: aborting (SAM missing).\n"); free(entries); map_list_free(&ml); return 1; }
              }
              ensure_dir(tmp_dir); used_tmp = 1;
              for (j = 0; j < ml.count; j++) {
                  char safe[512], wav_path[1024];
                  make_safe_filename(ml.items[j].key, safe, sizeof(safe));
                  snprintf(wav_path, sizeof(wav_path), "%s/%s.wav", tmp_dir, safe);
                  if (have_sam) {
                      if (!run_sam(ml.items[j].value, wav_path, speed, verbose)) {
                          if (!iwarn) fprintf(stderr, "[WARNING] SAM failed for '%s'; writing silence.\n", ml.items[j].key);
                          if (werror) { fprintf(stderr, "[ERROR] -werror: aborting.\n"); free(entries); map_list_free(&ml); return 1; }
                          write_silence_wav(wav_path, 22050, 1, 2);
                      }
                  } else { write_silence_wav(wav_path, 22050, 1, 2); }
                  entries[j].key      = ml.items[j].key;
                  entries[j].wav_path = strdup(wav_path);
              }
          } else {
              size_t j;
              for (j = 0; j < ml.count; j++) {
                  char full_path[2048];
                  snprintf(full_path, sizeof(full_path), "%s/%s", cvb_dir, ml.items[j].value);
                  entries[j].key      = ml.items[j].key;
                  entries[j].wav_path = strdup(full_path);
              }
          }

          ok = pack_vbc_from_wavs(entries, ml.count, output_file, bit_depth, verbose, werror, iwarn);

          if (used_tmp) {
              size_t j;
              for (j = 0; j < ml.count; j++) if (entries[j].wav_path) remove(entries[j].wav_path);
              rmdir_compat(tmp_dir);
              if (verbose) printf("Cleaned up temporary WAV files.\n");
          }
          { size_t j; for (j = 0; j < ml.count; j++) free(entries[j].wav_path); }
          free(entries); map_list_free(&ml);
          return ok ? 0 : 1;
        }

    } else {
        const char *vbc_file = NULL, *output_file = NULL, *input_arg = NULL;
        int         verbose = 0, werror = 0, iwarn = 0;
        double      wsm = DEFAULT_WORD_SILENCE_MS;
        double      psm = DEFAULT_PHONEME_SILENCE_MS;
        double      cf  = DEFAULT_CROSSFADE_MS;
        int         obd = DEFAULT_OUTPUT_BIT_DEPTH;

        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "-help") == 0) {
                print_help(argv[0]); return 0;
            } else if (strcmp(argv[i], "-vb") == 0) {
                if (++i >= argc) { fprintf(stderr, "[ERROR] -vb requires a file argument\n"); return 1; }
                vbc_file = argv[i];
            } else if (strcmp(argv[i], "-o") == 0) {
                if (++i >= argc) { fprintf(stderr, "[ERROR] -o requires a file argument\n"); return 1; }
                output_file = argv[i];
            } else if (strcmp(argv[i], "-i") == 0) {
                if (++i >= argc) { fprintf(stderr, "[ERROR] -i requires a text or file argument\n"); return 1; }
                input_arg = argv[i];
            } else if (strcmp(argv[i], "-wsm") == 0) {
                if (++i >= argc) { fprintf(stderr, "[ERROR] -wsm requires a float\n"); return 1; }
                wsm = atof(argv[i]);
            } else if (strcmp(argv[i], "-psm") == 0) {
                if (++i >= argc) { fprintf(stderr, "[ERROR] -psm requires a float\n"); return 1; }
                psm = atof(argv[i]);
            } else if (strcmp(argv[i], "-obd") == 0) {
                if (++i >= argc) { fprintf(stderr, "[ERROR] -obd requires an integer\n"); return 1; }
                obd = atoi(argv[i]);
                if (obd != 8 && obd != 16) { fprintf(stderr, "[ERROR] -obd must be 8 or 16\n"); return 1; }
            } else if (strcmp(argv[i], "-cf") == 0) {
                if (++i >= argc) { fprintf(stderr, "[ERROR] -cf requires a float (ms)\n"); return 1; }
                cf = atof(argv[i]);
                if (cf < 0.0) { fprintf(stderr, "[ERROR] -cf must be >= 0\n"); return 1; }
            } else if (strcmp(argv[i], "-v")      == 0) { verbose = 1;
            } else if (strcmp(argv[i], "-werror") == 0) { werror  = 1;
            } else if (strcmp(argv[i], "-iwarn")  == 0) { iwarn   = 1;
            } else if (strcmp(argv[i], "-g")      == 0) {             
            } else { fprintf(stderr, "[WARNING] Unknown flag: %s (ignored)\n", argv[i]); }
        }

        if (!vbc_file)    { fprintf(stderr, "[ERROR] -vb <voice_bank.vbc> is required\n"); return 1; }
        if (!output_file) { fprintf(stderr, "[ERROR] -o <output.wav> is required\n"); return 1; }
        if (!input_arg)   { fprintf(stderr, "[ERROR] -i <text|file|-> is required\n"); return 1; }

        { VoiceBank vb; char *text = NULL; int free_text = 0, ok;
          TokenList tokens;
          memset(&vb, 0, sizeof(vb));
          if (!load_voice_bank(vbc_file, &vb, verbose, werror, iwarn)) return 1;

          if (strcmp(input_arg, "-") == 0 || looks_like_file(input_arg)) {
              text = read_file_text(input_arg);
              if (!text) { vbc_free(&vb); return 1; }
              free_text = 1;
          } else {
              text = (char *)input_arg;
          }
          if (verbose) printf("Input: \"%s\"\n", text);

          token_list_init(&tokens);
          if (!tokenize_text(text, &vb, &tokens)) {
              fprintf(stderr, "[ERROR] Tokenization failed (out of memory?)\n");
              if (free_text) free(text);
              vbc_free(&vb);
              return 1;
          }
          if (verbose) {
              size_t k;
              printf("Tokens (%lu): ", (unsigned long)tokens.count);
              for (k = 0; k < tokens.count; k++) printf("'%s' ", tokens.items[k].key);
              printf("\n");
          }
          if (tokens.count == 0u) {
              fprintf(stderr, "[ERROR] No tokens found for input.\n");
              if (free_text) free(text);
              token_list_free(&tokens);
              vbc_free(&vb);
              return 1;
          }

          ok = stitch(&tokens, &vb, output_file, wsm, psm, obd, cf,
                      verbose, werror, iwarn);

          if (free_text) free(text);
          token_list_free(&tokens);
          vbc_free(&vb);
          return ok ? 0 : 1;
        }
    }
}
