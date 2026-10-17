/* Audio device layer: pure PTT/level logic (portable) + WinMM
 * capture/playback (_WIN32 only). See voice_audio.h. */
#include "voice_audio.h"
#include <string.h>

int vptt_gate(int ptt_enabled, int ptt_key_down)
{
    if (!ptt_enabled)
        return 1; /* open mic */
    return ptt_key_down ? 1 : 0;
}

int vptt_level(const int16_t *pcm, size_t samples)
{
    int peak = 0;
    for (size_t i = 0; i < samples; i++) {
        int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v > peak)
            peak = v;
    }
    /* scale 32767 -> 100 */
    return (peak * 100) / 32767 > 100 ? 100 : (peak * 100) / 32767;
}

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>

static HWAVEIN g_win;
static HWAVEOUT g_wout;
static VaFrameCb g_cb;
static void *g_ud;
static int g_cap_open;
static int g_play_open;

/* four rotating capture buffers: WinMM fills, callback fires, we resubmit */
static WAVEHDR g_hdrs[4];

static void CALLBACK va_in_cb(HWAVEIN hwi, UINT uMsg, DWORD_PTR dwInstance,
                              DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    (void)hwi; (void)dwInstance; (void)dwParam2;
    if (uMsg != WIM_DATA)
        return;
    WAVEHDR *h = (WAVEHDR *)dwParam1;
    if (!h || !g_cb)
        return;
    size_t samples = h->dwBytesRecorded / 2;
    if (samples > 0)
        g_cb((const int16_t *)h->lpData, samples, g_ud);
    if (g_cap_open)
        waveInAddBuffer(hwi, h, sizeof(WAVEHDR));
}

int va_capture_open(const char *device_name, VaFrameCb cb, void *ud)
{
    if (g_cap_open)
        return 0;
    UINT dev = WAVE_MAPPER;
    if (device_name && device_name[0]) {
        UINT n = waveInGetNumDevs();
        WAVEINCAPSA caps;
        dev = (UINT)-1;
        for (UINT i = 0; i < n; i++)
            if (waveInGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR &&
                strcmp(caps.szPname, device_name) == 0) {
                dev = i;
                break;
            }
        if (dev == (UINT)-1)
            dev = WAVE_MAPPER; /* saved device vanished: fall back */
    }
    WAVEFORMATEX fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = VOICE_AUDIO_RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = VOICE_AUDIO_RATE * 2;
    if (waveInOpen(&g_win, dev, &fmt, (DWORD_PTR)va_in_cb, 0,
                   CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
        return -1;
    g_cb = cb;
    g_ud = ud;
    g_cap_open = 1;
    for (int i = 0; i < 4; i++) {
        memset(&g_hdrs[i], 0, sizeof(g_hdrs[i]));
        g_hdrs[i].lpData = (LPSTR)malloc(VOICE_AUDIO_BYTES);
        g_hdrs[i].dwBufferLength = VOICE_AUDIO_BYTES;
        waveInPrepareHeader(g_win, &g_hdrs[i], sizeof(WAVEHDR));
        waveInAddBuffer(g_win, &g_hdrs[i], sizeof(WAVEHDR));
    }
    waveInStart(g_win);
    return 0;
}

void va_capture_close(void)
{
    if (!g_cap_open)
        return;
    g_cap_open = 0;
    waveInStop(g_win);
    waveInReset(g_win);
    for (int i = 0; i < 4; i++) {
        if (g_hdrs[i].dwFlags & WHDR_PREPARED) {
            waveInUnprepareHeader(g_win, &g_hdrs[i], sizeof(WAVEHDR));
            free(g_hdrs[i].lpData);
            g_hdrs[i].lpData = NULL;
        }
    }
    waveInClose(g_win);
    g_win = NULL;
}

/* playback: jitter-cushioned queue. Frames arrive in bursts (network
 * jitter + thread timing); a thin 3-buffer ring starved the device
 * and the resulting dropouts are what made speech sound robotic.
 * We queue up to a ~140ms cushion and let the device drain it
 * smoothly; overflow discards oldest (never blocks the voice thread). */
#define VA_PLAY_QUEUE 7 /* 7 x 20ms = 140ms cushion */
static WAVEHDR g_ohdrs[VA_PLAY_QUEUE];
static int g_obuf;   /* next slot to submit */
static int g_oplayed;/* slots currently in the device */

int va_play_open(const char *device_name)
{
    if (g_play_open)
        return 0;
    UINT dev = WAVE_MAPPER;
    if (device_name && device_name[0]) {
        UINT n = waveOutGetNumDevs();
        WAVEOUTCAPSA caps;
        dev = (UINT)-1;
        for (UINT i = 0; i < n; i++)
            if (waveOutGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR &&
                strcmp(caps.szPname, device_name) == 0) {
                dev = i;
                break;
            }
        if (dev == (UINT)-1)
            dev = WAVE_MAPPER;
    }
    WAVEFORMATEX fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = VOICE_AUDIO_RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = VOICE_AUDIO_RATE * 2;
    if (waveOutOpen(&g_wout, dev, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        return -1;
    g_play_open = 1;
    g_obuf = 0;
    g_oplayed = 0;
    for (int i = 0; i < VA_PLAY_QUEUE; i++)
        memset(&g_ohdrs[i], 0, sizeof(g_ohdrs[i]));
    return 0;
}

void va_play_frame(const int16_t *pcm, size_t samples)
{
    if (!g_play_open || !pcm || !samples)
        return;
    size_t bytes = samples * 2;
    WAVEHDR *h = &g_ohdrs[g_obuf];
    if (h->dwFlags & WHDR_PREPARED) {
        if (!(h->dwFlags & WHDR_DONE)) {
            /* cushion full: drop this frame (better one lost frame
             * than a stalled audio thread) */
            return;
        }
        waveOutUnprepareHeader(g_wout, h, sizeof(WAVEHDR));
        if (h->lpData) {
            free(h->lpData);
            h->lpData = NULL;
        }
    }
    h->lpData = (LPSTR)malloc(bytes);
    memcpy(h->lpData, pcm, bytes);
    h->dwBufferLength = (DWORD)bytes;
    waveOutPrepareHeader(g_wout, h, sizeof(WAVEHDR));
    waveOutWrite(g_wout, h, sizeof(WAVEHDR));
    g_obuf = (g_obuf + 1) % VA_PLAY_QUEUE;
}

void va_play_close(void)
{
    if (!g_play_open)
        return;
    waveOutReset(g_wout);
    for (int i = 0; i < VA_PLAY_QUEUE; i++) {
        if (g_ohdrs[i].dwFlags & WHDR_PREPARED) {
            waveOutUnprepareHeader(g_wout, &g_ohdrs[i], sizeof(WAVEHDR));
            free(g_ohdrs[i].lpData);
            g_ohdrs[i].lpData = NULL;
            g_ohdrs[i].dwFlags = 0;
        }
    }
    waveOutClose(g_wout);
    g_wout = NULL;
    g_play_open = 0;
}

int va_play_running(void)
{
    return g_play_open;
}
#endif
