#include "database.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// SQL schema for creating tables
static const char *CREATE_TABLES_SQL = 
    "CREATE TABLE IF NOT EXISTS users ("
    "    user_id INTEGER NOT NULL,"
    "    guild_id INTEGER NOT NULL,"
    "    created_at INTEGER DEFAULT (strftime('%s', 'now')),"
    "    PRIMARY KEY (user_id, guild_id)"
    ");"
    ""
    "CREATE TABLE IF NOT EXISTS warnings ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    user_id INTEGER NOT NULL,"
    "    guild_id INTEGER NOT NULL,"
    "    moderator_id INTEGER NOT NULL,"
    "    reason TEXT,"
    "    timestamp INTEGER DEFAULT (strftime('%s', 'now')),"
    "    FOREIGN KEY (user_id, guild_id) REFERENCES users(user_id, guild_id)"
    ");"
    ""
    "CREATE TABLE IF NOT EXISTS mod_logs ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    action_type INTEGER NOT NULL,"
    "    user_id INTEGER NOT NULL,"
    "    guild_id INTEGER NOT NULL,"
    "    moderator_id INTEGER NOT NULL,"
    "    reason TEXT,"
    "    timestamp INTEGER DEFAULT (strftime('%s', 'now'))"
    ");"
    ""
    /* One active timeout per (user, guild) pair.  Replaced on re-timeout,
     * deleted when the timeout is lifted or expires. */
    "CREATE TABLE IF NOT EXISTS timeouts ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    user_id INTEGER NOT NULL,"
    "    guild_id INTEGER NOT NULL,"
    "    moderator_id INTEGER NOT NULL,"
    "    reason TEXT,"
    "    expires_at INTEGER NOT NULL,"
    "    created_at INTEGER DEFAULT (strftime('%s', 'now')),"
    "    UNIQUE (user_id, guild_id)"
    ");";

int db_init(Database *db, const char *path) {
    db->db_path = path;
    
    int rc = sqlite3_open(path, &db->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db->db));
        return -1;
    }
    
    // Execute schema creation
    char *err_msg = NULL;
    rc = sqlite3_exec(db->db, CREATE_TABLES_SQL, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(db->db);
        return -1;
    }
    
    printf("Database initialised successfully: %s\n", path);
    return 0;
}

void db_cleanup(Database *db) {
    if (db->db) {
        sqlite3_close(db->db);
        db->db = NULL;
    }
}

int db_create_user(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id) {
    const char *sql = "INSERT OR IGNORE INTO users (user_id, guild_id) VALUES (?, ?)";
    sqlite3_stmt *stmt;
    
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db->db));
        return -1;
    }
    
    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_bind_int64(stmt, 2, guild_id);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Failed to create user: %s\n", sqlite3_errmsg(db->db));
        return -1;
    }
    
    return 0;
}

bool db_user_exists(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id) {
    const char *sql = "SELECT COUNT(*) FROM users WHERE user_id = ? AND guild_id = ?";
    sqlite3_stmt *stmt;
    
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return false;
    }
    
    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_bind_int64(stmt, 2, guild_id);
    
    bool exists = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        exists = sqlite3_column_int(stmt, 0) > 0;
    }
    
    sqlite3_finalize(stmt);
    return exists;
}

int db_add_warning(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id,
                   u64_snowflake_t moderator_id, const char *reason) {
    // Ensure user exists first
    db_create_user(db, user_id, guild_id);
    
    const char *sql = "INSERT INTO warnings (user_id, guild_id, moderator_id, reason) "
                      "VALUES (?, ?, ?, ?)";
    sqlite3_stmt *stmt;
    
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db->db));
        return -1;
    }
    
    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_bind_int64(stmt, 2, guild_id);
    sqlite3_bind_int64(stmt, 3, moderator_id);
    sqlite3_bind_text(stmt, 4, reason ? reason : "No reason provided", -1, SQLITE_TRANSIENT);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Failed to add warning: %s\n", sqlite3_errmsg(db->db));
        return -1;
    }
    
    return 0;
}

int db_get_warnings(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id,
                    Warning **warnings, int *count) {
    const char *sql = "SELECT id, user_id, guild_id, moderator_id, reason, timestamp "
                      "FROM warnings WHERE user_id = ? AND guild_id = ? "
                      "ORDER BY timestamp DESC";
    sqlite3_stmt *stmt;
    
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db->db));
        return -1;
    }
    
    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_bind_int64(stmt, 2, guild_id);
    
    // First pass: count rows
    int row_count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        row_count++;
    }
    
    *count = row_count;
    if (row_count == 0) {
        sqlite3_finalize(stmt);
        *warnings = NULL;
        return 0;
    }
    
    // Allocate array
    *warnings = malloc(sizeof(Warning) * row_count);
    if (!*warnings) {
        sqlite3_finalize(stmt);
        return -1;
    }
    
    // Reset and populate
    sqlite3_reset(stmt);
    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < row_count) {
        (*warnings)[idx].id = sqlite3_column_int(stmt, 0);
        (*warnings)[idx].user_id = sqlite3_column_int64(stmt, 1);
        (*warnings)[idx].guild_id = sqlite3_column_int64(stmt, 2);
        (*warnings)[idx].moderator_id = sqlite3_column_int64(stmt, 3);
        
        const char *reason = (const char *)sqlite3_column_text(stmt, 4);
        (*warnings)[idx].reason = reason ? strdup(reason) : NULL;
        
        (*warnings)[idx].timestamp = sqlite3_column_int64(stmt, 5);
        idx++;
    }
    
    sqlite3_finalize(stmt);
    return 0;
}

void db_free_warnings(Warning *warnings, int count) {
    if (!warnings) return;
    
    for (int i = 0; i < count; i++) {
        free(warnings[i].reason);
    }
    free(warnings);
}

int db_clear_warnings(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id) {
    const char *sql = "DELETE FROM warnings WHERE user_id = ? AND guild_id = ?";
    sqlite3_stmt *stmt;
    
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db->db));
        return -1;
    }
    
    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_bind_int64(stmt, 2, guild_id);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Failed to clear warnings: %s\n", sqlite3_errmsg(db->db));
        return -1;
    }
    
    return 0;
}

int db_log_action(Database *db, ModActionType action, u64_snowflake_t user_id,
                  u64_snowflake_t guild_id, u64_snowflake_t moderator_id,
                  const char *reason) {
    const char *sql = "INSERT INTO mod_logs (action_type, user_id, guild_id, moderator_id, reason) "
                      "VALUES (?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt;
    
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db->db));
        return -1;
    }
    
    sqlite3_bind_int(stmt, 1, action);
    sqlite3_bind_int64(stmt, 2, user_id);
    sqlite3_bind_int64(stmt, 3, guild_id);
    sqlite3_bind_int64(stmt, 4, moderator_id);
    sqlite3_bind_text(stmt, 5, reason ? reason : "No reason provided", -1, SQLITE_TRANSIENT);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Failed to log action: %s\n", sqlite3_errmsg(db->db));
        return -1;
    }
    
    return 0;
}

int db_get_warning_count(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id) {
    const char *sql = "SELECT COUNT(*) FROM warnings WHERE user_id = ? AND guild_id = ?";
    sqlite3_stmt *stmt;
    
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return -1;
    }
    
    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_bind_int64(stmt, 2, guild_id);
    
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    
    sqlite3_finalize(stmt);
    return count;
}

/* ---------------------------------------------------------------------------
 * Timeout operations
 * --------------------------------------------------------------------------- */

/*
 * db_add_timeout
 *
 * Inserts or replaces an active timeout for (user_id, guild_id).
 * If the user is already timed out, the record is overwritten with the new
 * expiry and moderator details (i.e. extending or shortening is idempotent).
 *
 * expires_at  – Unix timestamp at which the timeout ends.
 *
 * Returns 0 on success, -1 on failure.
 */
int db_add_timeout(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id,
                   u64_snowflake_t moderator_id, const char *reason, long expires_at) {
    /* Ensure the user row exists so foreign-key-style queries work. */
    db_create_user(db, user_id, guild_id);

    const char *sql =
        "INSERT INTO timeouts (user_id, guild_id, moderator_id, reason, expires_at) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(user_id, guild_id) DO UPDATE SET "
        "    moderator_id = excluded.moderator_id,"
        "    reason       = excluded.reason,"
        "    expires_at   = excluded.expires_at,"
        "    created_at   = strftime('%s', 'now');";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_add_timeout: prepare failed: %s\n",
                sqlite3_errmsg(db->db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)user_id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)guild_id);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)moderator_id);
    sqlite3_bind_text (stmt, 4, reason ? reason : "No reason provided", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)expires_at);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_add_timeout: step failed: %s\n",
                sqlite3_errmsg(db->db));
        return -1;
    }
    return 0;
}

/*
 * db_get_timeout
 *
 * Populates *out with the active timeout record for (user_id, guild_id).
 * The caller must call db_free_timeout(out) when done.
 *
 * Returns  0 if a record was found and written into *out.
 * Returns  1 if no active (non-expired) timeout exists.
 * Returns -1 on a database error.
 */
int db_get_timeout(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id,
                   TimeoutRecord *out) {
    if (!out) return -1;

    const char *sql =
        "SELECT id, user_id, guild_id, moderator_id, reason, expires_at, created_at "
        "FROM timeouts "
        "WHERE user_id = ? AND guild_id = ? AND expires_at > strftime('%s', 'now') "
        "LIMIT 1;";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_get_timeout: prepare failed: %s\n",
                sqlite3_errmsg(db->db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)user_id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)guild_id);

    int result = 1; /* assume not found */
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out->id           = sqlite3_column_int  (stmt, 0);
        out->user_id      = (u64_snowflake_t)sqlite3_column_int64(stmt, 1);
        out->guild_id     = (u64_snowflake_t)sqlite3_column_int64(stmt, 2);
        out->moderator_id = (u64_snowflake_t)sqlite3_column_int64(stmt, 3);

        const char *reason = (const char *)sqlite3_column_text(stmt, 4);
        out->reason    = reason ? strdup(reason) : NULL;
        out->expires_at = (long)sqlite3_column_int64(stmt, 5);
        out->created_at = (long)sqlite3_column_int64(stmt, 6);
        result = 0;
    }

    sqlite3_finalize(stmt);
    return result;
}

/*
 * db_remove_timeout
 *
 * Deletes the timeout record for (user_id, guild_id), whether or not it has
 * already expired.  Safe to call even if no record exists.
 *
 * Returns 0 on success, -1 on failure.
 */
int db_remove_timeout(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id) {
    const char *sql =
        "DELETE FROM timeouts WHERE user_id = ? AND guild_id = ?;";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_remove_timeout: prepare failed: %s\n",
                sqlite3_errmsg(db->db));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)user_id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)guild_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "db_remove_timeout: step failed: %s\n",
                sqlite3_errmsg(db->db));
        return -1;
    }
    return 0;
}

/*
 * db_user_is_timed_out
 *
 * Returns true if (user_id, guild_id) has a timeout record whose expiry is
 * still in the future.
 */
bool db_user_is_timed_out(Database *db, u64_snowflake_t user_id,
                            u64_snowflake_t guild_id) {
    TimeoutRecord rec = { 0 };
    int result = db_get_timeout(db, user_id, guild_id, &rec);
    if (result == 0)
        db_free_timeout(&rec);
    return result == 0;
}

/*
 * db_free_timeout
 *
 * Releases heap memory owned by a TimeoutRecord populated by db_get_timeout.
 * Safe to call on a zero-initialised struct.
 */
void db_free_timeout(TimeoutRecord *record) {
    if (!record) return;
    free(record->reason);
    record->reason = NULL;
}