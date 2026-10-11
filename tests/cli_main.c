/* Testable headless harness for the core scanner. Not shipped. */
#include "core.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: cli <logdir>\n");
        return 2;
    }
    LfgState *s = lfg_new();
    lfg_set_dir(s, argv[1]);
    lfg_rescan(s);

    /* generate a fresh log so the staleness filter doesn't drop entries */
    char path[512];
    snprintf(path, sizeof(path), "%s/eqlog_Livetest_P1999Green.txt", argv[1]);
    FILE *f = fopen(path, "w");
    time_t now = time(NULL);
    time_t t = now - 60;
    char buf[64];
    strftime(buf, sizeof(buf), "%a %b %d %H:%M:%S %Y", localtime(&t));
    fprintf(f, "[%s] [40 Bard] Orgtal (Wood Elf) <Good Guys> ZONE: ecommons  LFG\n", buf);
    fprintf(f, "[%s] [60 Warlord] Pyroboooze (Wood Elf)  ZONE: ecommons  LFG\n", buf);
    t = now - 30;
    strftime(buf, sizeof(buf), "%a %b %d %H:%M:%S %Y", localtime(&t));
    fprintf(f, "[%s] You are now LFG.\n", buf);
    fclose(f);
    lfg_rescan(s);

    lfg_poll(s);

    lfg_add_manual(s, "Myalt", "eci", "52 Necro");

    lfg_poll(s);
    for (int i = 0; i < lfg_count(s); i++) {
        const LfgEntry *e = lfg_get(s, i);
        printf("%s src=%d zone=%s detail=%s\n", e->name, e->source, e->zone, e->detail);
    }
    remove(path);
    lfg_free(s);
    return 0;
}
