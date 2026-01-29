#include <glib.h>
#include "discord-ipc.h"
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* ── Helper: build a Discord IPC frame ─────────────── */

static guint8 *build_frame(guint32 opcode, const gchar *json, gsize *out_len)
{
    guint32 json_len = json ? (guint32)strlen(json) : 0;
    gsize total = DISCORD_HEADER_SIZE + json_len;
    guint8 *buf = g_malloc(total);

    guint32 op_le = GUINT32_TO_LE(opcode);
    guint32 len_le = GUINT32_TO_LE(json_len);
    memcpy(buf, &op_le, 4);
    memcpy(buf + 4, &len_le, 4);
    if (json)
        memcpy(buf + DISCORD_HEADER_SIZE, json, json_len);

    *out_len = total;
    return buf;
}

static gchar *create_test_tmpdir(void)
{
    gchar *tmpdir = g_dir_make_tmp("discord-ipc-test-XXXXXX", NULL);
    g_assert_nonnull(tmpdir);
    return tmpdir;
}

static void cleanup_test_tmpdir(gchar *tmpdir)
{
    gchar *argv[] = {"rm", "-rf", tmpdir, NULL};
    g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                 NULL, NULL, NULL, NULL, NULL, NULL);
    g_free(tmpdir);
}

/* ── discord_parse_frame tests ─────────────────────── */

static void test_parse_frame_handshake(void)
{
    const gchar *json = "{\"v\":1,\"client_id\":\"12345\"}";
    gsize frame_len;
    guint8 *frame = build_frame(DISCORD_OP_HANDSHAKE, json, &frame_len);

    guint32 opcode;
    gchar *parsed_json = NULL;
    gsize consumed;
    gboolean ok = discord_parse_frame(frame, frame_len, &opcode, &parsed_json, &consumed);

    g_assert_true(ok);
    g_assert_cmpuint(opcode, ==, DISCORD_OP_HANDSHAKE);
    g_assert_cmpstr(parsed_json, ==, json);
    g_assert_cmpuint(consumed, ==, frame_len);

    g_free(parsed_json);
    g_free(frame);
}

static void test_parse_frame_activity(void)
{
    const gchar *json = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":1234}}";
    gsize frame_len;
    guint8 *frame = build_frame(DISCORD_OP_FRAME, json, &frame_len);

    guint32 opcode;
    gchar *parsed_json = NULL;
    gsize consumed;
    gboolean ok = discord_parse_frame(frame, frame_len, &opcode, &parsed_json, &consumed);

    g_assert_true(ok);
    g_assert_cmpuint(opcode, ==, DISCORD_OP_FRAME);
    g_assert_cmpstr(parsed_json, ==, json);

    g_free(parsed_json);
    g_free(frame);
}

static void test_parse_frame_truncated(void)
{
    guint8 buf[4] = {0, 0, 0, 0};
    guint32 opcode;
    gchar *json = NULL;
    gsize consumed;

    g_assert_false(discord_parse_frame(buf, 4, &opcode, &json, &consumed));
    g_assert_null(json);
}

static void test_parse_frame_partial_payload(void)
{
    /* Header says 100 bytes but we only have 50 */
    guint8 buf[58]; /* 8 header + 50 data */
    memset(buf, 0, sizeof(buf));
    guint32 op = GUINT32_TO_LE(1);
    guint32 len = GUINT32_TO_LE(100);
    memcpy(buf, &op, 4);
    memcpy(buf + 4, &len, 4);

    guint32 opcode;
    gchar *json = NULL;
    gsize consumed;

    g_assert_false(discord_parse_frame(buf, sizeof(buf), &opcode, &json, &consumed));
}

static void test_parse_frame_zero_length(void)
{
    gsize frame_len;
    guint8 *frame = build_frame(DISCORD_OP_FRAME, NULL, &frame_len);
    g_assert_cmpuint(frame_len, ==, DISCORD_HEADER_SIZE);

    guint32 opcode;
    gchar *json = NULL;
    gsize consumed;
    gboolean ok = discord_parse_frame(frame, frame_len, &opcode, &json, &consumed);

    g_assert_true(ok);
    g_assert_cmpuint(opcode, ==, DISCORD_OP_FRAME);
    g_assert_nonnull(json);
    g_assert_cmpstr(json, ==, "");
    g_assert_cmpuint(consumed, ==, DISCORD_HEADER_SIZE);

    g_free(json);
    g_free(frame);
}

static void test_parse_frame_multiple(void)
{
    const gchar *json1 = "{\"cmd\":\"FIRST\"}";
    const gchar *json2 = "{\"cmd\":\"SECOND\"}";
    gsize len1, len2;
    guint8 *frame1 = build_frame(0, json1, &len1);
    guint8 *frame2 = build_frame(1, json2, &len2);

    /* Concatenate both frames */
    gsize total = len1 + len2;
    guint8 *combined = g_malloc(total);
    memcpy(combined, frame1, len1);
    memcpy(combined + len1, frame2, len2);

    /* Parse first frame */
    guint32 opcode;
    gchar *json = NULL;
    gsize consumed;
    gboolean ok = discord_parse_frame(combined, total, &opcode, &json, &consumed);

    g_assert_true(ok);
    g_assert_cmpuint(opcode, ==, 0);
    g_assert_cmpstr(json, ==, json1);
    g_assert_cmpuint(consumed, ==, len1);
    g_free(json);

    /* Parse second frame from remainder */
    ok = discord_parse_frame(combined + consumed, total - consumed,
                             &opcode, &json, &consumed);
    g_assert_true(ok);
    g_assert_cmpuint(opcode, ==, 1);
    g_assert_cmpstr(json, ==, json2);
    g_free(json);

    g_free(combined);
    g_free(frame1);
    g_free(frame2);
}

/* ── discord_extract_activity tests ────────────────── */

static void test_extract_activity_full(void)
{
    const gchar *json =
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{"
        "\"pid\":1234,"
        "\"activity\":{\"state\":\"Editing main.py\",\"details\":\"my-project\"}"
        "}}";

    pid_t pid;
    gchar *state = NULL, *details = NULL;
    gboolean ok = discord_extract_activity(json, &pid, &state, &details);

    g_assert_true(ok);
    g_assert_cmpint(pid, ==, 1234);
    g_assert_cmpstr(state, ==, "Editing main.py");
    g_assert_cmpstr(details, ==, "my-project");

    g_free(state);
    g_free(details);
}

static void test_extract_activity_no_state(void)
{
    const gchar *json =
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{"
        "\"pid\":5678,"
        "\"activity\":{\"details\":\"workspace\"}"
        "}}";

    pid_t pid;
    gchar *state = NULL, *details = NULL;
    gboolean ok = discord_extract_activity(json, &pid, &state, &details);

    g_assert_true(ok);
    g_assert_cmpint(pid, ==, 5678);
    g_assert_null(state);
    g_assert_cmpstr(details, ==, "workspace");

    g_free(details);
}

static void test_extract_activity_no_details(void)
{
    const gchar *json =
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{"
        "\"pid\":9999,"
        "\"activity\":{\"state\":\"Browsing\"}"
        "}}";

    pid_t pid;
    gchar *state = NULL, *details = NULL;
    gboolean ok = discord_extract_activity(json, &pid, &state, &details);

    g_assert_true(ok);
    g_assert_cmpint(pid, ==, 9999);
    g_assert_cmpstr(state, ==, "Browsing");
    g_assert_null(details);

    g_free(state);
}

static void test_extract_activity_no_pid(void)
{
    const gchar *json =
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{"
        "\"activity\":{\"state\":\"Editing\",\"details\":\"project\"}"
        "}}";

    pid_t pid;
    gchar *state = NULL, *details = NULL;
    gboolean ok = discord_extract_activity(json, &pid, &state, &details);

    g_assert_true(ok);
    g_assert_cmpint(pid, ==, 0);
    g_assert_cmpstr(state, ==, "Editing");
    g_assert_cmpstr(details, ==, "project");

    g_free(state);
    g_free(details);
}

static void test_extract_activity_not_set(void)
{
    const gchar *json = "{\"cmd\":\"SUBSCRIBE\",\"args\":{}}";

    pid_t pid;
    gchar *state = NULL, *details = NULL;
    gboolean ok = discord_extract_activity(json, &pid, &state, &details);

    g_assert_false(ok);
}

static void test_extract_activity_malformed(void)
{
    pid_t pid;
    gchar *state = NULL, *details = NULL;
    g_assert_false(discord_extract_activity("{not valid json", &pid, &state, &details));
    g_assert_false(discord_extract_activity(NULL, &pid, &state, &details));
    g_assert_false(discord_extract_activity("", &pid, &state, &details));
}

/* ── discord_build_ready_response tests ────────────── */

static void test_build_ready_response(void)
{
    guint8 *resp;
    gsize resp_len;
    discord_build_ready_response(&resp, &resp_len);

    g_assert_nonnull(resp);
    g_assert_cmpuint(resp_len, >, DISCORD_HEADER_SIZE);

    /* Check header */
    guint32 opcode = GUINT32_FROM_LE(*(guint32 *)resp);
    guint32 payload_len = GUINT32_FROM_LE(*(guint32 *)(resp + 4));

    g_assert_cmpuint(opcode, ==, DISCORD_OP_FRAME);
    g_assert_cmpuint(payload_len, ==, resp_len - DISCORD_HEADER_SIZE);

    g_free(resp);
}

static void test_ready_response_parseable(void)
{
    guint8 *resp;
    gsize resp_len;
    discord_build_ready_response(&resp, &resp_len);

    /* Parse it back */
    guint32 opcode;
    gchar *json = NULL;
    gsize consumed;
    gboolean ok = discord_parse_frame(resp, resp_len, &opcode, &json, &consumed);

    g_assert_true(ok);
    g_assert_cmpuint(opcode, ==, DISCORD_OP_FRAME);
    g_assert_nonnull(json);
    g_assert_nonnull(strstr(json, "DISPATCH"));
    g_assert_nonnull(strstr(json, "READY"));

    g_free(json);
    g_free(resp);
}

/* ── Presence store tests ──────────────────────────── */

static void test_presence_store_and_lookup(void)
{
    DiscordIpcState state = {0};
    state.presence_by_pid = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                   NULL, g_free);

    RichPresenceEntry *entry = g_new0(RichPresenceEntry, 1);
    entry->state = g_strdup("Editing");
    entry->details = g_strdup("my-project");
    entry->pid = 1234;
    entry->last_updated = g_get_monotonic_time();
    g_hash_table_insert(state.presence_by_pid, GINT_TO_POINTER(1234), entry);

    const RichPresenceEntry *found = discord_ipc_lookup_pid(&state, 1234);
    g_assert_nonnull(found);
    g_assert_cmpstr(found->state, ==, "Editing");
    g_assert_cmpstr(found->details, ==, "my-project");

    /* Cleanup manually since we used a simple free func */
    g_free(entry->state);
    g_free(entry->details);
    g_hash_table_destroy(state.presence_by_pid);
}

static void test_presence_overwrite(void)
{
    DiscordIpcState state = {0};
    state.presence_by_pid = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                   NULL, NULL);

    discord_presence_store(&state, 1234, "First", "project-a");
    discord_presence_store(&state, 1234, "Second", "project-b");

    const RichPresenceEntry *found = discord_ipc_lookup_pid(&state, 1234);
    g_assert_nonnull(found);
    g_assert_cmpstr(found->state, ==, "Second");
    g_assert_cmpstr(found->details, ==, "project-b");

    /* Cleanup: the first entry was leaked since we used NULL destroy.
     * Use proper cleanup for this test */
    g_free(found->state);
    g_free(found->details);
    g_free((gpointer)found);
    g_hash_table_destroy(state.presence_by_pid);
}

static void test_presence_lookup_missing(void)
{
    DiscordIpcState state = {0};
    state.presence_by_pid = g_hash_table_new(g_direct_hash, g_direct_equal);

    const RichPresenceEntry *found = discord_ipc_lookup_pid(&state, 9999);
    g_assert_null(found);

    g_hash_table_destroy(state.presence_by_pid);
}

static void test_presence_multiple_pids(void)
{
    DiscordIpcState state = {0};
    state.presence_by_pid = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                                   NULL, NULL);

    discord_presence_store(&state, 100, "State-A", "Details-A");
    discord_presence_store(&state, 200, "State-B", "Details-B");
    discord_presence_store(&state, 300, "State-C", "Details-C");

    const RichPresenceEntry *a = discord_ipc_lookup_pid(&state, 100);
    const RichPresenceEntry *b = discord_ipc_lookup_pid(&state, 200);
    const RichPresenceEntry *c = discord_ipc_lookup_pid(&state, 300);

    g_assert_nonnull(a);
    g_assert_cmpstr(a->state, ==, "State-A");
    g_assert_nonnull(b);
    g_assert_cmpstr(b->state, ==, "State-B");
    g_assert_nonnull(c);
    g_assert_cmpstr(c->state, ==, "State-C");

    /* Cleanup */
    g_free(a->state); g_free(a->details); g_free((gpointer)a);
    g_free(b->state); g_free(b->details); g_free((gpointer)b);
    g_free(c->state); g_free(c->details); g_free((gpointer)c);
    g_hash_table_destroy(state.presence_by_pid);
}

/* ── Socket liveness tests ─────────────────────────── */

static void test_socket_alive_nonexistent(void)
{
    g_assert_false(is_discord_socket_alive("/tmp/nonexistent-discord-socket-test"));
}

static void test_socket_alive_stale(void)
{
    gchar *tmpdir = create_test_tmpdir();
    gchar *sock_path = g_strdup_printf("%s/test-socket", tmpdir);

    /* Create a socket, bind but don't listen — then close */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    g_assert_cmpint(fd, >=, 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    g_assert_cmpint(bind(fd, (struct sockaddr *)&addr, sizeof(addr)), ==, 0);
    close(fd);

    /* Socket file exists but nobody is listening */
    g_assert_false(is_discord_socket_alive(sock_path));

    g_free(sock_path);
    cleanup_test_tmpdir(tmpdir);
}

/* ── main ──────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    /* Frame parsing */
    g_test_add_func("/discord/parse_frame_handshake", test_parse_frame_handshake);
    g_test_add_func("/discord/parse_frame_activity", test_parse_frame_activity);
    g_test_add_func("/discord/parse_frame_truncated", test_parse_frame_truncated);
    g_test_add_func("/discord/parse_frame_partial_payload", test_parse_frame_partial_payload);
    g_test_add_func("/discord/parse_frame_zero_length", test_parse_frame_zero_length);
    g_test_add_func("/discord/parse_frame_multiple", test_parse_frame_multiple);

    /* Activity extraction */
    g_test_add_func("/discord/extract_activity_full", test_extract_activity_full);
    g_test_add_func("/discord/extract_activity_no_state", test_extract_activity_no_state);
    g_test_add_func("/discord/extract_activity_no_details", test_extract_activity_no_details);
    g_test_add_func("/discord/extract_activity_no_pid", test_extract_activity_no_pid);
    g_test_add_func("/discord/extract_activity_not_set", test_extract_activity_not_set);
    g_test_add_func("/discord/extract_activity_malformed", test_extract_activity_malformed);

    /* READY response */
    g_test_add_func("/discord/build_ready_response", test_build_ready_response);
    g_test_add_func("/discord/ready_response_parseable", test_ready_response_parseable);

    /* Presence store */
    g_test_add_func("/discord/presence_store_and_lookup", test_presence_store_and_lookup);
    g_test_add_func("/discord/presence_overwrite", test_presence_overwrite);
    g_test_add_func("/discord/presence_lookup_missing", test_presence_lookup_missing);
    g_test_add_func("/discord/presence_multiple_pids", test_presence_multiple_pids);

    /* Socket liveness */
    g_test_add_func("/discord/socket_alive_nonexistent", test_socket_alive_nonexistent);
    g_test_add_func("/discord/socket_alive_stale", test_socket_alive_stale);

    return g_test_run();
}
