/* speak.c — Piper TTS + ALSA playback for EduOS libedgai
 *
 * Pipeline per edgai_speak() call:
 *   1. Sanitize LLM text (strip LaTeX, Markdown, URLs)
 *   2. Split into sentences at .?! boundaries (force-split at 200 chars)
 *   3. For each sentence: synthesize with Piper, play via ALSA S16_LE 22050 Hz
 *   4. Barge-in: check session->speak_interrupt before each ALSA write
 *
 * On 2 GB tier (is_mobile): load Piper → speak → free Piper each call.
 * On 4 GB+ tier: Piper context stays resident in session->piper_ctx.
 *
 * Author: Edgai Contributors
 * License: GPL v3
 */

/* 1. System includes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* 2. Third-party includes */
#ifdef EDGAI_VOICE_ENABLED
#include <alsa/asoundlib.h>
#include "piper/libpiper/piper.h"
#endif

/* 3. Local includes */
#include "edgai/edgai.h"
#include "speak.h"
#include "sanitize.h"

/* 4. Module-private constants */
#define EDGAI_PLAYBACK_RATE      22050   /* Hz — Piper S16_LE output rate  */
#define EDGAI_PLAYBACK_CHANNELS      1   /* mono                            */
#define EDGAI_SENTENCE_MAX_CHARS   200   /* force-split threshold (chars)   */
#define EDGAI_SENTENCE_MIN_CHARS     3   /* skip strings shorter than this  */
#define EDGAI_SANITIZE_BUF       16384   /* sanitize output buffer size     */
#define EDGAI_SENTENCE_BUF_MAX    4096   /* max bytes per sentence          */

/* 5. Module-private types — none (Piper context is void* in EdgaiSession) */

/* 6. Static function declarations */
#ifdef EDGAI_VOICE_ENABLED
static const char *resolve_voice_model(void);
static int         play_pcm(EdgaiSession *session,
                             const int16_t *samples, size_t n_samples);
static int         speak_sentence(EdgaiSession *session, const char *sentence);
#endif
static int next_sentence(const char *text, int offset,
                          char *out_buf, size_t out_buf_len);

/* ── Public API ─────────────────────────────────────────────────────────── */

int edgai_speak_init(EdgaiSession *session)
{
    if (!session)
        return EDGAI_ERR_INTERNAL;

#ifndef EDGAI_VOICE_ENABLED
    session->voice_enabled = 0;
    return EDGAI_OK;
#else
    const char *model_path = resolve_voice_model();
    if (!model_path) {
        fprintf(stderr, "edgai: Piper voice model not found — text-only mode\n"
                        "       Set EDGAI_VOICE_MODEL or download to "
                        "~/.edgai/models/piper/en_US-lessac-medium.onnx\n");
        session->voice_enabled = 0;
        return EDGAI_ERR_VOICE_UNAVAILABLE;
    }

    /* On 4 GB+ tier, load Piper once and keep it resident */
    if (!session->is_mobile) {
        PiperContext *ctx = piper_context_create(model_path);
        if (!ctx) {
            fprintf(stderr, "edgai: failed to load Piper model: %s\n", model_path);
            session->voice_enabled = 0;
            return EDGAI_ERR_VOICE_UNAVAILABLE;
        }
        session->piper_ctx = ctx;
    }
    /* On 2 GB mobile tier, piper_ctx stays NULL and is loaded per-call */
    session->voice_enabled = 1;
    return EDGAI_OK;
#endif
}

int edgai_speak(EdgaiSession *session, const char *text)
{
    if (!session || !text)
        return EDGAI_ERR_INTERNAL;

    if (!session->voice_enabled)
        return EDGAI_ERR_VOICE_UNAVAILABLE;

    /* Reset barge-in flag at the start of a new utterance */
    session->speak_interrupt = 0;

#ifndef EDGAI_VOICE_ENABLED
    return EDGAI_ERR_VOICE_UNAVAILABLE;
#else
    /* ── 2 GB tier: load Piper just for this call ────────────────────── */
    int loaded_for_call = 0;
    if (session->is_mobile && !session->piper_ctx) {
        const char *model_path = resolve_voice_model();
        if (!model_path)
            return EDGAI_ERR_VOICE_UNAVAILABLE;
        session->piper_ctx = piper_context_create(model_path);
        if (!session->piper_ctx)
            return EDGAI_ERR_VOICE_UNAVAILABLE;
        loaded_for_call = 1;
    }

    /* ── Sanitize ────────────────────────────────────────────────────── */
    char clean[EDGAI_SANITIZE_BUF];
    int clean_len = edgai_sanitize_for_tts(text, clean, sizeof(clean));
    if (clean_len <= 0)
        goto done;

    /* ── Sentence-loop ───────────────────────────────────────────────── */
    char sentence[EDGAI_SENTENCE_BUF_MAX];
    int  offset = 0;
    while (offset < clean_len && !session->speak_interrupt) {
        int consumed = next_sentence(clean, offset, sentence, sizeof(sentence));
        if (consumed <= 0)
            break;
        offset += consumed;

        if ((int)strlen(sentence) < EDGAI_SENTENCE_MIN_CHARS)
            continue;

        int err = speak_sentence(session, sentence);
        if (err != EDGAI_OK)
            break;
    }

done:
    /* ── 2 GB tier: free Piper immediately ───────────────────────────── */
    if (loaded_for_call && session->piper_ctx) {
        piper_context_free((PiperContext *)session->piper_ctx);
        session->piper_ctx = NULL;
    }
    return EDGAI_OK;
#endif /* EDGAI_VOICE_ENABLED */
}

void edgai_speak_interrupt(EdgaiSession *session)
{
    if (session)
        session->speak_interrupt = 1;
}

void edgai_speak_destroy(EdgaiSession *session)
{
    if (!session)
        return;
#ifdef EDGAI_VOICE_ENABLED
    if (session->piper_ctx) {
        piper_context_free((PiperContext *)session->piper_ctx);
        session->piper_ctx = NULL;
    }
#endif
}

/* ── Static helpers ─────────────────────────────────────────────────────── */

/*
 * Extract the next sentence from text+offset into out_buf.
 * Sentence boundaries: . ? ! followed by whitespace or end of string.
 * Force-split at EDGAI_SENTENCE_MAX_CHARS if no boundary found:
 *   1. Last comma before the limit
 *   2. Hard cut at the limit
 * Returns bytes consumed from text (advance offset by this amount),
 * or 0 when there is nothing left to read.
 */
static int next_sentence(const char *text, int offset,
                          char *out_buf, size_t out_buf_len)
{
    const char *start = text + offset;
    if (*start == '\0')
        return 0;

    /* Skip leading whitespace */
    while (*start == ' ' || *start == '\t' || *start == '\n') start++;
    if (*start == '\0')
        return (int)(start - (text + offset)) + 1;

    const char *p           = start;
    const char *last_comma  = NULL;
    int         char_count  = 0;

    while (*p != '\0') {
        char_count++;

        /* Sentence-ending punctuation */
        if ((*p == '.' || *p == '?' || *p == '!') &&
            (p[1] == ' ' || p[1] == '\n' || p[1] == '\0')) {
            p++; /* include the punctuation */
            break;
        }

        if (*p == ',')
            last_comma = p;

        /* Force-split at max chars */
        if (char_count >= EDGAI_SENTENCE_MAX_CHARS) {
            if (last_comma)
                p = last_comma + 1;
            break;
        }
        p++;
    }

    size_t len = (size_t)(p - start);
    if (len == 0)
        return 1; /* advance past one odd character */

    if (len >= out_buf_len)
        len = out_buf_len - 1;

    memcpy(out_buf, start, len);
    out_buf[len] = '\0';

    /* Strip trailing whitespace */
    while (len > 0 && (out_buf[len - 1] == ' ' || out_buf[len - 1] == '\n'))
        out_buf[--len] = '\0';

    return (int)(p - (text + offset));
}

#ifdef EDGAI_VOICE_ENABLED

/* Resolve the Piper voice model path.
 * Search order: EDGAI_VOICE_MODEL env var → ~/.edgai/models/piper/ default. */
static const char *resolve_voice_model(void)
{
    const char *env = getenv("EDGAI_VOICE_MODEL");
    if (env && env[0] != '\0') {
        FILE *f = fopen(env, "rb");
        if (f) { fclose(f); return env; }
        fprintf(stderr, "edgai: EDGAI_VOICE_MODEL='%s' not found\n", env);
        return NULL;
    }

    static char path[4096];
    const char *home = getenv("HOME");
    if (home) {
        snprintf(path, sizeof(path),
                 "%s/.edgai/models/piper/en_US-lessac-medium.onnx", home);
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); return path; }
    }
    return NULL;
}

/*
 * Play raw S16_LE mono samples at EDGAI_PLAYBACK_RATE Hz via ALSA.
 * Opens the playback device, writes all samples, then drains and closes.
 * Checks session->speak_interrupt between ALSA periods for barge-in.
 */
static int play_pcm(EdgaiSession *session,
                     const int16_t *samples, size_t n_samples)
{
    const char *device = getenv("EDGAI_ALSA_DEVICE");
    if (!device || device[0] == '\0')
        device = "default";

    snd_pcm_t *pcm = NULL;
    int err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "edgai: cannot open playback device '%s': %s\n",
                device, snd_strerror(err));
        return EDGAI_ERR_AUDIO_BUSY;
    }

    err = snd_pcm_set_params(pcm,
                              SND_PCM_FORMAT_S16_LE,
                              SND_PCM_ACCESS_RW_INTERLEAVED,
                              EDGAI_PLAYBACK_CHANNELS,
                              EDGAI_PLAYBACK_RATE,
                              1,      /* allow resampling */
                              50000); /* 50 ms latency    */
    if (err < 0) {
        fprintf(stderr, "edgai: cannot set playback params: %s\n",
                snd_strerror(err));
        snd_pcm_close(pcm);
        return EDGAI_ERR_AUDIO_BUSY;
    }

    const int16_t       *ptr       = samples;
    snd_pcm_uframes_t    remaining = (snd_pcm_uframes_t)n_samples;

    while (remaining > 0 && !session->speak_interrupt) {
        snd_pcm_sframes_t written = snd_pcm_writei(pcm, ptr, remaining);
        if (written == -EPIPE) {
            snd_pcm_prepare(pcm);
            continue;
        }
        if (written < 0) {
            fprintf(stderr, "edgai: ALSA playback error: %s\n",
                    snd_strerror((int)written));
            break;
        }
        ptr       += written;
        remaining -= (snd_pcm_uframes_t)written;
    }

    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    return EDGAI_OK;
}

/* Synthesize one sentence with Piper and play it via ALSA. */
static int speak_sentence(EdgaiSession *session, const char *sentence)
{
    if (session->speak_interrupt)
        return EDGAI_OK;

    PiperContext *ctx = (PiperContext *)session->piper_ctx;
    if (!ctx)
        return EDGAI_ERR_VOICE_UNAVAILABLE;

    int16_t *samples   = NULL;
    size_t   n_samples = 0;

    int err = piper_synthesize(ctx, sentence, &samples, &n_samples);
    if (err != 0 || !samples) {
        fprintf(stderr, "edgai: Piper synthesis failed for: %.60s\n", sentence);
        return EDGAI_ERR_INTERNAL;
    }

    int ret = play_pcm(session, samples, n_samples);
    free(samples);
    return ret;
}

#endif /* EDGAI_VOICE_ENABLED */
