/* Favourite players: a small persistent list of pubkeys (or character names)
 * kept next to the key file, shared by all platforms. */
#include "fav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAV_MAX 128

struct FavList {
    char names[FAV_MAX][FAV_NAME_MAX];
    int count;
    char path[512];
};

FavList *fav_load(const char *path)
{
    FavList *f = calloc(1, sizeof(FavList));
    if (!f)
        return NULL;
    snprintf(f->path, sizeof(f->path), "%s", path);
    FILE *fp = fopen(f->path, "r");
    if (!fp)
        return f;
    char line[FAV_NAME_MAX];
    while (f->count < FAV_MAX && fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0])
            snprintf(f->names[f->count++], FAV_NAME_MAX, "%s", line);
    }
    fclose(fp);
    return f;
}

static int fav_save(const FavList *f)
{
    FILE *fp = fopen(f->path, "w");
    if (!fp)
        return -1;
    for (int i = 0; i < f->count; i++)
        fprintf(fp, "%s\n", f->names[i]);
    fclose(fp);
    return 0;
}

int fav_has(const FavList *f, const char *name)
{
    for (int i = 0; i < f->count; i++)
        if (strcmp(f->names[i], name) == 0)
            return 1;
    return 0;
}

int fav_toggle(FavList *f, const char *name)
{
    if (fav_has(f, name)) {
        for (int i = 0; i < f->count; i++) {
            if (strcmp(f->names[i], name) == 0) {
                memmove(f->names[i], f->names[i + 1],
                        (size_t)(f->count - i - 1) * FAV_NAME_MAX);
                f->count--;
                break;
            }
        }
    } else {
        if (f->count >= FAV_MAX)
            return -1;
        snprintf(f->names[f->count++], FAV_NAME_MAX, "%s", name);
    }
    return fav_save(f);
}

int fav_count(const FavList *f)
{
    return f->count;
}

const char *fav_get(const FavList *f, int i)
{
    if (i < 0 || i >= f->count)
        return NULL;
    return f->names[i];
}

void fav_free(FavList *f)
{
    free(f);
}
