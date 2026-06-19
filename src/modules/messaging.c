/*
 * messaging.c
 *
 * Implements bot_send_message() and bot_send_cv2_message().
 *
 * bot_send_message() uses Orca's discord_create_message() for plain text.
 *
 * bot_send_cv2_message() delegates to cv2_send() in components_v2.c, which
 * POSTs directly to Discord's REST API via libcurl -- the same approach used
 * in moderation.c for the timeout PATCH call -- because Orca's typed struct
 * layer predates the Components V2 API and does not expose the required
 * IS_COMPONENTS_V2 flag (1<<15).
 *
 * Threading note
 * --------------
 * Orca's discord_create_message() is safe to call from any thread that holds
 * the client pointer.  The g_client pointer is written once during bot startup
 * (messaging_module_init) before any REST calls are made, so no mutex is
 * required for the pointer itself.
 */

#include "messaging.h"
#include "components_v2.h"
#include "database.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>

#include <curl/curl.h>

#include <orca/discord.h>

/* ---------------------------------------------------------------------------
 * Module-private state
 * ---------------------------------------------------------------------------
 * Both set once by messaging_module_init(); never written to again.
 * --------------------------------------------------------------------------- */
static struct discord *g_client = NULL;

/*
 * Local copy of the bot token, retained so messaging_refresh_guild_channels()
 * can issue REST GETs to Discord (e.g. fetch guild channels) without relying
 * on the Components V2 module's private copy.  Populated once by
 * messaging_module_init(); never written again.
 */
static char g_token[512] = {0};

/* ---------------------------------------------------------------------------
 * messaging_module_init
 * --------------------------------------------------------------------------- */
void messaging_module_init(struct discord *client, const char *bot_token) {
    g_client = client;
    /* Keep a local copy of the token for REST GETs (channel cache refresh). */
    if (bot_token) {
        strncpy(g_token, bot_token, sizeof(g_token) - 1);
        g_token[sizeof(g_token) - 1] = '\0';
    }
    /* Forward the token to the Components V2 module for REST calls. */
    cv2_components_init(bot_token);
    printf("[messaging] Module initialised.\n");
}

/* ---------------------------------------------------------------------------
 * bot_send_message
 * --------------------------------------------------------------------------- */
int bot_send_message(sqlite3_int64 guild_id,
                     sqlite3_int64 channel_id,
                     const char   *content) {

    /* ── Guard: module must be initialised ───────────────────────────────── */
    if (!g_client) {
        fprintf(stderr,
                "[messaging] ERROR: bot_send_message called before "
                "messaging_module_init().\n");
        return -1;
    }

    /* ── Guard: content must be non-empty ────────────────────────────────── */
    if (!content || content[0] == '\0') {
        fprintf(stderr,
                "[messaging] ERROR: bot_send_message called with NULL or "
                "empty content (guild=%" PRId64 " channel=%" PRId64 ").\n",
                guild_id, channel_id);
        return -2;
    }

    /* ── Guard: respect Discord's hard length limit ───────────────────────── */
    size_t content_len = strlen(content);
    if (content_len > DISCORD_MAX_MESSAGE_LEN) {
        fprintf(stderr,
                "[messaging] ERROR: content length %zu exceeds Discord "
                "limit of %d (guild=%" PRId64 " channel=%" PRId64 ").\n",
                content_len, DISCORD_MAX_MESSAGE_LEN, guild_id, channel_id);
        return -3;
    }

    printf("[messaging] Sending message to channel %" PRId64
           " (guild %" PRId64 ") – %zu char(s).\n",
           channel_id, guild_id, content_len);

    /* ── Build the create-message params struct ───────────────────────────── */
    struct discord_create_message_params params = {
        .content = (char *)content,   /* Orca treats this as read-only */
    };

    /* ── Dispatch via Orca ───────────────────────────────────────────────── */
    ORCAcode code = discord_create_message(g_client,
                                           (u64_snowflake_t)channel_id,
                                           &params,
                                           NULL /* ret – we don't need it */);

    if (code != ORCA_OK) {
        fprintf(stderr,
                "[messaging] ERROR: discord_create_message failed "
                "(ORCAcode %d) for channel %" PRId64
                " guild %" PRId64 ".\n",
                code, channel_id, guild_id);
        return -4;
    }

    printf("[messaging] Message delivered successfully to channel "
           "%" PRId64 ".\n", channel_id);
    return 0;
}

/* ---------------------------------------------------------------------------
 * bot_send_cv2_message
 * --------------------------------------------------------------------------- */
int bot_send_cv2_message(sqlite3_int64 guild_id,
                          sqlite3_int64 channel_id,
                          CV2Msg       *msg) {

    /* ── Guard: module must be initialised ───────────────────────────────── */
    if (!g_client) {
        fprintf(stderr,
                "[messaging] ERROR: bot_send_cv2_message called before "
                "messaging_module_init().\n");
        return -1;
    }

    /* ── Guard: builder must be valid ────────────────────────────────────── */
    if (!msg) {
        fprintf(stderr,
                "[messaging] ERROR: bot_send_cv2_message called with NULL msg "
                "(guild=%" PRId64 " channel=%" PRId64 ").\n",
                guild_id, channel_id);
        return -2;
    }

    printf("[messaging] Sending Components V2 message to channel %" PRId64
           " (guild %" PRId64 ").\n", channel_id, guild_id);

    /* ── Delegate to the cv2 module's REST sender ─────────────────────────
     * cv2_send() builds the JSON internally, POSTs it via libcurl, and
     * returns 0 on success or a negative/HTTP error code on failure.
     * --------------------------------------------------------------------- */
    int rc = cv2_send(msg, (uint64_t)channel_id);

    if (rc != 0) {
        fprintf(stderr,
                "[messaging] ERROR: cv2_send() failed (rc=%d) for channel "
                "%" PRId64 " guild %" PRId64 ".\n",
                rc, channel_id, guild_id);
        return -3;
    }

    printf("[messaging] Components V2 message delivered to channel "
           "%" PRId64 ".\n", channel_id);
    return 0;
}

/* ---------------------------------------------------------------------------
 * bot_send_cv2_raw
 *
 * Low-level shim used by api.c's handle_send_components_v2() when the caller
 * has already built the complete JSON payload string (envelope included).
 * This avoids constructing a CV2Msg just to send pre-built JSON from the
 * dashboard frontend.
 *
 * json_payload must be a valid, complete Discord message JSON string with
 * "flags":32768 already set, e.g.:
 *   {"flags":32768,"components":[{"type":10,"content":"Hello"}]}
 *
 * Returns 0 on success, -1 if module uninitialised, or the cv2 error code.
 * --------------------------------------------------------------------------- */
int bot_send_cv2_raw(sqlite3_int64 guild_id,
                      sqlite3_int64 channel_id,
                      const char   *json_payload) {
    if (!g_client) {
        fprintf(stderr,
                "[messaging] ERROR: bot_send_cv2_raw called before "
                "messaging_module_init().\n");
        return -1;
    }

    printf("[messaging] Sending raw Components V2 payload to channel "
           "%" PRId64 " (guild %" PRId64 ").\n", channel_id, guild_id);

    /*
     * cv2_post_raw() is a small internal helper exposed from components_v2.c
     * for this specific use case: posting a fully-formed payload string.
     */
    extern int cv2_post_raw(uint64_t channel_id, const char *json_payload);
    return cv2_post_raw((uint64_t)channel_id, json_payload);
}
/* ===========================================================================
 * Discord guild-channel fetch + cache
 * ===========================================================================
 *
 * The dashboard's Messaging page needs the real list of sendable text channels
 * for a guild.  Previously handle_channels() in api.c emitted a SYNTHETIC
 * "general" channel (id = guild_id) when no channels were cached, which caused
 * discord_create_message() to fail with ORCAcode 1 because no such channel
 * exists on Discord's side.
 *
 * messaging_refresh_guild_channels() does a REST GET to
 * /guilds/{guild_id}/channels, parses the JSON array, and stores sendable
 * text channels (type 0 = text, 5 = announcement) in the channels table.
 * It is wired into on_guild_create (async, detached) and exposed via
 * POST /api/guilds/<id>/refresh-channels for manual re-sync.
 * ========================================================================= */

/* In-memory response buffer used by libcurl's WRITEFUNCTION callback. */
typedef struct { char *data; size_t len; } MsgCurlBuf;

static size_t _msgbuf_write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    MsgCurlBuf *b = (MsgCurlBuf *)ud;
    size_t extra = size * nmemb;
    char *tmp = realloc(b->data, b->len + extra + 1);
    if (!tmp) return 0;
    b->data = tmp;
    memcpy(b->data + b->len, ptr, extra);
    b->len += extra;
    b->data[b->len] = '\0';
    return extra;
}

/*
 * store_one_channel
 *
 * Parse ONE channel object substring (a top-level {...} from the JSON array)
 * and, if it is a sendable text channel (type 0 or 5), upsert it into the
 * channels table.
 *
 * The substring starts at '{' and ends at the matching '}'.  It may contain
 * nested objects/arrays (e.g. permission_overwrites — whose entries ALSO
 * have "id"/"type" keys).  We take the FIRST "id"/"type"/"name" within the
 * substring because Discord's serialisation always emits the channel's own
 * fields before any nested object.
 *
 * Returns 1 if a sendable channel was stored, 0 otherwise.
 */
static int store_one_channel(Database *db, sqlite3_int64 guild_id,
                             const char *obj) {
    sqlite3_int64 cid = 0;
    int ctype = -1;
    char cname[128];
    cname[0] = '\0';

    /* id: first "id":"<digits>" */
    const char *idp = strstr(obj, "\"id\"");
    if (idp) {
        const char *q = idp + 4;
        while (*q == ' ' || *q == ':' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q == '"') {
            q++;
            sqlite3_int64 v = 0;
            while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; }
            cid = v;
        }
    }
    if (cid == 0) return 0;

    /* type: first "type":<int> */
    const char *tp = strstr(obj, "\"type\"");
    if (tp) {
        const char *q = tp + 6;
        while (*q == ' ' || *q == ':' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        int v = 0, neg = 0;
        if (*q == '-') { neg = 1; q++; }
        while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; }
        ctype = neg ? -v : v;
    }

    /* name: first "name":"<escaped>" */
    const char *np = strstr(obj, "\"name\"");
    if (np) {
        const char *q = np + 6;
        while (*q == ' ' || *q == ':' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q == '"') {
            q++;
            size_t oi = 0;
            while (*q && *q != '"') {
                if (*q == '\\' && q[1]) {
                    q++;
                    switch (*q) {
                        case 'n':  if (oi < sizeof cname - 1) cname[oi++] = '\n'; break;
                        case 't':  if (oi < sizeof cname - 1) cname[oi++] = '\t'; break;
                        case 'r':  if (oi < sizeof cname - 1) cname[oi++] = '\r'; break;
                        case '"':  if (oi < sizeof cname - 1) cname[oi++] = '"';  break;
                        case '\\': if (oi < sizeof cname - 1) cname[oi++] = '\\'; break;
                        case '/':  if (oi < sizeof cname - 1) cname[oi++] = '/';  break;
                        case 'u':  /* \uXXXX — skip 4 hex, emit nothing (rare) */
                            if (q[1] && q[2] && q[3] && q[4]) q += 4;
                            break;
                        default:   if (oi < sizeof cname - 1) cname[oi++] = *q;   break;
                    }
                } else {
                    if (oi < sizeof cname - 1) cname[oi++] = *q;
                }
                q++;
            }
            cname[oi] = '\0';
        }
    }

    /* Only cache sendable text channels (0 = text, 5 = announcement). */
    if (ctype != 0 && ctype != 5) return 0;
    db_upsert_channel(db, (u64_snowflake_t)cid, (u64_snowflake_t)guild_id,
                      cname[0] ? cname : "channel", ctype);
    return 1;
}

/*
 * parse_and_store_channels
 *
 * Split the JSON array response into top-level channel-object substrings
 * (brace-depth aware, string aware — so a '{' inside a string literal does
 * not change depth) and store each sendable text channel.
 *
 * Clears the previously-cached channels for the guild first.
 *
 * Returns the number of channels stored, or -1 on argument error.
 */
static int parse_and_store_channels(Database *db,
                                    sqlite3_int64 guild_id,
                                    const char *json) {
    if (!db || !db->db || !json || guild_id == 0) return -1;
    db_clear_guild_channels(db, (u64_snowflake_t)guild_id);

    int depth = 0, in_string = 0, escaped = 0, stored = 0;
    const char *obj_start = NULL;

    for (const char *p = json; *p; p++) {
        char c = *p;
        if (in_string) {
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if (c == '"') in_string = 0;
            continue;
        }
        if (c == '"') { in_string = 1; continue; }
        if (c == '{') {
            if (depth == 0) obj_start = p;
            depth++;
            continue;
        }
        if (c == '}') {
            depth--;
            if (depth == 0 && obj_start) {
                size_t len = (size_t)(p - obj_start) + 1;
                char *obj = malloc(len + 1);
                if (obj) {
                    memcpy(obj, obj_start, len);
                    obj[len] = '\0';
                    stored += store_one_channel(db, guild_id, obj);
                    free(obj);
                }
                obj_start = NULL;
            }
            continue;
        }
    }
    return stored;
}

/*
 * messaging_refresh_guild_channels
 *
 * Synchronously fetch the guild's channels from Discord's REST API and cache
 * them.  Blocks for the duration of one HTTP GET (typically < 1 s).  Safe to
 * call from any thread.
 *
 * Returns the number of channels stored, or -1 on error.
 */
int messaging_refresh_guild_channels(Database *db, u64_snowflake_t guild_id) {
    if (!g_token[0] || !db || guild_id == 0) return -1;

    CURL *curl = curl_easy_init();
    if (!curl) { fprintf(stderr, "[messaging] curl_easy_init failed\n"); return -1; }

    char url[256];
    snprintf(url, sizeof url,
             "https://discord.com/api/v10/guilds/%llu/channels",
             (unsigned long long)guild_id);

    char auth_hdr[640];
    snprintf(auth_hdr, sizeof auth_hdr, "Authorization: Bot %s", g_token);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_hdr);

    MsgCurlBuf resp = { NULL, 0 };

    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET,       1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _msgbuf_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       12L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "[messaging] channels GET curl error: %s\n", curl_easy_strerror(res));
        free(resp.data);
        return -1;
    }
    if (http_code != 200 || !resp.data) {
        fprintf(stderr, "[messaging] channels GET for guild %llu returned HTTP %ld\n",
                (unsigned long long)guild_id, http_code);
        free(resp.data);
        return -1;
    }

    int n = parse_and_store_channels(db, (sqlite3_int64)guild_id, resp.data);
    free(resp.data);
    printf("[messaging] Cached %d channel(s) for guild %llu.\n",
           n, (unsigned long long)guild_id);
    return n;
}

/* ── Async wrapper ────────────────────────────────────────────────────────
 * Spawns a detached pthread so callers on the gateway / HTTP thread do not
 * block on a curl GET.  Used by on_guild_create and POST /refresh-channels.
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct { Database *db; u64_snowflake_t guild_id; } ChanRefreshArgs;

static void *refresh_channels_thread(void *arg) {
    ChanRefreshArgs *a = (ChanRefreshArgs *)arg;
    int n = messaging_refresh_guild_channels(a->db, a->guild_id);
    if (n < 0)
        fprintf(stderr, "[messaging] background channel refresh failed for guild %llu\n",
                (unsigned long long)a->guild_id);
    free(a);
    return NULL;
}

void messaging_refresh_guild_channels_async(Database *db, u64_snowflake_t guild_id) {
    ChanRefreshArgs *a = malloc(sizeof *a);
    if (!a) return;
    a->db = db;
    a->guild_id = guild_id;
    pthread_t tid;
    if (pthread_create(&tid, NULL, refresh_channels_thread, a) != 0) {
        free(a);
    } else {
        pthread_detach(tid);
    }
}
