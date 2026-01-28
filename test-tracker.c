#include <glib.h>
#include "tracker-core.h"
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>

/* ── format_iso8601 ────────────────────────────────────────── */

static void test_format_iso8601(void)
{
    char buf[32];
    /* Use a known timestamp: 2024-01-15 10:30:00 UTC */
    time_t t = 1705311000;
    format_iso8601(t, buf, sizeof(buf));

    /* Verify it produces a valid ISO 8601 local-time string.
       Exact value depends on timezone, so just check structure. */
    g_assert_cmpuint(strlen(buf), ==, 19);
    g_assert_true(buf[4] == '-');
    g_assert_true(buf[7] == '-');
    g_assert_true(buf[10] == 'T');
    g_assert_true(buf[13] == ':');
    g_assert_true(buf[16] == ':');
}

/* ── csv_escape_to_buffer ──────────────────────────────────── */

static void test_csv_escape_simple(void)
{
    GString *buf = g_string_new(NULL);
    csv_escape_to_buffer(buf, "hello world");
    g_assert_cmpstr(buf->str, ==, "\"hello world\"");
    g_string_free(buf, TRUE);
}

static void test_csv_escape_with_quotes(void)
{
    GString *buf = g_string_new(NULL);
    csv_escape_to_buffer(buf, "say \"hi\"");
    g_assert_cmpstr(buf->str, ==, "\"say \"\"hi\"\"\"");
    g_string_free(buf, TRUE);
}

static void test_csv_escape_empty(void)
{
    GString *buf = g_string_new(NULL);
    csv_escape_to_buffer(buf, "");
    g_assert_cmpstr(buf->str, ==, "\"\"");
    g_string_free(buf, TRUE);
}

/* ── start_tracking ────────────────────────────────────────── */

static void test_start_tracking(void)
{
    AppState state = {0};
    start_tracking(&state, "My Window", "MyApp", "myapp", FALSE);

    g_assert_cmpstr(state.current_title, ==, "My Window");
    g_assert_cmpstr(state.current_wm_class, ==, "MyApp");
    g_assert_cmpstr(state.current_wm_class_instance, ==, "myapp");
    g_assert_false(state.is_locked);
    g_assert_cmpint(state.current_start, >, 0);
    g_assert_cmpint(state.current_wall, >, 0);

    g_free(state.current_title);
    g_free(state.current_wm_class);
    g_free(state.current_wm_class_instance);
}

static void test_start_tracking_null_title(void)
{
    AppState state = {0};
    start_tracking(&state, NULL, NULL, NULL, TRUE);

    g_assert_cmpstr(state.current_title, ==, "");
    g_assert_cmpstr(state.current_wm_class, ==, "");
    g_assert_cmpstr(state.current_wm_class_instance, ==, "");
    g_assert_true(state.is_locked);

    g_free(state.current_title);
    g_free(state.current_wm_class);
    g_free(state.current_wm_class_instance);
}

/* ── emit_csv_to_buffer ────────────────────────────────────── */

static void test_emit_csv_active(void)
{
    AppState state = {0};
    state.current_title = g_strdup("Firefox");
    state.current_wm_class = g_strdup("Firefox");
    state.current_wm_class_instance = g_strdup("navigator");
    state.current_wall = 1705311000;
    state.current_start = 0;
    state.is_locked = FALSE;

    GString *buf = g_string_new(NULL);
    /* now = 5 seconds after start */
    emit_csv_to_buffer(buf, &state, 5 * G_USEC_PER_SEC);

    /* Should contain: timestamp,5,active,"Firefox","Firefox","navigator"\n */
    g_assert_true(g_str_has_suffix(buf->str, ",5,active,\"Firefox\",\"Firefox\",\"navigator\"\n"));
    g_assert_cmpuint(buf->len, >, 30);

    g_string_free(buf, TRUE);
    g_free(state.current_title);
    g_free(state.current_wm_class);
    g_free(state.current_wm_class_instance);
}

static void test_emit_csv_locked(void)
{
    AppState state = {0};
    state.current_title = g_strdup("Ignored Title");
    state.current_wm_class = g_strdup("SomeClass");
    state.current_wm_class_instance = g_strdup("someinstance");
    state.current_wall = 1705311000;
    state.current_start = 0;
    state.is_locked = TRUE;

    GString *buf = g_string_new(NULL);
    emit_csv_to_buffer(buf, &state, 10 * G_USEC_PER_SEC);

    g_assert_true(g_str_has_suffix(buf->str, ",10,locked,\"\",\"\",\"\"\n"));

    g_string_free(buf, TRUE);
    g_free(state.current_title);
    g_free(state.current_wm_class);
    g_free(state.current_wm_class_instance);
}

static void test_emit_csv_skips_short_duration(void)
{
    AppState state = {0};
    state.current_title = g_strdup("Short");
    state.current_start = 0;
    state.current_wall = 1705311000;
    state.is_locked = FALSE;

    GString *buf = g_string_new(NULL);
    /* now is only 0.5s after start */
    emit_csv_to_buffer(buf, &state, G_USEC_PER_SEC / 2);

    g_assert_cmpuint(buf->len, ==, 0);

    g_string_free(buf, TRUE);
    g_free(state.current_title);
}

static void test_emit_csv_no_title(void)
{
    AppState state = {0};
    state.current_title = NULL;
    state.current_start = 0;
    state.current_wall = 1705311000;

    GString *buf = g_string_new(NULL);
    emit_csv_to_buffer(buf, &state, 5 * G_USEC_PER_SEC);

    g_assert_cmpuint(buf->len, ==, 0);

    g_string_free(buf, TRUE);
}

/* ── parse_focused_window ──────────────────────────────────── */

static void test_parse_focused_window_found(void)
{
    const gchar *json =
        "[{\"id\":1,\"title\":\"Terminal\",\"wm_class\":\"Gnome-terminal\","
         "\"wm_class_instance\":\"gnome-terminal\",\"focus\":false},"
         "{\"id\":2,\"title\":\"Firefox - Google\",\"wm_class\":\"Firefox\","
         "\"wm_class_instance\":\"navigator\",\"focus\":true},"
         "{\"id\":3,\"title\":\"Files\",\"wm_class\":\"Nautilus\","
         "\"wm_class_instance\":\"nautilus\",\"focus\":false}]";
    FocusedWindowInfo info = parse_focused_window(json);
    g_assert_cmpstr(info.title, ==, "Firefox - Google");
    g_assert_cmpstr(info.wm_class, ==, "Firefox");
    g_assert_cmpstr(info.wm_class_instance, ==, "navigator");
    free_focused_window_info(&info);
}

static void test_parse_focused_window_none(void)
{
    const gchar *json =
        "[{\"id\":1,\"title\":\"Terminal\",\"wm_class\":\"Gnome-terminal\","
         "\"wm_class_instance\":\"gnome-terminal\",\"focus\":false},"
         "{\"id\":2,\"title\":\"Firefox\",\"wm_class\":\"Firefox\","
         "\"wm_class_instance\":\"navigator\",\"focus\":false}]";
    FocusedWindowInfo info = parse_focused_window(json);
    g_assert_null(info.title);
    g_assert_null(info.wm_class);
    g_assert_null(info.wm_class_instance);
    free_focused_window_info(&info);
}

static void test_parse_focused_window_empty_array(void)
{
    FocusedWindowInfo info = parse_focused_window("[]");
    g_assert_null(info.title);
    g_assert_null(info.wm_class);
    g_assert_null(info.wm_class_instance);
    free_focused_window_info(&info);
}

static void test_parse_focused_window_malformed(void)
{
    FocusedWindowInfo info = parse_focused_window("{not valid json!!");
    g_assert_null(info.title);
    g_assert_null(info.wm_class);
    g_assert_null(info.wm_class_instance);
    free_focused_window_info(&info);
}

static void test_parse_focused_window_null(void)
{
    FocusedWindowInfo info = parse_focused_window(NULL);
    g_assert_null(info.title);
    g_assert_null(info.wm_class);
    g_assert_null(info.wm_class_instance);
    free_focused_window_info(&info);
}

static void test_parse_focused_window_empty_string(void)
{
    FocusedWindowInfo info = parse_focused_window("");
    g_assert_null(info.title);
    g_assert_null(info.wm_class);
    g_assert_null(info.wm_class_instance);
    free_focused_window_info(&info);
}

static void test_parse_focused_window_missing_wm_fields(void)
{
    const gchar *json =
        "[{\"id\":1,\"title\":\"Terminal\",\"focus\":true}]";
    FocusedWindowInfo info = parse_focused_window(json);
    g_assert_cmpstr(info.title, ==, "Terminal");
    g_assert_null(info.wm_class);
    g_assert_null(info.wm_class_instance);
    free_focused_window_info(&info);
}

/* ── file output ───────────────────────────────────────────── */

static gchar *create_test_tmpdir(void)
{
    gchar *tmpdir = g_dir_make_tmp("activity-tracker-test-XXXXXX", NULL);
    g_assert_nonnull(tmpdir);
    return tmpdir;
}

static void cleanup_test_tmpdir(gchar *tmpdir)
{
    /* Remove directory tree */
    gchar *argv[] = {"rm", "-rf", tmpdir, NULL};
    g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                 NULL, NULL, NULL, NULL, NULL, NULL);
    g_free(tmpdir);
}

static void test_ensure_output_creates(void)
{
    gchar *tmpdir = create_test_tmpdir();
    AppState state = {0};
    state.data_dir = tmpdir;

    /* Use a known time: 2026-01-28 12:00:00 */
    struct tm tm = {0};
    tm.tm_year = 126; /* 2026 */
    tm.tm_mon = 0;    /* January */
    tm.tm_mday = 28;
    tm.tm_hour = 12;
    time_t t = mktime(&tm);

    gboolean ok = ensure_output_file(&state, t);
    g_assert_true(ok);
    g_assert_nonnull(state.output_fp);
    g_assert_cmpint(state.file_year, ==, 2026);
    g_assert_cmpint(state.file_month, ==, 1);
    g_assert_cmpint(state.file_day, ==, 28);

    /* Verify directory and file exist */
    gchar *file_path = g_strdup_printf("%s/activity-tracker/2026-01/2026-01-28.csv", tmpdir);
    g_assert_true(g_file_test(file_path, G_FILE_TEST_EXISTS));

    /* Verify header was written */
    close_output_file(&state);
    gchar *contents = NULL;
    g_file_get_contents(file_path, &contents, NULL, NULL);
    g_assert_nonnull(contents);
    g_assert_true(g_str_has_prefix(contents, "timestamp,duration_seconds,status,"));

    g_free(contents);
    g_free(file_path);
    cleanup_test_tmpdir(tmpdir);
}

static void test_ensure_output_same_date(void)
{
    gchar *tmpdir = create_test_tmpdir();
    AppState state = {0};
    state.data_dir = tmpdir;

    struct tm tm = {0};
    tm.tm_year = 126;
    tm.tm_mon = 0;
    tm.tm_mday = 28;
    tm.tm_hour = 12;
    time_t t = mktime(&tm);

    ensure_output_file(&state, t);
    FILE *first_fp = state.output_fp;

    /* Call again with same date — should be no-op */
    gboolean ok = ensure_output_file(&state, t + 3600);
    g_assert_true(ok);
    g_assert_true(state.output_fp == first_fp);

    close_output_file(&state);
    cleanup_test_tmpdir(tmpdir);
}

static void test_ensure_output_date_rotation(void)
{
    gchar *tmpdir = create_test_tmpdir();
    AppState state = {0};
    state.data_dir = tmpdir;

    struct tm tm = {0};
    tm.tm_year = 126;
    tm.tm_mon = 0;
    tm.tm_mday = 28;
    tm.tm_hour = 23;
    tm.tm_min = 59;
    time_t t1 = mktime(&tm);

    ensure_output_file(&state, t1);
    g_assert_cmpint(state.file_day, ==, 28);

    /* Next day */
    tm.tm_mday = 29;
    tm.tm_hour = 0;
    tm.tm_min = 1;
    time_t t2 = mktime(&tm);

    gboolean ok = ensure_output_file(&state, t2);
    g_assert_true(ok);
    g_assert_cmpint(state.file_day, ==, 29);

    /* Both files should exist */
    gchar *f1 = g_strdup_printf("%s/activity-tracker/2026-01/2026-01-28.csv", tmpdir);
    gchar *f2 = g_strdup_printf("%s/activity-tracker/2026-01/2026-01-29.csv", tmpdir);
    g_assert_true(g_file_test(f1, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(f2, G_FILE_TEST_EXISTS));

    g_free(f1);
    g_free(f2);
    close_output_file(&state);
    cleanup_test_tmpdir(tmpdir);
}

static void test_close_output_file(void)
{
    gchar *tmpdir = create_test_tmpdir();
    AppState state = {0};
    state.data_dir = tmpdir;

    struct tm tm = {0};
    tm.tm_year = 126;
    tm.tm_mon = 0;
    tm.tm_mday = 28;
    tm.tm_hour = 12;
    time_t t = mktime(&tm);

    ensure_output_file(&state, t);
    g_assert_nonnull(state.output_fp);

    close_output_file(&state);
    g_assert_null(state.output_fp);
    g_assert_cmpint(state.file_year, ==, 0);
    g_assert_cmpint(state.file_month, ==, 0);
    g_assert_cmpint(state.file_day, ==, 0);

    cleanup_test_tmpdir(tmpdir);
}

static void test_ensure_output_appends(void)
{
    gchar *tmpdir = create_test_tmpdir();
    AppState state = {0};
    state.data_dir = tmpdir;

    struct tm tm = {0};
    tm.tm_year = 126;
    tm.tm_mon = 0;
    tm.tm_mday = 28;
    tm.tm_hour = 12;
    time_t t = mktime(&tm);

    /* First open — writes header */
    ensure_output_file(&state, t);
    fprintf(state.output_fp, "fake,data,line\n");
    close_output_file(&state);

    /* Second open — should NOT write header again */
    ensure_output_file(&state, t);
    fprintf(state.output_fp, "more,data,here\n");
    close_output_file(&state);

    gchar *file_path = g_strdup_printf("%s/activity-tracker/2026-01/2026-01-28.csv", tmpdir);
    gchar *contents = NULL;
    g_file_get_contents(file_path, &contents, NULL, NULL);
    g_assert_nonnull(contents);

    /* Count header occurrences — should be exactly 1 */
    gchar **lines = g_strsplit(contents, "\n", -1);
    int header_count = 0;
    for (int i = 0; lines[i]; i++) {
        if (g_str_has_prefix(lines[i], "timestamp,duration_seconds,"))
            header_count++;
    }
    g_assert_cmpint(header_count, ==, 1);

    /* Should have the data lines */
    g_assert_nonnull(strstr(contents, "fake,data,line"));
    g_assert_nonnull(strstr(contents, "more,data,here"));

    g_strfreev(lines);
    g_free(contents);
    g_free(file_path);
    cleanup_test_tmpdir(tmpdir);
}

static void test_csv_escape_print_fp(void)
{
    gchar *tmppath = NULL;
    gint fd = g_file_open_tmp("csv-test-XXXXXX", &tmppath, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    FILE *fp = fopen(tmppath, "w");
    g_assert_nonnull(fp);

    csv_escape_and_print_fp(fp, "hello \"world\"");
    fclose(fp);

    gchar *contents = NULL;
    g_file_get_contents(tmppath, &contents, NULL, NULL);
    g_assert_cmpstr(contents, ==, "\"hello \"\"world\"\"\"");

    g_free(contents);
    unlink(tmppath);
    g_free(tmppath);
}

/* ── format_duration ───────────────────────────────────────── */

static void test_format_duration_hours(void)
{
    gchar *d = format_duration(5521); /* 1h 32m 01s */
    g_assert_cmpstr(d, ==, " 1h 32m 01s");
    g_assert_cmpuint(strlen(d), ==, 11);
    g_free(d);
}

static void test_format_duration_minutes(void)
{
    gchar *d = format_duration(125); /* 2m 05s */
    g_assert_cmpstr(d, ==, "     2m 05s");
    g_assert_cmpuint(strlen(d), ==, 11);
    g_free(d);
}

static void test_format_duration_seconds(void)
{
    gchar *d = format_duration(7);
    g_assert_cmpstr(d, ==, "         7s");
    g_assert_cmpuint(strlen(d), ==, 11);
    g_free(d);
}

static void test_format_duration_zero(void)
{
    gchar *d = format_duration(0);
    g_assert_cmpstr(d, ==, "         0s");
    g_free(d);
}

static void test_format_duration_large(void)
{
    gchar *d = format_duration(36000 + 59 * 60 + 59); /* 10h 59m 59s */
    g_assert_cmpstr(d, ==, "10h 59m 59s");
    g_free(d);
}

/* ── parse_csv_line ───────────────────────────────────────── */

static void test_parse_csv_simple(void)
{
    gchar *ts, *status, *title, *cls, *inst;
    long dur;
    gboolean ok = parse_csv_line(
        "2026-01-28T10:00:00,120,active,\"Firefox\",\"Firefox\",\"navigator\"",
        &ts, &dur, &status, &title, &cls, &inst);
    g_assert_true(ok);
    g_assert_cmpstr(ts, ==, "2026-01-28T10:00:00");
    g_assert_cmpint(dur, ==, 120);
    g_assert_cmpstr(status, ==, "active");
    g_assert_cmpstr(title, ==, "Firefox");
    g_assert_cmpstr(cls, ==, "Firefox");
    g_assert_cmpstr(inst, ==, "navigator");
    g_free(ts); g_free(status); g_free(title);
    g_free(cls); g_free(inst);
}

static void test_parse_csv_quoted_title(void)
{
    gchar *ts, *status, *title, *cls, *inst;
    long dur;
    gboolean ok = parse_csv_line(
        "2026-01-28T10:00:00,60,active,\"Say \"\"hello\"\"\",\"App\",\"app\"",
        &ts, &dur, &status, &title, &cls, &inst);
    g_assert_true(ok);
    g_assert_cmpstr(title, ==, "Say \"hello\"");
    g_free(ts); g_free(status); g_free(title);
    g_free(cls); g_free(inst);
}

static void test_parse_csv_header_line(void)
{
    gchar *ts, *status, *title, *cls, *inst;
    long dur;
    gboolean ok = parse_csv_line(
        "timestamp,duration_seconds,status,window_title,wm_class,wm_class_instance",
        &ts, &dur, &status, &title, &cls, &inst);
    g_assert_false(ok);
}

static void test_parse_csv_locked(void)
{
    gchar *ts, *status, *title, *cls, *inst;
    long dur;
    gboolean ok = parse_csv_line(
        "2026-01-28T12:00:00,300,locked,\"\",\"\",\"\"",
        &ts, &dur, &status, &title, &cls, &inst);
    g_assert_true(ok);
    g_assert_cmpstr(status, ==, "locked");
    g_assert_cmpstr(title, ==, "");
    g_assert_cmpint(dur, ==, 300);
    g_free(ts); g_free(status); g_free(title);
    g_free(cls); g_free(inst);
}

static void test_parse_csv_empty_line(void)
{
    gchar *ts, *status, *title, *cls, *inst;
    long dur;
    g_assert_false(parse_csv_line("", &ts, &dur, &status, &title, &cls, &inst));
    g_assert_false(parse_csv_line(NULL, &ts, &dur, &status, &title, &cls, &inst));
}

/* ── build_csv_path ───────────────────────────────────────── */

static void test_build_csv_path(void)
{
    gchar *path = build_csv_path("/tmp/test-data", 2026, 1, 28);
    g_assert_cmpstr(path, ==, "/tmp/test-data/activity-tracker/2026-01/2026-01-28.csv");
    g_free(path);
}

static void test_build_csv_path_padding(void)
{
    gchar *path = build_csv_path("/tmp/d", 2026, 3, 5);
    g_assert_cmpstr(path, ==, "/tmp/d/activity-tracker/2026-03/2026-03-05.csv");
    g_free(path);
}

/* ── compute_day_stats ────────────────────────────────────── */

static void test_compute_day_stats(void)
{
    gchar *tmpdir = create_test_tmpdir();
    gchar *csv_path = g_strdup_printf("%s/test.csv", tmpdir);

    const gchar *csv =
        "timestamp,duration_seconds,status,window_title,wm_class,wm_class_instance\n"
        "2026-01-28T10:00:00,60,active,\"Tab 1\",\"Firefox\",\"navigator\"\n"
        "2026-01-28T10:01:00,30,active,\"Tab 2\",\"Firefox\",\"navigator\"\n"
        "2026-01-28T10:02:00,120,active,\"Terminal\",\"Gnome-terminal\",\"gnome-terminal\"\n"
        "2026-01-28T10:04:00,45,locked,\"\",\"\",\"\"\n";
    g_file_set_contents(csv_path, csv, -1, NULL);

    DayStats *stats = compute_day_stats(csv_path);
    g_assert_nonnull(stats);
    g_assert_cmpint(stats->total_active_seconds, ==, 210);
    g_assert_cmpint(stats->total_locked_seconds, ==, 45);
    g_assert_cmpuint(stats->apps->len, ==, 2);

    /* First app should be Gnome-terminal (120s) */
    AppStat *first = g_ptr_array_index(stats->apps, 0);
    g_assert_cmpstr(first->wm_class, ==, "Gnome-terminal");
    g_assert_cmpint(first->total_seconds, ==, 120);

    /* Second app should be Firefox (90s) */
    AppStat *second = g_ptr_array_index(stats->apps, 1);
    g_assert_cmpstr(second->wm_class, ==, "Firefox");
    g_assert_cmpint(second->total_seconds, ==, 90);

    /* Firefox should have 2 titles */
    g_assert_cmpuint(g_hash_table_size(second->titles), ==, 2);

    free_day_stats(stats);
    g_free(csv_path);
    cleanup_test_tmpdir(tmpdir);
}

static void test_compute_day_stats_empty(void)
{
    gchar *tmpdir = create_test_tmpdir();
    gchar *csv_path = g_strdup_printf("%s/empty.csv", tmpdir);

    const gchar *csv =
        "timestamp,duration_seconds,status,window_title,wm_class,wm_class_instance\n";
    g_file_set_contents(csv_path, csv, -1, NULL);

    DayStats *stats = compute_day_stats(csv_path);
    g_assert_nonnull(stats);
    g_assert_cmpint(stats->total_active_seconds, ==, 0);
    g_assert_cmpint(stats->total_locked_seconds, ==, 0);
    g_assert_cmpuint(stats->apps->len, ==, 0);

    free_day_stats(stats);
    g_free(csv_path);
    cleanup_test_tmpdir(tmpdir);
}

static void test_compute_day_stats_nonexistent(void)
{
    DayStats *stats = compute_day_stats("/nonexistent/path.csv");
    g_assert_null(stats);
}

/* ── stats report options ─────────────────────────────────── */

static gchar *capture_stats_output(const DayStats *stats,
                                   int year, int month, int day,
                                   const StatsOptions *opts)
{
    FILE *tmp = tmpfile();
    g_assert_nonnull(tmp);
    print_stats_report(tmp, stats, year, month, day, opts);
    long len = ftell(tmp);
    rewind(tmp);
    gchar *buf = g_malloc(len + 1);
    size_t read = fread(buf, 1, len, tmp);
    (void)read;
    buf[len] = '\0';
    fclose(tmp);
    return buf;
}

static void test_stats_top_apps_limit(void)
{
    gchar *tmpdir = create_test_tmpdir();
    gchar *csv_path = g_strdup_printf("%s/test.csv", tmpdir);

    /* Create 5 apps */
    GString *csv = g_string_new(
        "timestamp,duration_seconds,status,window_title,wm_class,wm_class_instance\n");
    for (int i = 0; i < 5; i++) {
        g_string_append_printf(csv,
            "2026-01-28T10:%02d:00,%d,active,\"Win\",\"App%d\",\"app%d\"\n",
            i, (5 - i) * 100, i, i);
    }
    g_file_set_contents(csv_path, csv->str, -1, NULL);
    g_string_free(csv, TRUE);

    DayStats *stats = compute_day_stats(csv_path);
    g_assert_nonnull(stats);
    g_assert_cmpuint(stats->apps->len, ==, 5);

    StatsOptions opts = { .top_apps = 2, .top_titles = 5 };
    gchar *output = capture_stats_output(stats, 2026, 1, 28, &opts);

    /* Should show "3 other applications" */
    g_assert_nonnull(strstr(output, "3 other applications"));
    /* Should show apps numbered 1. and 2. */
    g_assert_nonnull(strstr(output, "  1."));
    g_assert_nonnull(strstr(output, "  2."));
    /* Should NOT show app 3 */
    g_assert_null(strstr(output, "  3."));

    g_free(output);
    free_day_stats(stats);
    g_free(csv_path);
    cleanup_test_tmpdir(tmpdir);
}

static void test_stats_top_titles_limit(void)
{
    gchar *tmpdir = create_test_tmpdir();
    gchar *csv_path = g_strdup_printf("%s/test.csv", tmpdir);

    /* Create 1 app with 5 different titles */
    GString *csv = g_string_new(
        "timestamp,duration_seconds,status,window_title,wm_class,wm_class_instance\n");
    for (int i = 0; i < 5; i++) {
        g_string_append_printf(csv,
            "2026-01-28T10:%02d:00,%d,active,\"Title %d\",\"Firefox\",\"navigator\"\n",
            i, (5 - i) * 60, i);
    }
    g_file_set_contents(csv_path, csv->str, -1, NULL);
    g_string_free(csv, TRUE);

    DayStats *stats = compute_day_stats(csv_path);
    g_assert_nonnull(stats);

    StatsOptions opts = { .top_apps = 20, .top_titles = 1 };
    gchar *output = capture_stats_output(stats, 2026, 1, 28, &opts);

    /* Should show "4 other windows" */
    g_assert_nonnull(strstr(output, "4 other windows"));

    g_free(output);
    free_day_stats(stats);
    g_free(csv_path);
    cleanup_test_tmpdir(tmpdir);
}

/* ── truncation / overflow protection ─────────────────────── */

static void test_format_long_app_name(void)
{
    gchar *tmpdir = create_test_tmpdir();
    gchar *csv_path = g_strdup_printf("%s/test.csv", tmpdir);

    /* App name with 80 characters */
    gchar long_name[81];
    memset(long_name, 'A', 80);
    long_name[80] = '\0';

    GString *csv = g_string_new(
        "timestamp,duration_seconds,status,window_title,wm_class,wm_class_instance\n");
    g_string_append_printf(csv,
        "2026-01-28T10:00:00,60,active,\"Win\",\"%s\",\"inst\"\n", long_name);
    g_file_set_contents(csv_path, csv->str, -1, NULL);
    g_string_free(csv, TRUE);

    DayStats *stats = compute_day_stats(csv_path);
    g_assert_nonnull(stats);

    StatsOptions opts = { .top_apps = 20, .top_titles = 5 };
    gchar *output = capture_stats_output(stats, 2026, 1, 28, &opts);

    /* Verify app name is truncated with "..." */
    g_assert_nonnull(strstr(output, "..."));

    /* Verify no output line exceeds a reasonable width */
    gchar **lines = g_strsplit(output, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (lines[i][0] == '\0')
            continue;
        g_assert_cmpuint(strlen(lines[i]), <=, 60);
    }
    g_strfreev(lines);

    g_free(output);
    free_day_stats(stats);
    g_free(csv_path);
    cleanup_test_tmpdir(tmpdir);
}

static void test_format_long_window_title(void)
{
    gchar *tmpdir = create_test_tmpdir();
    gchar *csv_path = g_strdup_printf("%s/test.csv", tmpdir);

    /* Window title with 120 characters */
    gchar long_title[121];
    memset(long_title, 'T', 120);
    long_title[120] = '\0';

    GString *csv = g_string_new(
        "timestamp,duration_seconds,status,window_title,wm_class,wm_class_instance\n");
    g_string_append_printf(csv,
        "2026-01-28T10:00:00,60,active,\"%s\",\"App\",\"app\"\n", long_title);
    g_file_set_contents(csv_path, csv->str, -1, NULL);
    g_string_free(csv, TRUE);

    DayStats *stats = compute_day_stats(csv_path);
    g_assert_nonnull(stats);

    StatsOptions opts = { .top_apps = 20, .top_titles = 5 };
    gchar *output = capture_stats_output(stats, 2026, 1, 28, &opts);

    /* Verify title is truncated with "..." */
    g_assert_nonnull(strstr(output, "..."));

    /* Verify no output line exceeds a reasonable width */
    gchar **lines = g_strsplit(output, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (lines[i][0] == '\0')
            continue;
        g_assert_cmpuint(strlen(lines[i]), <=, 60);
    }
    g_strfreev(lines);

    g_free(output);
    free_day_stats(stats);
    g_free(csv_path);
    cleanup_test_tmpdir(tmpdir);
}

static void test_no_line_overflow(void)
{
    gchar *tmpdir = create_test_tmpdir();
    gchar *csv_path = g_strdup_printf("%s/test.csv", tmpdir);

    /* Create data with various extreme-length names */
    gchar name80[81], title120[121];
    memset(name80, 'X', 80); name80[80] = '\0';
    memset(title120, 'Y', 120); title120[120] = '\0';

    GString *csv = g_string_new(
        "timestamp,duration_seconds,status,window_title,wm_class,wm_class_instance\n");
    g_string_append_printf(csv,
        "2026-01-28T10:00:00,3661,active,\"%s\",\"%s\",\"inst\"\n",
        title120, name80);
    g_string_append_printf(csv,
        "2026-01-28T11:00:00,300,locked,\"\",\"\",\"\"\n");
    g_file_set_contents(csv_path, csv->str, -1, NULL);
    g_string_free(csv, TRUE);

    DayStats *stats = compute_day_stats(csv_path);
    g_assert_nonnull(stats);

    StatsOptions opts = { .top_apps = 20, .top_titles = 5 };
    gchar *output = capture_stats_output(stats, 2026, 1, 28, &opts);

    /* No line should be excessively long */
    gchar **lines = g_strsplit(output, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (lines[i][0] == '\0')
            continue;
        g_assert_cmpuint(strlen(lines[i]), <=, 60);
    }
    g_strfreev(lines);

    g_free(output);
    free_day_stats(stats);
    g_free(csv_path);
    cleanup_test_tmpdir(tmpdir);
}

/* ── main ──────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/format/iso8601", test_format_iso8601);
    g_test_add_func("/csv/escape_simple", test_csv_escape_simple);
    g_test_add_func("/csv/escape_with_quotes", test_csv_escape_with_quotes);
    g_test_add_func("/csv/escape_empty", test_csv_escape_empty);
    g_test_add_func("/tracking/start", test_start_tracking);
    g_test_add_func("/tracking/start_null_title", test_start_tracking_null_title);
    g_test_add_func("/emit/csv_active", test_emit_csv_active);
    g_test_add_func("/emit/csv_locked", test_emit_csv_locked);
    g_test_add_func("/emit/csv_skips_short_duration", test_emit_csv_skips_short_duration);
    g_test_add_func("/emit/csv_no_title", test_emit_csv_no_title);
    g_test_add_func("/parse/focused_window_found", test_parse_focused_window_found);
    g_test_add_func("/parse/focused_window_none", test_parse_focused_window_none);
    g_test_add_func("/parse/focused_window_empty_array", test_parse_focused_window_empty_array);
    g_test_add_func("/parse/focused_window_malformed", test_parse_focused_window_malformed);
    g_test_add_func("/parse/focused_window_null", test_parse_focused_window_null);
    g_test_add_func("/parse/focused_window_empty_string", test_parse_focused_window_empty_string);
    g_test_add_func("/parse/focused_window_missing_wm_fields", test_parse_focused_window_missing_wm_fields);
    g_test_add_func("/file/ensure_output_creates", test_ensure_output_creates);
    g_test_add_func("/file/ensure_output_same_date", test_ensure_output_same_date);
    g_test_add_func("/file/ensure_output_date_rotation", test_ensure_output_date_rotation);
    g_test_add_func("/file/close_output_file", test_close_output_file);
    g_test_add_func("/file/ensure_output_appends", test_ensure_output_appends);
    g_test_add_func("/file/csv_escape_print_fp", test_csv_escape_print_fp);

    /* Statistics tests */
    g_test_add_func("/stats/format_duration_hours", test_format_duration_hours);
    g_test_add_func("/stats/format_duration_minutes", test_format_duration_minutes);
    g_test_add_func("/stats/format_duration_seconds", test_format_duration_seconds);
    g_test_add_func("/stats/format_duration_zero", test_format_duration_zero);
    g_test_add_func("/stats/format_duration_large", test_format_duration_large);
    g_test_add_func("/stats/parse_csv_simple", test_parse_csv_simple);
    g_test_add_func("/stats/parse_csv_quoted_title", test_parse_csv_quoted_title);
    g_test_add_func("/stats/parse_csv_header_line", test_parse_csv_header_line);
    g_test_add_func("/stats/parse_csv_locked", test_parse_csv_locked);
    g_test_add_func("/stats/parse_csv_empty_line", test_parse_csv_empty_line);
    g_test_add_func("/stats/build_csv_path", test_build_csv_path);
    g_test_add_func("/stats/build_csv_path_padding", test_build_csv_path_padding);
    g_test_add_func("/stats/compute_day_stats", test_compute_day_stats);
    g_test_add_func("/stats/compute_day_stats_empty", test_compute_day_stats_empty);
    g_test_add_func("/stats/compute_day_stats_nonexistent", test_compute_day_stats_nonexistent);
    g_test_add_func("/stats/top_apps_limit", test_stats_top_apps_limit);
    g_test_add_func("/stats/top_titles_limit", test_stats_top_titles_limit);
    g_test_add_func("/stats/format_long_app_name", test_format_long_app_name);
    g_test_add_func("/stats/format_long_window_title", test_format_long_window_title);
    g_test_add_func("/stats/no_line_overflow", test_no_line_overflow);

    return g_test_run();
}
