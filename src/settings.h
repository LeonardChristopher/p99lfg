#ifndef P99LFG_SETTINGS_H
#define P99LFG_SETTINGS_H

/* Tiny settings file: key=value lines, stored next to the exe
 * (p99lfg.ini). Currently: logdir, server. */

typedef struct {
    char logdir[512];
    char server[32];
} Settings;

/* Load from path (missing file = defaults, no error). */
void settings_load(Settings *s, const char *path);

/* Save to path. Returns 0 on success. */
int settings_save(const Settings *s, const char *path);

#endif
