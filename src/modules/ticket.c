/*
 * ticket.c  –  Ticket system with staff management commands
 *
 * NOTE: ticket.h requires no changes from the original.
 * The existing `subject` field on Ticket is repurposed to hold the community
 * server snowflake ID supplied via /ticket server:<id>.
 *
 * User commands:
 *   /ticket server:<id>    – open a ticket linked to a community server
 *   /ticketclose           – close the current ticket (opener or staff)
 *
 * Staff commands (require MANAGE_MESSAGES permission):
 *   /ticketnote    add <text>           – add a private staff note
 *   /ticketnote    list                 – list all notes on this ticket
 *   /ticketnote    pin <note_id>        – pin an important note
 *   /ticketnote    delete <note_id>     – delete one of your own notes
 *   /ticketoutcome set <outcome> [text] – record the ticket resolution
 *   /ticketoutcome clear                – clear the current outcome
 *   /ticketassign  <staff_member>       – assign ticket to a staff member
 *   /ticketpriority <level>             – set LOW / MEDIUM / HIGH / URGENT
 *   /ticketstatus  <status>             – set OPEN / IN_PROGRESS / PENDING_USER / RESOLVED
 *   /ticketsummary                      – full summary (notes, outcome, history)
 *   /ticketconfig  [ticket_channel:#ch] [log_channel:#ch]
 *                  [main_server:<id>]   [staff_server:<id>]
 *                  (no arguments = view current config)
 *
 * Multi-server use:
 *   All data is partitioned by guild_id, so every Discord server that adds
 *   this bot gets a fully isolated ticket system.  Pass guild_id = 0 to
 *   register_ticket_commands() to register global (cross-guild) slash commands,
 *   or pass a specific guild_id for faster propagation during development.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <orca/discord.h>

#include "ticket.h"
#include "database.h"

/* ============================================================================
 * Module-level globals
 * ========================================================================= */

static Database       *g_db     = NULL;
static struct discord *g_client = NULL;

/* ============================================================================
 * String helpers
 * ========================================================================= */

const char *ticket_status_string(TicketStatus s) {
    switch (s) {
        case TICKET_STATUS_OPEN:         return "Open";
        case TICKET_STATUS_IN_PROGRESS:  return "In Progress";
        case TICKET_STATUS_PENDING_USER: return "Pending User";
        case TICKET_STATUS_RESOLVED:     return "Resolved";
        case TICKET_STATUS_CLOSED:       return "Closed";
        default:                         return "Unknown";
    }
}

const char *ticket_priority_string(TicketPriority p) {
    switch (p) {
        case TICKET_PRIORITY_LOW:    return "🟢 Low";
        case TICKET_PRIORITY_MEDIUM: return "🟡 Medium";
        case TICKET_PRIORITY_HIGH:   return "🟠 High";
        case TICKET_PRIORITY_URGENT: return "🔴 Urgent";
        default:                     return "Unknown";
    }
}

const char *ticket_outcome_string(TicketOutcome o) {
    switch (o) {
        case TICKET_OUTCOME_NONE:        return "None";
        case TICKET_OUTCOME_RESOLVED:    return "✅ Resolved";
        case TICKET_OUTCOME_DUPLICATE:   return "🔁 Duplicate";
        case TICKET_OUTCOME_INVALID:     return "❌ Invalid";
        case TICKET_OUTCOME_NO_RESPONSE: return "🔇 No Response";
        case TICKET_OUTCOME_ESCALATED:   return "⬆️ Escalated";
        case TICKET_OUTCOME_OTHER:       return "📝 Other";
        default:                         return "Unknown";
    }
}

static TicketPriority priority_from_string(const char *s) {
    if (!s) return TICKET_PRIORITY_MEDIUM;
    if (strcasecmp(s, "low")    == 0) return TICKET_PRIORITY_LOW;
    if (strcasecmp(s, "high")   == 0) return TICKET_PRIORITY_HIGH;
    if (strcasecmp(s, "urgent") == 0) return TICKET_PRIORITY_URGENT;
    return TICKET_PRIORITY_MEDIUM;
}

static TicketOutcome outcome_from_string(const char *s) {
    if (!s) return TICKET_OUTCOME_NONE;
    if (strcasecmp(s, "resolved")    == 0) return TICKET_OUTCOME_RESOLVED;
    if (strcasecmp(s, "duplicate")   == 0) return TICKET_OUTCOME_DUPLICATE;
    if (strcasecmp(s, "invalid")     == 0) return TICKET_OUTCOME_INVALID;
    if (strcasecmp(s, "noresponse")  == 0 ||
        strcasecmp(s, "no_response") == 0) return TICKET_OUTCOME_NO_RESPONSE;
    if (strcasecmp(s, "escalated")   == 0) return TICKET_OUTCOME_ESCALATED;
    return TICKET_OUTCOME_OTHER;
}

static TicketStatus status_from_string(const char *s) {
    if (!s) return TICKET_STATUS_OPEN;
    if (strcasecmp(s, "open")          == 0) return TICKET_STATUS_OPEN;
    if (strcasecmp(s, "in_progress")   == 0 ||
        strcasecmp(s, "inprogress")    == 0) return TICKET_STATUS_IN_PROGRESS;
    if (strcasecmp(s, "pending_user")  == 0 ||
        strcasecmp(s, "pendinguser")   == 0) return TICKET_STATUS_PENDING_USER;
    if (strcasecmp(s, "resolved")      == 0) return TICKET_STATUS_RESOLVED;
    return TICKET_STATUS_OPEN;
}

/* ============================================================================
 * Database – table initialisation
 * ========================================================================= */

int ticket_db_init_tables(Database *db) {
    const char *sql =
        /* Core ticket record */
        "CREATE TABLE IF NOT EXISTS tickets ("
        "  id                 INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  channel_id         INTEGER NOT NULL UNIQUE,"
        "  guild_id           INTEGER NOT NULL,"
        "  opener_id          INTEGER NOT NULL,"
        "  assigned_to   INTEGER DEFAULT 0,"
        "  status        INTEGER DEFAULT 0,"
        "  priority      INTEGER DEFAULT 1,"
        "  outcome       INTEGER DEFAULT 0,"
        "  subject       TEXT,"                      /* stores the community server snowflake ID */
        "  outcome_notes TEXT,"
        "  created_at         INTEGER DEFAULT (strftime('%s','now')),"
        "  updated_at         INTEGER DEFAULT (strftime('%s','now')),"
        "  closed_at          INTEGER DEFAULT 0"
        ");"

        /* Private staff notes */
        "CREATE TABLE IF NOT EXISTS ticket_notes ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ticket_id  INTEGER NOT NULL REFERENCES tickets(id) ON DELETE CASCADE,"
        "  author_id  INTEGER NOT NULL,"
        "  content    TEXT NOT NULL,"
        "  is_pinned  INTEGER DEFAULT 0,"
        "  created_at INTEGER DEFAULT (strftime('%s','now'))"
        ");"

        /*
         * Per-guild configuration.
         * main_server_id / staff_server_id are TEXT because they are user-supplied
         * snowflakes that the bot may not necessarily be a member of.
         */
        "CREATE TABLE IF NOT EXISTS ticket_config ("
        "  id                INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  guild_id          INTEGER NOT NULL UNIQUE,"
        "  ticket_channel_id INTEGER DEFAULT 0,"
        "  log_channel_id    INTEGER DEFAULT 0,"
        "  main_server_id    TEXT    DEFAULT '',"
        "  staff_server_id   TEXT    DEFAULT '',"
        "  updated_at        INTEGER DEFAULT (strftime('%s','now'))"
        ");"

        /* Indexes */
        "CREATE INDEX IF NOT EXISTS idx_tickets_channel   ON tickets(channel_id);"
        "CREATE INDEX IF NOT EXISTS idx_tickets_guild     ON tickets(guild_id);"
        "CREATE INDEX IF NOT EXISTS idx_ticket_notes_tid  ON ticket_notes(ticket_id);"
        "CREATE INDEX IF NOT EXISTS idx_ticket_config_gid ON ticket_config(guild_id);";

    char *err = NULL;
    int rc = sqlite3_exec(db->db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[ticket] DB init error: %s\n", err);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

/* ============================================================================
 * Database – ticket CRUD
 * ========================================================================= */

int ticket_db_create(Database *db, Ticket *t) {
    const char *sql =
        "INSERT INTO tickets "
        "  (channel_id, guild_id, opener_id, status, priority, subject) "
        "VALUES (?, ?, ?, ?, ?, ?) RETURNING id;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, t->channel_id);
    sqlite3_bind_int64(stmt, 2, t->guild_id);
    sqlite3_bind_int64(stmt, 3, t->opener_id);
    sqlite3_bind_int  (stmt, 4, t->status);
    sqlite3_bind_int  (stmt, 5, t->priority);
    sqlite3_bind_text (stmt, 6,
                       t->subject ? t->subject : "",
                       -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        t->id = (int)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_ROW) ? 0 : -1;
}

/*
 * Column order for every SELECT that feeds row_to_ticket().
 * Index  0  id
 * Index  1  channel_id
 * Index  2  guild_id
 * Index  3  opener_id
 * Index  4  assigned_to
 * Index  5  status
 * Index  6  priority
 * Index  7  outcome
 * Index  8  created_at
 * Index  9  updated_at
 * Index  10 closed_at
 * Index  11 subject          (repurposed: stores community server snowflake ID)
 * Index  12 outcome_notes
 */
static const char *SELECT_TICKET_COLS =
    "SELECT id, channel_id, guild_id, opener_id, assigned_to, "
    "       status, priority, outcome, created_at, updated_at, closed_at, "
    "       subject, outcome_notes ";

static void row_to_ticket(sqlite3_stmt *stmt, Ticket *t) {
    t->id          = (int)sqlite3_column_int64(stmt, 0);
    t->channel_id  = (u64_snowflake_t)sqlite3_column_int64(stmt, 1);
    t->guild_id    = (u64_snowflake_t)sqlite3_column_int64(stmt, 2);
    t->opener_id   = (u64_snowflake_t)sqlite3_column_int64(stmt, 3);
    t->assigned_to = (u64_snowflake_t)sqlite3_column_int64(stmt, 4);
    t->status      = (TicketStatus)sqlite3_column_int(stmt, 5);
    t->priority    = (TicketPriority)sqlite3_column_int(stmt, 6);
    t->outcome     = (TicketOutcome)sqlite3_column_int(stmt, 7);
    t->created_at  = (long)sqlite3_column_int64(stmt, 8);
    t->updated_at  = (long)sqlite3_column_int64(stmt, 9);
    t->closed_at   = (long)sqlite3_column_int64(stmt, 10);

    const char *rsid     = (const char *)sqlite3_column_text(stmt, 11);
    t->subject           = (rsid && rsid[0]) ? strdup(rsid) : NULL;

    const char *onotes   = (const char *)sqlite3_column_text(stmt, 12);
    t->outcome_notes     = onotes ? strdup(onotes) : NULL;
}

int ticket_db_get_by_channel(Database *db, u64_snowflake_t channel_id, Ticket *out) {
    char sql[512];
    snprintf(sql, sizeof(sql), "%s FROM tickets WHERE channel_id = ?;",
             SELECT_TICKET_COLS);

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, channel_id);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) row_to_ticket(stmt, out);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_ROW) ? 0 : -1;
}

int ticket_db_get_by_id(Database *db, int ticket_id, Ticket *out) {
    char sql[512];
    snprintf(sql, sizeof(sql), "%s FROM tickets WHERE id = ?;",
             SELECT_TICKET_COLS);

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int(stmt, 1, ticket_id);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) row_to_ticket(stmt, out);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_ROW) ? 0 : -1;
}

int ticket_db_update_status(Database *db, int ticket_id, TicketStatus status) {
    const char *sql = (status == TICKET_STATUS_CLOSED || status == TICKET_STATUS_RESOLVED)
        ? "UPDATE tickets SET status = ?, closed_at = strftime('%s','now'), "
          "updated_at = strftime('%s','now') WHERE id = ?;"
        : "UPDATE tickets SET status = ?, updated_at = strftime('%s','now') WHERE id = ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int(stmt, 1, status);
    sqlite3_bind_int(stmt, 2, ticket_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int ticket_db_update_priority(Database *db, int ticket_id, TicketPriority priority) {
    const char *sql =
        "UPDATE tickets SET priority = ?, updated_at = strftime('%s','now') WHERE id = ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int(stmt, 1, priority);
    sqlite3_bind_int(stmt, 2, ticket_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int ticket_db_update_assigned(Database *db, int ticket_id, u64_snowflake_t staff_id) {
    const char *sql =
        "UPDATE tickets SET assigned_to = ?, updated_at = strftime('%s','now') WHERE id = ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, staff_id);
    sqlite3_bind_int  (stmt, 2, ticket_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int ticket_db_set_outcome(Database *db, int ticket_id,
                           TicketOutcome outcome, const char *notes) {
    const char *sql =
        "UPDATE tickets SET outcome = ?, outcome_notes = ?, "
        "updated_at = strftime('%s','now') WHERE id = ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int (stmt, 1, outcome);
    sqlite3_bind_text(stmt, 2, notes ? notes : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (stmt, 3, ticket_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

void ticket_db_free(Ticket *t) {
    if (!t) return;
    free(t->subject);
    free(t->outcome_notes);
    t->subject       = NULL;
    t->outcome_notes = NULL;
}

/* ============================================================================
 * Database – notes CRUD
 * ========================================================================= */

int ticket_note_add(Database *db, int ticket_id,
                    u64_snowflake_t author_id, const char *content) {
    const char *sql =
        "INSERT INTO ticket_notes (ticket_id, author_id, content) VALUES (?, ?, ?);";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int  (stmt, 1, ticket_id);
    sqlite3_bind_int64(stmt, 2, author_id);
    sqlite3_bind_text (stmt, 3, content, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int ticket_note_get_all(Database *db, int ticket_id,
                        TicketNote **notes_out, int *count_out) {
    *notes_out = NULL;
    *count_out = 0;

    const char *sql =
        "SELECT id, ticket_id, author_id, content, is_pinned, created_at "
        "FROM ticket_notes WHERE ticket_id = ? "
        "ORDER BY is_pinned DESC, created_at ASC;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int(stmt, 1, ticket_id);

    int cap = 8;
    TicketNote *arr = malloc(sizeof(TicketNote) * cap);
    if (!arr) { sqlite3_finalize(stmt); return -1; }
    int n = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= 2;
            TicketNote *tmp = realloc(arr, sizeof(TicketNote) * cap);
            if (!tmp) { free(arr); sqlite3_finalize(stmt); return -1; }
            arr = tmp;
        }
        TicketNote *note = &arr[n++];
        note->id         = (int)sqlite3_column_int64(stmt, 0);
        note->ticket_id  = (int)sqlite3_column_int64(stmt, 1);
        note->author_id  = (u64_snowflake_t)sqlite3_column_int64(stmt, 2);
        const char *txt  = (const char *)sqlite3_column_text(stmt, 3);
        note->content    = txt ? strdup(txt) : strdup("");
        note->is_pinned  = (bool)sqlite3_column_int(stmt, 4);
        note->created_at = (long)sqlite3_column_int64(stmt, 5);
    }

    sqlite3_finalize(stmt);
    *notes_out = arr;
    *count_out = n;
    return 0;
}

int ticket_note_set_pinned(Database *db, int note_id, bool pinned) {
    const char *sql = "UPDATE ticket_notes SET is_pinned = ? WHERE id = ?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int(stmt, 1, pinned ? 1 : 0);
    sqlite3_bind_int(stmt, 2, note_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int ticket_note_delete(Database *db, int note_id,
                       u64_snowflake_t requesting_staff_id) {
    /*
     * Only allow deletion if the requester is the note author.
     * Pass 0 as requesting_staff_id to bypass the authorship check (admins).
     */
    const char *sql = (requesting_staff_id == 0)
        ? "DELETE FROM ticket_notes WHERE id = ?;"
        : "DELETE FROM ticket_notes WHERE id = ? AND author_id = ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int(stmt, 1, note_id);
    if (requesting_staff_id != 0)
        sqlite3_bind_int64(stmt, 2, requesting_staff_id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE && sqlite3_changes(db->db) > 0) ? 0 : -1;
}

void ticket_notes_free(TicketNote *notes, int count) {
    if (!notes) return;
    for (int i = 0; i < count; i++)
        free(notes[i].content);
    free(notes);
}

/* ============================================================================
 * Database – config CRUD
 * ========================================================================= */

int ticket_config_get(Database *db, u64_snowflake_t guild_id, TicketConfig *out) {
    /* Upsert a default row so we always have something to read back */
    const char *upsert =
        "INSERT OR IGNORE INTO ticket_config (guild_id) VALUES (?);";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, upsert, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, guild_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    const char *sel =
        "SELECT id, guild_id, ticket_channel_id, log_channel_id, "
        "       main_server_id, staff_server_id "
        "FROM ticket_config WHERE guild_id = ?;";

    if (sqlite3_prepare_v2(db->db, sel, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, guild_id);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        out->id                = (int)sqlite3_column_int64(stmt, 0);
        out->guild_id          = (u64_snowflake_t)sqlite3_column_int64(stmt, 1);
        out->ticket_channel_id = (u64_snowflake_t)sqlite3_column_int64(stmt, 2);
        out->log_channel_id    = (u64_snowflake_t)sqlite3_column_int64(stmt, 3);

        const char *ms = (const char *)sqlite3_column_text(stmt, 4);
        out->main_server_id  = (ms && ms[0]) ? strdup(ms) : NULL;

        const char *ss = (const char *)sqlite3_column_text(stmt, 5);
        out->staff_server_id = (ss && ss[0]) ? strdup(ss) : NULL;
    }
    sqlite3_finalize(stmt);
    return (rc == SQLITE_ROW) ? 0 : -1;
}

void ticket_config_free(TicketConfig *cfg) {
    if (!cfg) return;
    free(cfg->main_server_id);
    free(cfg->staff_server_id);
    cfg->main_server_id  = NULL;
    cfg->staff_server_id = NULL;
}

/*
 * Generic upsert helpers.
 * `col` must be a compile-time literal – never pass user input here.
 */
static int config_upsert_snowflake(Database *db, u64_snowflake_t guild_id,
                                    const char *col, u64_snowflake_t value) {
    char sql[256];
    snprintf(sql, sizeof(sql),
             "INSERT INTO ticket_config (guild_id, %s, updated_at) "
             "VALUES (?, ?, strftime('%%s','now')) "
             "ON CONFLICT(guild_id) DO UPDATE SET %s = excluded.%s, "
             "updated_at = strftime('%%s','now');",
             col, col, col);

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, guild_id);
    sqlite3_bind_int64(stmt, 2, value);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

static int config_upsert_text(Database *db, u64_snowflake_t guild_id,
                               const char *col, const char *value) {
    char sql[256];
    snprintf(sql, sizeof(sql),
             "INSERT INTO ticket_config (guild_id, %s, updated_at) "
             "VALUES (?, ?, strftime('%%s','now')) "
             "ON CONFLICT(guild_id) DO UPDATE SET %s = excluded.%s, "
             "updated_at = strftime('%%s','now');",
             col, col, col);

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, guild_id);
    sqlite3_bind_text (stmt, 2, value ? value : "", -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int ticket_config_set_ticket_channel(Database *db, u64_snowflake_t guild_id,
                                      u64_snowflake_t channel_id) {
    return config_upsert_snowflake(db, guild_id, "ticket_channel_id", channel_id);
}

int ticket_config_set_log_channel(Database *db, u64_snowflake_t guild_id,
                                   u64_snowflake_t channel_id) {
    return config_upsert_snowflake(db, guild_id, "log_channel_id", channel_id);
}

int ticket_config_set_main_server(Database *db, u64_snowflake_t guild_id,
                                   const char *server_id) {
    return config_upsert_text(db, guild_id, "main_server_id", server_id);
}

int ticket_config_set_staff_server(Database *db, u64_snowflake_t guild_id,
                                    const char *server_id) {
    return config_upsert_text(db, guild_id, "staff_server_id", server_id);
}

/* ============================================================================
 * Runtime config cache
 *
 * One cached TicketConfig per guild in a fixed-size table.  64 entries covers
 * most self-hosted deployments; a hash map would be appropriate for larger
 * public bots.
 * ========================================================================= */

#define CONFIG_CACHE_SIZE 64

typedef struct {
    u64_snowflake_t guild_id;
    TicketConfig    cfg;
    bool            valid;
} ConfigCacheEntry;

static ConfigCacheEntry g_config_cache[CONFIG_CACHE_SIZE];

static void config_cache_invalidate(u64_snowflake_t guild_id) {
    for (int i = 0; i < CONFIG_CACHE_SIZE; i++) {
        if (g_config_cache[i].valid && g_config_cache[i].guild_id == guild_id) {
            ticket_config_free(&g_config_cache[i].cfg);
            g_config_cache[i].valid = false;
            return;
        }
    }
}

static const TicketConfig *config_for_guild(u64_snowflake_t guild_id) {
    for (int i = 0; i < CONFIG_CACHE_SIZE; i++) {
        if (g_config_cache[i].valid && g_config_cache[i].guild_id == guild_id)
            return &g_config_cache[i].cfg;
    }

    /* Find a free slot; evict slot 0 as a simple fallback */
    int slot = 0;
    for (int i = 0; i < CONFIG_CACHE_SIZE; i++) {
        if (!g_config_cache[i].valid) { slot = i; break; }
    }

    if (g_config_cache[slot].valid)
        ticket_config_free(&g_config_cache[slot].cfg);

    memset(&g_config_cache[slot].cfg, 0, sizeof(TicketConfig));
    if (ticket_config_get(g_db, guild_id, &g_config_cache[slot].cfg) != 0)
        return NULL;

    g_config_cache[slot].guild_id = guild_id;
    g_config_cache[slot].valid    = true;
    return &g_config_cache[slot].cfg;
}

static u64_snowflake_t log_channel_for_guild(u64_snowflake_t guild_id) {
    const TicketConfig *cfg = config_for_guild(guild_id);
    return cfg ? cfg->log_channel_id : 0;
}

/* ============================================================================
 * Permission helper
 * ========================================================================= */

static bool is_staff(const struct discord_interaction *event) {
    if (!event->member) return false;
    uint64_t perms = (uint64_t)event->member->permissions;
    return (perms & 0x0000000000002000ULL) != 0; /* MANAGE_MESSAGES */
}

/* ============================================================================
 * Discord helpers
 * ========================================================================= */

/*
 * send_to_channel – single chokepoint for all outbound messages.
 * Replace the body with the correct call for your Orca build.
 */
static void send_to_channel(struct discord *client,
                             u64_snowflake_t channel_id,
                             const char *content) {
    /* TODO: replace with the correct API call for your Orca version */
    printf("[ticket] send_to_channel (ch=%" PRIu64 "): %s\n", channel_id, content);
    (void)client;
}

static void reply_ephemeral(struct discord *client,
                             const struct discord_interaction *event,
                             const char *message) {
    struct discord_interaction_response resp;
    memset(&resp, 0, sizeof(resp));
    resp.type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE;

    struct discord_interaction_callback_data data;
    memset(&data, 0, sizeof(data));
    data.content = (char *)message;
    data.flags   = 64; /* EPHEMERAL (0x40) */

    resp.data = &data;
    discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
}

static void reply_public(struct discord *client,
                          const struct discord_interaction *event,
                          const char *message) {
    struct discord_interaction_response resp;
    memset(&resp, 0, sizeof(resp));
    resp.type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE;

    struct discord_interaction_callback_data data;
    memset(&data, 0, sizeof(data));
    data.content = (char *)message;

    resp.data = &data;
    discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
}

static void log_to_staff_channel(u64_snowflake_t guild_id, const char *message) {
    if (!g_client) return;
    u64_snowflake_t ch = log_channel_for_guild(guild_id);
    if (!ch) return;
    send_to_channel(g_client, ch, message);
}

/*
 * get_active_subcommand / find_option / find_sub_option
 * Direct-indexed rather than NULL-sentinel loops; 25 is Discord's hard max.
 */
static const struct discord_application_command_interaction_data_option *
get_active_subcommand(const struct discord_interaction *event) {
    if (!event->data || !event->data->options) return NULL;
    return event->data->options[0];
}

static const struct discord_application_command_interaction_data_option *
find_option(const struct discord_interaction *event, const char *name) {
    if (!event->data || !event->data->options) return NULL;
    for (int i = 0; i < 25; i++) {
        if (!event->data->options[i]) break;
        if (strcmp(event->data->options[i]->name, name) == 0)
            return event->data->options[i];
    }
    return NULL;
}

static const struct discord_application_command_interaction_data_option *
find_sub_option(const struct discord_application_command_interaction_data_option *subcmd,
                const char *name) {
    if (!subcmd || !subcmd->options) return NULL;
    for (int i = 0; i < 25; i++) {
        if (!subcmd->options[i]) break;
        if (strcmp(subcmd->options[i]->name, name) == 0)
            return subcmd->options[i];
    }
    return NULL;
}

/* ============================================================================
 * Command handler – /ticket server:<id>
 *
 * Opens a new ticket in the current channel, linked to the given community
 * server ID.  The server_id is stored for staff context but is not validated
 * against Discord's API (it may be a server the bot isn't a member of).
 * ========================================================================= */

static void handle_ticket_open(struct discord *client,
                                const struct discord_interaction *event) {
    /* Prevent opening a second ticket in an existing ticket channel */
    Ticket existing = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &existing) == 0) {
        ticket_db_free(&existing);
        reply_ephemeral(client, event,
                        "❌ A ticket already exists in this channel.");
        return;
    }

    const struct discord_application_command_interaction_data_option *server_opt =
        find_option(event, "server");
    if (!server_opt || !server_opt->value || server_opt->value[0] == '\0') {
        reply_ephemeral(client, event,
                        "❌ Please provide the ID of the server your ticket relates to.");
        return;
    }

    /* Validate: must be a numeric snowflake */
    for (const char *p = server_opt->value; *p; p++) {
        if (*p < '0' || *p > '9') {
            reply_ephemeral(client, event,
                            "❌ Server ID must be a numeric Discord snowflake.");
            return;
        }
    }

    Ticket t;
    memset(&t, 0, sizeof(t));
    t.channel_id = event->channel_id;
    t.guild_id   = event->guild_id;
    t.opener_id  = event->member->user->id;
    t.status     = TICKET_STATUS_OPEN;
    t.priority   = TICKET_PRIORITY_MEDIUM;
    t.subject    = (char *)server_opt->value; /* stack – not freed */

    if (ticket_db_create(g_db, &t) != 0) {
        reply_ephemeral(client, event,
                        "❌ Failed to create ticket – please try again.");
        return;
    }

    char msg[512];
    snprintf(msg, sizeof(msg),
             "🎫 **Ticket #%d opened**\n"
             "**Server:** `%s`\n"
             "Staff will be with you shortly.\n\n"
             "_Use `/ticketclose` to close this ticket at any time._",
             t.id, server_opt->value);
    reply_public(client, event, msg);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg),
             "🎫 Ticket #%d opened by <@%" PRIu64 "> — server `%s`",
             t.id, t.opener_id, server_opt->value);
    log_to_staff_channel(event->guild_id, log_msg);
}

/* ============================================================================
 * Command handler – /ticketclose
 * ========================================================================= */

static void handle_ticket_close(struct discord *client,
                                 const struct discord_interaction *event) {
    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) {
        reply_ephemeral(client, event, "❌ This channel isn't an open ticket.");
        return;
    }

    if (t.status == TICKET_STATUS_CLOSED || t.status == TICKET_STATUS_RESOLVED) {
        reply_ephemeral(client, event, "ℹ️ This ticket is already closed.");
        ticket_db_free(&t);
        return;
    }

    u64_snowflake_t caller_id = event->member->user->id;
    if (caller_id != t.opener_id && !is_staff(event)) {
        reply_ephemeral(client, event,
                        "❌ Only the ticket opener or a staff member can close this ticket.");
        ticket_db_free(&t);
        return;
    }

    if (ticket_db_update_status(g_db, t.id, TICKET_STATUS_CLOSED) != 0) {
        reply_ephemeral(client, event, "❌ Failed to close ticket – please try again.");
        ticket_db_free(&t);
        return;
    }

    char msg[256];
    snprintf(msg, sizeof(msg),
             "🔒 **Ticket #%d closed** by <@%" PRIu64 ">.",
             t.id, caller_id);
    reply_public(client, event, msg);

    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg),
             "🔒 Ticket #%d closed by <@%" PRIu64 ">.",
             t.id, caller_id);
    log_to_staff_channel(event->guild_id, log_msg);

    ticket_db_free(&t);
}

/* ============================================================================
 * Command handlers – /ticketnote (staff)
 * ========================================================================= */

static void handle_note_add(struct discord *client,
                             const struct discord_interaction *event,
                             const struct discord_application_command_interaction_data_option *subcmd) {
    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) {
        reply_ephemeral(client, event, "❌ This channel isn't a ticket.");
        return;
    }

    const struct discord_application_command_interaction_data_option *text_opt =
        find_sub_option(subcmd, "text");
    if (!text_opt || !text_opt->value) {
        reply_ephemeral(client, event, "❌ You must provide note text.");
        ticket_db_free(&t);
        return;
    }

    if (ticket_note_add(g_db, t.id, event->member->user->id, text_opt->value) != 0) {
        reply_ephemeral(client, event, "❌ Failed to save note.");
        ticket_db_free(&t);
        return;
    }

    char msg[512];
    snprintf(msg, sizeof(msg), "📝 Note added to ticket #%d.", t.id);
    reply_ephemeral(client, event, msg);
    ticket_db_free(&t);
}

static void handle_note_list(struct discord *client,
                              const struct discord_interaction *event) {
    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) {
        reply_ephemeral(client, event, "❌ This channel isn't a ticket.");
        return;
    }

    TicketNote *notes = NULL;
    int count = 0;
    if (ticket_note_get_all(g_db, t.id, &notes, &count) != 0) {
        reply_ephemeral(client, event, "❌ Failed to retrieve notes.");
        ticket_db_free(&t);
        return;
    }

    if (count == 0) {
        reply_ephemeral(client, event, "No notes on this ticket yet.");
        ticket_db_free(&t);
        return;
    }

    char buf[2000];
    int offset = snprintf(buf, sizeof(buf),
                          "📋 **Notes for ticket #%d** (%d total)\n\n",
                          t.id, count);

    for (int i = 0; i < count && offset < (int)sizeof(buf) - 200; i++) {
        TicketNote *n = &notes[i];
        offset += snprintf(buf + offset, sizeof(buf) - offset,
                           "%s**[#%d]** <@%" PRIu64 "> — <t:%ld:R>\n%s\n\n",
                           n->is_pinned ? "📌 " : "",
                           n->id, n->author_id, n->created_at, n->content);
    }

    if (count > 10)
        offset += snprintf(buf + offset, sizeof(buf) - offset,
                           "*…and %d more notes not shown.*", count - 10);

    reply_ephemeral(client, event, buf);
    ticket_notes_free(notes, count);
    ticket_db_free(&t);
}

static void handle_note_pin(struct discord *client,
                             const struct discord_interaction *event,
                             const struct discord_application_command_interaction_data_option *subcmd) {
    const struct discord_application_command_interaction_data_option *id_opt =
        find_sub_option(subcmd, "note_id");
    if (!id_opt || !id_opt->value) {
        reply_ephemeral(client, event, "❌ Provide a note ID.");
        return;
    }

    int note_id = atoi(id_opt->value);
    if (note_id <= 0) {
        reply_ephemeral(client, event, "❌ Invalid note ID.");
        return;
    }

    if (ticket_note_set_pinned(g_db, note_id, true) == 0)
        reply_ephemeral(client, event, "📌 Note pinned.");
    else
        reply_ephemeral(client, event, "❌ Note not found or couldn't be pinned.");
}

static void handle_note_delete(struct discord *client,
                                const struct discord_interaction *event,
                                const struct discord_application_command_interaction_data_option *subcmd) {
    const struct discord_application_command_interaction_data_option *id_opt =
        find_sub_option(subcmd, "note_id");
    if (!id_opt || !id_opt->value) {
        reply_ephemeral(client, event, "❌ Provide a note ID.");
        return;
    }

    int note_id = atoi(id_opt->value);
    if (note_id <= 0) {
        reply_ephemeral(client, event, "❌ Invalid note ID.");
        return;
    }

    u64_snowflake_t requester = event->member->user->id;
    if (ticket_note_delete(g_db, note_id, requester) == 0)
        reply_ephemeral(client, event, "🗑️ Note deleted.");
    else
        reply_ephemeral(client, event,
                        "❌ Note not found, or you can only delete your own notes.");
}

/* ============================================================================
 * Command handlers – /ticketoutcome (staff)
 * ========================================================================= */

static void handle_outcome_set(struct discord *client,
                                const struct discord_interaction *event,
                                const struct discord_application_command_interaction_data_option *subcmd) {
    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) {
        reply_ephemeral(client, event, "❌ This channel isn't a ticket.");
        return;
    }

    const struct discord_application_command_interaction_data_option *outcome_opt =
        find_sub_option(subcmd, "outcome");
    const struct discord_application_command_interaction_data_option *notes_opt =
        find_sub_option(subcmd, "notes");

    if (!outcome_opt || !outcome_opt->value) {
        reply_ephemeral(client, event, "❌ Provide an outcome value.");
        ticket_db_free(&t);
        return;
    }

    TicketOutcome outcome = outcome_from_string(outcome_opt->value);
    const char   *notes  = notes_opt ? notes_opt->value : NULL;

    if (ticket_db_set_outcome(g_db, t.id, outcome, notes) != 0) {
        reply_ephemeral(client, event, "❌ Failed to set outcome.");
        ticket_db_free(&t);
        return;
    }

    char msg[512];
    snprintf(msg, sizeof(msg),
             "✅ Outcome for ticket #%d set to **%s**%s%s",
             t.id,
             ticket_outcome_string(outcome),
             notes ? "\n> " : "",
             notes ? notes  : "");
    reply_ephemeral(client, event, msg);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg),
             "📋 Ticket #%d outcome set to **%s** by <@%" PRIu64 ">.",
             t.id, ticket_outcome_string(outcome), event->member->user->id);
    log_to_staff_channel(event->guild_id, log_msg);

    ticket_db_free(&t);
}

static void handle_outcome_clear(struct discord *client,
                                  const struct discord_interaction *event) {
    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) {
        reply_ephemeral(client, event, "❌ This channel isn't a ticket.");
        return;
    }

    if (ticket_db_set_outcome(g_db, t.id, TICKET_OUTCOME_NONE, NULL) != 0) {
        reply_ephemeral(client, event, "❌ Failed to clear outcome.");
        ticket_db_free(&t);
        return;
    }

    reply_ephemeral(client, event, "🗑️ Outcome cleared.");
    ticket_db_free(&t);
}

/* ============================================================================
 * Command handler – /ticketassign (staff)
 * ========================================================================= */

static void handle_assign(struct discord *client,
                           const struct discord_interaction *event) {
    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) {
        reply_ephemeral(client, event, "❌ This channel isn't a ticket.");
        return;
    }

    const struct discord_application_command_interaction_data_option *staff_opt =
        find_option(event, "staff_member");
    if (!staff_opt || !staff_opt->value) {
        reply_ephemeral(client, event, "❌ Specify a staff member.");
        ticket_db_free(&t);
        return;
    }

    u64_snowflake_t staff_id = (u64_snowflake_t)strtoull(staff_opt->value, NULL, 10);
    if (staff_id == 0) {
        reply_ephemeral(client, event, "❌ Invalid staff member ID.");
        ticket_db_free(&t);
        return;
    }

    if (ticket_db_update_assigned(g_db, t.id, staff_id) != 0) {
        reply_ephemeral(client, event, "❌ Failed to assign ticket.");
        ticket_db_free(&t);
        return;
    }

    char msg[256];
    snprintf(msg, sizeof(msg),
             "👤 Ticket #%d assigned to <@%" PRIu64 ">.", t.id, staff_id);
    reply_public(client, event, msg);

    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg),
             "👤 Ticket #%d assigned to <@%" PRIu64 "> by <@%" PRIu64 ">.",
             t.id, staff_id, event->member->user->id);
    log_to_staff_channel(event->guild_id, log_msg);

    ticket_db_free(&t);
}

/* ============================================================================
 * Command handler – /ticketpriority (staff)
 * ========================================================================= */

static void handle_priority(struct discord *client,
                             const struct discord_interaction *event) {
    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) {
        reply_ephemeral(client, event, "❌ This channel isn't a ticket.");
        return;
    }

    const struct discord_application_command_interaction_data_option *level_opt =
        find_option(event, "level");
    if (!level_opt || !level_opt->value) {
        reply_ephemeral(client, event, "❌ Provide a priority level.");
        ticket_db_free(&t);
        return;
    }

    TicketPriority priority = priority_from_string(level_opt->value);
    if (ticket_db_update_priority(g_db, t.id, priority) != 0) {
        reply_ephemeral(client, event, "❌ Failed to update priority.");
        ticket_db_free(&t);
        return;
    }

    char msg[256];
    snprintf(msg, sizeof(msg),
             "Priority for ticket #%d set to **%s**.",
             t.id, ticket_priority_string(priority));
    reply_ephemeral(client, event, msg);

    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg),
             "🏷️ Ticket #%d priority set to **%s** by <@%" PRIu64 ">.",
             t.id, ticket_priority_string(priority), event->member->user->id);
    log_to_staff_channel(event->guild_id, log_msg);

    ticket_db_free(&t);
}

/* ============================================================================
 * Command handler – /ticketstatus (staff)
 * ========================================================================= */

static void handle_status(struct discord *client,
                           const struct discord_interaction *event) {
    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) {
        reply_ephemeral(client, event, "❌ This channel isn't a ticket.");
        return;
    }

    const struct discord_application_command_interaction_data_option *status_opt =
        find_option(event, "status");
    if (!status_opt || !status_opt->value) {
        reply_ephemeral(client, event, "❌ Provide a status value.");
        ticket_db_free(&t);
        return;
    }

    TicketStatus new_status = status_from_string(status_opt->value);
    if (ticket_db_update_status(g_db, t.id, new_status) != 0) {
        reply_ephemeral(client, event, "❌ Failed to update status.");
        ticket_db_free(&t);
        return;
    }

    char msg[256];
    snprintf(msg, sizeof(msg),
             "Status of ticket #%d updated to **%s**.",
             t.id, ticket_status_string(new_status));
    reply_public(client, event, msg);

    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg),
             "🔄 Ticket #%d status → **%s** by <@%" PRIu64 ">.",
             t.id, ticket_status_string(new_status), event->member->user->id);
    log_to_staff_channel(event->guild_id, log_msg);

    ticket_db_free(&t);
}

/* ============================================================================
 * Command handler – /ticketsummary (staff)
 * ========================================================================= */

static void handle_summary(struct discord *client,
                            const struct discord_interaction *event) {
    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) {
        reply_ephemeral(client, event, "❌ This channel isn't a ticket.");
        return;
    }

    TicketNote *notes      = NULL;
    int         note_count = 0;
    ticket_note_get_all(g_db, t.id, &notes, &note_count);

    char buf[2000];
    int  off = 0;

    off += snprintf(buf + off, sizeof(buf) - off,
                    "📋 **Ticket #%d Summary**\n"
                    "**Server:** %s\n"
                    "**Opened by:** <@%" PRIu64 ">\n"
                    "**Assigned to:** %s\n"
                    "**Status:** %s\n"
                    "**Priority:** %s\n"
                    "**Outcome:** %s\n",
                    t.id,
                    t.subject ? t.subject : "—",
                    t.opener_id,
                    t.assigned_to ? "see below" : "Unassigned",
                    ticket_status_string(t.status),
                    ticket_priority_string(t.priority),
                    ticket_outcome_string(t.outcome));

    if (t.assigned_to)
        off += snprintf(buf + off, sizeof(buf) - off,
                        "**Assignee:** <@%" PRIu64 ">\n", t.assigned_to);

    if (t.outcome_notes && t.outcome_notes[0])
        off += snprintf(buf + off, sizeof(buf) - off,
                        "**Outcome notes:** %s\n", t.outcome_notes);

    if (t.created_at)
        off += snprintf(buf + off, sizeof(buf) - off,
                        "**Opened:** <t:%ld:F>\n", t.created_at);

    if (t.closed_at)
        off += snprintf(buf + off, sizeof(buf) - off,
                        "**Closed:** <t:%ld:F>\n", t.closed_at);

    if (note_count > 0) {
        off += snprintf(buf + off, sizeof(buf) - off,
                        "\n📝 **Staff Notes** (%d)\n", note_count);

        int show = note_count > 5 ? 5 : note_count;
        for (int i = 0; i < show && off < (int)sizeof(buf) - 200; i++) {
            TicketNote *n = &notes[i];
            off += snprintf(buf + off, sizeof(buf) - off,
                            "%s**[#%d]** <@%" PRIu64 "> — <t:%ld:R>\n> %s\n",
                            n->is_pinned ? "📌 " : "",
                            n->id, n->author_id, n->created_at, n->content);
        }
        if (note_count > 5)
            off += snprintf(buf + off, sizeof(buf) - off,
                            "*…and %d more notes. Use `/ticketnote list` to see all.*",
                            note_count - 5);
    } else {
        off += snprintf(buf + off, sizeof(buf) - off, "\n*No staff notes yet.*");
    }

    reply_ephemeral(client, event, buf);
    ticket_notes_free(notes, note_count);
    ticket_db_free(&t);
}

/* ============================================================================
 * Command handler – /ticketconfig (staff)
 *
 * Flat command: no subcommands.
 *   • No options provided  → display current configuration (view mode).
 *   • Any options provided → apply each supplied field and confirm.
 *
 * All four options are optional so the user can update any combination of
 * fields in a single invocation.
 * ========================================================================= */

static void handle_config(struct discord *client,
                           const struct discord_interaction *event) {

    typedef const struct discord_application_command_interaction_data_option opt_t;

    opt_t *tc_opt = find_option(event, "ticket_channel");
    opt_t *lc_opt = find_option(event, "log_channel");
    opt_t *ms_opt = find_option(event, "main_server");
    opt_t *ss_opt = find_option(event, "staff_server");

    /* ── View mode: no options supplied ─────────────────────────────────── */
    if (!tc_opt && !lc_opt && !ms_opt && !ss_opt) {
        TicketConfig cfg = {0};
        if (ticket_config_get(g_db, event->guild_id, &cfg) != 0) {
            reply_ephemeral(client, event, "❌ Failed to load configuration.");
            return;
        }

        char buf[1024];
        int  off = snprintf(buf, sizeof(buf), "⚙️ **Ticket Configuration**\n\n");

        off += (cfg.ticket_channel_id)
            ? snprintf(buf + off, sizeof(buf) - off,
                       "**Ticket Channel:** <#%" PRIu64 ">\n", cfg.ticket_channel_id)
            : snprintf(buf + off, sizeof(buf) - off,
                       "**Ticket Channel:** *(not set)*\n");

        off += (cfg.log_channel_id)
            ? snprintf(buf + off, sizeof(buf) - off,
                       "**Log Channel:** <#%" PRIu64 ">\n", cfg.log_channel_id)
            : snprintf(buf + off, sizeof(buf) - off,
                       "**Log Channel:** *(not set)*\n");

        off += snprintf(buf + off, sizeof(buf) - off,
                        "**Main Server ID:** %s\n",
                        (cfg.main_server_id && cfg.main_server_id[0])
                            ? cfg.main_server_id : "*(not set)*");

        off += snprintf(buf + off, sizeof(buf) - off,
                        "**Staff Server ID:** %s\n",
                        (cfg.staff_server_id && cfg.staff_server_id[0])
                            ? cfg.staff_server_id : "*(not set)*");

        reply_ephemeral(client, event, buf);
        ticket_config_free(&cfg);
        return;
    }

    /* ── Set mode: apply whichever options were supplied ─────────────────── */
    char  reply[1024];
    int   roff     = snprintf(reply, sizeof(reply), "⚙️ **Configuration updated:**\n");
    char  log_msg[512];
    int   loff     = snprintf(log_msg, sizeof(log_msg), "⚙️ Config updated by <@%" PRIu64 ">:",
                               event->member->user->id);
    bool  any_ok   = false;

    if (tc_opt && tc_opt->value) {
        u64_snowflake_t ch = (u64_snowflake_t)strtoull(tc_opt->value, NULL, 10);
        if (ch && ticket_config_set_ticket_channel(g_db, event->guild_id, ch) == 0) {
            roff += snprintf(reply + roff, sizeof(reply) - roff,
                             "✅ Ticket channel → <#%" PRIu64 ">\n", ch);
            loff += snprintf(log_msg + loff, sizeof(log_msg) - loff,
                             " ticket_channel=<#%" PRIu64 ">", ch);
            any_ok = true;
        } else {
            roff += snprintf(reply + roff, sizeof(reply) - roff,
                             "❌ Failed to set ticket channel.\n");
        }
    }

    if (lc_opt && lc_opt->value) {
        u64_snowflake_t ch = (u64_snowflake_t)strtoull(lc_opt->value, NULL, 10);
        if (ch && ticket_config_set_log_channel(g_db, event->guild_id, ch) == 0) {
            roff += snprintf(reply + roff, sizeof(reply) - roff,
                             "✅ Log channel → <#%" PRIu64 ">\n", ch);
            loff += snprintf(log_msg + loff, sizeof(log_msg) - loff,
                             " log_channel=<#%" PRIu64 ">", ch);
            any_ok = true;
        } else {
            roff += snprintf(reply + roff, sizeof(reply) - roff,
                             "❌ Failed to set log channel.\n");
        }
    }

    if (ms_opt && ms_opt->value && ms_opt->value[0]) {
        /* Snowflake digit check */
        bool valid = true;
        for (const char *p = ms_opt->value; *p; p++)
            if (*p < '0' || *p > '9') { valid = false; break; }

        if (valid && ticket_config_set_main_server(g_db, event->guild_id, ms_opt->value) == 0) {
            roff += snprintf(reply + roff, sizeof(reply) - roff,
                             "✅ Main server → `%s`\n", ms_opt->value);
            loff += snprintf(log_msg + loff, sizeof(log_msg) - loff,
                             " main_server=%s", ms_opt->value);
            any_ok = true;
        } else {
            roff += snprintf(reply + roff, sizeof(reply) - roff,
                             "❌ Failed to set main server (must be a numeric snowflake).\n");
        }
    }

    if (ss_opt && ss_opt->value && ss_opt->value[0]) {
        bool valid = true;
        for (const char *p = ss_opt->value; *p; p++)
            if (*p < '0' || *p > '9') { valid = false; break; }

        if (valid && ticket_config_set_staff_server(g_db, event->guild_id, ss_opt->value) == 0) {
            roff += snprintf(reply + roff, sizeof(reply) - roff,
                             "✅ Staff server → `%s`\n", ss_opt->value);
            loff += snprintf(log_msg + loff, sizeof(log_msg) - loff,
                             " staff_server=%s", ss_opt->value);
            any_ok = true;
        } else {
            roff += snprintf(reply + roff, sizeof(reply) - roff,
                             "❌ Failed to set staff server (must be a numeric snowflake).\n");
        }
    }

    if (any_ok)
        config_cache_invalidate(event->guild_id);

    reply_ephemeral(client, event, reply);

    if (any_ok)
        log_to_staff_channel(event->guild_id, log_msg);
}

/* ============================================================================
 * Top-level interaction router
 * ========================================================================= */

void on_ticket_interaction(struct discord *client,
                            const struct discord_interaction *event) {
    if (!event->data) return;
    const char *cmd = event->data->name;

    /* ── /ticket ─────────────────────────────────────────────────────────── */
    if (strcmp(cmd, "ticket") == 0) {
        handle_ticket_open(client, event);
        return;
    }

    /* ── /ticketclose ────────────────────────────────────────────────────── */
    if (strcmp(cmd, "ticketclose") == 0) {
        handle_ticket_close(client, event);
        return;
    }

    /* ── /ticketnote ─────────────────────────────────────────────────────── */
    if (strcmp(cmd, "ticketnote") == 0) {
        if (!is_staff(event)) {
            reply_ephemeral(client, event, "❌ Staff only.");
            return;
        }
        const struct discord_application_command_interaction_data_option *sub =
            get_active_subcommand(event);
        if (!sub) { reply_ephemeral(client, event, "Missing sub-command."); return; }

        if (strcmp(sub->name, "add")    == 0) { handle_note_add(client, event, sub);    return; }
        if (strcmp(sub->name, "list")   == 0) { handle_note_list(client, event);        return; }
        if (strcmp(sub->name, "pin")    == 0) { handle_note_pin(client, event, sub);    return; }
        if (strcmp(sub->name, "delete") == 0) { handle_note_delete(client, event, sub); return; }

        reply_ephemeral(client, event, "Unknown sub-command.");
        return;
    }

    /* ── /ticketoutcome ──────────────────────────────────────────────────── */
    if (strcmp(cmd, "ticketoutcome") == 0) {
        if (!is_staff(event)) {
            reply_ephemeral(client, event, "❌ Staff only.");
            return;
        }
        const struct discord_application_command_interaction_data_option *sub =
            get_active_subcommand(event);
        if (!sub) { reply_ephemeral(client, event, "Missing sub-command."); return; }

        if (strcmp(sub->name, "set")   == 0) { handle_outcome_set(client, event, sub); return; }
        if (strcmp(sub->name, "clear") == 0) { handle_outcome_clear(client, event);    return; }

        reply_ephemeral(client, event, "Unknown sub-command.");
        return;
    }

    /* ── /ticketassign ───────────────────────────────────────────────────── */
    if (strcmp(cmd, "ticketassign") == 0) {
        if (!is_staff(event)) { reply_ephemeral(client, event, "❌ Staff only."); return; }
        handle_assign(client, event);
        return;
    }

    /* ── /ticketpriority ─────────────────────────────────────────────────── */
    if (strcmp(cmd, "ticketpriority") == 0) {
        if (!is_staff(event)) { reply_ephemeral(client, event, "❌ Staff only."); return; }
        handle_priority(client, event);
        return;
    }

    /* ── /ticketstatus ───────────────────────────────────────────────────── */
    if (strcmp(cmd, "ticketstatus") == 0) {
        if (!is_staff(event)) { reply_ephemeral(client, event, "❌ Staff only."); return; }
        handle_status(client, event);
        return;
    }

    /* ── /ticketsummary ──────────────────────────────────────────────────── */
    if (strcmp(cmd, "ticketsummary") == 0) {
        if (!is_staff(event)) { reply_ephemeral(client, event, "❌ Staff only."); return; }
        handle_summary(client, event);
        return;
    }

    /* ── /ticketconfig ───────────────────────────────────────────────────── */
    if (strcmp(cmd, "ticketconfig") == 0) {
        if (!is_staff(event)) { reply_ephemeral(client, event, "❌ Staff only."); return; }
        handle_config(client, event);
        return;
    }
}

/* ============================================================================
 * Message handler – reserved for transcript / inactivity features
 * ========================================================================= */

void on_ticket_message(struct discord *client,
                       const struct discord_message *event) {
    (void)client;
    (void)event;
}

/* ============================================================================
 * Slash command registration
 *
 * Pass guild_id = 0  → register as global commands  (up to 1 hour propagation)
 * Pass guild_id = X  → register as guild commands   (instant, dev-friendly)
 *
 * For a public multi-server bot, call this once at startup with guild_id = 0.
 * ========================================================================= */

void register_ticket_commands(struct discord *client,
                               u64_snowflake_t application_id,
                               u64_snowflake_t guild_id) {

/*
 * REG dispatches to the guild or global endpoint depending on guild_id.
 * Adjust discord_create_global_application_command's signature to match
 * your Orca build if needed (some builds omit the application_id param).
 */
#define REG(params_ptr, label)                                                       \
    do {                                                                             \
        ORCAcode _c;                                                                 \
        if (guild_id)                                                                \
            _c = discord_create_guild_application_command(                           \
                     client, application_id, guild_id, (params_ptr), NULL);          \
        else                                                                         \
            _c = discord_create_global_application_command(                          \
                     client, application_id,                                         \
                     (struct discord_create_global_application_command_params *)      \
                         (params_ptr),                                               \
                     NULL);                                                          \
        if (_c == ORCA_OK)                                                           \
            printf("[ticket] /%s registered (%s)\n", (label),                        \
                   guild_id ? "guild" : "global");                                   \
        else                                                                         \
            printf("[ticket] Failed to register /%s (code %d)\n", (label), _c);     \
    } while (0)

    /* ── /ticket server:<id> ──────────────────────────────────────────────── */
    {
        struct discord_application_command_option server_opt = {
            .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name        = "server",
            .description = "Snowflake ID of the server your ticket relates to",
            .required    = true,
        };
        struct discord_application_command_option *ticket_opts[] = {
            &server_opt, NULL,
        };
        struct discord_create_guild_application_command_params ticket_cmd = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticket",
            .description = "Open a support ticket for your server",
            .options     = ticket_opts,
        };
        REG(&ticket_cmd, "ticket");
    }

    /* ── /ticketclose ─────────────────────────────────────────────────────── */
    {
        struct discord_create_guild_application_command_params close_cmd = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticketclose",
            .description = "Close this support ticket",
        };
        REG(&close_cmd, "ticketclose");
    }

    /* ── /ticketnote ──────────────────────────────────────────────────────── */
    {
        struct discord_application_command_option add_text_opt = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name = "text", .description = "Note content", .required = true,
        };
        struct discord_application_command_option *add_opts[] = { &add_text_opt, NULL };

        struct discord_application_command_option note_id_opt = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_INTEGER,
            .name = "note_id", .description = "ID of the note", .required = true,
        };
        struct discord_application_command_option *nid_opts[] = { &note_id_opt, NULL };

        struct discord_application_command_option add_sub = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
            .name = "add", .description = "Add a private staff note", .options = add_opts,
        };
        struct discord_application_command_option list_sub = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
            .name = "list", .description = "List all notes on this ticket",
        };
        struct discord_application_command_option pin_sub = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
            .name = "pin", .description = "Pin a note", .options = nid_opts,
        };
        struct discord_application_command_option del_sub = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
            .name = "delete", .description = "Delete one of your notes", .options = nid_opts,
        };
        struct discord_application_command_option *note_opts[] = {
            &add_sub, &list_sub, &pin_sub, &del_sub, NULL,
        };
        struct discord_create_guild_application_command_params note_cmd = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticketnote",
            .description = "(Staff) Manage private notes on this ticket",
            .options     = note_opts,
        };
        REG(&note_cmd, "ticketnote");
    }

    /* ── /ticketoutcome ───────────────────────────────────────────────────── */
    {
        struct discord_application_command_option_choice outcome_choices[] = {
            { .name = "Resolved",    .value = "resolved"    },
            { .name = "Duplicate",   .value = "duplicate"   },
            { .name = "Invalid",     .value = "invalid"     },
            { .name = "No Response", .value = "no_response" },
            { .name = "Escalated",   .value = "escalated"   },
            { .name = "Other",       .value = "other"       },
            { 0 },
        };
        struct discord_application_command_option_choice *oc_ptrs[] = {
            &outcome_choices[0], &outcome_choices[1], &outcome_choices[2],
            &outcome_choices[3], &outcome_choices[4], &outcome_choices[5], NULL,
        };

        struct discord_application_command_option set_outcome_opt = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name = "outcome", .description = "Outcome type",
            .required = true, .choices = oc_ptrs,
        };
        struct discord_application_command_option set_notes_opt = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name = "notes", .description = "Additional notes (optional)",
            .required = false,
        };
        struct discord_application_command_option *set_opts[] = {
            &set_outcome_opt, &set_notes_opt, NULL,
        };
        struct discord_application_command_option set_sub = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
            .name = "set", .description = "Record the ticket outcome", .options = set_opts,
        };
        struct discord_application_command_option clear_sub = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
            .name = "clear", .description = "Clear the current outcome",
        };
        struct discord_application_command_option *outcome_opts[] = {
            &set_sub, &clear_sub, NULL,
        };
        struct discord_create_guild_application_command_params outcome_cmd = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticketoutcome",
            .description = "(Staff) Set or clear the resolution outcome",
            .options     = outcome_opts,
        };
        REG(&outcome_cmd, "ticketoutcome");
    }

    /* ── /ticketassign ────────────────────────────────────────────────────── */
    {
        struct discord_application_command_option staff_opt = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_USER,
            .name = "staff_member", .description = "Staff member to assign",
            .required = true,
        };
        struct discord_application_command_option *assign_opts[] = { &staff_opt, NULL };
        struct discord_create_guild_application_command_params assign_cmd = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticketassign",
            .description = "(Staff) Assign this ticket to a staff member",
            .options     = assign_opts,
        };
        REG(&assign_cmd, "ticketassign");
    }

    /* ── /ticketpriority ──────────────────────────────────────────────────── */
    {
        struct discord_application_command_option_choice pri_choices[] = {
            { .name = "Low",    .value = "low"    },
            { .name = "Medium", .value = "medium" },
            { .name = "High",   .value = "high"   },
            { .name = "Urgent", .value = "urgent" },
            { 0 },
        };
        struct discord_application_command_option_choice *pri_ptrs[] = {
            &pri_choices[0], &pri_choices[1], &pri_choices[2], &pri_choices[3], NULL,
        };
        struct discord_application_command_option level_opt = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name = "level", .description = "Priority level",
            .required = true, .choices = pri_ptrs,
        };
        struct discord_application_command_option *pri_opts[] = { &level_opt, NULL };
        struct discord_create_guild_application_command_params pri_cmd = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticketpriority",
            .description = "(Staff) Set the ticket priority",
            .options     = pri_opts,
        };
        REG(&pri_cmd, "ticketpriority");
    }

    /* ── /ticketstatus ────────────────────────────────────────────────────── */
    {
        struct discord_application_command_option_choice status_choices[] = {
            { .name = "Open",         .value = "open"         },
            { .name = "In Progress",  .value = "in_progress"  },
            { .name = "Pending User", .value = "pending_user" },
            { .name = "Resolved",     .value = "resolved"     },
            { 0 },
        };
        struct discord_application_command_option_choice *st_ptrs[] = {
            &status_choices[0], &status_choices[1],
            &status_choices[2], &status_choices[3], NULL,
        };
        struct discord_application_command_option status_opt = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name = "status", .description = "New status",
            .required = true, .choices = st_ptrs,
        };
        struct discord_application_command_option *st_opts[] = { &status_opt, NULL };
        struct discord_create_guild_application_command_params st_cmd = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticketstatus",
            .description = "(Staff) Update the workflow status of this ticket",
            .options     = st_opts,
        };
        REG(&st_cmd, "ticketstatus");
    }

    /* ── /ticketsummary ───────────────────────────────────────────────────── */
    {
        struct discord_create_guild_application_command_params summary_cmd = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticketsummary",
            .description = "(Staff) View full ticket details: notes, outcome, history",
        };
        REG(&summary_cmd, "ticketsummary");
    }

    /* ── /ticketconfig ────────────────────────────────────────────────────── */
    /*
     * Flat command – no subcommands.  All options are optional.
     * Omitting all options shows the current config; supplying any option sets it.
     */
    {
        struct discord_application_command_option tc_opt = {
            .type        = DISCORD_APPLICATION_COMMAND_OPTION_CHANNEL,
            .name        = "ticket_channel",
            .description = "Channel where tickets are created",
            .required    = false,
        };
        struct discord_application_command_option lc_opt = {
            .type        = DISCORD_APPLICATION_COMMAND_OPTION_CHANNEL,
            .name        = "log_channel",
            .description = "Staff log channel for ticket events",
            .required    = false,
        };
        struct discord_application_command_option ms_opt = {
            .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name        = "main_server",
            .description = "Snowflake ID of the main community server",
            .required    = false,
        };
        struct discord_application_command_option ss_opt = {
            .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name        = "staff_server",
            .description = "Snowflake ID of the private staff server",
            .required    = false,
        };
        struct discord_application_command_option *cfg_opts[] = {
            &tc_opt, &lc_opt, &ms_opt, &ss_opt, NULL,
        };
        struct discord_create_guild_application_command_params cfg_cmd = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticketconfig",
            .description = "(Staff) View or update ticket system configuration",
            .options     = cfg_opts,
        };
        REG(&cfg_cmd, "ticketconfig");
    }

#undef REG
}

/* ============================================================================
 * Module init
 * ========================================================================= */

void ticket_module_init(struct discord *client, Database *db) {
    g_client = client;
    g_db     = db;

    memset(g_config_cache, 0, sizeof(g_config_cache));

    if (ticket_db_init_tables(db) != 0)
        fprintf(stderr, "[ticket] Warning: DB table initialisation failed.\n");

    printf("[ticket] Module initialised.\n");
}