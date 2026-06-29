/* speak.h — Piper TTS + ALSA playback interface
 *
 * Manages the full TTS pipeline: text sanitization, sentence splitting,
 * Piper synthesis, and ALSA playback with streaming overlap (sentence N+1
 * is synthesized while sentence N plays).
 *
 * Thread safety: edgai_speak() is not reentrant. Call edgai_speak_interrupt()
 * from another thread to cancel active playback before starting a new one.
 *
 * Memory tiers:
 *   2 GB — Piper context is freed after each call and reloaded on the next.
 *   4 GB+ — Piper context stays resident in the session for the session lifetime.
 */

#ifndef EDGAI_SPEAK_H
#define EDGAI_SPEAK_H

#include "edgai/edgai.h"

/* Initialize Piper TTS. Called once during edgai_init().
 * Loads the voice model at EDGAI_VOICE_MODEL env var or the default path
 * ~/.edgai/models/piper/en_US-lessac-medium.onnx.
 * Returns EDGAI_OK on success.
 * Returns EDGAI_ERR_VOICE_UNAVAILABLE if the model file is not found —
 * the engine continues in text-only mode, no crash. */
int edgai_speak_init(EdgaiSession *session);

/* Synthesize text and play it through ALSA.
 * Sanitizes the text, splits into sentences, and streams synthesis+playback.
 * Blocks until playback is complete or edgai_speak_interrupt() is called.
 * Returns EDGAI_OK or a negative EDGAI_ERR_* code. */
int edgai_speak(EdgaiSession *session, const char *text);

/* Interrupt active TTS playback. Non-blocking. Safe to call from any thread.
 * Sets an atomic flag checked every audio frame by the playback loop.
 * Returns immediately — does not wait for the playback thread to stop. */
void edgai_speak_interrupt(EdgaiSession *session);

/* Free Piper resources. Called during edgai_destroy(). Safe with NULL. */
void edgai_speak_destroy(EdgaiSession *session);

#endif /* EDGAI_SPEAK_H */
