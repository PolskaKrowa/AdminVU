#ifndef DATABASE_H
#define DATABASE_H

#include <sqlite3.h>
#include <orca/discord.h>
#include <stdbool.h>

// Database structure
typedef struct {
    sqlite3 *db;
    const char *db_path;
} Database;

// Warning record structure
typedef struct {
    int id;
    u64_snowflake_t user_id;
    u64_snowflake_t guild_id;
    u64_snowflake_t moderator_id;
    char *reason;
    long timestamp;
} Warning;

// Moderation action types
typedef enum {
    MOD_ACTION_WARN,
    MOD_ACTION_KICK,
    MOD_ACTION_BAN,
    MOD_ACTION_UNBAN,
    MOD_ACTION_MUTE,
    MOD_ACTION_UNMUTE,
    MOD_ACTION_TIMEOUT,
    MOD_ACTION_TIMEOUT_REMOVE
} ModActionType;

// Timeout record structure
typedef struct {
    int id;
    u64_snowflake_t user_id;
    u64_snowflake_t guild_id;
    u64_snowflake_t moderator_id;
    char *reason;
    long expires_at;
    long created_at;
} TimeoutRecord;

// Database initialisation
int db_init(Database *db, const char *path);
void db_cleanup(Database *db);

// User operations
int db_create_user(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id);
bool db_user_exists(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id);

// Warning operations
int db_add_warning(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id,
                   u64_snowflake_t moderator_id, const char *reason);
int db_get_warnings(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id,
                    Warning **warnings, int *count);
int db_clear_warnings(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id);
void db_free_warnings(Warning *warnings, int count);

// Moderation log operations
int db_log_action(Database *db, ModActionType action, u64_snowflake_t user_id,
                  u64_snowflake_t guild_id, u64_snowflake_t moderator_id,
                  const char *reason);

// Get warning count
int db_get_warning_count(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id);

// Timeout operations
int db_add_timeout(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id,
                   u64_snowflake_t moderator_id, const char *reason, long expires_at);
int db_get_timeout(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id,
                   TimeoutRecord *out);
int db_remove_timeout(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id);
bool db_user_is_timed_out(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id);
void db_free_timeout(TimeoutRecord *record);

#endif // DATABASE_H