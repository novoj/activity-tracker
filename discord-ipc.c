#include "discord-ipc.h"
#include <json-glib/json-glib.h>
#include <glib-unix.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>

/* ── Per-connection state ───────────────────────────── */

struct _ClientConnection {
    int client_fd;
    int upstream_fd;     /* -1 if no upstream (passive mode) */
    GSource *client_source;
    GSource *upstream_source;
    GByteArray *client_buf;
    GByteArray *upstream_buf;
    DiscordIpcState *ipc_state;
    gboolean handshake_done;
};

/* ── Forward declarations ───────────────────────────── */

static void close_connection(ClientConnection *conn);
static gboolean on_server_accept(gint fd, GIOCondition cond, gpointer user_data);
static gboolean on_client_data(gint fd, GIOCondition cond, gpointer user_data);
static gboolean on_upstream_data(gint fd, GIOCondition cond, gpointer user_data);
static void process_client_buffer(ClientConnection *conn);

/* ── RichPresenceEntry lifecycle ────────────────────── */

static void free_rp_entry(gpointer data)
{
    RichPresenceEntry *e = data;
    g_free(e->state);
    g_free(e->details);
    g_free(e);
}

/* ── Socket liveness check ──────────────────────────── */

gboolean is_discord_socket_alive(const gchar *path)
{
    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS))
        return FALSE;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return FALSE;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    int err = errno;
    close(fd);

    if (ret == 0)
        return TRUE;

    /* ECONNREFUSED = file exists but nobody listening (stale/zombie) */
    (void)err;
    return FALSE;
}

/* ── Discord IPC frame parsing ──────────────────────── */

gboolean discord_parse_frame(const guint8 *data, gsize len,
                             guint32 *opcode, gchar **json_out,
                             gsize *consumed)
{
    if (len < DISCORD_HEADER_SIZE)
        return FALSE;

    guint32 op = GUINT32_FROM_LE(*(const guint32 *)data);
    guint32 payload_len = GUINT32_FROM_LE(*(const guint32 *)(data + 4));

    if (len < DISCORD_HEADER_SIZE + payload_len)
        return FALSE;

    *opcode = op;
    if (json_out) {
        *json_out = g_strndup((const gchar *)(data + DISCORD_HEADER_SIZE),
                              payload_len);
    }
    if (consumed)
        *consumed = DISCORD_HEADER_SIZE + payload_len;

    return TRUE;
}

/* ── SET_ACTIVITY extraction ────────────────────────── */

gboolean discord_extract_activity(const gchar *json,
                                  pid_t *pid, gchar **state, gchar **details)
{
    if (!json || !json[0])
        return FALSE;

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json, -1, NULL)) {
        g_object_unref(parser);
        return FALSE;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return FALSE;
    }

    JsonObject *obj = json_node_get_object(root);
    const gchar *cmd = NULL;
    if (json_object_has_member(obj, "cmd"))
        cmd = json_object_get_string_member(obj, "cmd");

    if (!cmd || g_strcmp0(cmd, "SET_ACTIVITY") != 0) {
        g_object_unref(parser);
        return FALSE;
    }

    *pid = 0;
    *state = NULL;
    *details = NULL;

    if (json_object_has_member(obj, "args")) {
        JsonObject *args = json_object_get_object_member(obj, "args");
        if (args) {
            if (json_object_has_member(args, "pid"))
                *pid = (pid_t)json_object_get_int_member(args, "pid");

            if (json_object_has_member(args, "activity")) {
                JsonObject *act = json_object_get_object_member(args, "activity");
                if (act) {
                    if (json_object_has_member(act, "state")) {
                        const gchar *s = json_object_get_string_member(act, "state");
                        if (s && s[0])
                            *state = g_strdup(s);
                    }
                    if (json_object_has_member(act, "details")) {
                        const gchar *d = json_object_get_string_member(act, "details");
                        if (d && d[0])
                            *details = g_strdup(d);
                    }
                }
            }
        }
    }

    g_object_unref(parser);
    return TRUE;
}

/* ── Fake READY response ───────────────────────────── */

void discord_build_ready_response(guint8 **out, gsize *out_len)
{
    const gchar *json =
        "{\"cmd\":\"DISPATCH\",\"evt\":\"READY\","
        "\"data\":{\"v\":1,\"user\":{\"id\":\"1\","
        "\"username\":\"Proxy\",\"discriminator\":\"0\"}}}";

    gsize json_len = strlen(json);
    gsize total = DISCORD_HEADER_SIZE + json_len;

    *out = g_malloc(total);
    guint32 op = GUINT32_TO_LE(DISCORD_OP_FRAME);
    guint32 len = GUINT32_TO_LE((guint32)json_len);
    memcpy(*out, &op, 4);
    memcpy(*out + 4, &len, 4);
    memcpy(*out + DISCORD_HEADER_SIZE, json, json_len);
    *out_len = total;
}

/* ── Presence store ─────────────────────────────────── */

void discord_presence_store(DiscordIpcState *state, pid_t pid,
                            const gchar *rp_state, const gchar *rp_details)
{
    if (!state->presence_by_pid || pid <= 0)
        return;

    RichPresenceEntry *entry = g_new0(RichPresenceEntry, 1);
    entry->state = g_strdup(rp_state);
    entry->details = g_strdup(rp_details);
    entry->pid = pid;
    entry->last_updated = g_get_monotonic_time();

    g_hash_table_insert(state->presence_by_pid,
                        GINT_TO_POINTER((gint)pid), entry);
}

const RichPresenceEntry *discord_ipc_lookup_pid(DiscordIpcState *state, pid_t pid)
{
    if (!state || !state->presence_by_pid || pid <= 0)
        return NULL;

    return g_hash_table_lookup(state->presence_by_pid,
                               GINT_TO_POINTER((gint)pid));
}

/* ── Connection management ──────────────────────────── */

static void close_connection(ClientConnection *conn)
{
    if (!conn)
        return;

    if (conn->client_source) {
        g_source_destroy(conn->client_source);
        g_source_unref(conn->client_source);
        conn->client_source = NULL;
    }
    if (conn->upstream_source) {
        g_source_destroy(conn->upstream_source);
        g_source_unref(conn->upstream_source);
        conn->upstream_source = NULL;
    }
    if (conn->client_fd >= 0) {
        close(conn->client_fd);
        conn->client_fd = -1;
    }
    if (conn->upstream_fd >= 0) {
        close(conn->upstream_fd);
        conn->upstream_fd = -1;
    }
    if (conn->client_buf) {
        g_byte_array_free(conn->client_buf, TRUE);
        conn->client_buf = NULL;
    }
    if (conn->upstream_buf) {
        g_byte_array_free(conn->upstream_buf, TRUE);
        conn->upstream_buf = NULL;
    }

    /* Remove from connections array */
    if (conn->ipc_state && conn->ipc_state->connections)
        g_ptr_array_remove(conn->ipc_state->connections, conn);

    g_free(conn);
}

/* ── Client data handler ───────────────────────────── */

static void process_client_buffer(ClientConnection *conn)
{
    while (conn->client_buf->len >= DISCORD_HEADER_SIZE) {
        guint32 opcode;
        gchar *json = NULL;
        gsize consumed;

        if (!discord_parse_frame(conn->client_buf->data,
                                 conn->client_buf->len,
                                 &opcode, &json, &consumed))
            break; /* incomplete frame */

        /* Handle handshake in passive mode */
        if (opcode == DISCORD_OP_HANDSHAKE &&
            !conn->ipc_state->upstream_active &&
            !conn->handshake_done) {
            guint8 *resp;
            gsize resp_len;
            discord_build_ready_response(&resp, &resp_len);
            /* Best effort send */
            if (conn->client_fd >= 0)
                (void)send(conn->client_fd, resp, resp_len, MSG_NOSIGNAL);
            g_free(resp);
            conn->handshake_done = TRUE;
        }

        /* Intercept SET_ACTIVITY */
        if (opcode == DISCORD_OP_FRAME && json) {
            pid_t pid;
            gchar *rp_state = NULL, *rp_details = NULL;
            if (discord_extract_activity(json, &pid, &rp_state, &rp_details)) {
                discord_presence_store(conn->ipc_state, pid,
                                       rp_state, rp_details);
                g_free(rp_state);
                g_free(rp_details);
            }
        }

        g_free(json);

        /* Forward to upstream if connected */
        if (conn->upstream_fd >= 0) {
            (void)send(conn->upstream_fd, conn->client_buf->data,
                       consumed, MSG_NOSIGNAL);
        }

        g_byte_array_remove_range(conn->client_buf, 0, consumed);
    }
}

static gboolean on_client_data(gint fd, GIOCondition cond, gpointer user_data)
{
    ClientConnection *conn = user_data;

    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        close_connection(conn);
        return G_SOURCE_REMOVE;
    }

    guint8 buf[4096];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
        close_connection(conn);
        return G_SOURCE_REMOVE;
    }

    g_byte_array_append(conn->client_buf, buf, n);
    process_client_buffer(conn);
    return G_SOURCE_CONTINUE;
}

static gboolean on_upstream_data(gint fd, GIOCondition cond, gpointer user_data)
{
    ClientConnection *conn = user_data;

    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        close_connection(conn);
        return G_SOURCE_REMOVE;
    }

    guint8 buf[4096];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
        close_connection(conn);
        return G_SOURCE_REMOVE;
    }

    /* Forward upstream response to client */
    if (conn->client_fd >= 0)
        (void)send(conn->client_fd, buf, n, MSG_NOSIGNAL);

    return G_SOURCE_CONTINUE;
}

/* ── Server accept handler ──────────────────────────── */

static gboolean on_server_accept(gint fd, GIOCondition cond, gpointer user_data)
{
    DiscordIpcState *state = user_data;

    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))
        return G_SOURCE_REMOVE;

    int client_fd = accept(fd, NULL, NULL);
    if (client_fd < 0)
        return G_SOURCE_CONTINUE;

    /* Set non-blocking */
    fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL) | O_NONBLOCK);

    ClientConnection *conn = g_new0(ClientConnection, 1);
    conn->client_fd = client_fd;
    conn->upstream_fd = -1;
    conn->client_buf = g_byte_array_new();
    conn->upstream_buf = g_byte_array_new();
    conn->ipc_state = state;
    conn->handshake_done = FALSE;

    /* Connect to upstream if available */
    if (state->upstream_active && state->real_ipc_path) {
        int ufd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (ufd >= 0) {
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, state->real_ipc_path,
                    sizeof(addr.sun_path) - 1);

            if (connect(ufd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                fcntl(ufd, F_SETFL, fcntl(ufd, F_GETFL) | O_NONBLOCK);
                conn->upstream_fd = ufd;

                conn->upstream_source = g_unix_fd_source_new(ufd,
                    G_IO_IN | G_IO_HUP | G_IO_ERR);
                g_source_set_callback(conn->upstream_source,
                    G_SOURCE_FUNC(on_upstream_data), conn, NULL);
                g_source_attach(conn->upstream_source, NULL);
            } else {
                close(ufd);
                g_printerr("[discord-ipc] Failed to connect to upstream\n");
            }
        }
    }

    /* Watch client for data */
    conn->client_source = g_unix_fd_source_new(client_fd,
        G_IO_IN | G_IO_HUP | G_IO_ERR);
    g_source_set_callback(conn->client_source,
        G_SOURCE_FUNC(on_client_data), conn, NULL);
    g_source_attach(conn->client_source, NULL);

    g_ptr_array_add(state->connections, conn);
    return G_SOURCE_CONTINUE;
}

/* ── Setup / Cleanup ────────────────────────────────── */

gboolean discord_ipc_setup(DiscordIpcState *state)
{
    memset(state, 0, sizeof(*state));
    state->server_fd = -1;

    const gchar *runtime_dir = g_getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir) {
        gchar *fallback = g_strdup_printf("/run/user/%d", getuid());
        state->ipc_path = g_build_filename(fallback, "discord-ipc-0", NULL);
        state->real_ipc_path = g_build_filename(fallback, "discord-ipc-original", NULL);
        g_free(fallback);
    } else {
        state->ipc_path = g_build_filename(runtime_dir, "discord-ipc-0", NULL);
        state->real_ipc_path = g_build_filename(runtime_dir, "discord-ipc-original", NULL);
    }

    state->presence_by_pid = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                    NULL, free_rp_entry);
    state->connections = g_ptr_array_new();

    /* Crash recovery: leftover backup socket from previous run */
    if (g_file_test(state->real_ipc_path, G_FILE_TEST_EXISTS)) {
        g_printerr("[discord-ipc] Found leftover backup socket, restoring...\n");
        if (g_file_test(state->ipc_path, G_FILE_TEST_EXISTS))
            unlink(state->ipc_path);
        rename(state->real_ipc_path, state->ipc_path);
    }

    /* Check if Discord is running */
    if (g_file_test(state->ipc_path, G_FILE_TEST_EXISTS)) {
        if (is_discord_socket_alive(state->ipc_path)) {
            g_printerr("[discord-ipc] Discord is running, hijacking socket...\n");
            if (rename(state->ipc_path, state->real_ipc_path) != 0) {
                g_printerr("[discord-ipc] Failed to rename socket: %s\n",
                           g_strerror(errno));
                goto fail;
            }
            state->upstream_active = TRUE;
        } else {
            g_printerr("[discord-ipc] Found stale socket, cleaning up...\n");
            unlink(state->ipc_path);
            state->upstream_active = FALSE;
        }
    } else {
        g_printerr("[discord-ipc] Discord not running, passive mode\n");
        state->upstream_active = FALSE;
    }

    /* Create our listening socket */
    state->server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (state->server_fd < 0) {
        g_printerr("[discord-ipc] Failed to create socket: %s\n",
                   g_strerror(errno));
        goto fail;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, state->ipc_path, sizeof(addr.sun_path) - 1);

    if (bind(state->server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        g_printerr("[discord-ipc] Failed to bind socket: %s\n",
                   g_strerror(errno));
        goto fail;
    }

    if (listen(state->server_fd, 5) != 0) {
        g_printerr("[discord-ipc] Failed to listen: %s\n",
                   g_strerror(errno));
        goto fail;
    }

    fcntl(state->server_fd, F_SETFL,
          fcntl(state->server_fd, F_GETFL) | O_NONBLOCK);

    /* Attach to GLib main loop */
    state->server_source = g_unix_fd_source_new(state->server_fd, G_IO_IN);
    g_source_set_callback(state->server_source,
        G_SOURCE_FUNC(on_server_accept), state, NULL);
    g_source_attach(state->server_source, NULL);

    state->active = TRUE;
    g_printerr("[discord-ipc] Listening on %s\n", state->ipc_path);
    return TRUE;

fail:
    /* Restore socket if we moved it */
    if (state->upstream_active && g_file_test(state->real_ipc_path, G_FILE_TEST_EXISTS)) {
        if (!g_file_test(state->ipc_path, G_FILE_TEST_EXISTS))
            rename(state->real_ipc_path, state->ipc_path);
    }
    if (state->server_fd >= 0) {
        close(state->server_fd);
        state->server_fd = -1;
    }
    return FALSE;
}

void discord_ipc_cleanup(DiscordIpcState *state)
{
    if (!state)
        return;

    state->active = FALSE;

    /* Close all connections */
    if (state->connections) {
        while (state->connections->len > 0) {
            ClientConnection *conn = g_ptr_array_index(state->connections, 0);
            close_connection(conn);
        }
        g_ptr_array_free(state->connections, TRUE);
        state->connections = NULL;
    }

    /* Close server */
    if (state->server_source) {
        g_source_destroy(state->server_source);
        g_source_unref(state->server_source);
        state->server_source = NULL;
    }
    if (state->server_fd >= 0) {
        close(state->server_fd);
        state->server_fd = -1;
    }

    /* Remove our fake socket */
    if (state->ipc_path && g_file_test(state->ipc_path, G_FILE_TEST_EXISTS))
        unlink(state->ipc_path);

    /* Restore original Discord socket */
    if (state->real_ipc_path &&
        g_file_test(state->real_ipc_path, G_FILE_TEST_EXISTS)) {
        g_printerr("[discord-ipc] Restoring original Discord socket...\n");
        rename(state->real_ipc_path, state->ipc_path);
    }

    if (state->presence_by_pid) {
        g_hash_table_destroy(state->presence_by_pid);
        state->presence_by_pid = NULL;
    }

    g_free(state->ipc_path);
    g_free(state->real_ipc_path);
    state->ipc_path = NULL;
    state->real_ipc_path = NULL;
}
