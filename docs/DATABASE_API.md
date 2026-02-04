# Database API Reference

This document describes the database functions available for extending the bot with new features.

## Database Structure

The `Database` structure wraps the SQLite database handle:

```c
typedef struct {
    sqlite3 *db;
    const char *db_path;
} Database;
```

## Initialisation and Cleanup

### `db_init()`

Initialises the database and creates tables if they don't exist.

```c
int db_init(Database *db, const char *path);
```

**Parameters:**
- `db` - Pointer to Database structure to initialise
- `path` - File path for the SQLite database

**Returns:**
- `0` on success
- `-1` on failure

**Example:**
```c
Database my_db;
if (db_init(&my_db, "bot_data.db") != 0) {
    fprintf(stderr, "Database initialisation failed\n");
    return EXIT_FAILURE;
}
```

### `db_cleanup()`

Closes the database connection and cleans up resources.

```c
void db_cleanup(Database *db);
```

**Example:**
```c
db_cleanup(&my_db);
```

## User Operations

### `db_create_user()`

Creates a user record in the database (or does nothing if already exists).

```c
int db_create_user(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id);
```

**Parameters:**
- `db` - Database handle
- `user_id` - Discord user snowflake ID
- `guild_id` - Discord guild snowflake ID

**Returns:**
- `0` on success
- `-1` on failure

**Example:**
```c
u64_snowflake_t user = 123456789012345678ULL;
u64_snowflake_t guild = 987654321098765432ULL;

if (db_create_user(&my_db, user, guild) == 0) {
    printf("User created successfully\n");
}
```

### `db_user_exists()`

Checks if a user exists in the database for a specific guild.

```c
bool db_user_exists(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id);
```

**Parameters:**
- `db` - Database handle
- `user_id` - Discord user snowflake ID
- `guild_id` - Discord guild snowflake ID

**Returns:**
- `true` if user exists
- `false` if user doesn't exist

**Example:**
```c
if (db_user_exists(&my_db, user_id, guild_id)) {
    printf("User is registered\n");
} else {
    printf("User not found\n");
}
```

## Warning Operations

### `db_add_warning()`

Adds a warning to a user's record.

```c
int db_add_warning(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id,
                   u64_snowflake_t moderator_id, const char *reason);
```

**Parameters:**
- `db` - Database handle
- `user_id` - Target user's Discord ID
- `guild_id` - Discord guild ID
- `moderator_id` - Moderator's Discord ID
- `reason` - Warning reason (can be NULL)

**Returns:**
- `0` on success
- `-1` on failure

**Example:**
```c
int result = db_add_warning(&my_db, target_user, guild_id, moderator_id,
                            "Spamming in general chat");
if (result == 0) {
    printf("Warning added\n");
}
```

### `db_get_warnings()`

Retrieves all warnings for a user in a guild.

```c
int db_get_warnings(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id,
                    Warning **warnings, int *count);
```

**Parameters:**
- `db` - Database handle
- `user_id` - Discord user ID
- `guild_id` - Discord guild ID
- `warnings` - Pointer to Warning array (will be allocated)
- `count` - Pointer to int that will receive the count

**Returns:**
- `0` on success
- `-1` on failure

**Important:** Call `db_free_warnings()` when done with the warnings array.

**Example:**
```c
Warning *warnings = NULL;
int count = 0;

if (db_get_warnings(&my_db, user_id, guild_id, &warnings, &count) == 0) {
    printf("User has %d warnings\n", count);
    
    for (int i = 0; i < count; i++) {
        printf("Warning %d: %s\n", i + 1, warnings[i].reason);
    }
    
    // Don't forget to free!
    db_free_warnings(warnings, count);
}
```

### `db_get_warning_count()`

Gets the total number of warnings for a user.

```c
int db_get_warning_count(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id);
```

**Parameters:**
- `db` - Database handle
- `user_id` - Discord user ID
- `guild_id` - Discord guild ID

**Returns:**
- Warning count (>= 0) on success
- `-1` on failure

**Example:**
```c
int count = db_get_warning_count(&my_db, user_id, guild_id);
if (count >= 0) {
    printf("User has %d warnings\n", count);
}
```

### `db_clear_warnings()`

Removes all warnings for a user in a guild.

```c
int db_clear_warnings(Database *db, u64_snowflake_t user_id, u64_snowflake_t guild_id);
```

**Parameters:**
- `db` - Database handle
- `user_id` - Discord user ID
- `guild_id` - Discord guild ID

**Returns:**
- `0` on success
- `-1` on failure

**Example:**
```c
if (db_clear_warnings(&my_db, user_id, guild_id) == 0) {
    printf("All warnings cleared for user\n");
}
```

### `db_free_warnings()`

Frees memory allocated by `db_get_warnings()`.

```c
void db_free_warnings(Warning *warnings, int count);
```

**Parameters:**
- `warnings` - Warnings array to free
- `count` - Number of warnings in array

**Example:**
```c
db_free_warnings(warnings, count);
warnings = NULL;  // Good practice
```

## Moderation Log Operations

### `db_log_action()`

Logs a moderation action to the database.

```c
int db_log_action(Database *db, ModActionType action, u64_snowflake_t user_id,
                  u64_snowflake_t guild_id, u64_snowflake_t moderator_id,
                  const char *reason);
```

**Parameters:**
- `db` - Database handle
- `action` - Type of action (see ModActionType enum)
- `user_id` - Target user's Discord ID
- `guild_id` - Discord guild ID
- `moderator_id` - Moderator's Discord ID
- `reason` - Action reason (can be NULL)

**Returns:**
- `0` on success
- `-1` on failure

**Action Types:**
```c
typedef enum {
    MOD_ACTION_WARN,     // Warning issued
    MOD_ACTION_KICK,     // User kicked
    MOD_ACTION_BAN,      // User banned
    MOD_ACTION_UNBAN,    // User unbanned
    MOD_ACTION_MUTE,     // User muted/timed out
    MOD_ACTION_UNMUTE    // User unmuted
} ModActionType;
```

**Example:**
```c
db_log_action(&my_db, MOD_ACTION_KICK, target_id, guild_id,
              moderator_id, "Violating server rules");
```

## Data Structures

### Warning Structure

```c
typedef struct {
    int id;                      // Database primary key
    u64_snowflake_t user_id;     // Discord user ID
    u64_snowflake_t guild_id;    // Discord guild ID
    u64_snowflake_t moderator_id; // Moderator's Discord ID
    char *reason;                // Warning reason (malloc'd string)
    long timestamp;              // Unix timestamp
} Warning;
```

## Adding Custom Tables

To add a new table to the database:

1. **Update the schema in `database.c`:**

```c
static const char *CREATE_TABLES_SQL = 
    // ... existing tables ...
    "CREATE TABLE IF NOT EXISTS my_custom_table ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    user_id INTEGER NOT NULL,"
    "    custom_data TEXT,"
    "    created_at INTEGER DEFAULT (strftime('%s', 'now'))"
    ");";
```

2. **Add functions in `database.h`:**

```c
// Custom table operations
int db_insert_custom_data(Database *db, u64_snowflake_t user_id, const char *data);
int db_get_custom_data(Database *db, u64_snowflake_t user_id, char **data);
```

3. **Implement in `database.c`:**

```c
int db_insert_custom_data(Database *db, u64_snowflake_t user_id, const char *data) {
    const char *sql = "INSERT INTO my_custom_table (user_id, custom_data) VALUES (?, ?)";
    sqlite3_stmt *stmt;
    
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare: %s\n", sqlite3_errmsg(db->db));
        return -1;
    }
    
    sqlite3_bind_int64(stmt, 1, user_id);
    sqlite3_bind_text(stmt, 2, data, -1, SQLITE_TRANSIENT);
    
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    return (rc == SQLITE_DONE) ? 0 : -1;
}
```

## Best Practices

1. **Always use prepared statements** - Prevents SQL injection
2. **Check return values** - All functions return status codes
3. **Free allocated memory** - Use `db_free_warnings()` and similar cleanup functions
4. **Handle NULL gracefully** - Many functions accept NULL for optional parameters
5. **Use transactions for multiple operations** - Wrap multiple writes in a transaction for atomicity

### Transaction Example

```c
sqlite3_exec(db->db, "BEGIN TRANSACTION", NULL, NULL, NULL);

// Multiple operations
db_add_warning(db, user1, guild, mod, "reason1");
db_add_warning(db, user2, guild, mod, "reason2");
db_log_action(db, MOD_ACTION_WARN, user1, guild, mod, "reason1");

sqlite3_exec(db->db, "COMMIT", NULL, NULL, NULL);
```

## Error Handling

All database functions follow these conventions:

- **Integer returns:** `0` = success, `-1` = failure
- **Boolean returns:** `true` = exists/success, `false` = doesn't exist/failure
- **Error messages:** Printed to stderr with `fprintf(stderr, ...)`

Always check return values:

```c
if (db_add_warning(&my_db, user, guild, mod, reason) != 0) {
    // Handle error
    send_error_message(client, "Database operation failed");
    return;
}
```

## SQLite Direct Access

For advanced operations, access the SQLite handle directly:

```c
Database *db = &g_database;

// Execute custom SQL
char *err_msg;
int rc = sqlite3_exec(db->db, 
                      "SELECT COUNT(*) FROM warnings WHERE timestamp > ?",
                      callback_function,
                      callback_data,
                      &err_msg);

if (rc != SQLITE_OK) {
    fprintf(stderr, "SQL error: %s\n", err_msg);
    sqlite3_free(err_msg);
}
```

## Performance Tips

1. **Batch operations** - Group multiple inserts in a transaction
2. **Index frequently queried columns** - Add indexes for user_id, guild_id
3. **Limit result sets** - Use LIMIT clauses for large datasets
4. **Clean old data** - Periodically archive or delete old records

### Adding an Index

```c
sqlite3_exec(db->db,
             "CREATE INDEX IF NOT EXISTS idx_warnings_user "
             "ON warnings(user_id, guild_id)",
             NULL, NULL, NULL);
```