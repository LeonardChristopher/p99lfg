#include "settings.h"

#include <stdio.h>
#include <string.h>

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r'))
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
    fclose(f);
    return 0;
}
