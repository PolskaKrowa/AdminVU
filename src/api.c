/*
 * api.c
 *
 * JSON REST API for the dashboard.
 * All snowflake IDs are serialised as strings to preserve JS precision.
 */

#include "http_server.h"
#include "api.h"
#include "modules/ticket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include <inttypes.h>

/* ── JSON buffer helper ─────────────────────────────────────────────────── */
/*
 * JB is a growable byte buffer used to accumulate the response body of every
 * /api/* endpoint.  It starts out wrapping a caller-provided buffer (so the
 * common small-JSON case still needs zero heap allocations) and transparently
 * upgrades to a heap-owned buffer the first time a write would overflow the
 * caller's storage.  Once upgraded, the buffer doubles on demand up to
 * JB_MAX_CAP, beyond which writes set `truncated=1` and are dropped (so the
 * caller always gets a defined, terminated string instead of a silent
 * half-written response).
 *
 * This growth path is what lets endpoints like GET /api/tickets/<id>/archive
 * return multi-megabyte HTML (every image attachment is embedded as base64,
 * which inflates the size by ~33%) without silently truncating to an empty
 * page — which was the original bug: jb_raw() previously set `truncated=1`
 * and returned WITHOUT copying anything whenever a single write exceeded the
 * fixed 128 KB api_buf, leaving the response body completely empty.
 */
#define JB_MAX_CAP (64UL * 1024 * 1024)   /* 64 MiB hard cap */

typedef struct {
    char  *buf;        /* current backing storage (caller's or heap)        */
    size_t pos;        /* bytes written so far (excluding NUL)              */
    size_t cap;        /* bytes allocated at *buf (including NUL room)      */
    int    truncated;  /* 1 once JB_MAX_CAP was hit — further writes drop  */
    int    heap_owned; /* 1 iff *buf was malloc'd by us and must be freed  */
} JB;

/*
 * Ensure at least `need` bytes are free past j->pos.  Returns 1 on success
 * (either there was already enough room, or we grew the heap buffer to fit),
 * 0 if the buffer cannot be grown any further (JB_MAX_CAP reached) — in
 * which case the caller should set j->truncated and skip the write.
 */
/*
 * jb_ensure — internal helper used by jb_raw / jb_printf.
 *
 * NOTE: this function references g_last_heap_response (defined further down
 * near api_handle_free_response) so that any heap allocation made for a JB
 * buffer is tracked and can later be freed by the HTTP layer.  We forward-
 * declare it here.
 */
static char *g_last_heap_response;

static int jb_ensure(JB *j, size_t need) {
    if (j->truncated) return 0;
    size_t free_bytes = j->cap > j->pos ? j->cap - j->pos : 0;
    if (free_bytes >= need + 1) return 1;     /* +1 for the NUL terminator */

    /* Not enough room — must grow.  Only possible if we already upgraded
     * to a heap buffer (or are willing to now). */
    size_t want = j->pos + need + 1;
    size_t new_cap = j->cap ? j->cap : 4096;
    while (new_cap < want) {
        if (new_cap >= JB_MAX_CAP) break;
        new_cap *= 2;
    }
    if (new_cap > JB_MAX_CAP) new_cap = JB_MAX_CAP;
    if (new_cap < want) {
        /* Even at the cap we can't fit this write.  Mark truncated and
         * refuse — the caller will skip the write, preserving whatever we
         * already have in the buffer rather than corrupting it. */
        j->truncated = 1;
        return 0;
    }

    char *new_buf;
    if (j->heap_owned) {
        new_buf = realloc(j->buf, new_cap);
        if (!new_buf) { j->truncated = 1; return 0; }
    } else {
        /* First overflow: copy the existing caller-provided contents into
         * a fresh heap buffer and switch over. */
        new_buf = malloc(new_cap);
        if (!new_buf) { j->truncated = 1; return 0; }
        if (j->pos) memcpy(new_buf, j->buf, j->pos);
        j->heap_owned = 1;
    }
    j->buf = new_buf;
    j->cap = new_cap;
    /* Track the most recent heap allocation so api_handle_free_response()
     * can find and free it.  Safe because the HTTP server is single-
     * threaded: there is only one in-flight api_handle() call at a time. */
    g_last_heap_response = new_buf;
    return 1;
}

static void jb_raw(JB *j, const char *s) {
    if (!s || j->truncated) return;
    size_t len = strlen(s);
    if (!jb_ensure(j, len)) return;
    memcpy(j->buf + j->pos, s, len);
    j->pos += len;
    j->buf[j->pos] = '\0';
}

static void jb_printf(JB *j, const char *fmt, ...) {
    if (j->truncated) return;
    va_list ap;
    va_start(ap, fmt);
    /* First, compute how many bytes would be needed (vsnprintf with NULL
     * just returns the count).  Then grow if necessary and print for real. */
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) { va_end(ap); j->truncated = 1; return; }

    if (!jb_ensure(j, (size_t)n)) { va_end(ap); return; }

    int n2 = vsnprintf(j->buf + j->pos, j->cap - j->pos, fmt, ap);
    va_end(ap);
    if (n2 < 0) { j->truncated = 1; return; }
    j->pos += (size_t)n2;
}

/* JSON-escape a string and append it, including surrounding quotes. */
static void jb_str(JB *j, const char *s) {
    jb_raw(j, "\"");
    if (!s) { jb_raw(j, "\""); return; }
    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '"':  jb_raw(j, "\\\""); break;
            case '\\': jb_raw(j, "\\\\"); break;
            case '\n': jb_raw(j, "\\n");  break;
            case '\r': jb_raw(j, "\\r");  break;
            case '\t': jb_raw(j, "\\t");  break;
            default: {
                char tmp[2] = { *p, '\0' };
                jb_raw(j, tmp);
            }
        }
    }
    jb_raw(j, "\"");
}

/* Output a uint64 as a JSON string (avoids JS precision loss). */
static void jb_u64str(JB *j, sqlite3_int64 v) {
    jb_printf(j, "\"%" PRId64 "\"", v);
}

/* ── Query-string parser ─────────────────────────────────────────────────── */

/*
 * get_param – extract a query-string value by name into dst.
 * query may be NULL.  dst is NUL-terminated.
 * Returns 1 if found, 0 if not.
 */
static int get_param(const char *query, const char *name,
                     char *dst, size_t dst_size) {
    if (!query || !name) return 0;
    dst[0] = '\0';
    size_t nlen = strlen(name);
    const char *p = query;
    while (*p) {
        if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            p += nlen + 1;
            size_t i = 0;
            while (*p && *p != '&' && i + 1 < dst_size)
                dst[i++] = *p++;
            dst[i] = '\0';
            return 1;
        }
        /* Skip to next param */
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return 0;
}

/* get_param_i64 – returns default_val if not found or invalid. */
static long long get_param_i64(const char *query, const char *name,
                               long long default_val) {
    char tmp[64];
    if (!get_param(query, name, tmp, sizeof(tmp))) return default_val;
    char *end;
    long long v = strtoll(tmp, &end, 10);
    return (end != tmp) ? v : default_val;
}

/* Decode a URL-encoded string in-place. */
static void url_decode(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            char hex[3] = { r[1], r[2], '\0' };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else if (*r == '+') {
            *w++ = ' '; r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/*
 * get_body_param – parse a URL-encoded POST body for a key.
 * Works identically to get_param but on the body string.
 */
static int get_body_param(const char *body, size_t body_len,
                           const char *name, char *dst, size_t dst_size) {
    if (!body || !body_len) return 0;
    /* Make a local NUL-terminated copy (body may not be NUL-terminated). */
    char *copy = malloc(body_len + 1);
    if (!copy) return 0;
    memcpy(copy, body, body_len);
    copy[body_len] = '\0';
    int found = get_param(copy, name, dst, dst_size);
    if (found) url_decode(dst);
    free(copy);
    return found;
}

/* ── Static labels ──────────────────────────────────────────────────────── */

static const char *action_name(int a) {
    switch (a) {
        case 0: return "WARN";
        case 1: return "KICK";
        case 2: return "BAN";
        case 3: return "TIMEOUT";
        default: return "UNKNOWN";
    }
}

static const char *action_emoji(int a) {
    switch (a) {
        case 0: return "⚠️";
        case 1: return "👢";
        case 2: return "🔨";
        case 3: return "🔇";
        default: return "❓";
    }
}

static const char *ticket_status_name(int s) {
    switch (s) {
        case 0: return "Open";
        case 1: return "In Progress";
        case 2: return "Pending User";
        case 3: return "Resolved";
        case 4: return "Closed";
        default: return "Unknown";
    }
}

static const char *ticket_priority_name(int p) {
    switch (p) {
        case 0: return "Low";
        case 1: return "Medium";
        case 2: return "High";
        case 3: return "Urgent";
        default: return "Unknown";
    }
}

static const char *ticket_outcome_name(int o) {
    switch (o) {
        case 0: return "None";
        case 1: return "Resolved";
        case 2: return "Duplicate";
        case 3: return "Invalid";
        case 4: return "No Response";
        case 5: return "Escalated";
        case 6: return "Other";
        default: return "Unknown";
    }
}

/* ── api_init ───────────────────────────────────────────────────────────── */

int api_init(Database *db) {
    const char *sql =
        /* Guilds blocked from the propagation system via the dashboard */
        "CREATE TABLE IF NOT EXISTS propagation_blocked_guilds ("
        "    guild_id   INTEGER PRIMARY KEY,"
        "    blocked_by INTEGER NOT NULL DEFAULT 0,"
        "    reason     TEXT,"
        "    blocked_at INTEGER DEFAULT (strftime('%s','now'))"
        ");"
        /* Delivery log – created here so the dashboard can query it even
           before the bot module has recorded its first notification.
           message_id stores the Discord message ID of the posted alert so
           broadcast_appeal_update() can edit-in-place rather than posting
           a follow-up.                                                     */
        "CREATE TABLE IF NOT EXISTS propagation_notifications ("
        "    id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    event_id    INTEGER NOT NULL,"
        "    guild_id    INTEGER NOT NULL,"
        "    message_id  INTEGER NOT NULL DEFAULT 0,"
        "    notified_at INTEGER DEFAULT (strftime('%s','now')),"
        "    UNIQUE(event_id, guild_id)"
        ");"
        /* Tickets opened by warned users or staff that are linked to a
           specific propagation alert. Populated by the bot's button
           handler and queryable via GET /api/propagation/events/<id>/tickets. */
        "CREATE TABLE IF NOT EXISTS propagation_linked_tickets ("
        "    id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    alert_id   INTEGER NOT NULL,"
        "    ticket_id  INTEGER,"
        "    guild_id   INTEGER NOT NULL,"
        "    opener_id  INTEGER NOT NULL,"
        "    created_at INTEGER DEFAULT (strftime('%s','now')),"
        "    UNIQUE(alert_id, guild_id, opener_id)"
        ");"
        /* Single-row version counter bumped by every block/unblock action
           so propagation.c can detect dashboard state changes without a
           full restart (see propagation_poll_tick()).                       */
        "CREATE TABLE IF NOT EXISTS propagation_state_version ("
        "    id      INTEGER PRIMARY KEY CHECK(id = 1),"
        "    version INTEGER NOT NULL DEFAULT 0"
        ");"
        "INSERT OR IGNORE INTO propagation_state_version (id, version) VALUES (1, 0);"
        /* Staff message edit/delete audit log — populated by ticket.c and
           surfaced in the dashboard ticket log HTML.                        */
        "CREATE TABLE IF NOT EXISTS ticket_log_events ("
        "    id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    ticket_id   INTEGER NOT NULL,"
        "    event_type  TEXT    NOT NULL,"
        "    msg_index   INTEGER NOT NULL,"
        "    author_name TEXT    NOT NULL DEFAULT '',"
        "    old_content TEXT,"
        "    new_content TEXT,"
        "    occurred_at INTEGER NOT NULL"
        ");";

    char *err = NULL;
    int rc = sqlite3_exec(db->db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[api] init error: %s\n", err);
        sqlite3_free(err);
        return -1;
    }

    /*
     * Schema migration: add message_id to propagation_notifications for
     * databases created before this column was introduced.  sqlite3_exec
     * returns an error if the column already exists; we silently ignore it.
     */
    sqlite3_exec(db->db,
        "ALTER TABLE propagation_notifications ADD COLUMN message_id INTEGER NOT NULL DEFAULT 0;",
        NULL, NULL, NULL);

    return 0;
}

/* ── /api/status ────────────────────────────────────────────────────────── */

static int handle_status(Database *db, JB *j) {
    /* warning count */
    int warn_count = 0;
    {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db->db, "SELECT COUNT(*) FROM warnings", -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW) warn_count = sqlite3_column_int(s, 0);
            sqlite3_finalize(s);
        }
    }
    /* open ticket count */
    int open_tickets = 0;
    {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db->db,
                "SELECT COUNT(*) FROM tickets WHERE status < 3", -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW) open_tickets = sqlite3_column_int(s, 0);
            sqlite3_finalize(s);
        }
    }
    /* guild count */
    int guild_count = 0;
    {
        sqlite3_stmt *s;
        const char *q = "SELECT COUNT(*) FROM ("
                        "  SELECT DISTINCT guild_id FROM mod_logs"
                        "  UNION SELECT DISTINCT guild_id FROM tickets"
                        ")";
        if (sqlite3_prepare_v2(db->db, q, -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW) guild_count = sqlite3_column_int(s, 0);
            sqlite3_finalize(s);
        }
    }
    /* propagation events */
    int prop_count = 0;
    {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db->db,
                "SELECT COUNT(*) FROM propagation_events", -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW) prop_count = sqlite3_column_int(s, 0);
            sqlite3_finalize(s);
        }
    }

    jb_printf(j, "{\"warning_count\":%d,\"open_tickets\":%d,"
                  "\"guild_count\":%d,\"propagation_events\":%d}",
              warn_count, open_tickets, guild_count, prop_count);
    return 200;
}

/* ── /api/guilds ─────────────────────────────────────────────────────────── */

static int handle_guilds(Database *db, JB *j) {
    /*
     * Union of all known guild IDs (from mod_logs / warnings / tickets),
     * with per-guild warning + ticket counts.  The messaging page expects
     * an OBJECT of the shape  { "guilds": [ { "id", "guild_id", "name",
     * "warning_count", "ticket_count" } ] } — that is what we emit here.
     *
     * The `name` column comes from an optional `guilds(guild_id, name)`
     * table.  We first try to prepare the query WITH the guilds join; if
     * that fails (table doesn't exist) we re-prepare a query WITHOUT the
     * join and synthesise "Server <guild_id>" for every row instead.
     */
    const char *sql_with_names =
        "SELECT g.guild_id,"
        "       COALESCE(w.cnt,0) AS warn_cnt,"
        "       COALESCE(t.cnt,0) AS tkt_cnt,"
        "       guilds.name        AS name"
        " FROM ("
        "   SELECT guild_id FROM guilds"
        "   UNION SELECT DISTINCT guild_id FROM mod_logs"
        "   UNION SELECT DISTINCT guild_id FROM warnings"
        "   UNION SELECT DISTINCT guild_id FROM tickets"
        " ) AS g"
        " LEFT JOIN (SELECT guild_id, COUNT(*) AS cnt FROM warnings GROUP BY guild_id) w"
        "   ON w.guild_id = g.guild_id"
        " LEFT JOIN (SELECT guild_id, COUNT(*) AS cnt FROM tickets GROUP BY guild_id) t"
        "   ON t.guild_id = g.guild_id"
        " LEFT JOIN guilds ON guilds.guild_id = g.guild_id"
        " ORDER BY guilds.name, g.guild_id;";

    const char *sql_no_names =
        "SELECT g.guild_id,"
        "       COALESCE(w.cnt,0) AS warn_cnt,"
        "       COALESCE(t.cnt,0) AS tkt_cnt"
        " FROM ("
        "   SELECT DISTINCT guild_id FROM mod_logs"
        "   UNION SELECT DISTINCT guild_id FROM warnings"
        "   UNION SELECT DISTINCT guild_id FROM tickets"
        " ) AS g"
        " LEFT JOIN (SELECT guild_id, COUNT(*) AS cnt FROM warnings GROUP BY guild_id) w"
        "   ON w.guild_id = g.guild_id"
        " LEFT JOIN (SELECT guild_id, COUNT(*) AS cnt FROM tickets GROUP BY guild_id) t"
        "   ON t.guild_id = g.guild_id"
        " ORDER BY g.guild_id;";

    sqlite3_stmt *s = NULL;
    int have_names = 0;
    if (sqlite3_prepare_v2(db->db, sql_with_names, -1, &s, NULL) == SQLITE_OK) {
        have_names = 1;
    } else if (sqlite3_prepare_v2(db->db, sql_no_names, -1, &s, NULL) != SQLITE_OK) {
        /* Both preparations failed — treat as a fatal db error. */
        jb_raw(j, "{\"error\":\"db error\"}");
        return 500;
    }

    jb_raw(j, "{\"guilds\":[");
    int first = 1;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (!first) jb_raw(j, ",");
        first = 0;

        sqlite3_int64 guild_id = sqlite3_column_int64(s, 0);
        int warn_cnt = sqlite3_column_int(s, 1);
        int tkt_cnt  = sqlite3_column_int(s, 2);

        /* name column is only present when we used the with-names query. */
        const char *name = have_names
            ? (const char *)sqlite3_column_text(s, 3)
            : NULL;

        char name_buf[128];
        if (!name || !name[0]) {
            snprintf(name_buf, sizeof name_buf,
                     "Server %lld", (long long)guild_id);
            name = name_buf;
        }

        jb_raw(j, "{\"id\":");
        jb_u64str(j, guild_id);
        jb_raw(j, ",\"guild_id\":");
        jb_u64str(j, guild_id);
        jb_raw(j, ",\"name\":");
        jb_str(j, name);
        jb_printf(j, ",\"warning_count\":%d,\"ticket_count\":%d}",
                  warn_cnt, tkt_cnt);
    }
    sqlite3_finalize(s);
    jb_raw(j, "]}");
    return 200;
}

/* ── /api/mod-logs ──────────────────────────────────────────────────────── */

static int handle_mod_logs(Database *db, const char *query, JB *j) {
    /*
     * All user-supplied filter values are bound as SQL parameters — never
     * interpolated into the SQL string — to prevent SQL injection via the
     * query string (the dashboard is loopback-only but the principle still
     * holds, and it future-proofs the endpoint if it ever gets exposed).
     *
     * Binding layout (1-indexed):
     *   1: action_type (only when both guild and action filters are active)
     *   — guild_id is bound at the same slot for both count and data queries.
     */
    char guild_buf[64] = {0};
    char action_buf[16] = {0};
    long long limit  = get_param_i64(query, "limit",  50);
    long long offset = get_param_i64(query, "offset", 0);
    if (limit < 1 || limit > 200) limit = 50;
    if (offset < 0) offset = 0;

    get_param(query, "guild_id", guild_buf, sizeof(guild_buf));
    get_param(query, "action",   action_buf, sizeof(action_buf));

    /* Validate that guild_id, if supplied, is a positive integer (not arbitrary SQL). */
    sqlite3_int64 guild_filter = 0;
    if (guild_buf[0]) {
        char *end = NULL;
        long long v = strtoll(guild_buf, &end, 10);
        if (end != guild_buf && *end == '\0' && v > 0)
            guild_filter = (sqlite3_int64)v;
        /* else: invalid guild_id — treated as "no filter" rather than erroring. */
    }

    int has_action = action_buf[0] && strcmp(action_buf, "all") != 0;
    int action_filter = 0;
    if (has_action) action_filter = atoi(action_buf);

    /* Build COUNT query with parameterised placeholders. */
    const char *count_sql =
        (guild_filter != 0)
            ? (has_action
                ? "SELECT COUNT(*) FROM mod_logs WHERE guild_id = ? AND action_type = ?"
                : "SELECT COUNT(*) FROM mod_logs WHERE guild_id = ?")
            : "SELECT COUNT(*) FROM mod_logs";

    long long total = 0;
    sqlite3_stmt *cs = NULL;
    if (sqlite3_prepare_v2(db->db, count_sql, -1, &cs, NULL) == SQLITE_OK) {
        int bidx = 1;
        if (guild_filter != 0) sqlite3_bind_int64(cs, bidx++, guild_filter);
        if (has_action)        sqlite3_bind_int(cs, bidx++, action_filter);
        if (sqlite3_step(cs) == SQLITE_ROW)
            total = sqlite3_column_int64(cs, 0);
        sqlite3_finalize(cs);
    }

    /* Build the data query. LIMIT/OFFSET are integer constants here so they
     * can be formatted safely into the SQL — the values themselves are
     * clamped above and not user-supplied strings. */
    char sql[1024];
    if (guild_filter != 0) {
        snprintf(sql, sizeof(sql),
                 "SELECT id, action_type, user_id, guild_id, moderator_id, reason, timestamp"
                 " FROM mod_logs WHERE guild_id = ?%s"
                 " ORDER BY timestamp DESC LIMIT %lld OFFSET %lld",
                 has_action ? " AND action_type = ?" : "",
                 limit, offset);
    } else {
        snprintf(sql, sizeof(sql),
                 "SELECT id, action_type, user_id, guild_id, moderator_id, reason, timestamp"
                 " FROM mod_logs ORDER BY timestamp DESC LIMIT %lld OFFSET %lld",
                 limit, offset);
    }

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->db, sql, -1, &s, NULL) != SQLITE_OK) {
        jb_raw(j, "{\"error\":\"db error\"}");
        return 500;
    }
    /* Bind guild_id and action_type at the same positional slots used above. */
    int bidx = 1;
    if (guild_filter != 0) sqlite3_bind_int64(s, bidx++, guild_filter);
    if (has_action)        sqlite3_bind_int(s, bidx++, action_filter);

    jb_printf(j, "{\"total\":%lld,\"items\":[", total);
    int first = 1;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (!first) jb_raw(j, ",");
        first = 0;
        int at = sqlite3_column_int(s, 1);
        jb_raw(j, "{\"id\":");
        jb_printf(j, "%d", sqlite3_column_int(s, 0));
        jb_raw(j, ",\"action_type\":"); jb_printf(j, "%d", at);
        jb_raw(j, ",\"action_name\":"); jb_str(j, action_name(at));
        jb_raw(j, ",\"action_emoji\":"); jb_str(j, action_emoji(at));
        jb_raw(j, ",\"user_id\":");      jb_u64str(j, sqlite3_column_int64(s, 2));
        jb_raw(j, ",\"guild_id\":");     jb_u64str(j, sqlite3_column_int64(s, 3));
        jb_raw(j, ",\"moderator_id\":"); jb_u64str(j, sqlite3_column_int64(s, 4));
        jb_raw(j, ",\"reason\":");       jb_str(j, (const char*)sqlite3_column_text(s, 5));
        jb_raw(j, ",\"timestamp\":");    jb_printf(j, "%lld", (long long)sqlite3_column_int64(s, 6));
        jb_raw(j, "}");
    }
    sqlite3_finalize(s);
    jb_raw(j, "]}");
    return 200;
}

/* ── /api/warnings ──────────────────────────────────────────────────────── */

static int handle_warnings(Database *db, const char *query, JB *j) {
    char guild_buf[64] = {0};
    char user_buf[64]  = {0};
    get_param(query, "guild_id", guild_buf, sizeof(guild_buf));
    get_param(query, "user_id",  user_buf,  sizeof(user_buf));

    const char *sql;
    if (guild_buf[0] && user_buf[0])
        sql = "SELECT id, user_id, guild_id, moderator_id, reason, timestamp"
              " FROM warnings WHERE guild_id = ? AND user_id = ? ORDER BY timestamp DESC";
    else if (guild_buf[0])
        sql = "SELECT id, user_id, guild_id, moderator_id, reason, timestamp"
              " FROM warnings WHERE guild_id = ? ORDER BY timestamp DESC LIMIT 100";
    else
        sql = "SELECT id, user_id, guild_id, moderator_id, reason, timestamp"
              " FROM warnings ORDER BY timestamp DESC LIMIT 100";

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->db, sql, -1, &s, NULL) != SQLITE_OK) {
        jb_raw(j, "{\"error\":\"db error\"}");
        return 500;
    }
    if (guild_buf[0]) sqlite3_bind_int64(s, 1, atoll(guild_buf));
    if (user_buf[0])  sqlite3_bind_int64(s, 2, atoll(user_buf));

    jb_raw(j, "[");
    int first = 1;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (!first) jb_raw(j, ",");
        first = 0;
        jb_printf(j, "{\"id\":%d", sqlite3_column_int(s, 0));
        jb_raw(j, ",\"user_id\":");      jb_u64str(j, sqlite3_column_int64(s, 1));
        jb_raw(j, ",\"guild_id\":");     jb_u64str(j, sqlite3_column_int64(s, 2));
        jb_raw(j, ",\"moderator_id\":"); jb_u64str(j, sqlite3_column_int64(s, 3));
        jb_raw(j, ",\"reason\":");       jb_str(j, (const char*)sqlite3_column_text(s, 4));
        jb_raw(j, ",\"timestamp\":");    jb_printf(j, "%lld", (long long)sqlite3_column_int64(s, 5));
        jb_raw(j, "}");
    }
    sqlite3_finalize(s);
    jb_raw(j, "]");
    return 200;
}

/* ── /api/propagation/guilds ────────────────────────────────────────────── */

static int handle_prop_guilds(Database *db, JB *j) {
    /*
     * UNION both tables so guilds that are only in propagation_blocked_guilds
     * (blocked before they ever configured the bot) still appear in the list.
     */
    const char *sql =
        "SELECT g.guild_id,"
        "       pc.notification_channel,"
        "       COALESCE(pc.opted_in, 0) AS opted_in,"
        "       CASE WHEN pb.guild_id IS NOT NULL THEN 1 ELSE 0 END AS is_blocked,"
        "       pb.reason    AS block_reason,"
        "       pb.blocked_at"
        " FROM ("
        "   SELECT guild_id FROM propagation_guild_config"
        "   UNION"
        "   SELECT guild_id FROM propagation_blocked_guilds"
        " ) g"
        " LEFT JOIN propagation_guild_config    pc ON pc.guild_id = g.guild_id"
        " LEFT JOIN propagation_blocked_guilds pb ON pb.guild_id = g.guild_id"
        " ORDER BY g.guild_id;";

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->db, sql, -1, &s, NULL) != SQLITE_OK) {
        jb_raw(j, "[]");
        return 200;
    }

    jb_raw(j, "[");
    int first = 1;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (!first) jb_raw(j, ",");
        first = 0;
        jb_raw(j,    "{\"guild_id\":");    jb_u64str(j, sqlite3_column_int64(s, 0));
        jb_raw(j,    ",\"channel_id\":"); jb_u64str(j, sqlite3_column_int64(s, 1));
        jb_printf(j, ",\"opted_in\":%d",   sqlite3_column_int(s, 2));
        jb_printf(j, ",\"is_blocked\":%d", sqlite3_column_int(s, 3));
        jb_raw(j,    ",\"block_reason\":"); jb_str(j, (const char *)sqlite3_column_text(s, 4));
        jb_printf(j, ",\"blocked_at\":%lld}", (long long)sqlite3_column_int64(s, 5));
    }
    sqlite3_finalize(s);
    jb_raw(j, "]");
    return 200;
}

/* ── /api/propagation/events ────────────────────────────────────────────── */

static int handle_prop_events(Database *db, const char *query, JB *j) {
    long long limit  = get_param_i64(query, "limit",  50);
    long long offset = get_param_i64(query, "offset", 0);
    if (limit < 1 || limit > 200) limit = 50;
    if (offset < 0) offset = 0;

    char user_buf[64]  = {0};
    char guild_buf[64] = {0};
    get_param(query, "user_id",         user_buf,  sizeof(user_buf));
    get_param(query, "source_guild_id", guild_buf, sizeof(guild_buf));

    /*
     * Parse the filter values as integers up-front and bind them as SQL
     * parameters below.  We never splice raw user input into the SQL string —
     * that was a SQL-injection hole in the previous implementation.
     */
    sqlite3_int64 user_filter  = 0;
    sqlite3_int64 guild_filter = 0;
    if (user_buf[0])  user_filter  = (sqlite3_int64)strtoll(user_buf,  NULL, 10);
    if (guild_buf[0]) guild_filter = (sqlite3_int64)strtoll(guild_buf, NULL, 10);

    const char *where_clause = "";
    if (user_filter != 0 && guild_filter != 0)
        where_clause = " WHERE pe.target_user_id = ? AND pe.source_guild_id = ?";
    else if (user_filter != 0)
        where_clause = " WHERE pe.target_user_id = ?";
    else if (guild_filter != 0)
        where_clause = " WHERE pe.source_guild_id = ?";

    /* Total matching rows (for dashboard pagination). */
    long long total = 0;
    {
        char count_sql[512];
        snprintf(count_sql, sizeof(count_sql),
                 "SELECT COUNT(*) FROM propagation_events pe%s", where_clause);
        sqlite3_stmt *cs;
        if (sqlite3_prepare_v2(db->db, count_sql, -1, &cs, NULL) == SQLITE_OK) {
            int bidx = 1;
            if (user_filter  != 0) sqlite3_bind_int64(cs, bidx++, user_filter);
            if (guild_filter != 0) sqlite3_bind_int64(cs, bidx++, guild_filter);
            if (sqlite3_step(cs) == SQLITE_ROW)
                total = sqlite3_column_int64(cs, 0);
            sqlite3_finalize(cs);
        }
    }

    /*
     * Columns:  0=id  1=target_user_id  2=source_guild_id  3=moderator_id
     *           4=reason  5=evidence_url  6=timestamp
     *           7=severity  8=report_count  9=weighted_confirmation_score
     *           10=notified_count  11=appeal_status (NULL if no appeal)
     *           12=category 13=corroboration_count
     *
     * IDs are serialised as JSON strings to preserve JS integer precision
     * for 64-bit snowflakes — this includes the alert id itself.
     */
    char sql[1536];
    snprintf(sql, sizeof(sql),
             "SELECT pe.id, pe.target_user_id, pe.source_guild_id,"
             "       pe.moderator_id, pe.reason, pe.evidence_url, pe.timestamp,"
             "       pe.severity, pe.report_count, pe.weighted_confirmation_score,"
             "       COUNT(DISTINCT pn.guild_id) AS notified_count,"
             "       pa.status AS appeal_status,"
             "       pe.category, pe.corroboration_count"
             " FROM propagation_events pe"
             " LEFT JOIN propagation_notifications pn ON pn.event_id = pe.id"
             " LEFT JOIN propagation_appeals pa ON pa.propagation_id = pe.id"
             "%s"
             " GROUP BY pe.id"
             " ORDER BY pe.timestamp DESC"
             " LIMIT %lld OFFSET %lld",
             where_clause, limit, offset);

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->db, sql, -1, &s, NULL) != SQLITE_OK) {
        jb_printf(j, "{\"total\":0,\"items\":[]}");
        return 200;
    }

    /* Bind filter parameters in the same order as the COUNT query. */
    int bidx = 1;
    if (user_filter  != 0) sqlite3_bind_int64(s, bidx++, user_filter);
    if (guild_filter != 0) sqlite3_bind_int64(s, bidx++, guild_filter);

    jb_printf(j, "{\"total\":%lld,\"items\":[", total);
    int first = 1;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (!first) jb_raw(j, ",");
        first = 0;
        int sev = sqlite3_column_int(s, 7);
        /* id as string — avoids JS precision loss for large snowflakes. */
        jb_raw(j,    "{\"id\":");                               jb_u64str(j, sqlite3_column_int64(s, 0));
        jb_raw(j,    ",\"target_user_id\":");                  jb_u64str(j, sqlite3_column_int64(s, 1));
        jb_raw(j,    ",\"source_guild_id\":");                 jb_u64str(j, sqlite3_column_int64(s, 2));
        jb_raw(j,    ",\"moderator_id\":");                    jb_u64str(j, sqlite3_column_int64(s, 3));
        jb_raw(j,    ",\"reason\":");                          jb_str(j, (const char *)sqlite3_column_text(s, 4));
        jb_raw(j,    ",\"evidence_url\":");                    jb_str(j, (const char *)sqlite3_column_text(s, 5));
        jb_printf(j, ",\"timestamp\":%lld",                   (long long)sqlite3_column_int64(s, 6));
        jb_printf(j, ",\"severity\":%d",                      sev);
        jb_printf(j, ",\"report_count\":%d",                  sqlite3_column_int(s, 8));
        jb_printf(j, ",\"weighted_confirmation_score\":%d",   sqlite3_column_int(s, 9));
        jb_printf(j, ",\"notified_count\":%d",                sqlite3_column_int(s, 10));
        /* appeal_status is NULL when no appeal has been filed. */
        if (sqlite3_column_type(s, 11) == SQLITE_NULL)
            jb_raw(j, ",\"appeal_status\":null");
        else
            jb_printf(j, ",\"appeal_status\":%d", sqlite3_column_int(s, 11));
        jb_printf(j, ",\"category\":%d",              sqlite3_column_int(s, 12));
        jb_printf(j, ",\"corroboration_count\":%d}",  sqlite3_column_int(s, 13));
    }
    sqlite3_finalize(s);
    jb_raw(j, "]}");
    return 200;
}

/* ── /api/propagation/blocked ───────────────────────────────────────────── */

static int handle_prop_blocked(Database *db, JB *j) {
    const char *sql =
        "SELECT guild_id, blocked_by, reason, blocked_at"
        " FROM propagation_blocked_guilds ORDER BY blocked_at DESC;";

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->db, sql, -1, &s, NULL) != SQLITE_OK) {
        jb_raw(j, "[]");
        return 200;
    }
    jb_raw(j, "[");
    int first = 1;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (!first) jb_raw(j, ",");
        first = 0;
        jb_raw(j, "{\"guild_id\":"); jb_u64str(j, sqlite3_column_int64(s, 0));
        jb_raw(j, ",\"blocked_by\":"); jb_u64str(j, sqlite3_column_int64(s, 1));
        jb_raw(j, ",\"reason\":"); jb_str(j, (const char*)sqlite3_column_text(s, 2));
        jb_printf(j, ",\"blocked_at\":%lld}", (long long)sqlite3_column_int64(s, 3));
    }
    sqlite3_finalize(s);
    jb_raw(j, "]");
    return 200;
}

/* ── POST /api/propagation/block ────────────────────────────────────────── */

static int handle_prop_block(Database *db, const char *body,
                              size_t body_len, JB *j) {
    char guild_buf[64] = {0};
    char reason_buf[512] = {0};
    if (!get_body_param(body, body_len, "guild_id", guild_buf, sizeof(guild_buf))
            || !guild_buf[0]) {
        jb_raw(j, "{\"ok\":false,\"error\":\"missing guild_id\"}");
        return 400;
    }
    get_body_param(body, body_len, "reason", reason_buf, sizeof(reason_buf));

    const char *sql =
        "INSERT OR REPLACE INTO propagation_blocked_guilds"
        " (guild_id, blocked_by, reason) VALUES (?, 0, ?);";

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->db, sql, -1, &s, NULL) != SQLITE_OK) {
        jb_raw(j, "{\"ok\":false,\"error\":\"db error\"}");
        return 500;
    }
    sqlite3_bind_int64(s, 1, atoll(guild_buf));
    sqlite3_bind_text (s, 2, reason_buf[0] ? reason_buf : "Blocked via dashboard",
                       -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);

    if (rc != SQLITE_DONE) {
        jb_raw(j, "{\"ok\":false,\"error\":\"insert failed\"}");
        return 500;
    }

    /* Bump the shared state version so propagation.c detects the change. */
    sqlite3_exec(db->db,
        "UPDATE propagation_state_version SET version = version + 1 WHERE id = 1;",
        NULL, NULL, NULL);

    jb_raw(j, "{\"ok\":true}");
    return 200;
}

/* ── POST /api/propagation/unblock ─────────────────────────────────────── */

static int handle_prop_unblock(Database *db, const char *body,
                                size_t body_len, JB *j) {
    char guild_buf[64] = {0};
    if (!get_body_param(body, body_len, "guild_id", guild_buf, sizeof(guild_buf))
            || !guild_buf[0]) {
        jb_raw(j, "{\"ok\":false,\"error\":\"missing guild_id\"}");
        return 400;
    }

    const char *sql =
        "DELETE FROM propagation_blocked_guilds WHERE guild_id = ?;";

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->db, sql, -1, &s, NULL) != SQLITE_OK) {
        jb_raw(j, "{\"ok\":false,\"error\":\"db error\"}");
        return 500;
    }
    sqlite3_bind_int64(s, 1, atoll(guild_buf));
    sqlite3_step(s);
    sqlite3_finalize(s);

    /* Bump the shared state version so propagation.c detects the change. */
    sqlite3_exec(db->db,
        "UPDATE propagation_state_version SET version = version + 1 WHERE id = 1;",
        NULL, NULL, NULL);

    jb_raw(j, "{\"ok\":true}");
    return 200;
}

/* ── GET /api/propagation/events/<id>/tickets ───────────────────────────── */
/*
 * Returns all linked-ticket records for a given propagation alert.
 * The bot's button handler (on_propagation_component_interaction) writes
 * to propagation_linked_tickets; the dashboard reads it here.
 */
static int handle_prop_event_tickets(Database *db, const char *path, JB *j) {
    /*
     * Path is "/api/propagation/events/<id>/tickets".
     * Walk past the prefix to reach the numeric alert id.
     */
    const char *after = path + strlen("/api/propagation/events/");
    int64_t alert_id = (int64_t)strtoll(after, NULL, 10);
    if (alert_id <= 0) {
        jb_raw(j, "{\"error\":\"invalid alert id\"}");
        return 400;
    }

    const char *sql =
        "SELECT id, alert_id, ticket_id, guild_id, opener_id, created_at"
        " FROM propagation_linked_tickets"
        " WHERE alert_id = ?"
        " ORDER BY created_at ASC;";

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->db, sql, -1, &s, NULL) != SQLITE_OK) {
        jb_raw(j, "{\"error\":\"db error\"}");
        return 500;
    }
    sqlite3_bind_int64(s, 1, alert_id);

    jb_printf(j, "{\"alert_id\":\"%" PRId64 "\",\"tickets\":[", alert_id);
    int first = 1;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (!first) jb_raw(j, ",");
        first = 0;
        jb_printf(j, "{\"id\":%lld", (long long)sqlite3_column_int64(s, 0));
        jb_raw(j, ",\"alert_id\":");   jb_u64str(j, sqlite3_column_int64(s, 1));
        /* ticket_id may be NULL if a full ticket hasn't been created yet. */
        if (sqlite3_column_type(s, 2) == SQLITE_NULL)
            jb_raw(j, ",\"ticket_id\":null");
        else
            jb_printf(j, ",\"ticket_id\":%lld", (long long)sqlite3_column_int64(s, 2));
        jb_raw(j, ",\"guild_id\":");   jb_u64str(j, sqlite3_column_int64(s, 3));
        jb_raw(j, ",\"opener_id\":");  jb_u64str(j, sqlite3_column_int64(s, 4));
        jb_printf(j, ",\"created_at\":%lld}", (long long)sqlite3_column_int64(s, 5));
    }
    sqlite3_finalize(s);
    jb_raw(j, "]}");
    return 200;
}

/* ── /api/tickets ───────────────────────────────────────────────────────── */

static int handle_tickets(Database *db, const char *query, JB *j) {
    char guild_buf[64]   = {0};
    char status_buf[16]  = {0};
    get_param(query, "guild_id", guild_buf,  sizeof(guild_buf));
    get_param(query, "status",   status_buf, sizeof(status_buf));

    /*
     * Parse filter values as integers up-front; bind them as parameters
     * below rather than splicing the raw query string into the SQL.
     */
    sqlite3_int64 guild_filter  = 0;
    int           status_filter = -1;  /* -1 = no filter */
    if (guild_buf[0])
        guild_filter = (sqlite3_int64)strtoll(guild_buf, NULL, 10);
    if (status_buf[0] && strcmp(status_buf, "all") != 0)
        status_filter = atoi(status_buf);

    const char *sql;
    if (guild_filter != 0 && status_filter >= 0) {
        sql = "SELECT id, channel_id, guild_id, opener_id, assigned_to,"
              "       status, priority, outcome, subject, created_at, updated_at"
              " FROM tickets WHERE guild_id = ? AND status = ?"
              " ORDER BY created_at DESC LIMIT 100";
    } else if (guild_filter != 0) {
        sql = "SELECT id, channel_id, guild_id, opener_id, assigned_to,"
              "       status, priority, outcome, subject, created_at, updated_at"
              " FROM tickets WHERE guild_id = ?"
              " ORDER BY created_at DESC LIMIT 100";
    } else {
        sql = "SELECT id, channel_id, guild_id, opener_id, assigned_to,"
              "       status, priority, outcome, subject, created_at, updated_at"
              " FROM tickets ORDER BY created_at DESC LIMIT 100";
    }

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->db, sql, -1, &s, NULL) != SQLITE_OK) {
        jb_raw(j, "[]");
        return 200;
    }
    int bidx = 1;
    if (guild_filter != 0) sqlite3_bind_int64(s, bidx++, guild_filter);
    if (status_filter >= 0) sqlite3_bind_int(s, bidx++, status_filter);

    jb_raw(j, "[");
    int first = 1;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (!first) jb_raw(j, ",");
        first = 0;
        int status   = sqlite3_column_int(s, 5);
        int priority = sqlite3_column_int(s, 6);
        int outcome  = sqlite3_column_int(s, 7);
        jb_printf(j, "{\"id\":%d", sqlite3_column_int(s, 0));
        jb_raw(j, ",\"channel_id\":"); jb_u64str(j, sqlite3_column_int64(s, 1));
        jb_raw(j, ",\"guild_id\":");   jb_u64str(j, sqlite3_column_int64(s, 2));
        jb_raw(j, ",\"opener_id\":");  jb_u64str(j, sqlite3_column_int64(s, 3));
        jb_raw(j, ",\"assigned_to\":"); jb_u64str(j, sqlite3_column_int64(s, 4));
        jb_printf(j, ",\"status\":%d", status);
        jb_raw(j, ",\"status_name\":"); jb_str(j, ticket_status_name(status));
        jb_printf(j, ",\"priority\":%d", priority);
        jb_raw(j, ",\"priority_name\":"); jb_str(j, ticket_priority_name(priority));
        jb_printf(j, ",\"outcome\":%d", outcome);
        jb_raw(j, ",\"outcome_name\":"); jb_str(j, ticket_outcome_name(outcome));
        jb_raw(j, ",\"subject\":"); jb_str(j, (const char*)sqlite3_column_text(s, 8));
        jb_printf(j, ",\"created_at\":%lld", (long long)sqlite3_column_int64(s, 9));
        jb_printf(j, ",\"updated_at\":%lld}", (long long)sqlite3_column_int64(s, 10));
    }
    sqlite3_finalize(s);
    jb_raw(j, "]");
    return 200;
}

/* ── /api/tickets/<id> ──────────────────────────────────────────────────── */

static int handle_ticket_detail(Database *db, const char *path, JB *j) {
    /* Extract numeric ID from path "/api/tickets/NNN" */
    const char *id_str = strrchr(path, '/');
    if (!id_str || !*(id_str + 1)) {
        jb_raw(j, "{\"error\":\"missing ticket id\"}");
        return 400;
    }
    int ticket_id = atoi(id_str + 1);
    if (ticket_id <= 0) {
        jb_raw(j, "{\"error\":\"invalid ticket id\"}");
        return 400;
    }

    /* Main ticket record */
    const char *sql =
        "SELECT id, channel_id, guild_id, opener_id, assigned_to,"
        "       status, priority, outcome, subject, outcome_notes,"
        "       created_at, updated_at, closed_at"
        " FROM tickets WHERE id = ?;";

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->db, sql, -1, &s, NULL) != SQLITE_OK) {
        jb_raw(j, "{\"error\":\"db error\"}");
        return 500;
    }
    sqlite3_bind_int(s, 1, ticket_id);

    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        jb_raw(j, "{\"error\":\"not found\"}");
        return 404;
    }

    int status   = sqlite3_column_int(s, 5);
    int priority = sqlite3_column_int(s, 6);
    int outcome  = sqlite3_column_int(s, 7);

    jb_printf(j, "{\"id\":%d", sqlite3_column_int(s, 0));
    jb_raw(j, ",\"channel_id\":"); jb_u64str(j, sqlite3_column_int64(s, 1));
    jb_raw(j, ",\"guild_id\":");   jb_u64str(j, sqlite3_column_int64(s, 2));
    jb_raw(j, ",\"opener_id\":");  jb_u64str(j, sqlite3_column_int64(s, 3));
    jb_raw(j, ",\"assigned_to\":"); jb_u64str(j, sqlite3_column_int64(s, 4));
    jb_printf(j, ",\"status\":%d", status);
    jb_raw(j, ",\"status_name\":"); jb_str(j, ticket_status_name(status));
    jb_printf(j, ",\"priority\":%d", priority);
    jb_raw(j, ",\"priority_name\":"); jb_str(j, ticket_priority_name(priority));
    jb_printf(j, ",\"outcome\":%d", outcome);
    jb_raw(j, ",\"outcome_name\":"); jb_str(j, ticket_outcome_name(outcome));
    jb_raw(j, ",\"subject\":"); jb_str(j, (const char*)sqlite3_column_text(s, 8));
    jb_raw(j, ",\"outcome_notes\":"); jb_str(j, (const char*)sqlite3_column_text(s, 9));
    jb_printf(j, ",\"created_at\":%lld", (long long)sqlite3_column_int64(s, 10));
    jb_printf(j, ",\"updated_at\":%lld", (long long)sqlite3_column_int64(s, 11));
    jb_printf(j, ",\"closed_at\":%lld",  (long long)sqlite3_column_int64(s, 12));
    sqlite3_finalize(s);

    /* Notes */
    const char *note_sql =
        "SELECT id, author_id, content, is_pinned, created_at"
        " FROM ticket_notes WHERE ticket_id = ?"
        " ORDER BY is_pinned DESC, created_at ASC;";

    if (sqlite3_prepare_v2(db->db, note_sql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int(s, 1, ticket_id);
        jb_raw(j, ",\"notes\":[");
        int first = 1;
        while (sqlite3_step(s) == SQLITE_ROW) {
            if (!first) jb_raw(j, ",");
            first = 0;
            jb_printf(j, "{\"id\":%d", sqlite3_column_int(s, 0));
            jb_raw(j, ",\"author_id\":"); jb_u64str(j, sqlite3_column_int64(s, 1));
            jb_raw(j, ",\"content\":"); jb_str(j, (const char*)sqlite3_column_text(s, 2));
            jb_printf(j, ",\"is_pinned\":%d", sqlite3_column_int(s, 3));
            jb_printf(j, ",\"created_at\":%lld}", (long long)sqlite3_column_int64(s, 4));
        }
        sqlite3_finalize(s);
        jb_raw(j, "]");
    }

    /* Staff message edit/delete log */
    const char *log_sql =
        "SELECT id, event_type, msg_index, author_name,"
        "       old_content, new_content, occurred_at"
        " FROM ticket_log_events WHERE ticket_id = ?"
        " ORDER BY occurred_at ASC;";

    if (sqlite3_prepare_v2(db->db, log_sql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int(s, 1, ticket_id);
        jb_raw(j, ",\"log_events\":[");
        int first = 1;
        while (sqlite3_step(s) == SQLITE_ROW) {
            if (!first) jb_raw(j, ",");
            first = 0;
            jb_printf(j, "{\"id\":%d", sqlite3_column_int(s, 0));
            jb_raw(j, ",\"event_type\":"); jb_str(j, (const char *)sqlite3_column_text(s, 1));
            jb_printf(j, ",\"msg_index\":%d", sqlite3_column_int(s, 2));
            jb_raw(j, ",\"author_name\":"); jb_str(j, (const char *)sqlite3_column_text(s, 3));
            jb_raw(j, ",\"old_content\":"); jb_str(j, (const char *)sqlite3_column_text(s, 4));
            const char *nc = (const char *)sqlite3_column_text(s, 5);
            if (nc) { jb_raw(j, ",\"new_content\":"); jb_str(j, nc); }
            else      jb_raw(j, ",\"new_content\":null");
            jb_printf(j, ",\"occurred_at\":%lld}", (long long)sqlite3_column_int64(s, 6));
        }
        sqlite3_finalize(s);
        jb_raw(j, "]");
    }

    jb_raw(j, "}");
    return 200;
}
static int handle_ticket_events(const char *query, JB *j) {
    long long since = get_param_i64(query, "since", 0);
    /* http_sse_poll writes directly into the JB buffer. */
    http_sse_poll(since, j->buf + j->pos, j->cap - j->pos);
    j->pos += strlen(j->buf + j->pos);
    return 200;
}

/* ── GET /api/tickets/<id>/log ──────────────────────────────────────────── */

/*
 * Returns the ordered edit/delete audit log for a single ticket.
 * The dashboard ticket log HTML polls this (or receives events via SSE)
 * to render a live timeline of staff message modifications.
 */
static int handle_ticket_log(Database *db, const char *path, JB *j) {
    /* Path is "/api/tickets/<id>/log" — extract the numeric ID. */
    const char *after_tickets = path + strlen("/api/tickets/");
    int ticket_id = atoi(after_tickets);
    if (ticket_id <= 0) {
        jb_raw(j, "{\"error\":\"invalid ticket id\"}");
        return 400;
    }

    const char *sql =
        "SELECT id, event_type, msg_index, author_name,"
        "       old_content, new_content, occurred_at"
        " FROM ticket_log_events WHERE ticket_id = ?"
        " ORDER BY occurred_at ASC;";

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->db, sql, -1, &s, NULL) != SQLITE_OK) {
        jb_raw(j, "{\"error\":\"db error\"}");
        return 500;
    }
    sqlite3_bind_int(s, 1, ticket_id);

    jb_printf(j, "{\"ticket_id\":%d,\"events\":[", ticket_id);
    int first = 1;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (!first) jb_raw(j, ",");
        first = 0;
        jb_printf(j, "{\"id\":%d", sqlite3_column_int(s, 0));
        jb_raw(j, ",\"event_type\":"); jb_str(j, (const char *)sqlite3_column_text(s, 1));
        jb_printf(j, ",\"msg_index\":%d", sqlite3_column_int(s, 2));
        jb_raw(j, ",\"author_name\":"); jb_str(j, (const char *)sqlite3_column_text(s, 3));
        jb_raw(j, ",\"old_content\":"); jb_str(j, (const char *)sqlite3_column_text(s, 4));
        /* new_content is NULL for deletions. */
        const char *nc = (const char *)sqlite3_column_text(s, 5);
        if (nc) { jb_raw(j, ",\"new_content\":"); jb_str(j, nc); }
        else      jb_raw(j, ",\"new_content\":null");
        jb_printf(j, ",\"occurred_at\":%lld}", (long long)sqlite3_column_int64(s, 6));
    }
    sqlite3_finalize(s);
    jb_raw(j, "]}");
    return 200;
}

/* ── GET /api/tickets/<id>/chat ─────────────────────────────────────────── */

/*
 * Returns the ordered message history for a single ticket from
 * ticket_messages.  Used by the dashboard's "live chat" view while the
 * ticket is open (polled every few seconds alongside SSE events), and as
 * the source for the archive button on closed tickets (see /archive below).
 */
static int handle_ticket_chat(Database *db, const char *path, JB *j) {
    const char *after_tickets = path + strlen("/api/tickets/");
    int ticket_id = atoi(after_tickets);
    if (ticket_id <= 0) {
        jb_raw(j, "{\"error\":\"invalid ticket id\"}");
        return 400;
    }

    const char *sql =
        "SELECT id, direction, author_id, author_name, content, created_at"
        " FROM ticket_messages WHERE ticket_id = ?"
        " ORDER BY created_at ASC, id ASC;";

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->db, sql, -1, &s, NULL) != SQLITE_OK) {
        jb_raw(j, "{\"error\":\"db error\"}");
        return 500;
    }
    sqlite3_bind_int(s, 1, ticket_id);

    jb_printf(j, "{\"ticket_id\":%d,\"messages\":[", ticket_id);
    int first = 1;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (!first) jb_raw(j, ",");
        first = 0;
        jb_printf(j, "{\"id\":%lld", (long long)sqlite3_column_int64(s, 0));
        jb_raw(j, ",\"direction\":"); jb_str(j, (const char *)sqlite3_column_text(s, 1));
        jb_raw(j, ",\"author_id\":"); jb_u64str(j, (uint64_t)sqlite3_column_int64(s, 2));
        jb_raw(j, ",\"author_name\":"); jb_str(j, (const char *)sqlite3_column_text(s, 3));
        jb_raw(j, ",\"content\":"); jb_str(j, (const char *)sqlite3_column_text(s, 4));
        jb_printf(j, ",\"created_at\":%lld}", (long long)sqlite3_column_int64(s, 5));
    }
    sqlite3_finalize(s);
    jb_raw(j, "]}");
    return 200;
}

/* ── GET /api/tickets/<id>/archive ─────────────────────────────────────── */

/*
 * Serves the pre-rendered HTML archive for a closed ticket.
 * Returns the HTML directly rather than JSON (Content-Type: text/html).
 *
 * If the ticket is still open (no entry in ticket_archives yet), renders
 * a live snapshot via ticket_render_archive_html().
 */
static int handle_ticket_archive(Database *db, const char *path, JB *j,
                                  char **content_type_out) {
    const char *after_tickets = path + strlen("/api/tickets/");
    int ticket_id = atoi(after_tickets);
    if (ticket_id <= 0) {
        jb_raw(j, "<h1>Invalid ticket ID</h1>");
        if (content_type_out) *content_type_out = "text/html; charset=utf-8";
        return 400;
    }

    if (content_type_out) *content_type_out = "text/html; charset=utf-8";

    /* Try pre-rendered archive first. */
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->db,
            "SELECT html FROM ticket_archives WHERE ticket_id = ? LIMIT 1;",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int(s, 1, ticket_id);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char *html = (const char *)sqlite3_column_text(s, 0);
            if (html) {
                /*
                 * Pre-check the size before calling jb_raw: if the archive
                 * is larger than the JB hard cap, jb_raw would silently
                 * drop it (truncated=1) and the user would get a blank
                 * page.  Show a clear error instead.
                 */
                size_t html_len = strlen(html);
                if (html_len >= JB_MAX_CAP) {
                    sqlite3_finalize(s);
                    jb_raw(j, "<!DOCTYPE html><html><head><title>Archive too large</title></head>"
                              "<body><h1>Archive too large to display inline</h1>"
                              "<p>This ticket's transcript exceeds the server's in-memory cap. "
                              "Use the database export tool to retrieve it directly.</p></body></html>");
                    return 500;
                }
                jb_raw(j, html);
                sqlite3_finalize(s);
                return 200;
            }
        }
        sqlite3_finalize(s);
    }

    /* Fall back to live render (ticket still open, or archive wasn't stored). */
    char *html = ticket_render_archive_html(db, ticket_id);
    if (!html) {
        jb_raw(j, "<h1>Ticket not found or no messages yet.</h1>");
        return 404;
    }
    jb_raw(j, html);
    free(html);
    return 200;
}

static int handle_channels(Database *db, const char *path, JB *j) {
    /* Extract guild_id from path "/api/guilds/<guild_id>/channels" */
    const char *after_guilds = path + strlen("/api/guilds/");
    char guild_id_str[64] = {0};
    const char *p = after_guilds;
    int i = 0;
    while (*p && *p != '/' && i < 63) {
        guild_id_str[i++] = *p++;
    }
    guild_id_str[i] = '\0';

    if (!guild_id_str[0]) {
        jb_raw(j, "{\"error\":\"missing guild_id\"}");
        return 400;
    }

    sqlite3_int64 guild_id = atoll(guild_id_str);

    /*
     * Query the cached channels for this guild.  The channels table is
     * populated by messaging_refresh_guild_channels() (called from
     * on_guild_create and POST /api/guilds/<id>/refresh-channels).
     *
     * If the table doesn't exist (e.g. a very old database that hasn't been
     * migrated), we return an EMPTY channel list — NOT a synthetic "general"
     * channel.  The old fallback (id = guild_id, name = "general") produced
     * a fake channel_id that discord_create_message() always rejected with
     * ORCAcode 1, breaking the Messaging page end-to-end.
     */
    const char *sql = "SELECT id, guild_id, name, type FROM channels"
                      " WHERE guild_id = ? ORDER BY name;";
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(db->db, sql, -1, &s, NULL) != SQLITE_OK) {
        /* channels table missing — return an empty list (NOT a fake channel). */
        jb_raw(j, "{\"channels\":[]}");
        return 200;
    }
    sqlite3_bind_int64(s, 1, guild_id);

    jb_raw(j, "{\"channels\":[");
    int first = 1;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (!first) jb_raw(j, ",");
        first = 0;
        jb_raw(j, "{\"id\":");       jb_u64str(j, sqlite3_column_int64(s, 0));
        jb_raw(j, ",\"guild_id\":"); jb_u64str(j, sqlite3_column_int64(s, 1));
        jb_raw(j, ",\"name\":");     jb_str(j, (const char *)sqlite3_column_text(s, 2));
        jb_printf(j, ",\"type\":%d}", sqlite3_column_int(s, 3));
    }
    sqlite3_finalize(s);
    jb_raw(j, "]}");
    return 200;
}

/*
 * handle_refresh_channels
 *
 * POST /api/guilds/<guild_id>/refresh-channels
 *
 * Kicks off a background refresh of the guild's cached channel list (a REST
 * GET to Discord's /guilds/{id}/channels).  Returns immediately so the
 * single-threaded HTTP server isn't blocked on a curl call.  The dashboard
 * can re-poll GET /api/guilds/<id>/channels shortly afterwards to see the
 * updated list.
 */
static int handle_refresh_channels(Database *db, const char *path, JB *j) {
    const char *after_guilds = path + strlen("/api/guilds/");
    char guild_id_str[64] = {0};
    const char *p = after_guilds;
    int i = 0;
    while (*p && *p != '/' && i < 63) guild_id_str[i++] = *p++;
    guild_id_str[i] = '\0';
    if (!guild_id_str[0]) { jb_raw(j, "{\"error\":\"missing guild_id\"}"); return 400; }

    sqlite3_int64 guild_id = atoll(guild_id_str);
    if (guild_id == 0) { jb_raw(j, "{\"error\":\"invalid guild_id\"}"); return 400; }

    /* Kick off a background refresh so we don't block the (single-threaded)
     * HTTP server on a curl GET. The dashboard can re-poll /channels shortly
     * after to see the updated list. */
    extern void messaging_refresh_guild_channels_async(Database *db,
                                                       u64_snowflake_t guild_id);
    messaging_refresh_guild_channels_async(db, (u64_snowflake_t)guild_id);

    jb_raw(j, "{\"ok\":true,\"message\":\"refreshing channels in the background\",\"guild_id\":");
    jb_u64str(j, guild_id);
    jb_raw(j, "}");
    return 200;
}

/* ── POST /api/send-message ────────────────────────────────────────────── */

static int handle_send_message(Database *db, const char *body,
                                size_t body_len, JB *j) {
    char guild_id_buf[64] = {0};
    char channel_id_buf[64] = {0};
    char content_buf[2048] = {0};

    if (!get_body_param(body, body_len, "guild_id", guild_id_buf, sizeof(guild_id_buf))
            || !guild_id_buf[0]) {
        jb_raw(j, "{\"error\":\"missing guild_id\"}");
        return 400;
    }

    if (!get_body_param(body, body_len, "channel_id", channel_id_buf, sizeof(channel_id_buf))
            || !channel_id_buf[0]) {
        jb_raw(j, "{\"error\":\"missing channel_id\"}");
        return 400;
    }

    if (!get_body_param(body, body_len, "content", content_buf, sizeof(content_buf))
            || !content_buf[0]) {
        jb_raw(j, "{\"error\":\"missing content\"}");
        return 400;
    }

    /* Validate content length (Discord limit is 2000 characters) */
    if (strlen(content_buf) > 2000) {
        jb_raw(j, "{\"error\":\"content exceeds 2000 character limit\"}");
        return 400;
    }

    sqlite3_int64 guild_id = atoll(guild_id_buf);
    sqlite3_int64 channel_id = atoll(channel_id_buf);

    /*
     * Attempt to send the message through the bot.
     * This function should be implemented in your bot code to actually
     * dispatch the message to Discord.
     * For now, we'll log it to indicate success and can integrate with
     * bot_send_message() or similar once the bot module is available.
     */
    extern int bot_send_message(sqlite3_int64 guild_id, sqlite3_int64 channel_id,
                                 const char *content);

    int send_result = bot_send_message(guild_id, channel_id, content_buf);

    if (send_result != 0) {
        jb_raw(j, "{\"error\":\"failed to send message\"}");
        return 500;
    }

    jb_raw(j, "{\"ok\":true,\"guild_id\":");
    jb_u64str(j, guild_id);
    jb_raw(j, ",\"channel_id\":");
    jb_u64str(j, channel_id);
    jb_raw(j, ",\"message_length\":");
    jb_printf(j, "%zu", strlen(content_buf));
    jb_raw(j, "}");
    return 200;
}

/* ── Response Content-Type helper ─────────────────────────────────────────
 *
 * Non-JSON endpoints (currently only /api/tickets/<id>/archive) call
 * api_set_response_content_type() so http_server.c can set the right header.
 * Reset to NULL at the top of every api_handle() call.
 * ── */

static const char *g_response_content_type = NULL;

void api_set_response_content_type(const char *ct) {
    g_response_content_type = ct;
}

const char *api_get_response_content_type(void) {
    return g_response_content_type ? g_response_content_type : "application/json";
}

/* ── POST /api/send-components-v2 ──────────────────────────────────────── */
/*
 * Accepts a URL-encoded body with three required fields:
 *   guild_id    – target guild snowflake (for logging)
 *   channel_id  – target channel snowflake
 *   components  – JSON array string of Components V2 component objects
 *                 (the raw value of the "components" key, e.g.
 *                  '[{"type":10,"content":"Hello!"}]')
 *
 * The `components` value is embedded directly into the IS_COMPONENTS_V2
 * payload (flag 1<<15) and POSTed to Discord via libcurl, bypassing Orca's
 * typed struct layer exactly as handle_send_message() does for plain text.
 *
 * This endpoint is for dashboard-driven sends where the component tree is
 * already built on the client side.  For bot-internal sends, use the
 * CV2Msg builder API (cv2_msg_new / cv2_add_text / ... / cv2_send).
 */
static int handle_send_components_v2(Database *db, const char *body,
                                       size_t body_len, JB *j) {
    (void)db;   /* not used for this route */

    char guild_id_buf[64]     = {0};
    char channel_id_buf[64]   = {0};
    /* Components JSON can be large; give it generous room.
     * Discord's payload cap is well above 8 KB in practice for components. */
    char *components_buf = calloc(1, 16384);
    if (!components_buf) {
        jb_raw(j, "{\"error\":\"OOM allocating components buffer\"}");
        return 500;
    }

    /* ── Parse body fields ─────────────────────────────────────────────── */
    if (!get_body_param(body, body_len, "guild_id",
                        guild_id_buf, sizeof guild_id_buf)
            || !guild_id_buf[0]) {
        jb_raw(j, "{\"error\":\"missing guild_id\"}");
        free(components_buf);
        return 400;
    }

    if (!get_body_param(body, body_len, "channel_id",
                        channel_id_buf, sizeof channel_id_buf)
            || !channel_id_buf[0]) {
        jb_raw(j, "{\"error\":\"missing channel_id\"}");
        free(components_buf);
        return 400;
    }

    if (!get_body_param(body, body_len, "components",
                        components_buf, 16384)
            || !components_buf[0]) {
        jb_raw(j, "{\"error\":\"missing components\"}");
        free(components_buf);
        return 400;
    }

    /* Basic sanity check: the value must start with '['. */
    if (components_buf[0] != '[') {
        jb_raw(j, "{\"error\":\"components must be a JSON array\"}");
        free(components_buf);
        return 400;
    }

    sqlite3_int64 guild_id   = atoll(guild_id_buf);
    sqlite3_int64 channel_id = atoll(channel_id_buf);

    /* ── Build the Components V2 envelope ────────────────────────────────
     * {"flags":32768,"components":<components_buf>}
     * ------------------------------------------------------------------- */
    /* 32 bytes overhead for {"flags":32768,"components":} + NUL */
    size_t clen  = strlen(components_buf);
    size_t total = clen + 48;
    char  *payload = malloc(total);
    if (!payload) {
        jb_raw(j, "{\"error\":\"OOM building payload\"}");
        free(components_buf);
        return 500;
    }
    snprintf(payload, total, "{\"flags\":32768,\"components\":%s}",
             components_buf);
    free(components_buf);

    /* ── Dispatch via the bot's cv2 send helper ──────────────────────────
     * We use the extern cv2_send_raw() shim rather than constructing a
     * CV2Msg, since the component JSON is already assembled by the caller.
     * ------------------------------------------------------------------- */
    extern int bot_send_cv2_raw(sqlite3_int64 guild_id,
                                 sqlite3_int64 channel_id,
                                 const char   *json_payload);

    int rc = bot_send_cv2_raw(guild_id, channel_id, payload);
    free(payload);

    if (rc != 0) {
        jb_raw(j, "{\"error\":\"failed to send components v2 message\"}");
        return 500;
    }

    jb_raw(j, "{\"ok\":true,\"guild_id\":");
    jb_u64str(j, guild_id);
    jb_raw(j, ",\"channel_id\":");
    jb_u64str(j, channel_id);
    jb_raw(j, "}");
    return 200;
}

/* ── Router ─────────────────────────────────────────────────────────────── */

int api_handle(Database *db,
               const char *method, const char *path,
               const char *query,  const char *body, size_t body_len,
               char *out_buf, size_t out_size,
               char **out_buf_ptr, size_t *out_len_ptr) {
    /* Reset the per-call content-type override so callers always get a
     * valid value from api_get_response_content_type() regardless of call
     * order. */
    g_response_content_type = NULL;

    /*
     * JB starts out wrapping the caller-provided buffer.  If a write would
     * overflow it, jb_ensure() transparently malloc's a larger buffer and
     * flips j.heap_owned to 1.  After the handler returns we expose the
     * final buffer pointer + length to the caller via out_buf_ptr /
     * out_len_ptr so the HTTP layer can send_response() the right memory
     * (which may now be heap-allocated rather than the original out_buf).
     */
    JB j = {
        .buf        = out_buf,
        .pos        = 0,
        .cap        = out_size,
        .truncated  = 0,
        .heap_owned = 0,
    };
    out_buf[0] = '\0';

    int is_get  = method && strcmp(method, "GET")  == 0;
    int is_post = method && strcmp(method, "POST") == 0;

    int status;
    if (is_get && strcmp(path, "/api/status") == 0)
        status = handle_status(db, &j);

    else if (is_get && strcmp(path, "/api/guilds") == 0)
        status = handle_guilds(db, &j);

    /* /api/guilds/<guild_id>/channels — get channels for a guild */
    else if (is_get && strncmp(path, "/api/guilds/", 12) == 0
               && strstr(path + 12, "/channels") != NULL)
        status = handle_channels(db, path, &j);

    /* POST /api/guilds/<guild_id>/refresh-channels — manually re-sync a
     * guild's cached channel list from Discord's REST API (non-blocking). */
    else if (is_post && strncmp(path, "/api/guilds/", 12) == 0
               && strstr(path + 12, "/refresh-channels") != NULL)
        status = handle_refresh_channels(db, path, &j);

    else if (is_get && strcmp(path, "/api/mod-logs") == 0)
        status = handle_mod_logs(db, query, &j);

    else if (is_get && strcmp(path, "/api/warnings") == 0)
        status = handle_warnings(db, query, &j);

    else if (is_get && strcmp(path, "/api/propagation/guilds") == 0)
        status = handle_prop_guilds(db, &j);

    else if (is_get && strcmp(path, "/api/propagation/events") == 0)
        status = handle_prop_events(db, query, &j);

    else if (is_get && strcmp(path, "/api/propagation/blocked") == 0)
        status = handle_prop_blocked(db, &j);

    else if (is_post && strcmp(path, "/api/propagation/block") == 0)
        status = handle_prop_block(db, body, body_len, &j);

    else if (is_post && strcmp(path, "/api/propagation/unblock") == 0)
        status = handle_prop_unblock(db, body, body_len, &j);

    /* /api/propagation/events/<id>/tickets — linked tickets for one alert.
       Must be matched before the bare /events handler. */
    else if (is_get && strncmp(path, "/api/propagation/events/", 24) == 0
               && strstr(path + 24, "/tickets") != NULL)
        status = handle_prop_event_tickets(db, path, &j);

    else if (is_post && strcmp(path, "/api/send-message") == 0)
        status = handle_send_message(db, body, body_len, &j);

    else if (is_post && strcmp(path, "/api/send-components-v2") == 0)
        status = handle_send_components_v2(db, body, body_len, &j);

    else if (is_get && strcmp(path, "/api/tickets") == 0)
        status = handle_tickets(db, query, &j);

    /* Exact named sub-routes must precede the numeric wildcard. */
    else if (is_get && strcmp(path, "/api/tickets/events") == 0)
        status = handle_ticket_events(query, &j);

    /* /api/tickets/<id>/log — edit/delete audit log for one ticket. */
    else if (is_get && strncmp(path, "/api/tickets/", 13) == 0
               && strstr(path + 13, "/log") != NULL)
        status = handle_ticket_log(db, path, &j);

    /* /api/tickets/<id>/chat — live message history (user/staff/system). */
    else if (is_get && strncmp(path, "/api/tickets/", 13) == 0
               && strstr(path + 13, "/chat") != NULL)
        status = handle_ticket_chat(db, path, &j);

    /* /api/tickets/<id>/archive — full HTML transcript. */
    else if (is_get && strncmp(path, "/api/tickets/", 13) == 0
               && strstr(path + 13, "/archive") != NULL) {
        char *ct = NULL;
        status = handle_ticket_archive(db, path, &j, &ct);
        if (ct) api_set_response_content_type(ct);
    }

    else if (is_get && strncmp(path, "/api/tickets/", 13) == 0)
        status = handle_ticket_detail(db, path, &j);

    else {
        jb_raw(&j, "{\"error\":\"not found\"}");
        status = 404;
    }

    /*
     * Report truncation loudly — silent truncation was the original bug
     * that produced empty archive pages, so we want any future regression
     * to be obvious in the log rather than invisible in the dashboard.
     */
    if (j.truncated) {
        fprintf(stderr,
                "[api] WARNING: response for %s %s was truncated at %zu bytes "
                "(cap=%zu, heap_owned=%d).  The client will receive a partial "
                "response.\n",
                method ? method : "?",
                path   ? path   : "?",
                j.pos, j.cap, j.heap_owned);
    }

    /* Export the final buffer + length to the caller.  The buffer may be
     * either the original out_buf (heap_owned=0) or a malloc'd replacement
     * (heap_owned=1); the caller frees it via api_handle_free_response(). */
    if (out_buf_ptr) *out_buf_ptr = j.buf;
    if (out_len_ptr) *out_len_ptr = j.pos;

    return status;
}

/*
 * api_handle_free_response
 *
 * Frees the response buffer returned by api_handle() if (and only if) the
 * API layer allocated it on the heap.  The caller passes the same char**
 * it passed to api_handle() as out_buf_ptr; we inspect the heap_owned flag
 * stored at the address to decide whether to free.
 *
 * Tracking the heap_owned flag in the buffer itself is not possible (the
 * buffer is raw bytes), so we keep a single static pointer (declared
 * earlier near jb_ensure) that records the most recent heap allocation.
 * This is safe because the HTTP server is single-threaded — only one
 * api_handle() call is in flight at a time, and api_handle_free_response()
 * is always called before the next one.
 */
void api_handle_free_response(char **out_buf_ptr) {
    if (!out_buf_ptr) return;
    /*
     * If the caller's pointer matches the most recent heap allocation,
     * free it.  Otherwise the response stayed in the caller's original
     * (stack/static) buffer and there is nothing to free.
     */
    if (g_last_heap_response && *out_buf_ptr == g_last_heap_response) {
        free(g_last_heap_response);
        g_last_heap_response = NULL;
    }
    *out_buf_ptr = NULL;
}