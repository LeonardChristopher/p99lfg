#include "voice_codec.h"
#include <opus.h>
#include <string.h>

int vcodec_open(VoiceCodec *c)
{
    memset(c, 0, sizeof(*c));
    int err = 0;
    c->enc = opus_encoder_create(VCODEC_RATE, VCODEC_CHANNELS,
                                 OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !c->enc)
        return -1;
    opus_encoder_ctl(c->enc, OPUS_SET_BITRATE(VCODEC_BITRATE));
    c->dec = opus_decoder_create(VCODEC_RATE, VCODEC_CHANNELS, &err);
    if (err != OPUS_OK || !c->dec) {
        opus_encoder_destroy(c->enc);
        c->enc = NULL;
        return -1;
    }
    return 0;
}

int vcodec_encode(VoiceCodec *c, const int16_t *pcm, int samples,
                  uint8_t *out, size_t outsz)
{
    if (!c->enc || !pcm || samples <= 0)
        return -1;
    opus_int32 n = opus_encode(c->enc, pcm, samples, out,
                               (opus_int32)(outsz > 127 ? 127 : outsz));
    return n > 0 ? (int)n : -1;
}

int vcodec_decode(VoiceCodec *c, const uint8_t *data, size_t len,
                  int16_t *pcm, size_t pcm_max)
{
    if (!c->dec || !data || len == 0)
        return -1;
    int n = opus_decode(c->dec, data, (opus_int32)len, pcm,
                        (opus_int32)pcm_max, 0);
    return n > 0 ? n : -1;
}

void vcodec_close(VoiceCodec *c)
{
    if (c->enc) {
        opus_encoder_destroy(c->enc);
        c->enc = NULL;
    }
    if (c->dec) {
        opus_decoder_destroy(c->dec);
        c->dec = NULL;
    }
}
