/*
 * activity-tracker - Track active window time on GNOME/Wayland
 *
 * Polls the active window title via the Window Calls GNOME Shell
 * extension's D-Bus interface, monitors screen lock via
 * org.gnome.ScreenSaver, and outputs CSV-formatted tracking data
 * to daily files under ~/.local/share/activity-tracker/.
 */

#include <gio/gio.h>
#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/file.h>
#include <fcntl.h>
#include <getopt.h>

#include "tracker-core.h"

#define POLL_INTERVAL_MS 1000
#define IDLE_THRESHOLD_MS (5 * 60 * 1000)  /* 5 minutes */

static FocusedWindowInfo query_active_window(AppState *state)
{
    FocusedWindowInfo info = {NULL, NULL, NULL};

    if (!state->shell_proxy)
        return info;

    GError *error = NULL;
    GVariant *result = g_dbus_proxy_call_sync(
        state->shell_proxy,
        "List",
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        500, /* timeout ms */
        NULL,
        &error);

    if (error) {
        g_error_free(error);
        return info;
    }

    const gchar *json_str = NULL;
    if (!g_variant_is_of_type(result, G_VARIANT_TYPE("(s)"))) {
        g_variant_unref(result);
        return info;
    }
    g_variant_get(result, "(&s)", &json_str);

    info = parse_focused_window(json_str);

    g_variant_unref(result);
    return info;
}

static gboolean query_screensaver_active(AppState *state)
{
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        state->connection,
        "org.gnome.ScreenSaver",
        "/org/gnome/ScreenSaver",
        "org.gnome.ScreenSaver",
        "GetActive",
        NULL,
        G_VARIANT_TYPE("(b)"),
        G_DBUS_CALL_FLAGS_NONE,
        500,
        NULL,
        &error);

    if (error) {
        g_error_free(error);
        return FALSE;  /* assume unlocked on failure */
    }

    gboolean active = FALSE;
    if (!g_variant_is_of_type(result, G_VARIANT_TYPE("(b)"))) {
        g_variant_unref(result);
        return FALSE;
    }
    g_variant_get(result, "(b)", &active);
    g_variant_unref(result);
    return active;
}

static guint64 query_idle_time(AppState *state)
{
    if (!state->idle_proxy)
        return 0;

    GError *error = NULL;
    GVariant *result = g_dbus_proxy_call_sync(
        state->idle_proxy,
        "GetIdletime",
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        500,
        NULL,
        &error);

    if (error) {
        g_error_free(error);
        return 0;
    }

    guint64 idle_ms = 0;
    if (g_variant_is_of_type(result, G_VARIANT_TYPE("(t)")))
        g_variant_get(result, "(t)", &idle_ms);
    g_variant_unref(result);
    return idle_ms;
}

static gboolean on_poll_timeout(gpointer user_data)
{
    AppState *state = user_data;

    if (state->is_locked)
        return G_SOURCE_CONTINUE;

    guint64 idle_ms = query_idle_time(state);

    if (idle_ms >= IDLE_THRESHOLD_MS && !state->is_idle) {
        emit_csv_line(state);
        state->is_idle = TRUE;
        start_tracking(state, "", "", "", FALSE);
        return G_SOURCE_CONTINUE;
    }

    if (idle_ms < IDLE_THRESHOLD_MS && state->is_idle) {
        emit_csv_line(state);
        state->is_idle = FALSE;
        FocusedWindowInfo info = query_active_window(state);
        start_tracking(state, info.title ? info.title : "",
                       info.wm_class, info.wm_class_instance, FALSE);
        free_focused_window_info(&info);
        return G_SOURCE_CONTINUE;
    }

    if (state->is_idle)
        return G_SOURCE_CONTINUE;

    FocusedWindowInfo info = query_active_window(state);
    if (!info.title) {
        free_focused_window_info(&info);
        return G_SOURCE_CONTINUE;
    }

    if (!state->current_title || g_strcmp0(state->current_title, info.title) != 0) {
        emit_csv_line(state);
        start_tracking(state, info.title, info.wm_class, info.wm_class_instance, FALSE);
    }

    free_focused_window_info(&info);
    return G_SOURCE_CONTINUE;
}

static void on_screensaver_signal(GDBusConnection *connection G_GNUC_UNUSED,
                                  const gchar *sender_name G_GNUC_UNUSED,
                                  const gchar *object_path G_GNUC_UNUSED,
                                  const gchar *interface_name G_GNUC_UNUSED,
                                  const gchar *signal_name G_GNUC_UNUSED,
                                  GVariant *parameters,
                                  gpointer user_data)
{
    AppState *state = user_data;
    gboolean active;
    if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(b)")))
        return;
    g_variant_get(parameters, "(b)", &active);

    if (active) {
        /* Screen locked — lock takes precedence over idle */
        state->is_idle = FALSE;
        emit_csv_line(state);
        start_tracking(state, "", "", "", TRUE);
    } else {
        /* Screen unlocked — user just interacted */
        state->is_idle = FALSE;
        emit_csv_line(state);
        /* Query current window to resume tracking */
        FocusedWindowInfo info = query_active_window(state);
        start_tracking(state, info.title ? info.title : "",
                       info.wm_class, info.wm_class_instance, FALSE);
        free_focused_window_info(&info);
    }
}

static gboolean on_signal(gpointer user_data)
{
    AppState *state = user_data;
    emit_csv_line(state);
    g_main_loop_quit(state->loop);
    return G_SOURCE_REMOVE;
}

/* ── Lock file for single-instance detection ─────────── */

static gchar *build_lock_path(void)
{
    const gchar *data_dir = g_get_user_data_dir();
    return g_strdup_printf("%s/activity-tracker/lock", data_dir);
}

/* Try to acquire an exclusive lock.
 * Returns fd >= 0 on success (caller keeps it open), -1 if already locked. */
static int try_acquire_lock(void)
{
    gchar *lock_path = build_lock_path();
    gchar *dir = g_path_get_dirname(lock_path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    int fd = open(lock_path, O_RDWR | O_CREAT, 0600);
    g_free(lock_path);
    if (fd < 0)
        return -1;

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ── Stats mode ──────────────────────────────────────── */

static int run_stats_mode(int year, int month, int day,
                          const StatsOptions *opts)
{
    gchar *csv_path = build_csv_path(NULL, year, month, day);

    if (!g_file_test(csv_path, G_FILE_TEST_EXISTS)) {
        g_printerr("No activity data for %04d-%02d-%02d.\n",
                    year, month, day);
        g_free(csv_path);
        return 1;
    }

    DayStats *stats = compute_day_stats(csv_path);
    g_free(csv_path);

    if (!stats) {
        g_printerr("Failed to parse activity data.\n");
        return 1;
    }

    if (opts->grep_pattern) {
        GError *error = NULL;
        DayStats *filtered = filter_stats_by_grep(stats, opts->grep_pattern,
                                                   &error);
        free_day_stats(stats);
        if (!filtered) {
            g_printerr("Invalid grep pattern: %s\n", error->message);
            g_error_free(error);
            return 1;
        }
        stats = filtered;
    }

    print_stats_report(stdout, stats, year, month, day, opts);
    free_day_stats(stats);
    return 0;
}

/* ── Tracker mode (original main body) ───────────────── */

static int run_tracker_mode(int lock_fd)
{
    AppState state = {0};
    GError *error = NULL;
    int ret = 1;

    state.loop = g_main_loop_new(NULL, FALSE);

    /* Connect to session bus */
    state.connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (error) {
        g_printerr("Failed to connect to session bus: %s\n", error->message);
        g_error_free(error);
        goto cleanup;
    }

    /* Create proxy for Window Calls GNOME Shell extension */
    state.shell_proxy = g_dbus_proxy_new_sync(
        state.connection,
        G_DBUS_PROXY_FLAGS_NONE,
        NULL,
        "org.gnome.Shell",
        "/org/gnome/Shell/Extensions/Windows",
        "org.gnome.Shell.Extensions.Windows",
        NULL,
        &error);
    if (error) {
        g_printerr("Failed to create Window Calls proxy: %s\n", error->message);
        g_error_free(error);
        goto cleanup;
    }

    /* Create proxy for Mutter idle monitor (optional — graceful degradation) */
    state.idle_proxy = g_dbus_proxy_new_sync(
        state.connection,
        G_DBUS_PROXY_FLAGS_NONE,
        NULL,
        "org.gnome.Mutter.IdleMonitor",
        "/org/gnome/Mutter/IdleMonitor/Core",
        "org.gnome.Mutter.IdleMonitor",
        NULL,
        &error);
    if (error) {
        g_warning("Idle monitor unavailable, idle detection disabled: %s",
                  error->message);
        g_clear_error(&error);
    }

    /* Subscribe to screen lock signals */
    state.screensaver_signal_id = g_dbus_connection_signal_subscribe(
        state.connection,
        "org.gnome.ScreenSaver",
        "org.gnome.ScreenSaver",
        "ActiveChanged",
        "/org/gnome/ScreenSaver",
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_screensaver_signal,
        &state,
        NULL);

    /* Open initial output file */
    if (!ensure_output_file(&state, time(NULL))) {
        g_printerr("Failed to open output file\n");
        goto cleanup;
    }

    /* Initialize tracking with current state */
    gboolean initially_locked = query_screensaver_active(&state);
    if (initially_locked) {
        start_tracking(&state, "", "", "", TRUE);
    } else if (query_idle_time(&state) >= IDLE_THRESHOLD_MS) {
        state.is_idle = TRUE;
        start_tracking(&state, "", "", "", FALSE);
    } else {
        FocusedWindowInfo info = query_active_window(&state);
        start_tracking(&state, info.title ? info.title : "",
                       info.wm_class, info.wm_class_instance, FALSE);
        free_focused_window_info(&info);
    }

    /* Set up polling timer */
    g_timeout_add(POLL_INTERVAL_MS, on_poll_timeout, &state);

    /* Handle SIGINT and SIGTERM for clean shutdown */
    g_unix_signal_add(SIGINT, on_signal, &state);
    g_unix_signal_add(SIGTERM, on_signal, &state);

    g_main_loop_run(state.loop);
    ret = 0;

cleanup:
    close_output_file(&state);
    if (state.screensaver_signal_id)
        g_dbus_connection_signal_unsubscribe(state.connection,
                                             state.screensaver_signal_id);
    g_clear_object(&state.idle_proxy);
    g_clear_object(&state.shell_proxy);
    g_clear_object(&state.connection);
    g_free(state.current_title);
    g_free(state.current_wm_class);
    g_free(state.current_wm_class_instance);
    g_main_loop_unref(state.loop);
    if (lock_fd >= 0)
        close(lock_fd);

    return ret;
}

/* ── CLI ─────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    g_printerr(
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Track active window time on GNOME/Wayland. Without options, starts\n"
        "tracking. If another instance is already running, prints today's\n"
        "activity report instead.\n"
        "\n"
        "Options:\n"
        "  -s, --stats              Show activity report and exit\n"
        "  -d, --date YYYY-MM-DD    Report for a specific date (default: today)\n"
        "  -n, --top-apps N         Number of applications to show (default: 20)\n"
        "  -t, --top-titles N       Window titles per application (default: 5)\n"
        "  -g, --grep PATTERN       Filter by regex on app names and titles\n"
        "  -h, --help               Show this help message\n",
        prog);
}

/* Parse YYYY-MM-DD into year/month/day. Returns TRUE on success. */
static gboolean parse_date(const char *str, int *year, int *month, int *day)
{
    if (!str || strlen(str) != 10 || str[4] != '-' || str[7] != '-')
        return FALSE;
    if (sscanf(str, "%d-%d-%d", year, month, day) != 3)
        return FALSE;
    if (*month < 1 || *month > 12 || *day < 1 || *day > 31)
        return FALSE;
    return TRUE;
}

int main(int argc, char *argv[])
{
    gboolean explicit_stats = FALSE;
    const char *date_str = NULL;
    StatsOptions opts = { .top_apps = 20, .top_titles = 5, .grep_pattern = NULL };

    static struct option long_options[] = {
        {"stats",      no_argument,       NULL, 's'},
        {"date",       required_argument, NULL, 'd'},
        {"top-apps",   required_argument, NULL, 'n'},
        {"top-titles", required_argument, NULL, 't'},
        {"grep",       required_argument, NULL, 'g'},
        {"help",       no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "sd:n:t:g:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 's':
            explicit_stats = TRUE;
            break;
        case 'd':
            date_str = optarg;
            explicit_stats = TRUE;
            break;
        case 'n': {
            char *endptr;
            errno = 0;
            long val = strtol(optarg, &endptr, 10);
            if (errno || *endptr != '\0' || val < 1 || val > INT_MAX) {
                g_printerr("--top-apps must be a positive integer\n");
                return 1;
            }
            opts.top_apps = (int)val;
            break;
        }
        case 't': {
            char *endptr;
            errno = 0;
            long val = strtol(optarg, &endptr, 10);
            if (errno || *endptr != '\0' || val < 1 || val > INT_MAX) {
                g_printerr("--top-titles must be a positive integer\n");
                return 1;
            }
            opts.top_titles = (int)val;
            break;
        }
        case 'g':
            opts.grep_pattern = optarg;
            explicit_stats = TRUE;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Resolve date */
    int year, month, day;
    if (date_str) {
        if (!parse_date(date_str, &year, &month, &day)) {
            g_printerr("Invalid date format: %s (expected YYYY-MM-DD)\n",
                        date_str);
            return 1;
        }
    } else {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        year = tm.tm_year + 1900;
        month = tm.tm_mon + 1;
        day = tm.tm_mday;
    }

    if (explicit_stats)
        return run_stats_mode(year, month, day, &opts);

    /* Auto-detect: try to acquire lock */
    int lock_fd = try_acquire_lock();
    if (lock_fd < 0)
        return run_stats_mode(year, month, day, &opts);

    return run_tracker_mode(lock_fd);
}
