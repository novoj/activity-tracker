# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

A C application that tracks active window time on GNOME/Wayland. It monitors the focused window via D-Bus (using the Window Calls GNOME Shell extension) and detects screen lock/unlock events, writing CSV records to daily files under `~/.local/share/activity-tracker/` with 1-second polling granularity. When a second instance is launched (or `--stats` is passed), it displays an activity report instead of tracking.

## Build Commands

```bash
make            # Build the activity-tracker binary
make test       # Build and run the test suite (test-tracker)
make clean      # Remove compiled artifacts
```

There is no separate lint step; the compiler flags `-Wall -Wextra` serve as the primary static analysis.

## Architecture

The project is a flat C codebase with three source files:

- **activity-tracker.c** — Entry point and event wiring. Sets up a GLib main loop with two event sources: a 1-second timeout that polls `org.gnome.Shell.Extensions.Windows.List()` via D-Bus for the focused window, and a signal subscription to `org.gnome.ScreenSaver.ActiveChanged` for lock/unlock detection. Handles SIGINT/SIGTERM for graceful shutdown.

- **tracker-core.c / tracker-core.h** — Pure logic with no D-Bus dependencies. Contains CSV formatting/escaping, ISO 8601 timestamp formatting, JSON parsing of the window list, and tracking state management. All functions operate on the `AppState` struct or are stateless utilities.

- **test-tracker.c** — GLib gtest suite covering tracker-core functions. Does not test D-Bus integration.

The separation means tracker-core can be tested without a running GNOME session.

## Key Data Structure

`AppState` holds all runtime state: the GLib main loop, D-Bus proxy/connection, current window title, interval start times (both monotonic and wall clock), and lock status. It is passed through all callbacks.

## Usage

```bash
activity-tracker                # Start tracking (or show stats if already running)
activity-tracker --stats        # Show today's activity report and exit
activity-tracker --date 2026-01-27  # Show report for a specific date
activity-tracker --top-apps 5   # Show only top 5 applications (default: 20)
activity-tracker --top-titles 2 # Show only top 2 window titles per app (default: 5)
activity-tracker --grep firefox # Show only apps/titles matching "firefox" (regex, case-insensitive)
activity-tracker --help         # Show help message
```

Short flags: `-s` (stats), `-d` (date), `-n` (top-apps), `-t` (top-titles), `-g` (grep), `-h` (help).

Only one instance can track at a time (enforced via `flock()` on a lock file). A second instance automatically shows the activity report for today.

## Output Format

CSV to daily files: `timestamp,duration_seconds,status,window_title,wm_class,wm_class_instance` where status is "active" or "locked". Files are stored at `~/.local/share/activity-tracker/YYYY-MM/YYYY-MM-DD.csv`.

## Git Conventions

Repository: https://github.com/novoj/activity-tracker

### Commit Messages

Use [Conventional Commits](https://www.conventionalcommits.org/) style:

```
<type>: <short summary>
```

Types: `feat`, `fix`, `refactor`, `test`, `docs`, `chore`, `build`, `ci`.

- Keep the subject line under 72 characters.
- Use imperative mood ("add feature" not "added feature").
- Do **not** include `Co-Authored-By`, `Signed-off-by`, or any author/co-author trailers in commits.

## Dependencies

System packages required: `libglib2.0-dev`, `libjson-glib-dev`, `pkg-config`, GCC. Runtime requires GNOME Shell with the Window Calls extension installed.
