#include "tracker-core.h"
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void format_iso8601(time_t t, char *buf, size_t len)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%S", &tm);
}

void csv_escape_to_buffer(GString *buf, const char *field)
{
    g_string_append_c(buf, '"');
    for (const char *p = field; *p; p++) {
        if (*p == '"')
            g_string_append_c(buf, '"');
        g_string_append_c(buf, *p);
    }
    g_string_append_c(buf, '"');
}

void csv_escape_and_print_fp(FILE *fp, const char *field)
{
    fputc('"', fp);
    for (const char *p = field; *p; p++) {
        if (*p == '"')
            fputc('"', fp);
        fputc(*p, fp);
    }
    fputc('"', fp);
}

void csv_escape_and_print(const char *field)
{
    csv_escape_and_print_fp(stdout, field);
}

void emit_csv_to_buffer(GString *buf, AppState *state, gint64 now)
{
    if (!state->current_title)
        return;

    gint64 duration_sec = (now - state->current_start) / G_USEC_PER_SEC;

    if (duration_sec < 1)
        return;

    char ts[32];
    format_iso8601(state->current_wall, ts, sizeof(ts));

    gboolean away = state->is_locked || state->is_idle;
    const gchar *empty = "";
    const gchar *title = away ? empty : state->current_title;
    const gchar *wm_class = away ? empty : (state->current_wm_class ? state->current_wm_class : empty);
    const gchar *wm_class_instance = away ? empty : (state->current_wm_class_instance ? state->current_wm_class_instance : empty);
    const gchar *status = state->is_locked ? "locked" : (state->is_idle ? "idle" : "active");

    const gchar *rp_state = away ? empty : (state->current_rp_state ? state->current_rp_state : empty);
    const gchar *rp_details = away ? empty : (state->current_rp_details ? state->current_rp_details : empty);

    g_string_append_printf(buf, "%s,%ld,%s,", ts, (long)duration_sec, status);
    csv_escape_to_buffer(buf, title);
    g_string_append_c(buf, ',');
    csv_escape_to_buffer(buf, wm_class);
    g_string_append_c(buf, ',');
    csv_escape_to_buffer(buf, wm_class_instance);
    g_string_append_c(buf, ',');
    csv_escape_to_buffer(buf, rp_state);
    g_string_append_c(buf, ',');
    csv_escape_to_buffer(buf, rp_details);
    g_string_append_c(buf, '\n');
}

void emit_csv_line(AppState *state)
{
    if (!state->current_title)
        return;

    gint64 now = g_get_monotonic_time();
    gint64 duration_sec = (now - state->current_start) / G_USEC_PER_SEC;

    if (duration_sec < 1)
        return;

    if (!ensure_output_file(state, state->current_wall))
        return;

    FILE *fp = state->output_fp;

    char ts[32];
    format_iso8601(state->current_wall, ts, sizeof(ts));

    gboolean away = state->is_locked || state->is_idle;
    const gchar *empty = "";
    const gchar *title = away ? empty : state->current_title;
    const gchar *wm_class = away ? empty : (state->current_wm_class ? state->current_wm_class : empty);
    const gchar *wm_class_instance = away ? empty : (state->current_wm_class_instance ? state->current_wm_class_instance : empty);
    const gchar *status = state->is_locked ? "locked" : (state->is_idle ? "idle" : "active");

    const gchar *rp_state = away ? empty : (state->current_rp_state ? state->current_rp_state : empty);
    const gchar *rp_details = away ? empty : (state->current_rp_details ? state->current_rp_details : empty);

    fprintf(fp, "%s,%ld,%s,", ts, (long)duration_sec, status);
    csv_escape_and_print_fp(fp, title);
    fprintf(fp, ",");
    csv_escape_and_print_fp(fp, wm_class);
    fprintf(fp, ",");
    csv_escape_and_print_fp(fp, wm_class_instance);
    fprintf(fp, ",");
    csv_escape_and_print_fp(fp, rp_state);
    fprintf(fp, ",");
    csv_escape_and_print_fp(fp, rp_details);
    fprintf(fp, "\n");
    fflush(fp);
    fsync(fileno(fp));
}

void start_tracking(AppState *state, const gchar *title,
                    const gchar *wm_class, const gchar *wm_class_instance,
                    const gchar *rp_state, const gchar *rp_details,
                    pid_t pid, gboolean locked)
{
    g_free(state->current_title);
    state->current_title = g_strdup(title ? title : "");
    g_free(state->current_wm_class);
    state->current_wm_class = g_strdup(wm_class ? wm_class : "");
    g_free(state->current_wm_class_instance);
    state->current_wm_class_instance = g_strdup(wm_class_instance ? wm_class_instance : "");
    g_free(state->current_rp_state);
    state->current_rp_state = g_strdup(rp_state ? rp_state : "");
    g_free(state->current_rp_details);
    state->current_rp_details = g_strdup(rp_details ? rp_details : "");
    state->current_pid = pid;
    state->current_start = g_get_monotonic_time();
    state->current_wall = time(NULL);
    state->is_locked = locked;
}

gchar *build_csv_path(const gchar *data_dir_override,
                      int year, int month, int day)
{
    const gchar *data_dir = data_dir_override ? data_dir_override
                                              : g_get_user_data_dir();
    return g_strdup_printf("%s/activity-tracker/%04d-%02d/%04d-%02d-%02d.csv",
                           data_dir, year, month, year, month, day);
}

gboolean ensure_output_file(AppState *state, time_t wall_time)
{
    struct tm tm;
    localtime_r(&wall_time, &tm);
    int year = tm.tm_year + 1900;
    int month = tm.tm_mon + 1;
    int day = tm.tm_mday;

    /* Already open for this date */
    if (state->output_fp &&
        state->file_year == year &&
        state->file_month == month &&
        state->file_day == day) {
        return TRUE;
    }

    /* Close previous file if open */
    close_output_file(state);

    gchar *file_path = build_csv_path(state->data_dir, year, month, day);
    gchar *dir_path = g_path_get_dirname(file_path);

    if (g_mkdir_with_parents(dir_path, 0700) != 0) {
        g_printerr("Failed to create directory: %s\n", dir_path);
        g_free(dir_path);
        g_free(file_path);
        return FALSE;
    }

    state->output_fp = fopen(file_path, "a");
    if (!state->output_fp) {
        g_printerr("Failed to open output file: %s\n", file_path);
        g_free(dir_path);
        g_free(file_path);
        return FALSE;
    }

    if (ftell(state->output_fp) == 0) {
        fprintf(state->output_fp,
                "timestamp,duration_seconds,status,window_title,wm_class,wm_class_instance,rp_state,rp_details\n");
        fflush(state->output_fp);
        fsync(fileno(state->output_fp));
    }

    state->file_year = year;
    state->file_month = month;
    state->file_day = day;

    g_free(dir_path);
    g_free(file_path);
    return TRUE;
}

void close_output_file(AppState *state)
{
    if (state->output_fp) {
        fflush(state->output_fp);
        fsync(fileno(state->output_fp));
        fclose(state->output_fp);
        state->output_fp = NULL;
    }
    state->file_year = 0;
    state->file_month = 0;
    state->file_day = 0;
}

FocusedWindowInfo parse_focused_window(const gchar *json)
{
    FocusedWindowInfo info = {NULL, NULL, NULL, 0};

    if (!json || !json[0])
        return info;

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json, -1, NULL)) {
        g_object_unref(parser);
        return info;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_ARRAY(root)) {
        g_object_unref(parser);
        return info;
    }

    JsonArray *array = json_node_get_array(root);
    guint len = json_array_get_length(array);

    for (guint i = 0; i < len; i++) {
        JsonObject *obj = json_array_get_object_element(array, i);
        if (!obj)
            continue;
        if (json_object_has_member(obj, "focus") &&
            json_object_get_boolean_member(obj, "focus")) {
            if (json_object_has_member(obj, "title")) {
                const gchar *t = json_object_get_string_member(obj, "title");
                if (t && t[0] != '\0')
                    info.title = g_strdup(t);
            }
            if (json_object_has_member(obj, "wm_class")) {
                const gchar *c = json_object_get_string_member(obj, "wm_class");
                if (c)
                    info.wm_class = g_strdup(c);
            }
            if (json_object_has_member(obj, "wm_class_instance")) {
                const gchar *ci = json_object_get_string_member(obj, "wm_class_instance");
                if (ci)
                    info.wm_class_instance = g_strdup(ci);
            }
            if (json_object_has_member(obj, "pid"))
                info.pid = (pid_t)json_object_get_int_member(obj, "pid");
            break;
        }
    }

    g_object_unref(parser);
    return info;
}

void free_focused_window_info(FocusedWindowInfo *info)
{
    g_free(info->title);
    g_free(info->wm_class);
    g_free(info->wm_class_instance);
    info->title = NULL;
    info->wm_class = NULL;
    info->wm_class_instance = NULL;
    info->pid = 0;
}

/* ── Statistics functions ──────────────────────────────── */

#define DURATION_WIDTH 11

gchar *format_duration(long seconds)
{
    if (seconds < 0)
        seconds = 0;
    long h = seconds / 3600;
    long m = (seconds % 3600) / 60;
    long s = seconds % 60;

    if (h > 0)
        return g_strdup_printf("%*ldh %02ldm %02lds", DURATION_WIDTH - 9, h, m, s);
    if (m > 0)
        return g_strdup_printf("%*ldm %02lds", DURATION_WIDTH - 5, m, s);
    return g_strdup_printf("%*lds", DURATION_WIDTH - 1, s);
}

/* Extract one RFC 4180 CSV field starting at line[*pos].
 * Handles quoted fields with escaped double-quotes.
 * Returns the field value (caller frees) and advances *pos past the delimiter. */
static gchar *extract_csv_field(const gchar *line, gsize *pos)
{
    GString *field = g_string_new(NULL);
    gsize i = *pos;

    if (line[i] == '"') {
        i++; /* skip opening quote */
        while (line[i]) {
            if (line[i] == '"') {
                if (line[i + 1] == '"') {
                    g_string_append_c(field, '"');
                    i += 2;
                } else {
                    i++; /* skip closing quote */
                    break;
                }
            } else {
                g_string_append_c(field, line[i]);
                i++;
            }
        }
    } else {
        while (line[i] && line[i] != ',' && line[i] != '\n' && line[i] != '\r') {
            g_string_append_c(field, line[i]);
            i++;
        }
    }

    if (line[i] == ',')
        i++;
    *pos = i;
    return g_string_free(field, FALSE);
}

gboolean parse_csv_line(const gchar *line,
                        gchar **timestamp, long *duration,
                        gchar **status, gchar **window_title,
                        gchar **wm_class, gchar **wm_class_instance,
                        gchar **rp_state, gchar **rp_details)
{
    if (!line || !line[0])
        return FALSE;

    gsize pos = 0;
    gchar *f_ts = extract_csv_field(line, &pos);
    gchar *f_dur = extract_csv_field(line, &pos);
    gchar *f_status = extract_csv_field(line, &pos);
    gchar *f_title = extract_csv_field(line, &pos);
    gchar *f_class = extract_csv_field(line, &pos);
    gchar *f_instance = extract_csv_field(line, &pos);

    char *endptr;
    long dur = strtol(f_dur, &endptr, 10);
    if (endptr == f_dur || (*endptr != '\0' && *endptr != '\n' && *endptr != '\r')) {
        g_free(f_ts); g_free(f_dur); g_free(f_status);
        g_free(f_title); g_free(f_class); g_free(f_instance);
        return FALSE;
    }

    /* Parse optional rich presence fields (backward compatible with old CSVs) */
    gchar *f_rp_state = NULL;
    gchar *f_rp_details = NULL;
    if (line[pos]) {
        f_rp_state = extract_csv_field(line, &pos);
        if (line[pos])
            f_rp_details = extract_csv_field(line, &pos);
    }

    *timestamp = f_ts;
    *duration = dur;
    *status = f_status;
    *window_title = f_title;
    *wm_class = f_class;
    *wm_class_instance = f_instance;
    *rp_state = f_rp_state ? f_rp_state : g_strdup("");
    *rp_details = f_rp_details ? f_rp_details : g_strdup("");
    g_free(f_dur);
    return TRUE;
}

static gint compare_app_stat_desc(gconstpointer a, gconstpointer b)
{
    const AppStat *sa = *(const AppStat **)a;
    const AppStat *sb = *(const AppStat **)b;
    if (sb->total_seconds > sa->total_seconds) return 1;
    if (sb->total_seconds < sa->total_seconds) return -1;
    return 0;
}

DayStats *compute_day_stats(const gchar *csv_path)
{
    gchar *contents = NULL;
    if (!g_file_get_contents(csv_path, &contents, NULL, NULL))
        return NULL;

    DayStats *stats = g_new0(DayStats, 1);
    GHashTable *app_map = g_hash_table_new(g_str_hash, g_str_equal);

    gchar **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (!lines[i][0])
            continue;

        gchar *ts, *status, *title, *wm_class, *wm_instance;
        gchar *rps, *rpd;
        long duration;
        if (!parse_csv_line(lines[i], &ts, &duration, &status, &title,
                            &wm_class, &wm_instance, &rps, &rpd))
            continue;

        if (g_strcmp0(status, "locked") == 0 || g_strcmp0(status, "idle") == 0) {
            stats->total_locked_seconds += duration;
        } else {
            stats->total_active_seconds += duration;

            AppStat *app = g_hash_table_lookup(app_map, wm_class);
            if (!app) {
                app = g_new0(AppStat, 1);
                app->wm_class = g_strdup(wm_class);
                app->titles = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     g_free, g_free);
                g_hash_table_insert(app_map, app->wm_class, app);
            }
            app->total_seconds += duration;

            /* Build display key: use rich presence if available, else window title */
            gchar *display_key;
            gboolean has_rps = rps && rps[0];
            gboolean has_rpd = rpd && rpd[0];
            if (has_rps && has_rpd)
                display_key = g_strdup_printf("%s | %s", rps, rpd);
            else if (has_rps)
                display_key = g_strdup(rps);
            else if (has_rpd)
                display_key = g_strdup(rpd);
            else
                display_key = g_strdup(title);

            long *title_secs = g_hash_table_lookup(app->titles, display_key);
            if (title_secs) {
                *title_secs += duration;
                g_free(display_key);
            } else {
                long *new_secs = g_new(long, 1);
                *new_secs = duration;
                g_hash_table_insert(app->titles, display_key, new_secs);
            }
        }

        g_free(ts); g_free(status); g_free(title);
        g_free(wm_class); g_free(wm_instance);
        g_free(rps); g_free(rpd);
    }

    stats->apps = g_ptr_array_new();
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, app_map);
    while (g_hash_table_iter_next(&iter, &key, &value))
        g_ptr_array_add(stats->apps, value);
    g_ptr_array_sort(stats->apps, compare_app_stat_desc);

    g_hash_table_destroy(app_map);
    g_strfreev(lines);
    g_free(contents);
    return stats;
}

DayStats *filter_stats_by_grep(const DayStats *stats, const gchar *pattern,
                               GError **error)
{
    GRegex *regex = g_regex_new(pattern, G_REGEX_CASELESS, 0, error);
    if (!regex)
        return NULL;

    DayStats *filtered = g_new0(DayStats, 1);
    filtered->total_active_seconds = stats->total_active_seconds;
    filtered->total_locked_seconds = stats->total_locked_seconds;
    filtered->apps = g_ptr_array_new();

    for (guint i = 0; i < stats->apps->len; i++) {
        AppStat *app = g_ptr_array_index(stats->apps, i);
        gboolean class_matches = g_regex_match(regex, app->wm_class, 0, NULL);

        /* Collect matching titles */
        AppStat *clone = NULL;
        long clone_total = 0;
        GHashTableIter titer;
        gpointer tkey, tval;
        g_hash_table_iter_init(&titer, app->titles);
        while (g_hash_table_iter_next(&titer, &tkey, &tval)) {
            if (g_regex_match(regex, (const gchar *)tkey, 0, NULL)) {
                if (!clone) {
                    clone = g_new0(AppStat, 1);
                    clone->wm_class = g_strdup(app->wm_class);
                    clone->titles = g_hash_table_new_full(
                        g_str_hash, g_str_equal, g_free, g_free);
                }
                long *secs = g_new(long, 1);
                *secs = *(long *)tval;
                clone_total += *secs;
                g_hash_table_insert(clone->titles,
                                    g_strdup(tkey), secs);
            }
        }

        if (clone) {
            /* Title(s) matched: show only matching titles */
            clone->total_seconds = clone_total;
            g_ptr_array_add(filtered->apps, clone);
        } else if (class_matches) {
            /* App name matched but no titles did: clone all titles */
            AppStat *full = g_new0(AppStat, 1);
            full->wm_class = g_strdup(app->wm_class);
            full->total_seconds = app->total_seconds;
            full->titles = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, g_free);
            g_hash_table_iter_init(&titer, app->titles);
            while (g_hash_table_iter_next(&titer, &tkey, &tval)) {
                long *secs = g_new(long, 1);
                *secs = *(long *)tval;
                g_hash_table_insert(full->titles, g_strdup(tkey), secs);
            }
            g_ptr_array_add(filtered->apps, full);
        }
    }

    g_ptr_array_sort(filtered->apps, compare_app_stat_desc);
    g_regex_unref(regex);
    return filtered;
}

typedef struct {
    const gchar *title;
    long total_seconds;
} TitleEntry;

static gint compare_title_entry_desc(gconstpointer a, gconstpointer b)
{
    const TitleEntry *ta = a;
    const TitleEntry *tb = b;
    if (tb->total_seconds > ta->total_seconds) return 1;
    if (tb->total_seconds < ta->total_seconds) return -1;
    return 0;
}

#define DEFAULT_COLS 80
#define MIN_COLS     40

/* Truncate a UTF-8 string to fit within max_chars, appending "..." if needed.
 * Returns a newly allocated string. */
static gchar *truncate_label(const gchar *str, int max_chars)
{
    if ((int)g_utf8_strlen(str, -1) <= max_chars)
        return g_strdup(str);
    gchar *sub = g_utf8_substring(str, 0, max_chars - 3);
    gchar *result = g_strdup_printf("%s...", sub);
    g_free(sub);
    return result;
}

/* Return printf field width adjusted for multi-byte UTF-8 overhead.
 * printf %-*s pads by byte count, so we add the difference between
 * byte length and character length to compensate. */
static int utf8_field_width(const gchar *str, int desired_columns)
{
    size_t bytes = strlen(str);
    glong chars = g_utf8_strlen(str, -1);
    return desired_columns + (int)(bytes - (size_t)chars);
}

void print_stats_report(FILE *out, const DayStats *stats,
                        int year, int month, int day,
                        const StatsOptions *opts)
{
    int top_apps = opts ? opts->top_apps : 20;
    int top_titles = opts ? opts->top_titles : 5;
    int cols = (opts && opts->cols >= MIN_COLS) ? opts->cols : DEFAULT_COLS;
    int label_width = cols - 1 - DURATION_WIDTH;

    long total = stats->total_active_seconds + stats->total_locked_seconds;
    gchar *total_dur = format_duration(total);
    fprintf(out, "Activity Report for %04d-%02d-%02d\n", year, month, day);
    fprintf(out, "Total tracked: %s\n\n", total_dur);
    g_free(total_dur);

    guint app_count = stats->apps->len;
    guint display_count = (guint)MIN((int)app_count, top_apps);

    for (guint i = 0; i < display_count; i++) {
        AppStat *app = g_ptr_array_index(stats->apps, i);
        gchar *dur = format_duration(app->total_seconds);
        gchar *name = truncate_label(app->wm_class, label_width - 5);
        fprintf(out, "%3u. %-*s %s\n", i + 1, utf8_field_width(name, label_width - 5), name, dur);
        g_free(name);
        g_free(dur);

        /* Collect and sort titles */
        GArray *titles = g_array_new(FALSE, FALSE, sizeof(TitleEntry));
        GHashTableIter titer;
        gpointer tkey, tval;
        g_hash_table_iter_init(&titer, app->titles);
        while (g_hash_table_iter_next(&titer, &tkey, &tval)) {
            TitleEntry te = { .title = tkey, .total_seconds = *(long *)tval };
            g_array_append_val(titles, te);
        }
        g_array_sort(titles, compare_title_entry_desc);

        guint title_count = titles->len;
        guint title_display = (guint)MIN((int)title_count, top_titles);
        long other_title_seconds = 0;

        for (guint j = 0; j < title_count; j++) {
            TitleEntry *te = &g_array_index(titles, TitleEntry, j);
            if (j < title_display) {
                gchar *td = format_duration(te->total_seconds);
                gchar *trunc = truncate_label(te->title, label_width - 7);
                fprintf(out, "       %-*s %s\n", utf8_field_width(trunc, label_width - 7), trunc, td);
                g_free(trunc);
                g_free(td);
            } else {
                other_title_seconds += te->total_seconds;
            }
        }

        if (title_count > (guint)top_titles) {
            gchar *od = format_duration(other_title_seconds);
            gchar *label = g_strdup_printf("%u other windows",
                                           title_count - title_display);
            gchar *trunc = truncate_label(label, label_width - 7);
            fprintf(out, "       %-*s %s\n", utf8_field_width(trunc, label_width - 7), trunc, od);
            g_free(trunc);
            g_free(label);
            g_free(od);
        }

        fprintf(out, "\n");
        g_array_free(titles, TRUE);
    }

    if ((int)app_count > top_apps) {
        long other_seconds = 0;
        for (guint i = (guint)top_apps; i < app_count; i++) {
            AppStat *app = g_ptr_array_index(stats->apps, i);
            other_seconds += app->total_seconds;
        }
        gchar *od = format_duration(other_seconds);
        gchar *label = g_strdup_printf("%u other applications",
                                       app_count - (guint)top_apps);
        gchar *trunc = truncate_label(label, label_width - 2);
        fprintf(out, "  %-*s %s\n\n", utf8_field_width(trunc, label_width - 2), trunc, od);
        g_free(trunc);
        g_free(label);
        g_free(od);
    }

    if (stats->total_locked_seconds > 0) {
        gchar *ld = format_duration(stats->total_locked_seconds);
        fprintf(out, "%-*s %s\n", utf8_field_width("Away", label_width), "Away", ld);
        g_free(ld);
    }
}

void free_day_stats(DayStats *stats)
{
    if (!stats)
        return;
    for (guint i = 0; i < stats->apps->len; i++) {
        AppStat *app = g_ptr_array_index(stats->apps, i);
        g_free(app->wm_class);
        g_hash_table_destroy(app->titles);
        g_free(app);
    }
    g_ptr_array_free(stats->apps, TRUE);
    g_free(stats);
}
