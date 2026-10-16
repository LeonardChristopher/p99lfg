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
} Settings;
/* Load from path (missing file = defaults, no error). */
void settings_load(Settings *s, const char *path);
/* Save to path. Returns 0 on success. */
int settings_save(const Settings *s, const char *path);
#endif
