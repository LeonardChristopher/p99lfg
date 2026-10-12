#ifndef P99LFG_CORE_H
#define P99LFG_CORE_H

#include <stddef.h>
#include <time.h>

#define LFG_NAME_MAX 64
#define LFG_ZONE_MAX 64
#define LFG_DETAIL_MAX 96
#define LFG_PATH_MAX 512

enum { LFG_SRC_LOG = 0, LFG_SRC_MANUAL = 1 };

enum {
    LFG_LINE_NONE = 0,
    LFG_LINE_SELF_ON,
    LFG_LINE_SELF_OFF,
    LFG_LINE_WHO
};

typedef struct {
    char name[LFG_NAME_MAX];
    char zone[LFG_ZONE_MAX];
    char detail[LFG_DETAIL_MAX];
    int source;
    time_t last_seen;
} LfgEntry;

typedef struct {
    int kind;
    char name[LFG_NAME_MAX];
    char zone[LFG_ZONE_MAX];
    char detail[LFG_DETAIL_MAX];
} LfgLine;

int lfg_parse_line(const char *line, LfgLine *out);
const char *lfg_name_from_filename(const char *filename);

typedef struct LfgState LfgState;

typedef void (*LfgDebugFn)(const char *msg);
void lfg_set_debug(LfgDebugFn fn);

LfgState *lfg_new(void);
void lfg_free(LfgState *s);
void lfg_set_dir(LfgState *s, const char *dir);
const char *lfg_get_dir(LfgState *s);
void lfg_rescan(LfgState *s);
void lfg_set_stale_minutes(LfgState *s, int mins);
int lfg_get_stale_minutes(LfgState *s);
void lfg_poll(LfgState *s);
int lfg_count(LfgState *s);
const LfgEntry *lfg_get(LfgState *s, int i);
void lfg_add_manual(LfgState *s, const char *name, const char *zone, const char *detail);
int lfg_remove(LfgState *s, const char *name);
void lfg_remove_manual_all(LfgState *s);

#endif
