#ifndef P99LFG_VOICE_CODEC_H
#define P99LFG_VOICE_CODEC_H
/* Opus encode/decode for group voice. 16kHz mono, 20ms frames,
 * 24kbps VBR - ~3kB/s on the wire vs 32kB/s raw PCM. The UDP packet
 * payload becomes: opus-frame-bytes (encrypted as before). */
#include <stdint.h>
#include <stddef.h>

#define VCODEC_RATE      16000
#define VCODEC_CHANNELS  1
#define VCODEC_FRAME_MS 20
#define VCODEC_SAMPLES  (VCODEC_RATE / 1000 * VCODEC_FRAME_MS) /* 320 */
#define VCODEC_BITRATE  32000
#define VCODEC_MAX_BYTES 400 /* plenty for 20ms @ 24kbps */

typedef struct {
    void *enc; /* OpusEncoder* */
    void *dec; /* OpusDecoder* */
} VoiceCodec;

/* Create encoder+decoder. Returns 0 on success. */
int vcodec_open(VoiceCodec *c);

/* Encode one 20ms PCM frame. Returns bytes written (>=1) or -1. */
int vcodec_encode(VoiceCodec *c, const int16_t *pcm, int samples,
                  uint8_t *out, size_t outsz);

/* Decode an opus frame back to PCM. Returns samples decoded or -1. */
int vcodec_decode(VoiceCodec *c, const uint8_t *data, size_t len,
                  int16_t *pcm, size_t pcm_max);

void vcodec_close(VoiceCodec *c);
#endif
