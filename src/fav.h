#ifndef P99LFG_FAV_H
#define P99LFG_FAV_H

#define FAV_NAME_MAX 64

typedef struct FavList FavList;

/* Load favourites from a plain newline-separated file (created on demand). */
FavList *fav_load(const char *path);

int fav_has(const FavList *f, const char *name);
/* Add if absent, remove if present. Persists. Returns 0 on success. */
int fav_toggle(FavList *f, const char *name);
int fav_count(const FavList *f);
const char *fav_get(const FavList *f, int i);
void fav_free(FavList *f);

#endif
