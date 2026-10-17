#ifndef P99LFG_SETTINGS_H
#define P99LFG_SETTINGS_H
/* Tiny settings file: key=value lines, stored next to the exe
 * (p99lfg.ini). Currently: logdir, server, guild relay. */
#define GUILD_CHANNELS_MAX 4
#define GUILD_CHANNEL_NAME 48
typedef struct {
    char logdir[512];
    char server[32];
    char displayname[64];
    /* guild integration: a guild can host its own relay endpoint on
     * its website (any Nostr relay, wss://...). When set, it is added
     * to the relay list at startup; guild channels below pass through
     * it like any other channel, with the guild site as a hop. */
    char guild_relay[192];
    /* channel tags (without the p99lfg-g- prefix) auto-joined at start */
    char guild_channels[GUILD_CHANNELS_MAX][GUILD_CHANNEL_NAME];
    int nguild_channels;
    /* voice options (Voice options dialog in the group window):
     * device names as WinMM reports them; empty = system default.
     * push-to-talk: 0 = voice-activated/always-on (once capture is
     * wired), 1 = hold the configured key to talk. */
    char voice_mic[160];
    char voice_output[160];
    int voice_ptt;            /* 1 = push-to-talk mode */
    int voice_ptt_key;        /* virtual-key code, e.g. VK_F1..VK_F12 */
    int voice_ptt_mouse;      /* 1 = a mouse button is the PTT trigger */
    int voice_ptt_button;    /* xbutton id if voice_ptt_mouse */
    /* LFG time limit: minutes a character stays LFG before the app
     * auto-toggles it off (and publishes lfg:false). 0 = no limit. */
    int lfg_limit_minutes;
} Settings;
/* Load from path (missing file = defaults, no error). */
void settings_load(Settings *s, const char *path);
/* Save to path. Returns 0 on success. */
int settings_save(const Settings *s, const char *path);
#endif
