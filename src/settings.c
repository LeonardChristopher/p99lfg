#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
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
        else if (strncmp(line, "voice_mic=", 10) == 0)
            snprintf(s->voice_mic, sizeof(s->voice_mic), "%s", line + 10);
        else if (strncmp(line, "voice_output=", 13) == 0)
            snprintf(s->voice_output, sizeof(s->voice_output), "%s", line + 13);
        else if (strncmp(line, "voice_ptt=", 10) == 0)
            s->voice_ptt = atoi(line + 10) == 1;
        else if (strncmp(line, "voice_ptt_key=", 14) == 0)
            s->voice_ptt_key = atoi(line + 14);
        else if (strncmp(line, "voice_ptt_mouse=", 16) == 0)
            s->voice_ptt_mouse = atoi(line + 16) == 1;
        else if (strncmp(line, "voice_ptt_button=", 17) == 0)
            s->voice_ptt_button = atoi(line + 17);
        else if (strncmp(line, "lfg_limit_minutes=", 18) == 0)
            s->lfg_limit_minutes = atoi(line + 18);
        else if (strncmp(line, "dark_mode=", 10) == 0)
            s->dark_mode = atoi(line + 10) == 1;
        else if (strncmp(line, "win_x=", 6) == 0)
            s->win_x = atoi(line + 6);
        else if (strncmp(line, "win_y=", 6) == 0)
            s->win_y = atoi(line + 6);
        else if (strncmp(line, "win_w=", 6) == 0)
            s->win_w = atoi(line + 6);
        else if (strncmp(line, "win_h=", 6) == 0)
            s->win_h = atoi(line + 6);
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
    fprintf(f, "voice_mic=%s\n", s->voice_mic);
    fprintf(f, "voice_output=%s\n", s->voice_output);
    fprintf(f, "voice_ptt=%d\n", s->voice_ptt ? 1 : 0);
    fprintf(f, "voice_ptt_key=%d\n", s->voice_ptt_key);
    fprintf(f, "voice_ptt_mouse=%d\n", s->voice_ptt_mouse ? 1 : 0);
    fprintf(f, "voice_ptt_button=%d\n", s->voice_ptt_button);
    fprintf(f, "lfg_limit_minutes=%d\n", s->lfg_limit_minutes);
    fprintf(f, "dark_mode=%d\n", s->dark_mode ? 1 : 0);
    fprintf(f, "win_x=%d\n", s->win_x);
    fprintf(f, "win_y=%d\n", s->win_y);
    fprintf(f, "win_w=%d\n", s->win_w);
    fprintf(f, "win_h=%d\n", s->win_h);
    for (int i = 0; i < s->nguild_channels; i++)
        fprintf(f, "guild_channel=%s\n", s->guild_channels[i]);
    fclose(f);
    return 0;
}
