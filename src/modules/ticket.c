/*
 * ticket.c  –  Ticket system
 *
 * User command (works in DMs and guilds):
 *   /ticket server:<guild_id> [subject:<text>]
 *       Opens a ticket in the specified server.  A dedicated channel is
 *       created inside that server's configured ticket category and the
 *       user receives a DM confirmation.
 *
 * Staff commands (require the configured staff role or MANAGE_MESSAGES):
 *   /ticketnote    add <text>           – add a private staff note
 *   /ticketnote    list                 – list all notes on this ticket
 *   /ticketnote    pin <note_id>        – pin/unpin an important note
 *   /ticketnote    delete <note_id>     – delete one of your own notes
 *   /ticketoutcome set <outcome> [notes]– record the ticket resolution
 *   /ticketoutcome clear                – clear the current outcome
 *   /ticketassign  <staff_member>       – assign ticket to a staff member
 *   /ticketpriority <level>             – LOW / MEDIUM / HIGH / URGENT
 *   /ticketstatus  <status>             – OPEN / IN_PROGRESS / PENDING_USER / RESOLVED
 *   /ticketsummary                      – full summary: notes, outcome, history
 *   /closeticket                        – close the current ticket
 *   /ticketconfig  …                    – server-level configuration
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <orca/discord.h>

#include "ticket.h"
#include "database.h"

/* ============================================================================
 * Module globals
 * ========================================================================= */

static Database        *g_db     = NULL;
static struct discord  *g_client = NULL;

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
    if (!s)                          return TICKET_PRIORITY_MEDIUM;
    if (strcasecmp(s, "low")    == 0) return TICKET_PRIORITY_LOW;
    if (strcasecmp(s, "high")   == 0) return TICKET_PRIORITY_HIGH;
    if (strcasecmp(s, "urgent") == 0) return TICKET_PRIORITY_URGENT;
    return TICKET_PRIORITY_MEDIUM;
}

static TicketOutcome outcome_from_string(const char *s) {
    if (!s)                                                   return TICKET_OUTCOME_NONE;
    if (strcasecmp(s, "resolved")   == 0)                    return TICKET_OUTCOME_RESOLVED;
    if (strcasecmp(s, "duplicate")  == 0)                    return TICKET_OUTCOME_DUPLICATE;
    if (strcasecmp(s, "invalid")    == 0)                    return TICKET_OUTCOME_INVALID;
    if (strcasecmp(s, "noresponse") == 0 ||
        strcasecmp(s, "no_response")== 0)                    return TICKET_OUTCOME_NO_RESPONSE;
    if (strcasecmp(s, "escalated")  == 0)                    return TICKET_OUTCOME_ESCALATED;
    return TICKET_OUTCOME_OTHER;
}

static TicketStatus status_from_string(const char *s) {
    if (!s)                                                   return TICKET_STATUS_OPEN;
    if (strcasecmp(s, "open")         == 0)                  return TICKET_STATUS_OPEN;
    if (strcasecmp(s, "in_progress")  == 0 ||
        strcasecmp(s, "inprogress")   == 0)                  return TICKET_STATUS_IN_PROGRESS;
    if (strcasecmp(s, "pending_user") == 0 ||
        strcasecmp(s, "pendinguser")  == 0)                  return TICKET_STATUS_PENDING_USER;
    if (strcasecmp(s, "resolved")     == 0)                  return TICKET_STATUS_RESOLVED;
    return TICKET_STATUS_OPEN;
}

/* ============================================================================
 * Database – table init
 * ========================================================================= */

int ticket_db_init_tables(Database *db) {
    const char *sql =
        "CREATE TABLE IF NOT EXISTS ticket_config ("
        "  guild_id      INTEGER PRIMARY KEY,"
        "  category_id   INTEGER DEFAULT 0,"   /* category to create ticket channels in */
        "  staff_log_id  INTEGER DEFAULT 0,"   /* staff log channel                      */
        "  staff_role_id INTEGER DEFAULT 0"    /* role ID for staff checks               */
        ");"

        "CREATE TABLE IF NOT EXISTS tickets ("
        "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  channel_id    INTEGER NOT NULL UNIQUE,"
        "  guild_id      INTEGER NOT NULL,"
        "  opener_id     INTEGER NOT NULL,"
        "  assigned_to   INTEGER DEFAULT 0,"
        "  status        INTEGER DEFAULT 0,"
        "  priority      INTEGER DEFAULT 1,"
        "  outcome       INTEGER DEFAULT 0,"
        "  subject       TEXT,"
        "  outcome_notes TEXT,"
        "  created_at    INTEGER DEFAULT (strftime('%s','now')),"
        "  updated_at    INTEGER DEFAULT (strftime('%s','now')),"
        "  closed_at     INTEGER DEFAULT 0"
        ");"

        "CREATE TABLE IF NOT EXISTS ticket_notes ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ticket_id  INTEGER NOT NULL REFERENCES tickets(id) ON DELETE CASCADE,"
        "  author_id  INTEGER NOT NULL,"
        "  content    TEXT    NOT NULL,"
        "  is_pinned  INTEGER DEFAULT 0,"
        "  created_at INTEGER DEFAULT (strftime('%s','now'))"
        ");"

        "CREATE INDEX IF NOT EXISTS idx_tickets_channel  ON tickets(channel_id);"
        "CREATE INDEX IF NOT EXISTS idx_ticket_notes_tid ON ticket_notes(ticket_id);";

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
 * Database – config
 * ========================================================================= */

int ticket_config_get(Database *db, u64_snowflake_t guild_id, TicketConfig *out) {
    const char *sql =
        "SELECT guild_id, category_id, staff_log_id, staff_role_id "
        "FROM ticket_config WHERE guild_id = ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)guild_id);

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        out->guild_id      = (u64_snowflake_t)sqlite3_column_int64(stmt, 0);
        out->category_id   = (u64_snowflake_t)sqlite3_column_int64(stmt, 1);
        out->staff_log_id  = (u64_snowflake_t)sqlite3_column_int64(stmt, 2);
        out->staff_role_id = (u64_snowflake_t)sqlite3_column_int64(stmt, 3);
    }
    sqlite3_finalize(stmt);
    return (rc == SQLITE_ROW) ? 0 : 1; /* 1 = no config row yet */
}

int ticket_config_set(Database *db, const TicketConfig *cfg) {
    const char *sql =
        "INSERT INTO ticket_config (guild_id, category_id, staff_log_id, staff_role_id) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT(guild_id) DO UPDATE SET "
        "  category_id   = excluded.category_id,"
        "  staff_log_id  = excluded.staff_log_id,"
        "  staff_role_id = excluded.staff_role_id;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)cfg->guild_id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)cfg->category_id);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)cfg->staff_log_id);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)cfg->staff_role_id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ============================================================================
 * Database – ticket CRUD
 * ========================================================================= */

int ticket_db_create(Database *db, Ticket *t) {
    const char *sql =
        "INSERT INTO tickets (channel_id, guild_id, opener_id, status, priority, subject) "
        "VALUES (?, ?, ?, ?, ?, ?) RETURNING id;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)t->channel_id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)t->guild_id);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)t->opener_id);
    sqlite3_bind_int  (stmt, 4, t->status);
    sqlite3_bind_int  (stmt, 5, t->priority);
    sqlite3_bind_text (stmt, 6, t->subject ? t->subject : "", -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        t->id = (int)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_ROW) ? 0 : -1;
}

/* Shared column list for SELECT queries */
static const char *TICKET_SELECT_COLS =
    "SELECT id, channel_id, guild_id, opener_id, assigned_to, "
    "       status, priority, outcome, created_at, updated_at, closed_at, "
    "       subject, outcome_notes ";

static void row_to_ticket(sqlite3_stmt *stmt, Ticket *t) {
    t->id           = (int)sqlite3_column_int64(stmt, 0);
    t->channel_id   = (u64_snowflake_t)sqlite3_column_int64(stmt, 1);
    t->guild_id     = (u64_snowflake_t)sqlite3_column_int64(stmt, 2);
    t->opener_id    = (u64_snowflake_t)sqlite3_column_int64(stmt, 3);
    t->assigned_to  = (u64_snowflake_t)sqlite3_column_int64(stmt, 4);
    t->status       = (TicketStatus)   sqlite3_column_int  (stmt, 5);
    t->priority     = (TicketPriority) sqlite3_column_int  (stmt, 6);
    t->outcome      = (TicketOutcome)  sqlite3_column_int  (stmt, 7);
    t->created_at   = (long)sqlite3_column_int64(stmt, 8);
    t->updated_at   = (long)sqlite3_column_int64(stmt, 9);
    t->closed_at    = (long)sqlite3_column_int64(stmt, 10);

    const char *subj  = (const char *)sqlite3_column_text(stmt, 11);
    t->subject        = subj  ? strdup(subj)  : NULL;

    const char *notes = (const char *)sqlite3_column_text(stmt, 12);
    t->outcome_notes  = notes ? strdup(notes) : NULL;
}

/* Returns 0 if found, -1 otherwise. */
static int ticket_fetch(Database *db, const char *where_clause,
                         void (*bind)(sqlite3_stmt *, void *), void *arg,
                         Ticket *out) {
    char sql[512];
    snprintf(sql, sizeof(sql), "%s FROM tickets WHERE %s;",
             TICKET_SELECT_COLS, where_clause);

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    bind(stmt, arg);

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        row_to_ticket(stmt, out);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_ROW) ? 0 : -1;
}

static void bind_int64_1(sqlite3_stmt *s, void *v) {
    sqlite3_bind_int64(s, 1, *(sqlite3_int64 *)v);
}
static void bind_int_1(sqlite3_stmt *s, void *v) {
    sqlite3_bind_int(s, 1, *(int *)v);
}

int ticket_db_get_by_channel(Database *db, u64_snowflake_t channel_id, Ticket *out) {
    sqlite3_int64 v = (sqlite3_int64)channel_id;
    return ticket_fetch(db, "channel_id = ?", bind_int64_1, &v, out);
}

int ticket_db_get_by_id(Database *db, int ticket_id, Ticket *out) {
    return ticket_fetch(db, "id = ?", bind_int_1, &ticket_id, out);
}

/* Generic single-field update helper */
static int ticket_update_int(Database *db, int ticket_id,
                               const char *col, int value) {
    char sql[256];
    snprintf(sql, sizeof(sql),
             "UPDATE tickets SET %s = ?, updated_at = strftime('%%s','now') "
             "WHERE id = ?;", col);

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int(stmt, 1, value);
    sqlite3_bind_int(stmt, 2, ticket_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int ticket_db_update_status(Database *db, int ticket_id, TicketStatus status) {
    return ticket_update_int(db, ticket_id, "status", (int)status);
}

int ticket_db_update_priority(Database *db, int ticket_id, TicketPriority priority) {
    return ticket_update_int(db, ticket_id, "priority", (int)priority);
}

int ticket_db_update_assigned(Database *db, int ticket_id, u64_snowflake_t staff_id) {
    const char *sql =
        "UPDATE tickets SET assigned_to = ?, updated_at = strftime('%s','now') "
        "WHERE id = ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)staff_id);
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

    sqlite3_bind_int  (stmt, 1, (int)outcome);
    sqlite3_bind_text (stmt, 2, notes ? notes : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (stmt, 3, ticket_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

void ticket_db_free(Ticket *t) {
    if (!t) return;
    free(t->subject);
    free(t->outcome_notes);
    t->subject = t->outcome_notes = NULL;
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
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)author_id);
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

    int cap = 8, n = 0;
    TicketNote *arr = malloc(sizeof(TicketNote) * cap);
    if (!arr) { sqlite3_finalize(stmt); return -1; }

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
     * Pass requesting_staff_id = 0 to allow deletion regardless of author.
     * Otherwise the DELETE is restricted to the note's author.
     */
    const char *sql = (requesting_staff_id == 0)
        ? "DELETE FROM ticket_notes WHERE id = ?;"
        : "DELETE FROM ticket_notes WHERE id = ? AND author_id = ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int(stmt, 1, note_id);
    if (requesting_staff_id != 0)
        sqlite3_bind_int64(stmt, 2, (sqlite3_int64)requesting_staff_id);

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
 * Discord helpers
 * ========================================================================= */

/*
 * reply_ephemeral / reply_public
 *
 * Sends an interaction response.  If the interaction originated from a DM
 * (event->guild_id == 0) there is no member, but the response path is
 * identical — Discord routes it to the user's DM thread automatically.
 */
static void send_interaction_reply(struct discord *client,
                                    const struct discord_interaction *event,
                                    const char *message,
                                    bool ephemeral) {
    struct discord_interaction_callback_data data;
    memset(&data, 0, sizeof(data));
    data.content = (char *)message;
    if (ephemeral)
        data.flags = 64; /* EPHEMERAL (0x40) */

    struct discord_interaction_response resp;
    memset(&resp, 0, sizeof(resp));
    resp.type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE;
    resp.data = &data;

    discord_create_interaction_response(client, event->id,
                                        event->token, &resp, NULL);
}

static void reply_ephemeral(struct discord *client,
                             const struct discord_interaction *event,
                             const char *message) {
    send_interaction_reply(client, event, message, true);
}

static void reply_public(struct discord *client,
                          const struct discord_interaction *event,
                          const char *message) {
    send_interaction_reply(client, event, message, false);
}

/*
 * send_to_channel
 *
 * Single chokepoint for fire-and-forget channel messages (staff log, etc.).
 * Replace the body with the correct discord_create_message() call for your
 * Orca build.  To find the right symbol, run:
 *   nm -D /usr/local/lib/libdiscord.so | grep -i "create_message\|send"
 *
 * Common patterns:
 *   struct discord_create_message params = { .content = (char *)content };
 *   discord_create_message(client, channel_id, &params, NULL);
 */
static void send_to_channel(struct discord *client,
                             u64_snowflake_t channel_id,
                             const char *content) {
    /* TODO: replace stub with your Orca build's discord_create_message() */
    printf("[ticket] send_to_channel(ch=%" PRIu64 "): %s\n", channel_id, content);
    (void)client;
}

/*
 * create_ticket_channel
 *
 * Creates a new text channel in guild_id under the given category.
 * Returns the new channel's snowflake, or 0 on failure.
 *
 * Replace the stub with your Orca build's discord_create_guild_channel().
 * The channel name is typically "ticket-NNNN" (zero-padded ticket number).
 *
 * Example for common Orca builds:
 *   struct discord_create_guild_channel_params p = {
 *       .name      = name,
 *       .type      = DISCORD_CHANNEL_GUILD_TEXT,
 *       .parent_id = category_id,
 *   };
 *   struct discord_channel *ch = NULL;
 *   discord_create_guild_channel(client, guild_id, &p, &ch);
 *   u64_snowflake_t id = ch ? ch->id : 0;
 *   discord_channel_free(ch);
 *   return id;
 */
static u64_snowflake_t create_ticket_channel(struct discord *client,
                                              u64_snowflake_t guild_id,
                                              u64_snowflake_t category_id,
                                              const char *name) {
    /* TODO: replace stub */
    printf("[ticket] create_ticket_channel(guild=%" PRIu64 " cat=%" PRIu64 " name=%s)\n",
           guild_id, category_id, name);
    (void)client;

    /* Stub: return a fake non-zero ID so the rest of the flow can be tested */
    return guild_id ^ (u64_snowflake_t)(uintptr_t)name;
}

/*
 * send_dm
 *
 * Sends a direct message to a user.  Replace the stub with your Orca build's
 * DM creation + message send calls.
 */
static void send_dm(struct discord *client,
                    u64_snowflake_t user_id,
                    const char *content) {
    /* TODO: replace stub */
    printf("[ticket] send_dm(user=%" PRIu64 "): %s\n", user_id, content);
    (void)client;
}

/* ============================================================================
 * Permission helpers
 * ========================================================================= */

/*
 * staff_role_for_guild
 *
 * Returns the configured staff role ID, or 0 if none is configured.
 */
static u64_snowflake_t staff_role_for_guild(u64_snowflake_t guild_id) {
    TicketConfig cfg = {0};
    if (ticket_config_get(g_db, guild_id, &cfg) == 0)
        return cfg.staff_role_id;
    return 0;
}

/*
 * is_staff
 *
 * Returns true if the interaction member:
 *   1. Has the configured staff role for this guild, OR
 *   2. Has the MANAGE_MESSAGES permission bit set.
 *
 * Falls back gracefully when guild_id is 0 (DM context – always false).
 */
static bool is_staff(const struct discord_interaction *event) {
    if (!event->member) return false;

    /* Check the configured staff role first */
    u64_snowflake_t staff_role = staff_role_for_guild(event->guild_id);
    if (staff_role != 0 && event->member->roles) {
        for (int i = 0; event->member->roles[i]; i++) {
            if (event->member->roles[i]->value == staff_role)
                return true;
        }
    }

    /* Fall back to MANAGE_MESSAGES permission bit */
    return ((uint64_t)event->member->permissions & 0x0000000000002000ULL) != 0;
}

/* ============================================================================
 * Interaction option helpers
 * ========================================================================= */

/* Returns the active sub-command (options[0]) or NULL. */
static const struct discord_application_command_interaction_data_option *
get_subcommand(const struct discord_interaction *event) {
    if (!event->data || !event->data->options) return NULL;
    return event->data->options[0];
}

/* Finds a top-level named option (max 25 per Discord's limit). */
static const struct discord_application_command_interaction_data_option *
find_option(const struct discord_interaction *event, const char *name) {
    if (!event->data || !event->data->options) return NULL;
    for (int i = 0; i < 25 && event->data->options[i]; i++) {
        if (strcmp(event->data->options[i]->name, name) == 0)
            return event->data->options[i];
    }
    return NULL;
}

/* Finds a named option one level inside a sub-command. */
static const struct discord_application_command_interaction_data_option *
find_sub_option(const struct discord_application_command_interaction_data_option *sub,
                const char *name) {
    if (!sub || !sub->options) return NULL;
    for (int i = 0; i < 25 && sub->options[i]; i++) {
        if (strcmp(sub->options[i]->name, name) == 0)
            return sub->options[i];
    }
    return NULL;
}

/* ============================================================================
 * require_ticket / require_staff
 *
 * Convenience macros used at the top of every staff handler.
 *
 *   REQUIRE_TICKET(t)    fetches the ticket for the current channel into `t`
 *                        and returns with an ephemeral error if not found.
 *
 *   REQUIRE_STAFF()      returns with an ephemeral error if the caller is
 *                        not a staff member.
 * ========================================================================= */

#define REQUIRE_TICKET(t_ptr)                                               \
    do {                                                                    \
        if (ticket_db_get_by_channel(g_db, event->channel_id, (t_ptr))     \
                != 0) {                                                     \
            reply_ephemeral(client, event,                                  \
                            "❌ This channel isn't a ticket.");             \
            return;                                                         \
        }                                                                   \
    } while (0)

#define REQUIRE_STAFF()                                                     \
    do {                                                                    \
        if (!is_staff(event)) {                                             \
            reply_ephemeral(client, event,                                  \
                            "❌ You don't have permission to do that.");    \
            return;                                                         \
        }                                                                   \
    } while (0)

/* ============================================================================
 * Staff log helper
 * ========================================================================= */

static void log_to_guild(u64_snowflake_t guild_id, const char *message) {
    if (!g_client) return;
    TicketConfig cfg = {0};
    if (ticket_config_get(g_db, guild_id, &cfg) != 0 || !cfg.staff_log_id)
        return;
    send_to_channel(g_client, cfg.staff_log_id, message);
}

/* ============================================================================
 * /ticket  (user-facing, works from DMs)
 * ========================================================================= */

/*
 * handle_ticket_open
 *
 * The user supplies a target guild_id and an optional subject.
 * This handler:
 *   1. Validates the guild and its config.
 *   2. Creates a ticket channel in that guild.
 *   3. Inserts the ticket row.
 *   4. Replies ephemerally to the user (works in both DMs and guilds).
 *   5. Posts to the staff log channel.
 */
static void handle_ticket_open(struct discord *client,
                                const struct discord_interaction *event) {
    /* Resolve the target guild */
    const struct discord_application_command_interaction_data_option *srv_opt =
        find_option(event, "server");
    if (!srv_opt || !srv_opt->value) {
        reply_ephemeral(client, event, "❌ Please specify a server ID.");
        return;
    }

    u64_snowflake_t target_guild =
        (u64_snowflake_t)strtoull(srv_opt->value, NULL, 10);
    if (!target_guild) {
        reply_ephemeral(client, event, "❌ Invalid server ID.");
        return;
    }

    /* Look up that guild's ticket config */
    TicketConfig cfg = {0};
    if (ticket_config_get(g_db, target_guild, &cfg) != 0) {
        reply_ephemeral(client, event,
                        "❌ That server hasn't configured the ticket system yet. "
                        "Ask a staff member to run `/ticketconfig`.");
        return;
    }

    const struct discord_application_command_interaction_data_option *subj_opt =
        find_option(event, "subject");
    const char *subject = (subj_opt && subj_opt->value)
                          ? subj_opt->value
                          : "No subject provided";

    u64_snowflake_t opener_id =
        event->member ? event->member->user->id : event->user->id;

    /* Create the Discord channel first so we have its ID */
    char channel_name[64];
    snprintf(channel_name, sizeof(channel_name), "ticket-new");

    u64_snowflake_t channel_id =
        create_ticket_channel(client, target_guild, cfg.category_id, channel_name);
    if (!channel_id) {
        reply_ephemeral(client, event,
                        "❌ Failed to create a ticket channel in that server. "
                        "Check the bot's permissions.");
        return;
    }

    /* Insert the DB record */
    Ticket t = {
        .channel_id = channel_id,
        .guild_id   = target_guild,
        .opener_id  = opener_id,
        .status     = TICKET_STATUS_OPEN,
        .priority   = TICKET_PRIORITY_MEDIUM,
        .subject    = (char *)subject,
    };

    if (ticket_db_create(g_db, &t) != 0) {
        reply_ephemeral(client, event,
                        "❌ Failed to record your ticket. Please try again.");
        return;
    }

    /* Rename the channel now we have the ticket ID */
    snprintf(channel_name, sizeof(channel_name), "ticket-%04d", t.id);
    /* TODO: discord_modify_channel() to apply the name */

    /* Confirm to the opener */
    char confirm[512];
    snprintf(confirm, sizeof(confirm),
             "🎫 **Ticket #%d opened** in the requested server.\n"
             "**Subject:** %s\n"
             "Staff will be in touch shortly. "
             "To close the ticket, run `/closeticket` in the ticket channel.",
             t.id, subject);
    reply_ephemeral(client, event, confirm);

    /*
     * If the interaction came from a DM, the ephemeral reply above IS the DM.
     * When it came from a guild channel we also send a DM so the user has a
     * permanent reference.
     */
    if (event->guild_id != 0) {
        char dm_msg[512];
        snprintf(dm_msg, sizeof(dm_msg),
                 "🎫 Your ticket #%d has been opened in the server.\n"
                 "**Subject:** %s",
                 t.id, subject);
        send_dm(client, opener_id, dm_msg);
    }

    /* Staff log */
    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg),
             "🎫 Ticket #%d opened by <@%" PRIu64 "> — *%s*",
             t.id, opener_id, subject);
    log_to_guild(target_guild, log_msg);
}

/* ============================================================================
 * /closeticket  (usable by opener or staff)
 * ========================================================================= */

static void handle_ticket_close(struct discord *client,
                                 const struct discord_interaction *event) {
    Ticket t = {0};
    REQUIRE_TICKET(&t);

    if (t.status == TICKET_STATUS_CLOSED) {
        reply_ephemeral(client, event, "This ticket is already closed.");
        ticket_db_free(&t);
        return;
    }

    u64_snowflake_t actor_id =
        event->member ? event->member->user->id : event->user->id;

    /* Only the opener or staff may close */
    if (actor_id != t.opener_id && !is_staff(event)) {
        reply_ephemeral(client, event,
                        "❌ Only the ticket opener or a staff member can close this ticket.");
        ticket_db_free(&t);
        return;
    }

    ticket_db_update_status(g_db, t.id, TICKET_STATUS_CLOSED);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "🔒 Ticket #%d closed by <@%" PRIu64 ">.", t.id, actor_id);
    reply_public(client, event, msg);

    /* DM the opener on close (unless they closed it themselves) */
    if (actor_id != t.opener_id) {
        char dm_msg[256];
        snprintf(dm_msg, sizeof(dm_msg),
                 "🔒 Your ticket #%d has been closed by staff (outcome: %s).",
                 t.id, ticket_outcome_string(t.outcome));
        send_dm(client, t.opener_id, dm_msg);
    }

    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg),
             "🔒 Ticket #%d closed by <@%" PRIu64 "> (outcome: %s).",
             t.id, actor_id, ticket_outcome_string(t.outcome));
    log_to_guild(t.guild_id, log_msg);

    ticket_db_free(&t);
}

/* ============================================================================
 * /ticketnote handlers
 * ========================================================================= */

static void handle_note_add(struct discord *client,
                             const struct discord_interaction *event,
                             const struct discord_application_command_interaction_data_option *sub) {
    Ticket t = {0};
    REQUIRE_TICKET(&t);

    const struct discord_application_command_interaction_data_option *text_opt =
        find_sub_option(sub, "text");
    if (!text_opt || !text_opt->value) {
        reply_ephemeral(client, event, "❌ You must provide note text.");
        ticket_db_free(&t);
        return;
    }

    u64_snowflake_t author_id = event->member->user->id;
    if (ticket_note_add(g_db, t.id, author_id, text_opt->value) != 0) {
        reply_ephemeral(client, event, "❌ Failed to save note.");
        ticket_db_free(&t);
        return;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "📝 Note added to ticket #%d.", t.id);
    reply_ephemeral(client, event, msg);
    ticket_db_free(&t);
}

static void handle_note_list(struct discord *client,
                              const struct discord_interaction *event) {
    Ticket t = {0};
    REQUIRE_TICKET(&t);

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
    int off = snprintf(buf, sizeof(buf),
                       "📋 **Notes for ticket #%d** (%d total)\n\n",
                       t.id, count);

    /* Show up to 10 notes; truncate with a notice if more exist */
    int show = count > 10 ? 10 : count;
    for (int i = 0; i < show && off < (int)sizeof(buf) - 200; i++) {
        TicketNote *n = &notes[i];
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%s**[#%d]** <@%" PRIu64 "> — <t:%ld:R>\n%s\n\n",
                        n->is_pinned ? "📌 " : "",
                        n->id, n->author_id, n->created_at, n->content);
    }
    if (count > 10)
        snprintf(buf + off, sizeof(buf) - off,
                 "*…and %d more notes not shown.*", count - 10);

    reply_ephemeral(client, event, buf);
    ticket_notes_free(notes, count);
    ticket_db_free(&t);
}

static void handle_note_pin(struct discord *client,
                             const struct discord_interaction *event,
                             const struct discord_application_command_interaction_data_option *sub) {
    const struct discord_application_command_interaction_data_option *id_opt =
        find_sub_option(sub, "note_id");
    if (!id_opt) { reply_ephemeral(client, event, "❌ Provide a note ID."); return; }

    int note_id = atoi(id_opt->value);
    if (ticket_note_set_pinned(g_db, note_id, true) == 0)
        reply_ephemeral(client, event, "📌 Note pinned.");
    else
        reply_ephemeral(client, event, "❌ Note not found or couldn't be pinned.");
}

static void handle_note_delete(struct discord *client,
                                const struct discord_interaction *event,
                                const struct discord_application_command_interaction_data_option *sub) {
    const struct discord_application_command_interaction_data_option *id_opt =
        find_sub_option(sub, "note_id");
    if (!id_opt) { reply_ephemeral(client, event, "❌ Provide a note ID."); return; }

    int note_id = atoi(id_opt->value);
    if (ticket_note_delete(g_db, note_id, event->member->user->id) == 0)
        reply_ephemeral(client, event, "🗑️ Note deleted.");
    else
        reply_ephemeral(client, event,
                        "❌ Note not found, or you can only delete your own notes.");
}

/* ============================================================================
 * /ticketoutcome handlers
 * ========================================================================= */

static void handle_outcome_set(struct discord *client,
                                const struct discord_interaction *event,
                                const struct discord_application_command_interaction_data_option *sub) {
    Ticket t = {0};
    REQUIRE_TICKET(&t);

    const struct discord_application_command_interaction_data_option *outcome_opt =
        find_sub_option(sub, "outcome");
    if (!outcome_opt) {
        reply_ephemeral(client, event, "❌ Provide an outcome value.");
        ticket_db_free(&t);
        return;
    }

    TicketOutcome outcome = outcome_from_string(outcome_opt->value);
    const struct discord_application_command_interaction_data_option *notes_opt =
        find_sub_option(sub, "notes");
    const char *notes = notes_opt ? notes_opt->value : NULL;

    if (ticket_db_set_outcome(g_db, t.id, outcome, notes) != 0) {
        reply_ephemeral(client, event, "❌ Failed to set outcome.");
        ticket_db_free(&t);
        return;
    }

    char msg[512];
    snprintf(msg, sizeof(msg),
             "✅ Outcome for ticket #%d set to **%s**%s%s",
             t.id, ticket_outcome_string(outcome),
             notes ? "\n> " : "", notes ? notes : "");
    reply_ephemeral(client, event, msg);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg),
             "📋 Ticket #%d outcome → **%s** (set by <@%" PRIu64 ">).",
             t.id, ticket_outcome_string(outcome), event->member->user->id);
    log_to_guild(t.guild_id, log_msg);

    ticket_db_free(&t);
}

static void handle_outcome_clear(struct discord *client,
                                  const struct discord_interaction *event) {
    Ticket t = {0};
    REQUIRE_TICKET(&t);
    ticket_db_set_outcome(g_db, t.id, TICKET_OUTCOME_NONE, NULL);
    reply_ephemeral(client, event, "Outcome cleared.");
    ticket_db_free(&t);
}

/* ============================================================================
 * /ticketassign
 * ========================================================================= */

static void handle_assign(struct discord *client,
                           const struct discord_interaction *event) {
    REQUIRE_STAFF();

    Ticket t = {0};
    REQUIRE_TICKET(&t);

    const struct discord_application_command_interaction_data_option *staff_opt =
        find_option(event, "staff_member");
    if (!staff_opt) {
        reply_ephemeral(client, event, "❌ Specify a staff member.");
        ticket_db_free(&t);
        return;
    }

    u64_snowflake_t staff_id =
        (u64_snowflake_t)strtoull(staff_opt->value, NULL, 10);
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
    log_to_guild(t.guild_id, log_msg);

    ticket_db_free(&t);
}

/* ============================================================================
 * /ticketpriority
 * ========================================================================= */

static void handle_priority(struct discord *client,
                             const struct discord_interaction *event) {
    REQUIRE_STAFF();

    Ticket t = {0};
    REQUIRE_TICKET(&t);

    const struct discord_application_command_interaction_data_option *level_opt =
        find_option(event, "level");
    if (!level_opt) {
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

    char msg[128];
    snprintf(msg, sizeof(msg),
             "Priority for ticket #%d set to **%s**.",
             t.id, ticket_priority_string(priority));
    reply_ephemeral(client, event, msg);
    ticket_db_free(&t);
}

/* ============================================================================
 * /ticketstatus
 * ========================================================================= */

static void handle_status(struct discord *client,
                           const struct discord_interaction *event) {
    REQUIRE_STAFF();

    Ticket t = {0};
    REQUIRE_TICKET(&t);

    const struct discord_application_command_interaction_data_option *status_opt =
        find_option(event, "status");
    if (!status_opt) {
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

    char msg[128];
    snprintf(msg, sizeof(msg),
             "Status of ticket #%d updated to **%s**.",
             t.id, ticket_status_string(new_status));
    reply_public(client, event, msg);
    ticket_db_free(&t);
}

/* ============================================================================
 * /ticketsummary
 * ========================================================================= */

static void handle_summary(struct discord *client,
                            const struct discord_interaction *event) {
    REQUIRE_STAFF();

    Ticket t = {0};
    REQUIRE_TICKET(&t);

    TicketNote *notes = NULL;
    int note_count = 0;
    ticket_note_get_all(g_db, t.id, &notes, &note_count);

    char buf[2000];
    int off = snprintf(buf, sizeof(buf),
                       "📋 **Ticket #%d Summary**\n"
                       "**Subject:** %s\n"
                       "**Opened by:** <@%" PRIu64 ">\n"
                       "**Assigned to:** %s\n"
                       "**Status:** %s  **Priority:** %s  **Outcome:** %s\n",
                       t.id,
                       t.subject ? t.subject : "—",
                       t.opener_id,
                       t.assigned_to ? "See below" : "Unassigned",
                       ticket_status_string(t.status),
                       ticket_priority_string(t.priority),
                       ticket_outcome_string(t.outcome));

    if (t.assigned_to)
        off += snprintf(buf + off, sizeof(buf) - off,
                        "**Assignee:** <@%" PRIu64 ">\n", t.assigned_to);

    if (t.outcome_notes && t.outcome_notes[0])
        off += snprintf(buf + off, sizeof(buf) - off,
                        "**Outcome notes:** %s\n", t.outcome_notes);

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
            snprintf(buf + off, sizeof(buf) - off,
                     "*…and %d more. Use `/ticketnote list` to see all.*",
                     note_count - 5);
    } else {
        snprintf(buf + off, sizeof(buf) - off, "\n*No staff notes yet.*");
    }

    reply_ephemeral(client, event, buf);
    ticket_notes_free(notes, note_count);
    ticket_db_free(&t);
}

/* ============================================================================
 * /ticketconfig  (staff only)
 * ========================================================================= */

static bool is_admin(const struct discord_interaction *event) {
    if (!event->member) return false;
    return ((uint64_t)event->member->permissions & 0x0000000000000008ULL) != 0;
}

static void handle_config(struct discord *client,
                           const struct discord_interaction *event) {
    if (!is_admin(event)) {
        reply_ephemeral(client, event,
                        "❌ You need the Administrator permission to configure the ticket system.");
        return;
    }
    reply_ephemeral(client, event,
                    "⚙️ Ticket configuration is not yet implemented.");
}


/* ============================================================================
 * Top-level interaction router
 * ========================================================================= */

void on_ticket_interaction(struct discord *client,
                            const struct discord_interaction *event) {
    if (!event->data) return;

    /*
     * /ticket may arrive from a DM (guild_id == 0, member == NULL).
     * All other commands are guild-only and require event->member.
     */
    const char *cmd = event->data->name;

    if (strcmp(cmd, "ticket") == 0) {
        handle_ticket_open(client, event);
        return;
    }

    /* Everything below is guild-only */
    if (!event->guild_id || !event->member) {
        reply_ephemeral(client, event,
                        "❌ This command can only be used inside a server.");
        return;
    }

    if (strcmp(cmd, "closeticket") == 0) {
        handle_ticket_close(client, event);
        return;
    }

    if (strcmp(cmd, "ticketnote") == 0) {
        REQUIRE_STAFF();
        const struct discord_application_command_interaction_data_option *sub =
            get_subcommand(event);
        if (!sub) { reply_ephemeral(client, event, "❌ Missing sub-command."); return; }

        if (strcmp(sub->name, "add")    == 0) { handle_note_add   (client, event, sub); return; }
        if (strcmp(sub->name, "list")   == 0) { handle_note_list  (client, event);      return; }
        if (strcmp(sub->name, "pin")    == 0) { handle_note_pin   (client, event, sub); return; }
        if (strcmp(sub->name, "delete") == 0) { handle_note_delete(client, event, sub); return; }
        reply_ephemeral(client, event, "❌ Unknown sub-command.");
        return;
    }

    if (strcmp(cmd, "ticketoutcome") == 0) {
        REQUIRE_STAFF();
        const struct discord_application_command_interaction_data_option *sub =
            get_subcommand(event);
        if (!sub) { reply_ephemeral(client, event, "❌ Missing sub-command."); return; }

        if (strcmp(sub->name, "set")   == 0) { handle_outcome_set  (client, event, sub); return; }
        if (strcmp(sub->name, "clear") == 0) { handle_outcome_clear(client, event);      return; }
        reply_ephemeral(client, event, "❌ Unknown sub-command.");
        return;
    }

    if (strcmp(cmd, "ticketassign")   == 0) { handle_assign  (client, event); return; }
    if (strcmp(cmd, "ticketpriority") == 0) { handle_priority(client, event); return; }
    if (strcmp(cmd, "ticketstatus")   == 0) { handle_status  (client, event); return; }
    if (strcmp(cmd, "ticketsummary")  == 0) { handle_summary (client, event); return; }
    if (strcmp(cmd, "ticketconfig")   == 0) { handle_config  (client, event); return; }
}

/* ============================================================================
 * Message handler (reserved for future use – transcripts, inactivity, etc.)
 * ========================================================================= */

void on_ticket_message(struct discord *client,
                       const struct discord_message *event) {
    (void)client;
    (void)event;
}

/* ============================================================================
 * Slash command registration
 * ========================================================================= */

void register_ticket_commands(struct discord *client,
                               u64_snowflake_t application_id,
                               u64_snowflake_t guild_id) {
#define REG(params_ptr, label)                                                    \
    do {                                                                          \
        ORCAcode _c = discord_create_guild_application_command(                   \
            client, application_id, guild_id, (params_ptr), NULL);                \
        printf("[ticket] /%s %s\n", (label),                                      \
               _c == ORCA_OK ? "registered" : "FAILED");                          \
    } while (0)

    /* ── /ticket ──────────────────────────────────────────────────────────── */
    {
        struct discord_application_command_option server_opt = {
            .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name        = "server",
            .description = "ID of the server you want to open a ticket in",
            .required    = true,
        };
        struct discord_application_command_option subject_opt = {
            .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name        = "subject",
            .description = "Brief description of your issue",
            .required    = false,
        };
        struct discord_application_command_option *opts[] = {
            &server_opt, &subject_opt, NULL,
        };
        struct discord_create_guild_application_command_params cmd = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticket",
            .description = "Open a support ticket in a server (works in DMs)",
            .options     = opts,
        };
        REG(&cmd, "ticket");
    }

    /* ── /closeticket ─────────────────────────────────────────────────────── */
    {
        struct discord_create_guild_application_command_params cmd = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "closeticket",
            .description = "Close this ticket",
        };
        REG(&cmd, "closeticket");
    }

    /* ── /ticketnote ──────────────────────────────────────────────────────── */
    {
        struct discord_application_command_option text_opt = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name = "text", .description = "Note content", .required = true,
        };
        struct discord_application_command_option *add_opts[] = { &text_opt, NULL };

        struct discord_application_command_option note_id_opt = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_INTEGER,
            .name = "note_id", .description = "ID of the note", .required = true,
        };
        struct discord_application_command_option *nid_opts[] = { &note_id_opt, NULL };

        struct discord_application_command_option subs[] = {
            { .type = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
              .name = "add",    .description = "Add a private staff note",   .options = add_opts },
            { .type = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
              .name = "list",   .description = "List all notes on this ticket" },
            { .type = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
              .name = "pin",    .description = "Pin a note",                 .options = nid_opts },
            { .type = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
              .name = "delete", .description = "Delete one of your notes",   .options = nid_opts },
        };
        struct discord_application_command_option *sub_ptrs[] = {
            &subs[0], &subs[1], &subs[2], &subs[3], NULL,
        };
        struct discord_create_guild_application_command_params cmd = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticketnote",
            .description = "(Staff) Manage private notes on this ticket",
            .options     = sub_ptrs,
        };
        REG(&cmd, "ticketnote");
    }

    /* ── /ticketoutcome ───────────────────────────────────────────────────── */
    {
        struct discord_application_command_option_choice oc[] = {
            { .name = "Resolved",    .value = "resolved"    },
            { .name = "Duplicate",   .value = "duplicate"   },
            { .name = "Invalid",     .value = "invalid"     },
            { .name = "No Response", .value = "no_response" },
            { .name = "Escalated",   .value = "escalated"   },
            { .name = "Other",       .value = "other"       },
        };
        struct discord_application_command_option_choice *oc_ptrs[] = {
            &oc[0], &oc[1], &oc[2], &oc[3], &oc[4], &oc[5], NULL,
        };
        struct discord_application_command_option outcome_opt = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name = "outcome", .description = "Outcome type",
            .required = true, .choices = oc_ptrs,
        };
        struct discord_application_command_option notes_opt = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name = "notes", .description = "Additional notes (optional)",
            .required = false,
        };
        struct discord_application_command_option *set_opts[] =
            { &outcome_opt, &notes_opt, NULL };

        struct discord_application_command_option set_sub = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
            .name = "set", .description = "Record the ticket outcome",
            .options = set_opts,
        };
        struct discord_application_command_option clear_sub = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
            .name = "clear", .description = "Clear the current outcome",
        };
        struct discord_application_command_option *outcome_opts[] =
            { &set_sub, &clear_sub, NULL };

        struct discord_create_guild_application_command_params cmd = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticketoutcome",
            .description = "(Staff) Set or clear the resolution outcome",
            .options     = outcome_opts,
        };
        REG(&cmd, "ticketoutcome");
    }

    /* ── /ticketassign ────────────────────────────────────────────────────── */
    {
        struct discord_application_command_option staff_opt = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_USER,
            .name = "staff_member", .description = "Staff member to assign",
            .required = true,
        };
        struct discord_application_command_option *opts[] = { &staff_opt, NULL };
        struct discord_create_guild_application_command_params cmd = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticketassign",
            .description = "(Staff) Assign this ticket to a staff member",
            .options     = opts,
        };
        REG(&cmd, "ticketassign");
    }

    /* ── /ticketpriority ──────────────────────────────────────────────────── */
    {
        struct discord_application_command_option_choice pc[] = {
            { .name = "Low",    .value = "low"    },
            { .name = "Medium", .value = "medium" },
            { .name = "High",   .value = "high"   },
            { .name = "Urgent", .value = "urgent" },
        };
        struct discord_application_command_option_choice *pc_ptrs[] =
            { &pc[0], &pc[1], &pc[2], &pc[3], NULL };
        struct discord_application_command_option level_opt = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name = "level", .description = "Priority level",
            .required = true, .choices = pc_ptrs,
        };
        struct discord_application_command_option *opts[] = { &level_opt, NULL };
        struct discord_create_guild_application_command_params cmd = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticketpriority",
            .description = "(Staff) Set the ticket priority",
            .options     = opts,
        };
        REG(&cmd, "ticketpriority");
    }

    /* ── /ticketstatus ────────────────────────────────────────────────────── */
    {
        struct discord_application_command_option_choice sc[] = {
            { .name = "Open",         .value = "open"         },
            { .name = "In Progress",  .value = "in_progress"  },
            { .name = "Pending User", .value = "pending_user" },
            { .name = "Resolved",     .value = "resolved"     },
        };
        struct discord_application_command_option_choice *sc_ptrs[] =
            { &sc[0], &sc[1], &sc[2], &sc[3], NULL };
        struct discord_application_command_option status_opt = {
            .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name = "status", .description = "New status",
            .required = true, .choices = sc_ptrs,
        };
        struct discord_application_command_option *opts[] = { &status_opt, NULL };
        struct discord_create_guild_application_command_params cmd = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticketstatus",
            .description = "(Staff) Update the workflow status",
            .options     = opts,
        };
        REG(&cmd, "ticketstatus");
    }

    /* ── /ticketsummary ───────────────────────────────────────────────────── */
    {
        struct discord_create_guild_application_command_params cmd = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticketsummary",
            .description = "(Staff) View full ticket details: notes, outcome, history",
        };
        REG(&cmd, "ticketsummary");
    }

    /* ── /ticketconfig ────────────────────────────────────────────────────── */
    {
        struct discord_create_guild_application_command_params cmd = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticketconfig",
            .description = "(Admin) Configure the ticket system for this server",
        };
        REG(&cmd, "ticketconfig");
    }

#undef REG
}

/* ============================================================================
 * Module init
 * ========================================================================= */

void ticket_module_init(struct discord *client, Database *db) {
    g_client = client;
    g_db     = db;

    if (ticket_db_init_tables(db) != 0)
        fprintf(stderr, "[ticket] Warning: DB table initialisation failed.\n");

    printf("[ticket] Module initialised.\n");
}