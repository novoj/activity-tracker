#ifndef TRACKER_CORE_H
#define TRACKER_CORE_H

#include <gio/gio.h>
#include <stdio.h>
#include <time.h>

typedef struct {
    GMainLoop *loop;
    GDBusProxy *shell_proxy;
    GDBusConnection *connection;
    guint screensaver_signal_id;
    gchar *current_title;
    gchar *current_wm_class;
    gchar *current_wm_class_instance;
    gint64 current_start;   /* monotonic time in microseconds */
    time_t current_wall;    /* wall clock at start */
    gboolean is_locked;
    GDBusProxy *idle_proxy; /* Proxy to org.gnome.Mutter.IdleMonitor */
    gboolean is_idle;       /* TRUE when user is idle */
    FILE *output_fp;        /* current output file handle */
    int file_year;          /* year of open file */
    int file_month;         /* month (1-12) of open file */
    int file_day;           /* day (1-31) of open file */
    const gchar *data_dir;  /* override for g_get_user_data_dir(), NULL = default */
    gchar *current_rp_state;   /* Discord rich presence state */
    gchar *current_rp_details; /* Discord rich presence details */
    pid_t current_pid;         /* PID of current focused window */
} AppState;

typedef struct {
    gchar *title;
    gchar *wm_class;
    gchar *wm_class_instance;
    pid_t pid;
} FocusedWindowInfo;

void format_iso8601(time_t t, char *buf, size_t len);
void csv_escape_to_buffer(GString *buf, const char *field);
void csv_escape_and_print(const char *field);
void emit_csv_to_buffer(GString *buf, AppState *state, gint64 now);
void emit_csv_line(AppState *state);
void start_tracking(AppState *state, const gchar *title,
                    const gchar *wm_class, const gchar *wm_class_instance,
                    const gchar *rp_state, const gchar *rp_details,
                    pid_t pid, gboolean locked);
FocusedWindowInfo parse_focused_window(const gchar *json);
void free_focused_window_info(FocusedWindowInfo *info);

gboolean ensure_output_file(AppState *state, time_t wall_time);
void close_output_file(AppState *state);
void csv_escape_and_print_fp(FILE *fp, const char *field);

/* ── Statistics ──────────────────────────────────────────── */

typedef struct {
    gchar *wm_class;
    long total_seconds;
    GHashTable *titles; /* gchar* -> long* (title -> cumulative seconds) */
} AppStat;

typedef struct {
    long total_active_seconds;
    long total_locked_seconds;
    GPtrArray *apps; /* AppStat*, sorted descending by total_seconds */
} DayStats;

typedef struct {
    int top_apps;              /* max apps to display (default 20) */
    int top_titles;            /* max titles per app (default 5) */
    const gchar *grep_pattern; /* regex filter, NULL = no filter */
    int cols;                  /* output width in columns (default 80) */
} StatsOptions;

gchar *build_csv_path(const gchar *data_dir_override,
                      int year, int month, int day);
gchar *format_duration(long seconds);
gboolean parse_csv_line(const gchar *line,
                        gchar **timestamp, long *duration,
                        gchar **status, gchar **window_title,
                        gchar **wm_class, gchar **wm_class_instance,
                        gchar **rp_state, gchar **rp_details);
DayStats *compute_day_stats(const gchar *csv_path);
DayStats *filter_stats_by_grep(const DayStats *stats, const gchar *pattern,
                               GError **error);
void print_stats_report(FILE *out, const DayStats *stats,
                        int year, int month, int day,
                        const StatsOptions *opts);
void free_day_stats(DayStats *stats);

#endif /* TRACKER_CORE_H */
