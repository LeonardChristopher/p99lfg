#include "core.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static LfgDebugFn g_debug_fn;

void lfg_set_debug(LfgDebugFn fn) { g_debug_fn = fn; }

static void dbg(const char *fmt, ...)
{
    if (!g_debug_fn)
        return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_debug_fn(buf);
}

#define MAX_LOG_FILES 32
#define MAX_ENTRIES 512
#define LINE_BUF 8192

typedef struct {
    char path[LFG_PATH_MAX];
    char name[LFG_NAME_MAX];
    FILE *fp;
    long offset;
    int truncated;
} LogFile;

struct LfgState {
    char dir[LFG_PATH_MAX];
    LogFile logs[MAX_LOG_FILES];
    int nlogs;
    LfgEntry entries[MAX_ENTRIES];
    int nentries;
    int stale_minutes;
    int prev_in_game;
};

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t'))
        s[--n] = '\0';
    size_t start = 0;
    while (s[start] == ' ' || s[start] == '\t')
        start++;
    if (start)
        memmove(s, s + start, n - start + 1);
}

static int copy_until(char *dst, size_t dstsz, const char *src, const char *end)
{
    size_t n = (size_t)(end - src);
    if (n >= dstsz)
        n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return (int)n;
}

const char *lfg_name_from_filename(const char *filename)
{
    static char buf[LFG_NAME_MAX];
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    /* eqlog_NAME_P1999Green.txt -> NAME */
    if (strncmp(base, "eqlog_", 6) == 0)
        base += 6;
    size_t n = 0;
    while (base[n] && base[n] != '_' && base[n] != '.')
        n++;
    if (n >= sizeof(buf))
        n = sizeof(buf) - 1;
    memcpy(buf, base, n);
    buf[n] = '\0';
    return buf;
}

static time_t parse_log_timestamp(const char *line)
{
    /* "[Sun Oct 11 14:58:10 2026] ..." -> epoch seconds, or 0 if absent */
    if (line[0] != '[')
        return 0;
    static const char *months[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                    "Jul","Aug","Sep","Oct","Nov","Dec" };
    char wd[8], mon[8];
    int day, hh, mm, ss, yr;
    if (sscanf(line, "[%3s %3s %d %d:%d:%d %d]", wd, mon, &day, &hh, &mm, &ss, &yr) != 7)
        return 0;
    int mon_idx = -1;
    for (int i = 0; i < 12; i++)
        if (strcmp(mon, months[i]) == 0) {
            mon_idx = i;
            break;
        }
    if (mon_idx < 0)
        return 0;
    struct tm tm = {0};
    tm.tm_year = yr - 1900;
    tm.tm_mon = mon_idx;
    tm.tm_mday = day;
    tm.tm_hour = hh;
    tm.tm_min = mm;
    tm.tm_sec = ss;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

static int find_marker(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

/*
 * Parses a single log line into an LfgLine.
 *
 * Recognized patterns:
 *   "You are now LFG." / "You are no longer LFG."         -> SELF_ON / SELF_OFF
 *   "... Players on EverQuest:" (who header)             -> LFG_LINE_WHO
 *   "[60 Archmage] Soandso (Dark Elf) ZONE: sebilis"      -> WHO rows
 *   "[60 Archmage] Soandso <LFM>` -- also matches LFM since they are group-seekers
 * Also handles the "There are X players in EverQuest." trailer.
 */
int lfg_parse_line(const char *line, LfgLine *out)
{
    memset(out, 0, sizeof(*out));
    if (!line || !*line)
        return LFG_LINE_NONE;

    if (find_marker(line, "You are now LFG")) {
        out->kind = LFG_LINE_SELF_ON;
        return LFG_LINE_SELF_ON;
    }
    if (find_marker(line, "You are no longer LFG")) {
        out->kind = LFG_LINE_SELF_OFF;
        return LFG_LINE_SELF_OFF;
    }

    const char *br = strchr(line, '[');
    if (!br)
        return LFG_LINE_NONE;

    /* class and level live inside [..] */
    const char *br_end = strchr(br, ']');
    if (!br_end)
        return LFG_LINE_NONE;
    copy_until(out->detail, LFG_DETAIL_MAX, br + 1, br_end);

    const char *p = br_end + 1;
    while (*p == ' ')
        p++;

    /* name ends at " (Race)" or " <Guild>" or end of token */
    const char *name_end = p;
    while (*name_end && *name_end != ' ' )
        name_end++;
    if (name_end == p)
        return LFG_LINE_NONE;
    copy_until(out->name, LFG_NAME_MAX, p, name_end);

    /* zone: "ZONE: name" if present */
    const char *z = strstr(name_end, "ZONE:");
    if (z) {
        z += 5;
        while (*z == ' ')
            z++;
        const char *ze = z;
        while (*ze && *ze != ' ' && *ze != '\n' && *ze != '\r')
            ze++;
        copy_until(out->zone, LFG_ZONE_MAX, z, ze);
    }

    /* who command output marks LFG with <LFG> or (LFG) or trailing LFG */
    if (find_marker(line, "<LFG>") || find_marker(line, "(LFG)") ||
        find_marker(line, " LFG") || find_marker(line, "LFM")) {
        out->kind = LFG_LINE_WHO;
        return LFG_LINE_WHO;
    }
    return LFG_LINE_NONE;
}

static LogFile *find_log_by_name(LfgState *s, const char *name)
{
    for (int i = 0; i < s->nlogs; i++)
        if (strcmp(s->logs[i].name, name) == 0)
            return &s->logs[i];
    return NULL;
}

static void upsert(LfgState *s, const char *name, const char *zone, const char *detail, int source, time_t now)
{
    for (int i = 0; i < s->nentries; i++) {
        if (strcmp(s->entries[i].name, name) == 0) {
            if (zone && *zone)
                snprintf(s->entries[i].zone, LFG_ZONE_MAX, "%s", zone);
            if (detail && *detail)
                snprintf(s->entries[i].detail, LFG_DETAIL_MAX, "%s", detail);
            s->entries[i].source = source;
            s->entries[i].last_seen = now;
            return;
        }
    }
    if (s->nentries >= MAX_ENTRIES)
        return;
    LfgEntry *e = &s->entries[s->nentries++];
    snprintf(e->name, LFG_NAME_MAX, "%s", name);
    snprintf(e->zone, LFG_ZONE_MAX, "%s", zone ? zone : "");
    snprintf(e->detail, LFG_DETAIL_MAX, "%s", detail ? detail : "");
    e->source = source;
    e->last_seen = now;
}

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void scan_dir(LfgState *s)
{
    DIR *d = opendir(s->dir);
    if (!d) {
        dbg("scan_dir: cannot open dir '%s' (errno=%d)", s->dir, errno);
        return;
    }
    dbg("scan_dir: opened '%s'", s->dir);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *fn = de->d_name;
        size_t len = strlen(fn);
        if (len < 12 || !starts_with(fn, "eqlog_"))
            continue;
        if (!strstr(fn, ".txt"))
            continue;
        /* accept P99 naming across servers: eqlog_NAME_P1999Green.txt,
           eqlog_NAME_project1999.txt, etc. */
        if (!strstr(fn, "P1999") && !strstr(fn, "p1999") && !strstr(fn, "project1999"))
            continue;
        if (find_log_by_name(s, lfg_name_from_filename(fn)))
            continue;
        if (s->nlogs >= MAX_LOG_FILES)
            break;
        LogFile *lf = &s->logs[s->nlogs++];
        snprintf(lf->path, LFG_PATH_MAX, "%s/%s", s->dir, fn);
        snprintf(lf->name, LFG_NAME_MAX, "%s", lfg_name_from_filename(fn));
        lf->fp = NULL;
        lf->offset = 0;
        lf->truncated = 0;
        dbg("scan_dir: watching '%s' as '%s'", fn, lf->name);
    }
    closedir(d);
    dbg("scan_dir: %d log file(s) registered", s->nlogs);
}

static void read_new_lines(LfgState *s, LogFile *lf, time_t now)
{
    struct stat st;
    if (stat(lf->path, &st) != 0) {
        dbg("read_new_lines: stat failed '%s'", lf->path);
        return;
    }
    if (st.st_size < lf->offset) {
        lf->offset = 0;
        lf->truncated = 1;
    }
    if (st.st_size == lf->offset)
        return;
    if (!lf->fp) {
        lf->fp = fopen(lf->path, "rb");
        if (!lf->fp)
            return;
        fseek(lf->fp, lf->offset, SEEK_SET);
    }
    char line[LINE_BUF];
    while (fgets(line, sizeof(line), lf->fp)) {
        char tstamp[32] = {0};
        const char *rest = line;
        time_t line_time = 0;
        if (line[0] == '[') {
            const char *close = strchr(line, ']');
            if (close && close - line < 30) {
                copy_until(tstamp, sizeof(tstamp), line + 1, close);
                rest = close + 1;
                line_time = parse_log_timestamp(line);
            }
        }
        time_t stamp = line_time > 0 ? line_time : now;
        LfgLine parsed;
        int kind = lfg_parse_line(rest, &parsed);
        if (kind == LFG_LINE_SELF_ON) {
            dbg("[%s] self LFG on", lf->name);
            upsert(s, lf->name, parsed.zone, parsed.detail, LFG_SRC_LOG, stamp);
        } else if (kind == LFG_LINE_SELF_OFF) {
            dbg("[%s] self LFG off", lf->name);
            lfg_remove(s, lf->name);
        } else if (kind == LFG_LINE_WHO) {
            dbg("[%s] who row: %s (%s)", lf->name, parsed.name, parsed.zone);
            upsert(s, parsed.name, parsed.zone, parsed.detail, LFG_SRC_LOG, stamp);
        }
    }
    lf->offset = ftell(lf->fp);
}

LfgState *lfg_new(void)
{
    LfgState *s = calloc(1, sizeof(LfgState));
    s->stale_minutes = 30;
    return s;
}

void lfg_free(LfgState *s)
{
    if (!s)
        return;
    for (int i = 0; i < s->nlogs; i++)
        if (s->logs[i].fp)
            fclose(s->logs[i].fp);
    free(s);
}

void lfg_set_dir(LfgState *s, const char *dir) { snprintf(s->dir, LFG_PATH_MAX, "%s", dir); }
const char *lfg_get_dir(LfgState *s) { return s->dir; }
void lfg_set_stale_minutes(LfgState *s, int mins) { s->stale_minutes = mins; }
int lfg_get_stale_minutes(LfgState *s) { return s->stale_minutes; }

void lfg_rescan(LfgState *s)
{
    for (int i = 0; i < s->nlogs; i++)
        if (s->logs[i].fp) {
            fclose(s->logs[i].fp);
            s->logs[i].fp = NULL;
        }
    s->nlogs = 0;
    scan_dir(s);
}

void lfg_poll(LfgState *s)
{
    time_t now = time(NULL);
    scan_dir(s);
    for (int i = 0; i < s->nlogs; i++)
        read_new_lines(s, &s->logs[i], now);

    /* drop stale log-derived entries */
    int w = 0;
    for (int i = 0; i < s->nentries; i++) {
        int keep = s->entries[i].source == LFG_SRC_MANUAL ||
                   now - s->entries[i].last_seen <= (time_t)s->stale_minutes * 60;
        s->entries[w] = s->entries[i];
        if (keep)
            w++;
    }
    s->nentries = w;
}

int lfg_count(LfgState *s) { return s->nentries; }

const LfgEntry *lfg_get(LfgState *s, int i)
{
    if (i < 0 || i >= s->nentries)
        return NULL;
    return &s->entries[i];
}

void lfg_add_manual(LfgState *s, const char *name, const char *zone, const char *detail)
{
    upsert(s, name, zone, detail, LFG_SRC_MANUAL, time(NULL));
}

int lfg_remove(LfgState *s, const char *name)
{
    for (int i = 0; i < s->nentries; i++)
        if (strcmp(s->entries[i].name, name) == 0) {
            memmove(&s->entries[i], &s->entries[i+1],
                    (size_t)(s->nentries - i - 1) * sizeof(LfgEntry));
            s->nentries--;
            return 1;
        }
    return 0;
}

void lfg_remove_manual_all(LfgState *s)
{
    int w = 0;
    for (int i = 0; i < s->nentries; i++) {
        s->entries[w] = s->entries[i];
        if (s->entries[i].source != LFG_SRC_MANUAL)
            w++;
    }
    s->nentries = w;
}

const char *lfg_entry_name(const LfgState *s, int i)
{
    const LfgEntry *e = lfg_get(s, i);
    return e ? e->name : NULL;
}

const char *lfg_entry_zone(const LfgState *s, int i)
{
    const LfgEntry *e = lfg_get(s, i);
    return e ? e->zone : NULL;
}

const char *lfg_entry_detail(const LfgState *s, int i)
{
    const LfgEntry *e = lfg_get(s, i);
    return e ? e->detail : NULL;
}

int lfg_entry_source(const LfgState *s, int i)
{
    const LfgEntry *e = lfg_get(s, i);
    return e ? e->source : 0;
}

time_t lfg_entry_last_seen(const LfgState *s, int i)
{
    const LfgEntry *e = lfg_get(s, i);
    return e ? e->last_seen : 0;
}
