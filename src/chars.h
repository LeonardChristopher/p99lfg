#ifndef P99LFG_CHARS_H
#define P99LFG_CHARS_H

/* Local character roster: the user's own P99 characters with an LFG flag
 * each. Persisted as a plain text file:
 *   name|class|level|lfg
 * e.g.  Orgtal|Bard|40|1
 */

#define CHARS_MAX 64
#define CHAR_NAME_MAX 32
#define CHAR_CLASS_MAX 24

typedef struct {
    char name[CHAR_NAME_MAX];
    char cls[CHAR_CLASS_MAX];
    int level;
    int lfg;
} Character;

typedef struct {
    Character chars[CHARS_MAX];
    int count;
    char path[512];
} CharList;

/* Load from path (file created on demand). Never returns NULL. */
CharList *chars_load(const char *path);
void chars_save(CharList *cl);

/* Add or update (by name). Returns 0 on success. */
int chars_upsert(CharList *cl, const char *name, const char *cls, int level);
int chars_remove(CharList *cl, const char *name);
int chars_set_lfg(CharList *cl, const char *name, int lfg);
int chars_toggle_lfg(CharList *cl, const char *name);

/* Find by name; NULL if absent. */
Character *chars_find(CharList *cl, const char *name);

void chars_free(CharList *cl);

#endif
