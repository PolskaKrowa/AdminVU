/*
 * database_propagation.c
 *
 * SQLite-backed persistence for the cross-server propagation system.
 *
 * Assumption: Database.db is a sqlite3 * handle.
 * If your Database struct uses a different field name, update the
 * DB(x) macro below accordingly.
 */

#include "database_propagation.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Convenience accessor – adjust "db" if your field is named differently. */
#define DB(x) ((x)->db)

/* ── Table creation ─────────────────────────────────────────────────────── */

int db_propagation_init(Database *db) {
    const char *sql =
        /* One row per cross-server alert that is fired. */
        "CREATE TABLE IF NOT EXISTS propagation_events ("
        "  id              INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  target_user_id  INTEGER NOT NULL,"
        "  source_guild_id INTEGER NOT NULL,"
        "  moderator_id    INTEGER NOT NULL,"
        "  reason          TEXT,"
        "  evidence_url    TEXT    NOT NULL,"
        "  timestamp       INTEGER NOT NULL"
        ");"

        /* Tracks which guilds received each alert (for deduplication). */
        "CREATE TABLE IF NOT EXISTS propagation_notifications ("
        "  id             INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  propagation_id INTEGER NOT NULL,"
        "  guild_id       INTEGER NOT NULL,"
        "  notified_at    INTEGER NOT NULL,"
        "  FOREIGN KEY(propagation_id) REFERENCES propagation_events(id)"
        ");"

        /* Per-guild settings: where to post alerts, and whether to receive them. */
        "CREATE TABLE IF NOT EXISTS propagation_guild_config ("
        "  guild_id             INTEGER PRIMARY KEY,"
        "  notification_channel INTEGER NOT NULL,"
        "  opted_in             INTEGER NOT NULL DEFAULT 1"
        ");"

        /* Moderators who have lost the right to issue propagation alerts. */
        "CREATE TABLE IF NOT EXISTS propagation_blacklist ("
        "  moderator_id INTEGER PRIMARY KEY,"
        "  banned_by    INTEGER NOT NULL,"
        "  reason       TEXT,"
        "  banned_at    INTEGER NOT NULL"
        ");"

        /* Every guild the bot is active in (populated via on_guild_create). */
        "CREATE TABLE IF NOT EXISTS known_guilds ("
        "  guild_id      INTEGER PRIMARY KEY,"
        "  registered_at INTEGER NOT NULL"
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

int db_get_propagation_events(Database          *db,
                               uint64_t           target_user_id,
                               PropagationEvent **events_out,
                               int               *count_out) {
    *events_out = NULL;
    *count_out  = 0;

    const char *sql =
        "SELECT id, target_user_id, source_guild_id, moderator_id, "
        "       reason, evidence_url, timestamp "
        "FROM propagation_events "
        "WHERE target_user_id = ? "
        "ORDER BY timestamp DESC;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(DB(db), sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, (int64_t)target_user_id);

    /* First pass – count rows. */
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) count++;
    sqlite3_reset(stmt);

    if (count == 0) { sqlite3_finalize(stmt); return 0; }

    PropagationEvent *events = calloc((size_t)count, sizeof(PropagationEvent));
    if (!events) { sqlite3_finalize(stmt); return -1; }

    int i = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && i < count) {
        events[i].id              = sqlite3_column_int64(stmt, 0);
        events[i].target_user_id  = (uint64_t)sqlite3_column_int64(stmt, 1);
        events[i].source_guild_id = (uint64_t)sqlite3_column_int64(stmt, 2);
        events[i].moderator_id    = (uint64_t)sqlite3_column_int64(stmt, 3);

        const char *reason = (const char *)sqlite3_column_text(stmt, 4);
        const char *evurl  = (const char *)sqlite3_column_text(stmt, 5);
        events[i].reason       = reason ? strdup(reason) : NULL;
        events[i].evidence_url = evurl  ? strdup(evurl)  : NULL;
        events[i].timestamp    = sqlite3_column_int64(stmt, 6);
        i++;
    }

    sqlite3_finalize(stmt);
    *events_out = events;
    *count_out  = i;
    return 0;
}

void db_free_propagation_events(PropagationEvent *events, int count) {
    if (!events) return;
    for (int i = 0; i < count; i++) {
        free(events[i].reason);
        free(events[i].evidence_url);
    }
    free(events);
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