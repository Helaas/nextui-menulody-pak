#ifndef MENULODY_PLAYER_H
#define MENULODY_PLAYER_H

/*
 * Audio player — decodes MP3 (minimp3) and WAV files, outputs via SDL2 audio.
 *
 * The player manages an SDL2 audio device and a decoding thread that feeds
 * PCM samples through a ring buffer to the audio callback.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <SDL2/SDL.h>

#define PLAYER_SAMPLE_RATE  44100
#define PLAYER_CHANNELS     2
#define PLAYER_BUFFER_SIZE  4096      /* SDL audio buffer size in samples */
#define RING_BUFFER_SIZE    (PLAYER_SAMPLE_RATE * PLAYER_CHANNELS * 4) /* ~4s worth */

typedef enum {
    PLAYER_FORMAT_UNKNOWN = 0,
    PLAYER_FORMAT_MP3,
    PLAYER_FORMAT_WAV,
} player_format_t;

typedef struct {
    /* SDL audio */
    SDL_AudioDeviceID   device;
    int                 audio_open;

    /* Decoder state */
    void               *decoder;      /* mp3dec_ex_t* for MP3, NULL for WAV */
    player_format_t     format;
    int                 sample_rate;
    int                 channels;

    /* WAV-specific state */
    short              *wav_data;      /* decoded WAV PCM (interleaved stereo) */
    int                 wav_frames;    /* total frames in wav_data */
    atomic_int          wav_position;  /* current read position in frames */

    /* Ring buffer: int16_t samples, interleaved stereo.
       ring_read, ring_write, and ring_count are all protected by ring_mutex. */
    short              *ring;
    int                 ring_read;
    int                 ring_write;
    int                 ring_count;
    pthread_mutex_t     ring_mutex;

    /* Decode thread */
    pthread_t           decode_thread;
    atomic_int          decode_running;
    atomic_int          decode_finished;   /* 1 when current file is fully decoded */

    /* Volume: 0-100 */
    atomic_int          volume;

    /* State flags */
    atomic_int          playing;
    atomic_int          track_done;        /* 1 when playback of current track completed */
} player_t;

/* Initialize the player (no audio device opened yet) */
int  player_init(player_t *p);

/* Open and start playing a file (MP3 or WAV). Opens SDL audio device if needed. */
int  player_open(player_t *p, const char *path);

/* Close current file and stop playback (closes SDL audio device) */
void player_close(player_t *p);

/* Pause audio output (keeps decoder state, closes audio device to release ALSA) */
void player_pause(player_t *p);

/* Resume audio output (reopens audio device) */
int  player_resume(player_t *p);

/* Check if the current track finished playing */
int  player_track_done(const player_t *p);

/* Set volume (0-100) */
void player_set_volume(player_t *p, int vol);

/* Destroy the player and free all resources */
void player_destroy(player_t *p);

#endif /* MENULODY_PLAYER_H */
