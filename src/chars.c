#include "chars.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

CharList *chars_load(const char *path)
{
    CharList *cl = calloc(1, sizeof(CharList));
    snprintf(cl->path, sizeof(cl->path), "%s", path);
    FILE *f = fopen(path, "r");
    if (!f)
        return cl;
    char line[256];
    while (fgets(line, sizeof(line), f) && cl->count < CHARS_MAX) {
        char *nl = strpbrk(line, "\r\n");
        if (nl)
            *nl = '\0';
        if (!line[0])
            continue;
        Character *c = &cl->chars[cl->count];
        char lvl[16] = "0", lfg[8] = "0";
        if (sscanf(line, "%31[^|]|%23[^|]|%15[^|]|%7s",
                   c->name, c->cls, lvl, lfg) >= 3) {
            c->level = atoi(lvl);
            c->lfg = atoi(lfg);
            cl->count++;
        }
    }
    fclose(f);
    return cl;
}

void chars_save(CharList *cl)
{
    FILE *f = fopen(cl->path, "w");
    if (!f)
        return;
    for (int i = 0; i < cl->count; i++)
        fprintf(f, "%s|%s|%d|%d\n",
                cl->chars[i].name, cl->chars[i].cls,
                cl->chars[i].level, cl->chars[i].lfg);
    fclose(f);
}

Character *chars_find(CharList *cl, const char *name)
{
    for (int i = 0; i < cl->count; i++)
        if (strcmp(cl->chars[i].name, name) == 0)
            return &cl->chars[i];
    return NULL;
}

int chars_upsert(CharList *cl, const char *name, const char *cls, int level)
{
    Character *c = chars_find(cl, name);
    if (c) {
        snprintf(c->cls, CHAR_CLASS_MAX, "%s", cls);
        c->level = level;
    } else {
        if (cl->count >= CHARS_MAX)
            return -1;
        c = &cl->chars[cl->count++];
        snprintf(c->name, CHAR_NAME_MAX, "%s", name);
        snprintf(c->cls, CHAR_CLASS_MAX, "%s", cls);
        c->level = level;
        c->lfg = 0;
    }
    chars_save(cl);
    return 0;
}

int chars_remove(CharList *cl, const char *name)
{
    for (int i = 0; i < cl->count; i++) {
        if (strcmp(cl->chars[i].name, name) == 0) {
            memmove(&cl->chars[i], &cl->chars[i + 1],
                    (size_t)(cl->count - i - 1) * sizeof(Character));
            cl->count--;
            chars_save(cl);
            return 1;
        }
    }
    return 0;
}

int chars_set_lfg(CharList *cl, const char *name, int lfg)
{
    Character *c = chars_find(cl, name);
    if (!c)
        return 0;
    c->lfg = lfg;
    chars_save(cl);
    return 1;
}

int chars_toggle_lfg(CharList *cl, const char *name)
{
    Character *c = chars_find(cl, name);
    if (!c)
        return 0;
    c->lfg = !c->lfg;
    chars_save(cl);
    return 1;
}

void chars_free(CharList *cl)
{
    free(cl);
}
