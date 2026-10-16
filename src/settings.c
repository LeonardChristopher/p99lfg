#include "settings.h"
#include <stdio.h>
#include <string.h>
static void trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' '))
        s[--n] = '\0';
}
void settings_load(Settings *s, const char *path)
{
    memset(s, 0, sizeof(*s));
    snprintf(s->server, sizeof(s->server), "green");
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[600];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (strncmp(line, "logdir=", 7) == 0)
            snprintf(s->logdir, sizeof(s->logdir), "%s", line + 7);
        else if (strncmp(line, "server=", 7) == 0)
            snprintf(s->server, sizeof(s->server), "%s", line + 7);
        else if (strncmp(line, "name=", 5) == 0)
            snprintf(s->displayname, sizeof(s->displayname), "%s", line + 5);
        else if (strncmp(line, "guild_relay=", 12) == 0)
            snprintf(s->guild_relay, sizeof(s->guild_relay), "%s", line + 12);
        else if (strncmp(line, "guild_channel=", 14) == 0 &&
                 s->nguild_channels < GUILD_CHANNELS_MAX) {
            snprintf(s->guild_channels[s->nguild_channels],
                     sizeof(s->guild_channels[0]), "%s", line + 14);
            s->nguild_channels++;
        }
    }
    fclose(f);
}
int settings_save(const Settings *s, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "logdir=%s\n", s->logdir);
    fprintf(f, "server=%s\n", s->server);
    fprintf(f, "name=%s\n", s->displayname);
    fprintf(f, "guild_relay=%s\n", s->guild_relay);
    for (int i = 0; i < s->nguild_channels; i++)
        fprintf(f, "guild_channel=%s\n", s->guild_channels[i]);
    fclose(f);
    return 0;
}
