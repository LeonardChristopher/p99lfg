#ifndef P99LFG_VOICE_AUDIO_H
#define P99LFG_VOICE_AUDIO_H
/* Audio device layer for group voice. Pure logic (PTT gating, level
 * metering) is platform-independent and CI-testable; WinMM capture/
 * playback compiles only on Windows. Raw 16-bit mono PCM at 16kHz,
 * 20ms frames (320 samples) - Opus-ready framing, no codec yet. */
#include <stdint.h>
#include <stddef.h>

#define VOICE_AUDIO_RATE   16000
#define VOICE_AUDIO_FRAME_MS 20
#define VOICE_AUDIO_SAMPLES (VOICE_AUDIO_RATE / 1000 * VOICE_AUDIO_FRAME_MS) /* 320 */
#define VOICE_AUDIO_BYTES  (VOICE_AUDIO_SAMPLES * 2) /* 640 */

/* Push-to-talk decision for one capture frame: 1 = transmit, 0 = muted. */
int vptt_gate(int ptt_enabled, int ptt_key_down);

/* Peak absolute sample of a 16-bit PCM frame, scaled 0-100. */
int vptt_level(const int16_t *pcm, size_t samples);

#ifdef _WIN32
/* Open mic (capture) with WinMM. device_name NULL/empty = default.
 * frames_cb receives 20ms PCM frames on the AUDIO thread - callers
 * must only copy/queue. Returns 0 on success. */
typedef void (*VaFrameCb)(const int16_t *pcm, size_t samples, void *ud);
int va_capture_open(const char *device_name, VaFrameCb cb, void *ud);
void va_capture_close(void);

/* Queue a 20ms frame for playback (drops silently if no device). */
void va_play_frame(const int16_t *pcm, size_t samples);

/* Open the output device. device_name NULL/empty = default. */
int va_play_open(const char *device_name);
void va_play_close(void);
int va_play_running(void);
#endif

#endif
