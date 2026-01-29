#ifndef DISCORD_IPC_H
#define DISCORD_IPC_H

#include <gio/gio.h>
#include <sys/types.h>

/* ── Discord IPC protocol constants ─────────────────── */

#define DISCORD_OP_HANDSHAKE 0
#define DISCORD_OP_FRAME     1
#define DISCORD_HEADER_SIZE  8

/* ── Data structures ────────────────────────────────── */

typedef struct {
    gchar *state;
    gchar *details;
    pid_t pid;
    gint64 last_updated;  /* monotonic time */
} RichPresenceEntry;

typedef struct _ClientConnection ClientConnection;

typedef struct {
    gchar *ipc_path;            /* $XDG_RUNTIME_DIR/discord-ipc-0 */
    gchar *real_ipc_path;       /* $XDG_RUNTIME_DIR/discord-ipc-original */
    gboolean upstream_active;
    int server_fd;              /* listening socket fd */
    GSource *server_source;     /* GSource for accept */
    GHashTable *presence_by_pid; /* GINT_TO_POINTER(pid) → RichPresenceEntry* */
    GPtrArray *connections;     /* active ClientConnection* */
    gboolean active;            /* TRUE when proxy is running */
} DiscordIpcState;

/* ── Lifecycle ──────────────────────────────────────── */

gboolean discord_ipc_setup(DiscordIpcState *state);
void discord_ipc_cleanup(DiscordIpcState *state);

/* ── Lookup ─────────────────────────────────────────── */

const RichPresenceEntry *discord_ipc_lookup_pid(DiscordIpcState *state, pid_t pid);

/* ── Testable helpers ───────────────────────────────── */

gboolean discord_parse_frame(const guint8 *data, gsize len,
                             guint32 *opcode, gchar **json_out,
                             gsize *consumed);

gboolean discord_extract_activity(const gchar *json,
                                  pid_t *pid, gchar **state, gchar **details);

void discord_build_ready_response(guint8 **out, gsize *out_len);

void discord_presence_store(DiscordIpcState *state, pid_t pid,
                            const gchar *rp_state, const gchar *rp_details);

gboolean is_discord_socket_alive(const gchar *path);

#endif /* DISCORD_IPC_H */
