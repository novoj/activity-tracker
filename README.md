# Activity Tracker

A lightweight C application that tracks which desktop window is active on a GNOME/Wayland session, detects when the screen is locked (user is AFK), and writes time-tracking data as daily CSV files under `~/.local/share/activity-tracker/`.

## Architecture

The application is a single-threaded C program built on the GLib main loop with three subsystems:

1. **Window Poller** - A 1-second GLib timeout callback that calls the [Window Calls](https://extensions.gnome.org/extension/4724/window-calls/) GNOME Shell extension's D-Bus `List` method to retrieve all windows as JSON, then finds the focused window's title. When the title changes from the previously tracked window, a CSV line is emitted for the completed interval.

2. **Lock Monitor** - A GDBus signal subscription to `org.gnome.ScreenSaver.ActiveChanged`. When the screen locks, the current window tracking interval is finalized and a "locked" interval begins. On unlock, the locked interval is emitted and active window tracking resumes.

3. **CSV Emitter** - Writes a line to a daily CSV file whenever a tracking interval ends (window change, lock/unlock, or shutdown). Each line is flushed and fsynced for crash safety. Files are automatically rotated at midnight.

Signal handlers for `SIGINT` and `SIGTERM` ensure the final tracking interval is emitted before the application exits.

### D-Bus Interfaces Used

| Interface | Method/Signal | Purpose |
|---|---|---|
| `org.gnome.Shell.Extensions.Windows` | `List()` | Get JSON array of all windows with focus state |
| `org.gnome.ScreenSaver` | `ActiveChanged(boolean)` | Detect screen lock/unlock |

## Prerequisites

- Ubuntu 24.04 (or compatible) with GNOME desktop on Wayland
- GCC, Make, pkg-config
- GLib/GIO development headers
- json-glib development headers
- [Window Calls](https://extensions.gnome.org/extension/4724/window-calls/) GNOME Shell extension

### Installing the Window Calls Extension

The Window Calls GNOME Shell extension exposes window information over D-Bus, which this application uses to detect the focused window.

**Option A: GNOME Extensions website**

1. Visit https://extensions.gnome.org/extension/4724/window-calls/
2. Toggle the switch to install and enable

**Option B: Extension Manager app**

```sh
sudo apt install gnome-shell-extension-manager
```

Open Extension Manager, search for "Window Calls", and install it.

**Verify it's working:**

```sh
gdbus call --session \
  --dest org.gnome.Shell \
  --object-path /org/gnome/Shell/Extensions/Windows \
  --method org.gnome.Shell.Extensions.Windows.List
```

This should return a JSON string containing your open windows.

## Installation

Install build dependencies:

```sh
sudo apt install build-essential libglib2.0-dev libjson-glib-dev pkg-config
```

Build the application:

```sh
git clone https://github.com/novoj/activity-tracker.git && cd activity-tracker
make
```

Run the tests:

```sh
make test
```

Optionally install system-wide:

```sh
sudo cp activity-tracker /usr/local/bin/
```

## Usage

```sh
./activity-tracker
```

CSV files are written to `~/.local/share/activity-tracker/` (or `$XDG_DATA_HOME/activity-tracker/`), organized by month:

```
~/.local/share/activity-tracker/
  2026-01/
    2026-01-28.csv
    2026-01-29.csv
  2026-02/
    2026-02-01.csv
```

Stop with `Ctrl+C` - the final interval will be flushed to disk before exit.

### GNOME Autostart

To run automatically on login, create `~/.config/autostart/activity-tracker.desktop`:

```ini
[Desktop Entry]
Type=Application
Name=Activity Tracker
Exec=/usr/local/bin/activity-tracker
Hidden=false
NoDisplay=true
X-GNOME-Autostart-enabled=true
Comment=Track active window time
```

## Output Format

CSV with the following columns:

| Column | Type | Description |
|---|---|---|
| `timestamp` | ISO 8601 (`YYYY-MM-DDTHH:MM:SS`) | When the interval started |
| `duration_seconds` | integer | Seconds spent in this interval |
| `status` | `active` or `locked` | Whether the user was active or AFK |
| `window_title` | quoted string | Title of the focused window (empty when locked) |

Example output:

```csv
timestamp,duration_seconds,status,window_title
2026-01-28T14:30:05,307,active,"Firefox - Google"
2026-01-28T14:35:12,120,locked,""
2026-01-28T14:37:12,45,active,"Terminal - bash"
```

## Limitations

- **GNOME Shell + Window Calls extension required** - The application uses the Window Calls GNOME Shell extension's D-Bus interface. This will not work on KDE Plasma, Sway, Hyprland, or other Wayland compositors, and the extension must be installed and enabled.
- **Requires active D-Bus session** - Must be run within a graphical session with access to the session bus.
- **1-second granularity** - Window changes shorter than 1 second may not be captured.
