#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"

#include "player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

/* ── Ring buffer helpers ────────────────────────────────────────── */

/* Call with ring_mutex held */
static int ring_available_locked(player_t *p) {
    return RING_BUFFER_SIZE - p->ring_count;
}

static int ring_available(player_t *p) {
    pthread_mutex_lock(&p->ring_mutex);
    int n = ring_available_locked(p);
    pthread_mutex_unlock(&p->ring_mutex);
    return n;
}

static void ring_write(player_t *p, const short *data, int count) {
    pthread_mutex_lock(&p->ring_mutex);
    for (int i = 0; i < count; i++) {
        p->ring[p->ring_write] = data[i];
        p->ring_write = (p->ring_write + 1) % RING_BUFFER_SIZE;
    }
    p->ring_count += count;
    pthread_mutex_unlock(&p->ring_mutex);
}

static int ring_read_samples(player_t *p, short *out, int count) {
    pthread_mutex_lock(&p->ring_mutex);
    int avail = p->ring_count;
    if (count > avail) count = avail;
    for (int i = 0; i < count; i++) {
        out[i] = p->ring[p->ring_read];
        p->ring_read = (p->ring_read + 1) % RING_BUFFER_SIZE;
    }
    p->ring_count -= count;
    pthread_mutex_unlock(&p->ring_mutex);
    return count;
}

/* ── SDL audio callback ────────────────────────────────────────── */

static void audio_callback(void *userdata, Uint8 *stream, int len) {
    player_t *p = (player_t *)userdata;
    int samples_needed = len / (int)sizeof(short);
    int got = ring_read_samples(p, (short *)stream, samples_needed);

    /* Apply volume scaling */
    int vol = atomic_load(&p->volume);
    if (vol < 100) {
        short *buf = (short *)stream;
        for (int i = 0; i < got; i++) {
            buf[i] = (short)((int)buf[i] * vol / 100);
        }
    }

    /* Fill remainder with silence */
    if (got < samples_needed) {
        memset(stream + got * (int)sizeof(short), 0,
               (samples_needed - got) * (int)sizeof(short));

        /* If decode is finished and buffer drained, signal track done */
        if (atomic_load(&p->decode_finished)) {
            pthread_mutex_lock(&p->ring_mutex);
            int empty = (p->ring_count == 0);
            pthread_mutex_unlock(&p->ring_mutex);
            if (empty)
                atomic_store(&p->track_done, 1);
        }
    }
}

/* ── Mono-to-stereo and resample helpers ────────────────────────── */

static void mono_to_stereo(const short *mono, short *stereo, int frames) {
    for (int i = 0; i < frames; i++) {
        stereo[i * 2]     = mono[i];
        stereo[i * 2 + 1] = mono[i];
    }
}

/* Simple linear resampling (for non-44100 sources) */
static int resample(const short *in, int in_frames, int in_rate,
                    short *out, int out_max, int out_rate, int channels) {
    if (in_rate == out_rate) {
        int copy = (in_frames < out_max) ? in_frames : out_max;
        memcpy(out, in, copy * channels * (int)sizeof(short));
        return copy;
    }

    double ratio = (double)out_rate / (double)in_rate;
    int out_frames = (int)(in_frames * ratio);
    if (out_frames > out_max) out_frames = out_max;

    for (int i = 0; i < out_frames; i++) {
        double src_pos = (double)i / ratio;
        int idx = (int)src_pos;
        double frac = src_pos - idx;
        if (idx >= in_frames - 1) idx = in_frames - 2;
        if (idx < 0) idx = 0;

        for (int c = 0; c < channels; c++) {
            double s0 = in[idx * channels + c];
            double s1 = in[(idx + 1) * channels + c];
            out[i * channels + c] = (short)(s0 + frac * (s1 - s0));
        }
    }
    return out_frames;
}

/* ── MP3 decode thread ─────────────────────────────────────────── */

static void *mp3_decode_thread_func(void *arg) {
    player_t *p = (player_t *)arg;
    mp3dec_ex_t *dec = (mp3dec_ex_t *)p->decoder;

    short decode_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];
    short stereo_buf[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
    short resample_buf[MINIMP3_MAX_SAMPLES_PER_FRAME * 4];

    while (atomic_load(&p->decode_running)) {
        if (ring_available(p) < MINIMP3_MAX_SAMPLES_PER_FRAME * 4) {
            usleep(10000);
            continue;
        }

        size_t samples_read = mp3dec_ex_read(dec, decode_buf,
                                              MINIMP3_MAX_SAMPLES_PER_FRAME);
        if (samples_read == 0) {
            atomic_store(&p->decode_finished, 1);
            break;
        }

        int frames = (int)samples_read / p->channels;
        short *src = decode_buf;
        int src_frames = frames;

        if (p->channels == 1) {
            mono_to_stereo(decode_buf, stereo_buf, frames);
            src = stereo_buf;
        }

        short *final_buf = src;
        int final_frames = src_frames;
        if (p->sample_rate != PLAYER_SAMPLE_RATE) {
            final_frames = resample(src, src_frames, p->sample_rate,
                                    resample_buf,
                                    MINIMP3_MAX_SAMPLES_PER_FRAME * 2,
                                    PLAYER_SAMPLE_RATE, PLAYER_CHANNELS);
            final_buf = resample_buf;
        }

        ring_write(p, final_buf, final_frames * PLAYER_CHANNELS);
    }

    return NULL;
}

/* ── WAV decode thread ─────────────────────────────────────────── */

static void *wav_decode_thread_func(void *arg) {
    player_t *p = (player_t *)arg;

    /* WAV data is already fully decoded into wav_data. Feed it into
       the ring buffer in chunks. */
    while (atomic_load(&p->decode_running)) {
        int pos = atomic_load(&p->wav_position);
        if (pos >= p->wav_frames) {
            atomic_store(&p->decode_finished, 1);
            break;
        }

        if (ring_available(p) < PLAYER_CHANNELS * 1024) {
            usleep(10000);
            continue;
        }

        int frames_left = p->wav_frames - pos;
        int chunk = (frames_left < 1024) ? frames_left : 1024;
        int samples = chunk * PLAYER_CHANNELS;

        ring_write(p, p->wav_data + pos * PLAYER_CHANNELS, samples);
        atomic_store(&p->wav_position, pos + chunk);
    }

    return NULL;
}

/* ── Format detection ──────────────────────────────────────────── */

static int str_ends_with_ci(const char *s, const char *suffix) {
    size_t slen = strlen(s);
    size_t xlen = strlen(suffix);
    if (xlen > slen) return 0;
    for (size_t i = 0; i < xlen; i++) {
        if (tolower((unsigned char)s[slen - xlen + i]) !=
            tolower((unsigned char)suffix[i]))
            return 0;
    }
    return 1;
}

static player_format_t detect_format(const char *path) {
    if (str_ends_with_ci(path, ".mp3")) return PLAYER_FORMAT_MP3;
    if (str_ends_with_ci(path, ".wav")) return PLAYER_FORMAT_WAV;
    return PLAYER_FORMAT_UNKNOWN;
}

/* ── WAV loading ──────────────────────────────────────────────── */

static int load_wav(player_t *p, const char *path) {
    SDL_AudioSpec wav_spec;
    Uint8 *wav_buf = NULL;
    Uint32 wav_len = 0;

    if (!SDL_LoadWAV(path, &wav_spec, &wav_buf, &wav_len)) {
        fprintf(stderr, "menulody: SDL_LoadWAV failed: %s\n", SDL_GetError());
        return -1;
    }

    /* Convert to our target format (S16, stereo, 44100Hz) */
    SDL_AudioCVT cvt;
    int ret = SDL_BuildAudioCVT(&cvt,
        wav_spec.format, wav_spec.channels, wav_spec.freq,
        AUDIO_S16SYS, PLAYER_CHANNELS, PLAYER_SAMPLE_RATE);

    if (ret < 0) {
        fprintf(stderr, "menulody: SDL_BuildAudioCVT failed: %s\n", SDL_GetError());
        SDL_FreeWAV(wav_buf);
        return -1;
    }

    if (ret > 0) {
        /* Conversion needed */
        cvt.len = (int)wav_len;
        cvt.buf = malloc(cvt.len * cvt.len_mult);
        if (!cvt.buf) {
            SDL_FreeWAV(wav_buf);
            return -1;
        }
        memcpy(cvt.buf, wav_buf, wav_len);
        SDL_FreeWAV(wav_buf);

        if (SDL_ConvertAudio(&cvt) < 0) {
            fprintf(stderr, "menulody: SDL_ConvertAudio failed: %s\n", SDL_GetError());
            free(cvt.buf);
            return -1;
        }

        p->wav_data = (short *)cvt.buf;
        p->wav_frames = cvt.len_cvt / (int)(sizeof(short) * PLAYER_CHANNELS);
    } else {
        /* No conversion needed — already in target format */
        p->wav_data = malloc(wav_len);
        if (!p->wav_data) {
            SDL_FreeWAV(wav_buf);
            return -1;
        }
        memcpy(p->wav_data, wav_buf, wav_len);
        SDL_FreeWAV(wav_buf);
        p->wav_frames = (int)(wav_len / (sizeof(short) * PLAYER_CHANNELS));
    }

    p->wav_position = 0;
    p->format = PLAYER_FORMAT_WAV;
    p->sample_rate = PLAYER_SAMPLE_RATE;
    p->channels = PLAYER_CHANNELS;
    return 0;
}

/* ── Open/close SDL audio device ─────────────────────────────── */

static int open_audio_device(player_t *p) {
    if (p->audio_open) return 0;

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq     = PLAYER_SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = PLAYER_CHANNELS;
    want.samples  = PLAYER_BUFFER_SIZE;
    want.callback = audio_callback;
    want.userdata = p;

    p->device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (p->device == 0) {
        fprintf(stderr, "menulody: SDL_OpenAudioDevice: %s\n", SDL_GetError());
        return -1;
    }

    p->audio_open = 1;
    return 0;
}

static void close_audio_device(player_t *p) {
    if (!p->audio_open) return;
    SDL_CloseAudioDevice(p->device);
    p->device = 0;
    p->audio_open = 0;
}

/* ── Public API ─────────────────────────────────────────────────── */

int player_init(player_t *p) {
    memset(p, 0, sizeof(*p));
    atomic_store(&p->volume, 80); /* default volume */
    p->ring = calloc(RING_BUFFER_SIZE, sizeof(short));
    if (!p->ring) return -1;
    pthread_mutex_init(&p->ring_mutex, NULL);

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "menulody: SDL_Init(AUDIO): %s\n", SDL_GetError());
        return -1;
    }

    return 0;
}

int player_open(player_t *p, const char *path) {
    player_close(p);

    player_format_t fmt = detect_format(path);
    if (fmt == PLAYER_FORMAT_UNKNOWN) {
        fprintf(stderr, "menulody: unsupported format: %s\n", path);
        return -1;
    }

    if (fmt == PLAYER_FORMAT_MP3) {
        mp3dec_ex_t *dec = calloc(1, sizeof(mp3dec_ex_t));
        if (!dec) return -1;

        if (mp3dec_ex_open(dec, path, MP3D_SEEK_TO_SAMPLE)) {
            fprintf(stderr, "menulody: cannot open %s\n", path);
            free(dec);
            return -1;
        }

        p->decoder = dec;
        p->format = PLAYER_FORMAT_MP3;
        p->channels = dec->info.channels;
        p->sample_rate = dec->info.hz;
    } else {
        if (load_wav(p, path) < 0) return -1;
    }

    /* Reset ring buffer */
    pthread_mutex_lock(&p->ring_mutex);
    p->ring_read  = 0;
    p->ring_write = 0;
    p->ring_count = 0;
    pthread_mutex_unlock(&p->ring_mutex);
    atomic_store(&p->decode_finished, 0);
    atomic_store(&p->track_done, 0);

    /* Open audio device */
    if (open_audio_device(p) < 0) {
        if (p->decoder) {
            mp3dec_ex_close((mp3dec_ex_t *)p->decoder);
            free(p->decoder);
            p->decoder = NULL;
        }
        free(p->wav_data);
        p->wav_data = NULL;
        return -1;
    }

    /* Start decode thread */
    atomic_store(&p->decode_running, 1);
    void *(*thread_func)(void *) = (fmt == PLAYER_FORMAT_MP3)
        ? mp3_decode_thread_func
        : wav_decode_thread_func;

    if (pthread_create(&p->decode_thread, NULL, thread_func, p) != 0) {
        perror("menulody: pthread_create");
        close_audio_device(p);
        if (p->decoder) {
            mp3dec_ex_close((mp3dec_ex_t *)p->decoder);
            free(p->decoder);
            p->decoder = NULL;
        }
        free(p->wav_data);
        p->wav_data = NULL;
        return -1;
    }

    SDL_PauseAudioDevice(p->device, 0);
    atomic_store(&p->playing, 1);
    return 0;
}

void player_close(player_t *p) {
    if (atomic_load(&p->decode_running)) {
        atomic_store(&p->decode_running, 0);
        pthread_join(p->decode_thread, NULL);
    }

    close_audio_device(p);

    if (p->decoder) {
        mp3dec_ex_close((mp3dec_ex_t *)p->decoder);
        free(p->decoder);
        p->decoder = NULL;
    }

    free(p->wav_data);
    p->wav_data = NULL;
    p->wav_frames = 0;
    atomic_store(&p->wav_position, 0);

    p->format = PLAYER_FORMAT_UNKNOWN;
    atomic_store(&p->playing, 0);
    atomic_store(&p->decode_finished, 0);
    atomic_store(&p->track_done, 0);

    pthread_mutex_lock(&p->ring_mutex);
    p->ring_read  = 0;
    p->ring_write = 0;
    p->ring_count = 0;
    pthread_mutex_unlock(&p->ring_mutex);
}

void player_pause(player_t *p) {
    if (!atomic_load(&p->playing)) return;
    close_audio_device(p);
    atomic_store(&p->playing, 0);
}

int player_resume(player_t *p) {
    if (atomic_load(&p->playing)) return 0;
    if (!p->decoder && !p->wav_data) return -1;

    if (open_audio_device(p) < 0) return -1;
    SDL_PauseAudioDevice(p->device, 0);
    atomic_store(&p->playing, 1);
    return 0;
}

int player_track_done(const player_t *p) {
    return atomic_load(&((player_t *)p)->track_done);
}

void player_set_volume(player_t *p, int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    atomic_store(&p->volume, vol);
}

void player_destroy(player_t *p) {
    player_close(p);
    free(p->ring);
    p->ring = NULL;
    pthread_mutex_destroy(&p->ring_mutex);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
