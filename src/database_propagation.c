/*
 * database_propagation.c
 *
 * SQLite-backed persistence for the cross-server propagation system.
 *
 * New in this revision
 * ────────────────────
 *  • propagation_events gains: severity, weighted_confirmation_score,
 *    report_count
 *  • propagation_guild_trust  – per-guild trust levels
 *  • propagation_appeals      – user appeal records with live status
 *  • propagation_alert_reports – staff flagging of false/abusive alerts
 *  • propagation_central_config – single-row central admin destination
 *
 * Assumption: Database.db is a sqlite3 * handle.
 * If your Database struct uses a different field name, update DB() below.
 */

#include "database.h"
#include "database_propagation.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DB(x) ((x)->db)

/* ── Enum helpers ────────────────────────────────────────────────────────── */

const char *severity_name(PropagationSeverity s) {
    switch (s) {
        case SEVERITY_UNCONFIRMED: return "Unconfirmed";
        case SEVERITY_LOW:         return "Low";
        case SEVERITY_MEDIUM:      return "Medium";
        case SEVERITY_HIGH:        return "High";
        case SEVERITY_CRITICAL:    return "Critical";
        default:                   return "Unknown";
    }
}

const char *severity_emoji(PropagationSeverity s) {
    switch (s) {
        case SEVERITY_UNCONFIRMED: return "⬜";
        case SEVERITY_LOW:         return "🟡";
        case SEVERITY_MEDIUM:      return "🟠";
        case SEVERITY_HIGH:        return "🔴";
        case SEVERITY_CRITICAL:    return "🚨";
        default:                   return "❔";
    }
}

const char *severity_colour_hex(PropagationSeverity s) {
    switch (s) {
        case SEVERITY_UNCONFIRMED: return "#95a5a6";
        case SEVERITY_LOW:         return "#f1c40f";
        case SEVERITY_MEDIUM:      return "#e67e22";
        case SEVERITY_HIGH:        return "#e74c3c";
        case SEVERITY_CRITICAL:    return "#8e44ad";
        default:                   return "#7f8c8d";
    }
}

const char *trust_level_name(GuildTrustLevel t) {
    switch (t) {
        case TRUST_UNVERIFIED: return "Unverified";
        case TRUST_TRUSTED:    return "Trusted";
        case TRUST_VERIFIED:   return "Verified";
        case TRUST_PARTNER:    return "Partner";
        default:               return "Unknown";
    }
}

const char *trust_level_badge(GuildTrustLevel t) {
    switch (t) {
        case TRUST_UNVERIFIED: return "⚪";
        case TRUST_TRUSTED:    return "🔵";
        case TRUST_VERIFIED:   return "✅";
        case TRUST_PARTNER:    return "⭐";
        default:               return "❔";
    }
}

const char *appeal_status_name(AppealStatus a) {
    switch (a) {
        case APPEAL_PENDING:      return "Pending";
        case APPEAL_UNDER_REVIEW: return "Under Review";
        case APPEAL_APPROVED:     return "Approved";
        case APPEAL_DENIED:       return "Denied";
        default:                  return "Unknown";
    }
}

const char *appeal_status_emoji(AppealStatus a) {
    switch (a) {
        case APPEAL_PENDING:      return "⏳";
        case APPEAL_UNDER_REVIEW: return "🔍";
        case APPEAL_APPROVED:     return "✅";
        case APPEAL_DENIED:       return "❌";
        default:                  return "❔";
    }
}

/* ── Trust weight for severity calculation ───────────────────────────────── */

static int trust_weight(GuildTrustLevel t) {
    switch (t) {
        case TRUST_UNVERIFIED: return 1;
        case TRUST_TRUSTED:    return 2;
        case TRUST_VERIFIED:   return 3;
        case TRUST_PARTNER:    return 4;
        default:               return 1;
    }
}

static PropagationSeverity score_to_severity(int score) {
    if (score <= 0)  return SEVERITY_UNCONFIRMED;
    if (score <= 3)  return SEVERITY_LOW;
    if (score <= 8)  return SEVERITY_MEDIUM;
    if (score <= 15) return SEVERITY_HIGH;
    return SEVERITY_CRITICAL;
}

/* ── Table creation ─────────────────────────────────────────────────────── */

int db_propagation_init(Database *db) {
    const char *sql =
        /* Core alert log – extended with severity, score, report count. */
        "CREATE TABLE IF NOT EXISTS propagation_events ("
        "  id                          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  target_user_id              INTEGER NOT NULL,"
        "  source_guild_id             INTEGER NOT NULL,"
        "  moderator_id                INTEGER NOT NULL,"
        "  reason                      TEXT,"
        "  evidence_url                TEXT    NOT NULL,"
        "  timestamp                   INTEGER NOT NULL,"
        "  severity                    INTEGER NOT NULL DEFAULT 0,"
        "  weighted_confirmation_score INTEGER NOT NULL DEFAULT 0,"
        "  report_count                INTEGER NOT NULL DEFAULT 0"
        ");"

        /* Which guilds have received which alerts. */
        "CREATE TABLE IF NOT EXISTS propagation_notifications ("
        "  id             INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  propagation_id INTEGER NOT NULL,"
        "  guild_id       INTEGER NOT NULL,"
        "  notified_at    INTEGER NOT NULL,"
        "  FOREIGN KEY(propagation_id) REFERENCES propagation_events(id),"
        "  UNIQUE(propagation_id, guild_id)"
        ");"

        /* Per-guild alert channel + opt-in flag. */
        "CREATE TABLE IF NOT EXISTS propagation_guild_config ("
        "  guild_id             INTEGER PRIMARY KEY,"
        "  notification_channel INTEGER NOT NULL,"
        "  opted_in             INTEGER NOT NULL DEFAULT 1"
        ");"

        /* Per-guild trust level – affects severity weighting. */
        "CREATE TABLE IF NOT EXISTS propagation_guild_trust ("
        "  guild_id    INTEGER PRIMARY KEY,"
        "  trust_level INTEGER NOT NULL DEFAULT 0,"
        "  set_by      INTEGER NOT NULL,"
        "  set_at      INTEGER NOT NULL,"
        "  notes       TEXT"
        ");"

        /* Moderators permanently banned from issuing alerts. */
        "CREATE TABLE IF NOT EXISTS propagation_blacklist ("
        "  moderator_id INTEGER PRIMARY KEY,"
        "  banned_by    INTEGER NOT NULL,"
        "  reason       TEXT,"
        "  banned_at    INTEGER NOT NULL"
        ");"

        /* Staff reports flagging an alert as suspicious or false. */
        "CREATE TABLE IF NOT EXISTS propagation_alert_reports ("
        "  id             INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  propagation_id INTEGER NOT NULL,"
        "  reporter_guild INTEGER NOT NULL,"
        "  reporter_mod   INTEGER NOT NULL,"
        "  reason         TEXT    NOT NULL,"
        "  reported_at    INTEGER NOT NULL,"
        "  FOREIGN KEY(propagation_id) REFERENCES propagation_events(id),"
        "  UNIQUE(propagation_id, reporter_guild, reporter_mod)"
        ");"

        /* User appeals with live status. */
        "CREATE TABLE IF NOT EXISTS propagation_appeals ("
        "  id              INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  propagation_id  INTEGER NOT NULL,"
        "  user_id         INTEGER NOT NULL,"
        "  statement       TEXT    NOT NULL,"
        "  status          INTEGER NOT NULL DEFAULT 0,"
        "  reviewed_by     INTEGER,"
        "  reviewed_at     INTEGER,"
        "  reviewer_notes  TEXT,"
        "  submitted_at    INTEGER NOT NULL,"
        "  FOREIGN KEY(propagation_id) REFERENCES propagation_events(id),"
        "  UNIQUE(propagation_id, user_id)"
        ");"

        /* Single-row table for the central oversight destination. */
        "CREATE TABLE IF NOT EXISTS propagation_central_config ("
        "  id         INTEGER PRIMARY KEY CHECK (id = 1),"
        "  guild_id   INTEGER NOT NULL,"
        "  channel_id INTEGER NOT NULL,"
        "  set_by     INTEGER NOT NULL,"
        "  set_at     INTEGER NOT NULL"
        ");"

        /* Every guild the bot is active in. */
        "CREATE TABLE IF NOT EXISTS known_guilds ("
        "  guild_id      INTEGER PRIMARY KEY,"
        "  registered_at INTEGER NOT NULL"
        ");"
        
        /* Maps a community's public server to its paired staff server. */
        "CREATE TABLE IF NOT EXISTS propagation_guild_pairs ("
        "  main_guild_id  INTEGER PRIMARY KEY,"
        "  staff_guild_id INTEGER NOT NULL,"
        "  registered_by  INTEGER NOT NULL,"
        "  registered_at  INTEGER NOT NULL"
        ");";

    char *err = NULL;
    int rc = sqlite3_exec(DB(db), sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[prop-db] Failed to create tables: %s\n", err);
        sqlite3_free(err);
        return -1;
    }
    printf("[prop-db] Propagation tables ready.\n");
    return 0;
}

/* ── Propagation events ─────────────────────────────────────────────────── */

int64_t db_add_propagation_event(Database   *db,
                                  uint64_t    target_user_id,
                                  uint64_t    source_guild_id,
                                  uint64_t    moderator_id,
                                  const char *reason,
                                  const char *evidence_url) {
    const char *sql =
        "INSERT INTO propagation_events "
        "(target_user_id, source_guild_id, moderator_id, reason, evidence_url, timestamp) "
        "VALUES (?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[prop-db] prepare add_event: %s\n",
                sqlite3_errmsg(DB(db)));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, (int64_t)target_user_id);
    sqlite3_bind_int64(stmt, 2, (int64_t)source_guild_id);
    sqlite3_bind_int64(stmt, 3, (int64_t)moderator_id);
    sqlite3_bind_text (stmt, 4, reason       ? reason       : "", -1, SQLITE_STATIC);
    sqlite3_bind_text (stmt, 5, evidence_url ? evidence_url : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, (int64_t)time(NULL));

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "[prop-db] step add_event: %s\n", sqlite3_errmsg(DB(db)));
        return -1;
    }
    return (int64_t)sqlite3_last_insert_rowid(DB(db));
}

int db_record_propagation_notification(Database *db,
                                        int64_t   propagation_id,
                                        uint64_t  guild_id) {
    const char *sql =
        "INSERT OR IGNORE INTO propagation_notifications "
        "(propagation_id, guild_id, notified_at) VALUES (?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, propagation_id);
    sqlite3_bind_int64(stmt, 2, (int64_t)guild_id);
    sqlite3_bind_int64(stmt, 3, (int64_t)time(NULL));

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

PropagationSeverity db_recompute_severity(Database *db, int64_t propagation_id) {
    /*
     * Walk every notified guild for this event, look up its trust level,
     * sum the weights, derive a severity, and write it back.
     */
    const char *list_sql =
        "SELECT guild_id FROM propagation_notifications "
        "WHERE propagation_id = ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), list_sql, -1, &stmt, NULL) != SQLITE_OK)
        return SEVERITY_UNCONFIRMED;

    sqlite3_bind_int64(stmt, 1, propagation_id);

    int total_score = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        uint64_t gid = (uint64_t)sqlite3_column_int64(stmt, 0);
        GuildTrustLevel t = db_get_guild_trust(db, gid);
        total_score += trust_weight(t);
    }
    sqlite3_finalize(stmt);

    PropagationSeverity sev = score_to_severity(total_score);

    const char *update_sql =
        "UPDATE propagation_events "
        "SET weighted_confirmation_score = ?, severity = ? "
        "WHERE id = ?;";

    if (sqlite3_prepare_v2(DB(db), update_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int  (stmt, 1, total_score);
        sqlite3_bind_int  (stmt, 2, (int)sev);
        sqlite3_bind_int64(stmt, 3, propagation_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    return sev;
}

/* ── Retrieve events ─────────────────────────────────────────────────────── */

static void populate_event_from_stmt(PropagationEvent *e, sqlite3_stmt *stmt) {
    e->id              = sqlite3_column_int64(stmt, 0);
    e->target_user_id  = (uint64_t)sqlite3_column_int64(stmt, 1);
    e->source_guild_id = (uint64_t)sqlite3_column_int64(stmt, 2);
    e->moderator_id    = (uint64_t)sqlite3_column_int64(stmt, 3);

    const char *reason = (const char *)sqlite3_column_text(stmt, 4);
    const char *evurl  = (const char *)sqlite3_column_text(stmt, 5);
    e->reason       = reason ? strdup(reason) : NULL;
    e->evidence_url = evurl  ? strdup(evurl)  : NULL;

    e->timestamp                   = sqlite3_column_int64(stmt, 6);
    e->severity                    = sqlite3_column_int  (stmt, 7);
    e->weighted_confirmation_score = sqlite3_column_int  (stmt, 8);
    e->report_count                = sqlite3_column_int  (stmt, 9);
}

int db_get_propagation_events(Database          *db,
                               uint64_t           target_user_id,
                               PropagationEvent **events_out,
                               int               *count_out) {
    *events_out = NULL;
    *count_out  = 0;

    const char *sql =
        "SELECT id, target_user_id, source_guild_id, moderator_id, "
        "       reason, evidence_url, timestamp, severity, "
        "       weighted_confirmation_score, report_count "
        "FROM propagation_events "
        "WHERE target_user_id = ? "
        "ORDER BY timestamp DESC;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, (int64_t)target_user_id);

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) count++;
    sqlite3_reset(stmt);

    if (count == 0) { sqlite3_finalize(stmt); return 0; }

    PropagationEvent *events = calloc((size_t)count, sizeof(PropagationEvent));
    if (!events) { sqlite3_finalize(stmt); return -1; }

    int i = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && i < count)
        populate_event_from_stmt(&events[i++], stmt);

    sqlite3_finalize(stmt);
    *events_out = events;
    *count_out  = i;
    return 0;
}

int db_get_propagation_event_by_id(Database         *db,
                                    int64_t           event_id,
                                    PropagationEvent *event_out) {
    const char *sql =
        "SELECT id, target_user_id, source_guild_id, moderator_id, "
        "       reason, evidence_url, timestamp, severity, "
        "       weighted_confirmation_score, report_count "
        "FROM propagation_events WHERE id = ? LIMIT 1;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, event_id);

    int rc = 1;  /* not-found by default */
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        populate_event_from_stmt(event_out, stmt);
        rc = 0;
    }
    sqlite3_finalize(stmt);
    return rc;
}

void db_free_propagation_events(PropagationEvent *events, int count) {
    if (!events) return;
    for (int i = 0; i < count; i++) {
        free(events[i].reason);
        free(events[i].evidence_url);
    }
    free(events);
}

void db_free_propagation_event(PropagationEvent *event) {
    if (!event) return;
    free(event->reason);
    free(event->evidence_url);
}

/* ── Per-guild configuration ─────────────────────────────────────────────── */

int db_set_propagation_config(Database *db,
                               uint64_t  guild_id,
                               uint64_t  channel_id,
                               int       opted_in) {
    const char *sql =
        "INSERT INTO propagation_guild_config (guild_id, notification_channel, opted_in) "
        "VALUES (?, ?, ?) "
        "ON CONFLICT(guild_id) DO UPDATE SET "
        "  notification_channel = excluded.notification_channel,"
        "  opted_in             = excluded.opted_in;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, (int64_t)guild_id);
    sqlite3_bind_int64(stmt, 2, (int64_t)channel_id);
    sqlite3_bind_int  (stmt, 3, opted_in);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

uint64_t db_get_propagation_channel(Database *db, uint64_t guild_id) {
    const char *sql =
        "SELECT notification_channel FROM propagation_guild_config "
        "WHERE guild_id = ? AND opted_in = 1;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_int64(stmt, 1, (int64_t)guild_id);
    uint64_t channel = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        channel = (uint64_t)sqlite3_column_int64(stmt, 0);

    sqlite3_finalize(stmt);
    return channel;
}

bool db_guild_propagation_opted_in(Database *db, uint64_t guild_id) {
    return db_get_propagation_channel(db, guild_id) != 0;
}

int db_get_opted_in_guilds(Database  *db,
                            uint64_t **guild_ids_out,
                            int       *count_out) {
    *guild_ids_out = NULL;
    *count_out     = 0;

    const char *sql =
        "SELECT guild_id FROM propagation_guild_config WHERE opted_in = 1;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) count++;
    sqlite3_reset(stmt);

    if (count == 0) { sqlite3_finalize(stmt); return 0; }

    uint64_t *ids = malloc((size_t)count * sizeof(uint64_t));
    if (!ids) { sqlite3_finalize(stmt); return -1; }

    int i = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && i < count)
        ids[i++] = (uint64_t)sqlite3_column_int64(stmt, 0);

    sqlite3_finalize(stmt);
    *guild_ids_out = ids;
    *count_out     = i;
    return 0;
}

/* ── Guild trust ─────────────────────────────────────────────────────────── */

int db_set_guild_trust(Database       *db,
                        uint64_t        guild_id,
                        GuildTrustLevel level,
                        uint64_t        set_by,
                        const char     *notes) {
    const char *sql =
        "INSERT INTO propagation_guild_trust (guild_id, trust_level, set_by, set_at, notes) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(guild_id) DO UPDATE SET "
        "  trust_level = excluded.trust_level,"
        "  set_by      = excluded.set_by,"
        "  set_at      = excluded.set_at,"
        "  notes       = excluded.notes;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, (int64_t)guild_id);
    sqlite3_bind_int  (stmt, 2, (int)level);
    sqlite3_bind_int64(stmt, 3, (int64_t)set_by);
    sqlite3_bind_int64(stmt, 4, (int64_t)time(NULL));
    sqlite3_bind_text (stmt, 5, notes ? notes : "", -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

GuildTrustLevel db_get_guild_trust(Database *db, uint64_t guild_id) {
    const char *sql =
        "SELECT trust_level FROM propagation_guild_trust WHERE guild_id = ? LIMIT 1;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return TRUST_UNVERIFIED;

    sqlite3_bind_int64(stmt, 1, (int64_t)guild_id);
    GuildTrustLevel t = TRUST_UNVERIFIED;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        t = (GuildTrustLevel)sqlite3_column_int(stmt, 0);

    sqlite3_finalize(stmt);
    return t;
}

/* ── Moderator blacklist ─────────────────────────────────────────────────── */

int db_blacklist_moderator(Database   *db,
                            uint64_t    moderator_id,
                            uint64_t    banned_by,
                            const char *reason) {
    const char *sql =
        "INSERT OR REPLACE INTO propagation_blacklist "
        "(moderator_id, banned_by, reason, banned_at) VALUES (?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, (int64_t)moderator_id);
    sqlite3_bind_int64(stmt, 2, (int64_t)banned_by);
    sqlite3_bind_text (stmt, 3, reason ? reason : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, (int64_t)time(NULL));

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

bool db_is_moderator_blacklisted(Database *db, uint64_t moderator_id) {
    const char *sql =
        "SELECT 1 FROM propagation_blacklist WHERE moderator_id = ? LIMIT 1;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;

    sqlite3_bind_int64(stmt, 1, (int64_t)moderator_id);
    bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

/* ── Alert reports ───────────────────────────────────────────────────────── */

int64_t db_report_alert(Database   *db,
                         int64_t     propagation_id,
                         uint64_t    reporter_guild,
                         uint64_t    reporter_mod,
                         const char *reason) {
    const char *sql =
        "INSERT OR IGNORE INTO propagation_alert_reports "
        "(propagation_id, reporter_guild, reporter_mod, reason, reported_at) "
        "VALUES (?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, propagation_id);
    sqlite3_bind_int64(stmt, 2, (int64_t)reporter_guild);
    sqlite3_bind_int64(stmt, 3, (int64_t)reporter_mod);
    sqlite3_bind_text (stmt, 4, reason ? reason : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, (int64_t)time(NULL));

    int rc = sqlite3_step(stmt);
    int64_t new_id = (rc == SQLITE_DONE)
                   ? (int64_t)sqlite3_last_insert_rowid(DB(db))
                   : -1;
    sqlite3_finalize(stmt);

    if (new_id > 0) {
        /* Bump the denormalised counter on the event row. */
        const char *bump =
            "UPDATE propagation_events SET report_count = report_count + 1 "
            "WHERE id = ?;";
        if (sqlite3_prepare_v2(DB(db), bump, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, propagation_id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    return new_id;
}

int db_get_alert_report_count(Database *db, int64_t propagation_id) {
    const char *sql =
        "SELECT report_count FROM propagation_events WHERE id = ? LIMIT 1;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_int64(stmt, 1, propagation_id);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

/* ── Appeals ─────────────────────────────────────────────────────────────── */

int64_t db_submit_appeal(Database   *db,
                          int64_t     propagation_id,
                          uint64_t    user_id,
                          const char *statement) {
    const char *sql =
        "INSERT OR IGNORE INTO propagation_appeals "
        "(propagation_id, user_id, statement, status, submitted_at) "
        "VALUES (?, ?, ?, 0, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, propagation_id);
    sqlite3_bind_int64(stmt, 2, (int64_t)user_id);
    sqlite3_bind_text (stmt, 3, statement ? statement : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, (int64_t)time(NULL));

    int rc = sqlite3_step(stmt);
    int64_t new_id = (rc == SQLITE_DONE)
                   ? (int64_t)sqlite3_last_insert_rowid(DB(db))
                   : -1;
    sqlite3_finalize(stmt);
    return new_id;
}

int db_update_appeal_status(Database   *db,
                             int64_t     appeal_id,
                             AppealStatus status,
                             uint64_t    reviewed_by,
                             const char *reviewer_notes) {
    const char *sql =
        "UPDATE propagation_appeals "
        "SET status = ?, reviewed_by = ?, reviewed_at = ?, reviewer_notes = ? "
        "WHERE id = ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int  (stmt, 1, (int)status);
    sqlite3_bind_int64(stmt, 2, (int64_t)reviewed_by);
    sqlite3_bind_int64(stmt, 3, (int64_t)time(NULL));
    sqlite3_bind_text (stmt, 4, reviewer_notes ? reviewer_notes : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, appeal_id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

static void populate_appeal_from_stmt(PropagationAppeal *a, sqlite3_stmt *stmt) {
    a->id             = sqlite3_column_int64(stmt, 0);
    a->propagation_id = sqlite3_column_int64(stmt, 1);
    a->user_id        = (uint64_t)sqlite3_column_int64(stmt, 2);

    const char *st = (const char *)sqlite3_column_text(stmt, 3);
    const char *rn = (const char *)sqlite3_column_text(stmt, 7);
    a->statement      = st ? strdup(st) : NULL;
    a->status         = (AppealStatus)sqlite3_column_int(stmt, 4);
    a->reviewed_by    = (uint64_t)sqlite3_column_int64(stmt, 5);
    a->reviewed_at    = sqlite3_column_int64(stmt, 6);
    a->reviewer_notes = rn ? strdup(rn) : NULL;
    a->submitted_at   = sqlite3_column_int64(stmt, 8);
}

int db_get_appeal_for_event(Database          *db,
                             int64_t            propagation_id,
                             uint64_t           user_id,
                             PropagationAppeal *appeal_out) {
    const char *sql =
        "SELECT id, propagation_id, user_id, statement, status, "
        "       reviewed_by, reviewed_at, reviewer_notes, submitted_at "
        "FROM propagation_appeals "
        "WHERE propagation_id = ? AND user_id = ? LIMIT 1;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, propagation_id);
    sqlite3_bind_int64(stmt, 2, (int64_t)user_id);

    int rc = 1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        populate_appeal_from_stmt(appeal_out, stmt);
        rc = 0;
    }
    sqlite3_finalize(stmt);
    return rc;
}

int db_get_appeal_by_id(Database          *db,
                         int64_t            appeal_id,
                         PropagationAppeal *appeal_out) {
    const char *sql =
        "SELECT id, propagation_id, user_id, statement, status, "
        "       reviewed_by, reviewed_at, reviewer_notes, submitted_at "
        "FROM propagation_appeals WHERE id = ? LIMIT 1;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, appeal_id);

    int rc = 1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        populate_appeal_from_stmt(appeal_out, stmt);
        rc = 0;
    }
    sqlite3_finalize(stmt);
    return rc;
}

void db_free_appeal(PropagationAppeal *appeal) {
    if (!appeal) return;
    free(appeal->statement);
    free(appeal->reviewer_notes);
}

/* ── Central admin config ────────────────────────────────────────────────── */

int db_set_central_config(Database *db,
                            uint64_t  guild_id,
                            uint64_t  channel_id,
                            uint64_t  set_by) {
    const char *sql =
        "INSERT INTO propagation_central_config (id, guild_id, channel_id, set_by, set_at) "
        "VALUES (1, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  guild_id   = excluded.guild_id,"
        "  channel_id = excluded.channel_id,"
        "  set_by     = excluded.set_by,"
        "  set_at     = excluded.set_at;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, (int64_t)guild_id);
    sqlite3_bind_int64(stmt, 2, (int64_t)channel_id);
    sqlite3_bind_int64(stmt, 3, (int64_t)set_by);
    sqlite3_bind_int64(stmt, 4, (int64_t)time(NULL));

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

bool db_get_central_config(Database *db,
                             uint64_t *guild_id_out,
                             uint64_t *channel_id_out) {
    const char *sql =
        "SELECT guild_id, channel_id FROM propagation_central_config WHERE id = 1;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (guild_id_out)   *guild_id_out   = (uint64_t)sqlite3_column_int64(stmt, 0);
        if (channel_id_out) *channel_id_out = (uint64_t)sqlite3_column_int64(stmt, 1);
        found = true;
    }
    sqlite3_finalize(stmt);
    return found;
}

/* ── Known-guild registry ────────────────────────────────────────────────── */

int db_register_known_guild(Database *db, uint64_t guild_id) {
    const char *sql =
        "INSERT OR IGNORE INTO known_guilds (guild_id, registered_at) VALUES (?, ?);";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, (int64_t)guild_id);
    sqlite3_bind_int64(stmt, 2, (int64_t)time(NULL));

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* Register or update a main↔staff server pair. */
int db_register_guild_pair(Database *db,
                            uint64_t  main_guild_id,
                            uint64_t  staff_guild_id,
                            uint64_t  registered_by) {
    const char *sql =
        "INSERT INTO propagation_guild_pairs "
        "(main_guild_id, staff_guild_id, registered_by, registered_at) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT(main_guild_id) DO UPDATE SET "
        "  staff_guild_id = excluded.staff_guild_id,"
        "  registered_by  = excluded.registered_by,"
        "  registered_at  = excluded.registered_at;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, (int64_t)main_guild_id);
    sqlite3_bind_int64(stmt, 2, (int64_t)staff_guild_id);
    sqlite3_bind_int64(stmt, 3, (int64_t)registered_by);
    sqlite3_bind_int64(stmt, 4, (int64_t)time(NULL));

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/*
 * Given a community's main guild, return its paired staff guild ID.
 * Returns 0 if no pair is registered.
 */
uint64_t db_get_staff_guild_for(Database *db, uint64_t main_guild_id) {
    const char *sql =
        "SELECT staff_guild_id FROM propagation_guild_pairs "
        "WHERE main_guild_id = ? LIMIT 1;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_int64(stmt, 1, (int64_t)main_guild_id);
    uint64_t staff_id = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        staff_id = (uint64_t)sqlite3_column_int64(stmt, 0);

    sqlite3_finalize(stmt);
    return staff_id;
}

/*
 * Returns true if the given guild is registered as a staff server
 * in any community pair.  Used for permission checks.
 */
bool db_is_staff_guild(Database *db, uint64_t guild_id) {
    const char *sql =
        "SELECT 1 FROM propagation_guild_pairs "
        "WHERE staff_guild_id = ? LIMIT 1;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;

    sqlite3_bind_int64(stmt, 1, (int64_t)guild_id);
    bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}