/* Testable headless harness for the core scanner. Not shipped. */
#include "core.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: cli <logdir>\n");
        return 2;
    }
    LfgState *s = lfg_new();
    lfg_set_dir(s, argv[1]);
    lfg_rescan(s);
    lfg_poll(s);

    lfg_add_manual(s, "Myalt", "eci", "52 Necro");

    lfg_poll(s);
    for (int i = 0; i < lfg_count(s); i++) {
        const LfgEntry *e = lfg_get(s, i);
        printf("%s src=%d zone=%s detail=%s\n", e->name, e->source, e->zone, e->detail);
    }
    lfg_free(s);
    return 0;
}
