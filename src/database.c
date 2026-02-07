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