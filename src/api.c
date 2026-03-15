/*
 * api.c
 *
 * JSON REST API for the dashboard.
 * All snowflake IDs are serialised as strings to preserve JS precision.
 */

#include "http_server.h"
#include "api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include <inttypes.h>

/* ── JSON buffer helper ─────────────────────────────────────────────────── */

typedef struct { char *buf; size_t pos; size_t cap; int truncated; } JB;

static void jb_raw(JB *j, const char *s) {
    size_t len = strlen(s);
    if (j->pos + len + 1 >= j->cap) { j->truncated = 1; return; }
    memcpy(j->buf + j->pos, s, len);
    j->pos += len;
    j->buf[j->pos] = '\0';
}

static void jb_printf(JB *j, const char *fmt, ...) {
    if (j->truncated) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->buf + j->pos, j->cap - j->pos, fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < j->cap - j->pos)
        j->pos += n;
    else
        j->truncated = 1;
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
           before the bot module has recorded its first notification.       */
        "CREATE TABLE IF NOT EXISTS propagation_notifications ("
        "    id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    event_id    INTEGER NOT NULL,"
        "    guild_id    INTEGER NOT NULL,"
        "    notified_at INTEGER DEFAULT (strftime('%s','now')),"
        "    UNIQUE(event_id, guild_id)"
        ");";

    char *err = NULL;
    int rc = sqlite3_exec(db->db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[api] init error: %s\n", err);
        sqlite3_free(err);
        return -1;
    }
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
    /* Union of all known guild IDs, with per-guild warning + ticket counts */
    const char *sql =
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

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->db, sql, -1, &s, NULL) != SQLITE_OK) {
        jb_raw(j, "{\"error\":\"db error\"}");
        return 500;
    }

    jb_raw(j, "[");
    int first = 1;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (!first) jb_raw(j, ",");
        first = 0;
        jb_raw(j, "{\"guild_id\":");
        jb_u64str(j, sqlite3_column_int64(s, 0));
        jb_printf(j, ",\"warning_count\":%d,\"ticket_count\":%d}",
                  sqlite3_column_int(s, 1), sqlite3_column_int(s, 2));
    }
    sqlite3_finalize(s);
    jb_raw(j, "]");
    return 200;
}

/* ── /api/mod-logs ──────────────────────────────────────────────────────── */

static int handle_mod_logs(Database *db, const char *query, JB *j) {
    char guild_buf[64] = {0};
    char action_buf[16] = {0};
    long long limit  = get_param_i64(query, "limit",  50);
    long long offset = get_param_i64(query, "offset", 0);
    if (limit < 1 || limit > 200) limit = 50;

    get_param(query, "guild_id", guild_buf, sizeof(guild_buf));
    get_param(query, "action",   action_buf, sizeof(action_buf));

    /* Count total */
    char count_sql[512];
    if (guild_buf[0]) {
        snprintf(count_sql, sizeof(count_sql),
                 "SELECT COUNT(*) FROM mod_logs WHERE guild_id = %s%s",
                 guild_buf,
                 (action_buf[0] && strcmp(action_buf,"all")!=0)
                   ? " AND action_type = ?" : "");
    } else {
        snprintf(count_sql, sizeof(count_sql), "SELECT COUNT(*) FROM mod_logs");
    }

    /* Full query */
    char sql[1024];
    if (guild_buf[0]) {
        snprintf(sql, sizeof(sql),
                 "SELECT id, action_type, user_id, guild_id, moderator_id, reason, timestamp"
                 " FROM mod_logs WHERE guild_id = %s"
                 "%s"
                 " ORDER BY timestamp DESC LIMIT %lld OFFSET %lld",
                 guild_buf,
                 (action_buf[0] && strcmp(action_buf,"all")!=0)
                   ? " AND action_type = ?" : "",
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
    /* Bind action filter if needed */
    if (guild_buf[0] && action_buf[0] && strcmp(action_buf,"all") != 0)
        sqlite3_bind_int(s, 1, atoi(action_buf));

    jb_raw(j, "{\"items\":[");
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
        "       pc.channel_id,"
        "       COALESCE(pc.opted_in, 0) AS opted_in,"
        "       CASE WHEN pb.guild_id IS NOT NULL THEN 1 ELSE 0 END AS is_blocked,"
        "       pb.reason    AS block_reason,"
        "       pb.blocked_at"
        " FROM ("
        "   SELECT guild_id FROM propagation_config"
        "   UNION"
        "   SELECT guild_id FROM propagation_blocked_guilds"
        " ) g"
        " LEFT JOIN propagation_config         pc ON pc.guild_id = g.guild_id"
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

    char user_buf[64]  = {0};
    char guild_buf[64] = {0};
    get_param(query, "user_id",         user_buf,  sizeof(user_buf));
    get_param(query, "source_guild_id", guild_buf, sizeof(guild_buf));

    /*
     * Columns:  0=id  1=target_user_id  2=source_guild_id  3=moderator_id
     *           4=reason  5=evidence_url  6=timestamp
     *           7=severity  8=report_count  9=weighted_confirmation_score
     *           10=notified_count
     */
    const char *sql_base =
        "SELECT pe.id, pe.target_user_id, pe.source_guild_id,"
        "       pe.moderator_id, pe.reason, pe.evidence_url, pe.timestamp,"
        "       pe.severity, pe.report_count, pe.weighted_confirmation_score,"
        "       COUNT(pn.guild_id) AS notified_count"
        " FROM propagation_events pe"
        " LEFT JOIN propagation_notifications pn ON pn.event_id = pe.id";

    char sql[1024];
    if (user_buf[0] && guild_buf[0]) {
        snprintf(sql, sizeof(sql),
                 "%s WHERE pe.target_user_id = %s AND pe.source_guild_id = %s"
                 " GROUP BY pe.id ORDER BY pe.timestamp DESC LIMIT %lld OFFSET %lld",
                 sql_base, user_buf, guild_buf, limit, offset);
    } else if (user_buf[0]) {
        snprintf(sql, sizeof(sql),
                 "%s WHERE pe.target_user_id = %s"
                 " GROUP BY pe.id ORDER BY pe.timestamp DESC LIMIT %lld OFFSET %lld",
                 sql_base, user_buf, limit, offset);
    } else if (guild_buf[0]) {
        snprintf(sql, sizeof(sql),
                 "%s WHERE pe.source_guild_id = %s"
                 " GROUP BY pe.id ORDER BY pe.timestamp DESC LIMIT %lld OFFSET %lld",
                 sql_base, guild_buf, limit, offset);
    } else {
        snprintf(sql, sizeof(sql),
                 "%s GROUP BY pe.id ORDER BY pe.timestamp DESC LIMIT %lld OFFSET %lld",
                 sql_base, limit, offset);
    }

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db->db, sql, -1, &s, NULL) != SQLITE_OK) {
        jb_raw(j, "{\"items\":[]}");
        return 200;
    }

    jb_raw(j, "{\"items\":[");
    int first = 1;
    while (sqlite3_step(s) == SQLITE_ROW) {
        if (!first) jb_raw(j, ",");
        first = 0;
        int sev = sqlite3_column_int(s, 7);
        jb_printf(j, "{\"id\":%lld",                          (long long)sqlite3_column_int64(s, 0));
        jb_raw(j,    ",\"target_user_id\":");                  jb_u64str(j, sqlite3_column_int64(s, 1));
        jb_raw(j,    ",\"source_guild_id\":");                 jb_u64str(j, sqlite3_column_int64(s, 2));
        jb_raw(j,    ",\"moderator_id\":");                    jb_u64str(j, sqlite3_column_int64(s, 3));
        jb_raw(j,    ",\"reason\":");                          jb_str(j, (const char *)sqlite3_column_text(s, 4));
        jb_raw(j,    ",\"evidence_url\":");                    jb_str(j, (const char *)sqlite3_column_text(s, 5));
        jb_printf(j, ",\"timestamp\":%lld",                   (long long)sqlite3_column_int64(s, 6));
        jb_printf(j, ",\"severity\":%d",                      sev);
        jb_printf(j, ",\"report_count\":%d",                  sqlite3_column_int(s, 8));
        jb_printf(j, ",\"weighted_confirmation_score\":%d",   sqlite3_column_int(s, 9));
        jb_printf(j, ",\"notified_count\":%d}",               sqlite3_column_int(s, 10));
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
    jb_raw(j, "{\"ok\":true}");
    return 200;
}

/* ── /api/tickets ───────────────────────────────────────────────────────── */

static int handle_tickets(Database *db, const char *query, JB *j) {
    char guild_buf[64]   = {0};
    char status_buf[16]  = {0};
    get_param(query, "guild_id", guild_buf,  sizeof(guild_buf));
    get_param(query, "status",   status_buf, sizeof(status_buf));

    char sql[1024];
    if (guild_buf[0] && status_buf[0] && strcmp(status_buf, "all") != 0) {
        snprintf(sql, sizeof(sql),
                 "SELECT id, channel_id, guild_id, opener_id, assigned_to,"
                 "       status, priority, outcome, subject, created_at, updated_at"
                 " FROM tickets WHERE guild_id = %s AND status = %s"
                 " ORDER BY created_at DESC LIMIT 100",
                 guild_buf, status_buf);
    } else if (guild_buf[0]) {
        snprintf(sql, sizeof(sql),
                 "SELECT id, channel_id, guild_id, opener_id, assigned_to,"
                 "       status, priority, outcome, subject, created_at, updated_at"
                 " FROM tickets WHERE guild_id = %s"
                 " ORDER BY created_at DESC LIMIT 100",
                 guild_buf);
    } else {
        snprintf(sql, sizeof(sql),
                 "SELECT id, channel_id, guild_id, opener_id, assigned_to,"
                 "       status, priority, outcome, subject, created_at, updated_at"
                 " FROM tickets ORDER BY created_at DESC LIMIT 100");
    }

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

    jb_raw(j, "}");
    return 200;
}

/* ── GET /api/tickets/events?since=<unix_ts> ──────────────────────────── */
static int handle_ticket_events(const char *query, JB *j) {
    long long since = get_param_i64(query, "since", 0);
    /* http_sse_poll writes directly into the JB buffer. */
    http_sse_poll(since, j->buf + j->pos, j->cap - j->pos);
    j->pos += strlen(j->buf + j->pos);
    return 200;
}

/* ── Router ─────────────────────────────────────────────────────────────── */

int api_handle(Database *db,
               const char *method, const char *path,
               const char *query,  const char *body, size_t body_len,
               char *out_buf, size_t out_size) {
    JB j = { .buf = out_buf, .pos = 0, .cap = out_size, .truncated = 0 };
    out_buf[0] = '\0';

    int is_get  = method && strcmp(method, "GET")  == 0;
    int is_post = method && strcmp(method, "POST") == 0;

    if (is_get && strcmp(path, "/api/status") == 0)
        return handle_status(db, &j);

    if (is_get && strcmp(path, "/api/guilds") == 0)
        return handle_guilds(db, &j);

    if (is_get && strcmp(path, "/api/mod-logs") == 0)
        return handle_mod_logs(db, query, &j);

    if (is_get && strcmp(path, "/api/warnings") == 0)
        return handle_warnings(db, query, &j);

    if (is_get && strcmp(path, "/api/propagation/guilds") == 0)
        return handle_prop_guilds(db, &j);

    if (is_get && strcmp(path, "/api/propagation/events") == 0)
        return handle_prop_events(db, query, &j);

    if (is_get && strcmp(path, "/api/propagation/blocked") == 0)
        return handle_prop_blocked(db, &j);

    if (is_post && strcmp(path, "/api/propagation/block") == 0)
        return handle_prop_block(db, body, body_len, &j);

    if (is_post && strcmp(path, "/api/propagation/unblock") == 0)
        return handle_prop_unblock(db, body, body_len, &j);

    if (is_get && strcmp(path, "/api/tickets") == 0)
        return handle_tickets(db, query, &j);

    /* Exact named sub-routes must precede the numeric wildcard. */
    if (is_get && strcmp(path, "/api/tickets/events") == 0)
        return handle_ticket_events(query, &j);

    if (is_get && strncmp(path, "/api/tickets/", 13) == 0)
        return handle_ticket_detail(db, path, &j);

    jb_raw(&j, "{\"error\":\"not found\"}");
    return 404;
}