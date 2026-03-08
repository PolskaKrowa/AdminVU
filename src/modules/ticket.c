/*
 * ticket.c  –  Ticket system with staff management commands
 *
 * User commands:
 *   /ticket open <subject>     – open a new support ticket
 *   /ticket close              – close the current ticket
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
 *   /ticketstatus  <status>             – set OPEN / IN_PROGRESS / PENDING_USER
 *   /ticketsummary                      – full summary (notes, outcome, history)
 *   /closeticket                        – alias kept for back-compat
 *   /ticketconfig  …                    – server-level configuration
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

static Database           *g_db      = NULL;
static struct discord     *g_client  = NULL;
static u64_snowflake_t     g_log_channel = 0; /* optional staff log channel   */

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
        "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  channel_id   INTEGER NOT NULL UNIQUE,"
        "  guild_id     INTEGER NOT NULL,"
        "  opener_id    INTEGER NOT NULL,"
        "  assigned_to  INTEGER DEFAULT 0,"
        "  status       INTEGER DEFAULT 0,"
        "  priority     INTEGER DEFAULT 1,"
        "  outcome      INTEGER DEFAULT 0,"
        "  subject      TEXT,"
        "  outcome_notes TEXT,"
        "  created_at   INTEGER DEFAULT (strftime('%s','now')),"
        "  updated_at   INTEGER DEFAULT (strftime('%s','now')),"
        "  closed_at    INTEGER DEFAULT 0"
        ");"

        /* Private staff notes */
        "CREATE TABLE IF NOT EXISTS ticket_notes ("
        "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ticket_id    INTEGER NOT NULL REFERENCES tickets(id) ON DELETE CASCADE,"
        "  author_id    INTEGER NOT NULL,"
        "  content      TEXT NOT NULL,"
        "  is_pinned    INTEGER DEFAULT 0,"
        "  created_at   INTEGER DEFAULT (strftime('%s','now'))"
        ");"

        /* Indexes */
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
 * Database – ticket CRUD
 * ========================================================================= */

int ticket_db_create(Database *db, Ticket *t) {
    const char *sql =
        "INSERT INTO tickets (channel_id, guild_id, opener_id, status, priority, subject) "
        "VALUES (?, ?, ?, ?, ?, ?) RETURNING id;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, t->channel_id);
    sqlite3_bind_int64(stmt, 2, t->guild_id);
    sqlite3_bind_int64(stmt, 3, t->opener_id);
    sqlite3_bind_int  (stmt, 4, t->status);
    sqlite3_bind_int  (stmt, 5, t->priority);
    sqlite3_bind_text (stmt, 6, t->subject ? t->subject : "", -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        t->id = (int)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_ROW) ? 0 : -1;
}

/* Shared row-to-struct helper */
static void row_to_ticket(sqlite3_stmt *stmt, Ticket *t) {
    t->id           = (int)sqlite3_column_int64(stmt, 0);
    t->channel_id   = (u64_snowflake_t)sqlite3_column_int64(stmt, 1);
    t->guild_id     = (u64_snowflake_t)sqlite3_column_int64(stmt, 2);
    t->opener_id    = (u64_snowflake_t)sqlite3_column_int64(stmt, 3);
    t->assigned_to  = (u64_snowflake_t)sqlite3_column_int64(stmt, 4);
    t->status       = (TicketStatus)sqlite3_column_int(stmt, 5);
    t->priority     = (TicketPriority)sqlite3_column_int(stmt, 6);
    t->outcome      = (TicketOutcome)sqlite3_column_int(stmt, 7);
    t->created_at   = (long)sqlite3_column_int64(stmt, 8);
    t->updated_at   = (long)sqlite3_column_int64(stmt, 9);
    t->closed_at    = (long)sqlite3_column_int64(stmt, 10);

    const char *subj = (const char *)sqlite3_column_text(stmt, 11);
    t->subject       = subj ? strdup(subj) : NULL;

    const char *onotes = (const char *)sqlite3_column_text(stmt, 12);
    t->outcome_notes   = onotes ? strdup(onotes) : NULL;
}

static const char *SELECT_TICKET_COLS =
    "SELECT id, channel_id, guild_id, opener_id, assigned_to, "
    "       status, priority, outcome, created_at, updated_at, closed_at, "
    "       subject, outcome_notes ";

int ticket_db_get_by_channel(Database *db, u64_snowflake_t channel_id, Ticket *out) {
    char sql[512];
    snprintf(sql, sizeof(sql), "%s FROM tickets WHERE channel_id = ?;",
             SELECT_TICKET_COLS);

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, channel_id);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        row_to_ticket(stmt, out);
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
    if (rc == SQLITE_ROW)
        row_to_ticket(stmt, out);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_ROW) ? 0 : -1;
}

int ticket_db_update_status(Database *db, int ticket_id, TicketStatus status) {
    const char *sql =
        "UPDATE tickets SET status = ?, updated_at = strftime('%s','now') "
        "WHERE id = ?;";

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
        "UPDATE tickets SET priority = ?, updated_at = strftime('%s','now') "
        "WHERE id = ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int(stmt, 1, priority);
    sqlite3_bind_int(stmt, 2, ticket_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int ticket_db_update_assigned(Database *db, int ticket_id,
                               u64_snowflake_t staff_id) {
    const char *sql =
        "UPDATE tickets SET assigned_to = ?, updated_at = strftime('%s','now') "
        "WHERE id = ?;";

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

    sqlite3_bind_int  (stmt, 1, outcome);
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

    /* Two-pass: count then allocate */
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
        TicketNote *note  = &arr[n++];
        note->id          = (int)sqlite3_column_int64(stmt, 0);
        note->ticket_id   = (int)sqlite3_column_int64(stmt, 1);
        note->author_id   = (u64_snowflake_t)sqlite3_column_int64(stmt, 2);
        const char *txt   = (const char *)sqlite3_column_text(stmt, 3);
        note->content     = txt ? strdup(txt) : strdup("");
        note->is_pinned   = (bool)sqlite3_column_int(stmt, 4);
        note->created_at  = (long)sqlite3_column_int64(stmt, 5);
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
     * Only allow deletion if the requesting staff member is the note author.
     * Callers with elevated permissions should pass 0 to bypass this check.
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
    /* If no rows changed the note either didn't exist or belongs to someone else */
    return (rc == SQLITE_DONE && sqlite3_changes(db->db) > 0) ? 0 : -1;
}

void ticket_notes_free(TicketNote *notes, int count) {
    if (!notes) return;
    for (int i = 0; i < count; i++)
        free(notes[i].content);
    free(notes);
}

/* ============================================================================
 * Permission helper
 * ============================================================================
 * A real implementation would check msg->member->permissions against
 * DISCORD_PERM_MANAGE_MESSAGES.  Here we expose a small stub that can be
 * replaced with a proper role-based check once the guild config is in place.
 * ========================================================================= */

static bool is_staff(const struct discord_interaction *event) {
    if (!event->member) return false;
    /*
     * MANAGE_MESSAGES bit = 0x0000000000002000
     * Cast to uint64 to avoid sign issues.
     */
    uint64_t perms = (uint64_t)event->member->permissions;
    return (perms & 0x0000000000002000ULL) != 0;
}

/* ============================================================================
 * Discord helpers
 * ========================================================================= */

/*
 * reply_ephemeral / reply_public
 *
 * Interaction responses use discord_create_interaction_response(), which
 * requires discord_interaction_response / discord_interaction_callback_data
 * structs.  If those are also incomplete types in your build, swap the body
 * for the send_to_channel fallback shown in the comment.
 *
 * Preferred (interaction response — shows "Bot is thinking…" correctly):
 *   struct discord_interaction_response resp = { ... };
 *   discord_create_interaction_response(client, event->id, event->token, &resp, NULL);
 *
 * Fallback (plain channel message — always works):
 *   send_to_channel(client, event->channel_id, message);
 */
static void reply_ephemeral(struct discord *client,
                             const struct discord_interaction *event,
                             const char *message) {
    /*
     * Try the interaction response first.  If this fails to compile due to
     * incomplete struct types, replace the entire body with:
     *   send_to_channel(client, event->channel_id, message);
     */
    struct discord_interaction_response resp;
    memset(&resp, 0, sizeof(resp));
    resp.type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE;

    struct discord_interaction_callback_data data;
    memset(&data, 0, sizeof(data));
    data.content = (char *)message;
    data.flags   = 64; /* EPHEMERAL – Discord API flag 0x40 */

    resp.data = &data;
    discord_create_interaction_response(client, event->id,
                                        event->token, &resp, NULL);
}

static void reply_public(struct discord *client,
                          const struct discord_interaction *event,
                          const char *message) {
    /*
     * Same note as reply_ephemeral: replace with
     *   send_to_channel(client, event->channel_id, message);
     * if the structs are incomplete on your build.
     */
    struct discord_interaction_response resp;
    memset(&resp, 0, sizeof(resp));
    resp.type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE;

    struct discord_interaction_callback_data data;
    memset(&data, 0, sizeof(data));
    data.content = (char *)message;

    resp.data = &data;
    discord_create_interaction_response(client, event->id,
                                        event->token, &resp, NULL);
}

/*
 * get_active_subcommand
 *
 * When a slash command uses sub-commands, Discord sends exactly one top-level
 * option: the chosen sub-command.  It is always at options[0].
 *
 * Using a NULL-terminated loop is unreliable because some Orca builds don't
 * NULL-terminate the options array.  Direct indexing is always safe.
 */
static const struct discord_application_command_interaction_data_option *
get_active_subcommand(const struct discord_interaction *event) {
    if (!event->data || !event->data->options) return NULL;
    return event->data->options[0];
}

/* Name-based lookup capped at 25 (Discord's hard max), never relies on NULL sentinel */
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

/* Same but for options one level deeper (inside a sub-command) */
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

/* Post a message to the staff log channel (fire-and-forget) */
/*
 * send_to_channel – single chokepoint for all outbound messages.
 *
 * This is intentionally stubbed out until the correct API call for your Orca
 * installation is confirmed.  To find it, run:
 *
 *   nm -D /usr/local/lib/libdiscord.so | grep -i "message\|send\|channel"
 *
 * Then replace the body below with whichever of these matches:
 *
 *   Option A – struct init (most common):
 *     struct discord_create_message params;
 *     memset(&params, 0, sizeof(params));
 *     params.content = (char *)content;
 *     discord_create_message(client, channel_id, &params, NULL);
 *
 *   Option B – simple function:
 *     discord_send_message(client, channel_id, content);
 *
 *   Option C – REST style:
 *     discord_rest_run(client, NULL, NULL, discord_create_message,
 *                      channel_id, &params);
 *
 * Match whichever pattern already works in your ping module.
 */
static void send_to_channel(struct discord *client,
                             u64_snowflake_t channel_id,
                             const char *content) {
    /* TODO: replace with the correct API call for your Orca version */
    printf("[ticket] send_to_channel (ch=%" PRIu64 "): %s\n",
           channel_id, content);
    (void)client; /* suppress unused-parameter warning until implemented */
}

static void log_to_staff_channel(const char *message) {
    if (!g_client || !g_log_channel) return;
    send_to_channel(g_client, g_log_channel, message);
}

/* ============================================================================
 * Command handlers – /ticket (user-facing)
 * ========================================================================= */

static void handle_ticket_open(struct discord *client,
                                const struct discord_interaction *event) {
    /*
     * We were already dispatched here because options[0]->name == "open".
     * The subject is options[0]->options[0].  Use get_active_subcommand()
     * rather than calling find_option() again (which was the original bug).
     */
    const struct discord_application_command_interaction_data_option *sub =
        get_active_subcommand(event);
    if (!sub) { reply_ephemeral(client, event, "Missing sub-command data."); return; }

    const struct discord_application_command_interaction_data_option *subject_opt =
        find_sub_option(sub, "subject");
    const char *subject = subject_opt ? subject_opt->value : "No subject provided";

    /* TODO: create a private channel for the ticket, set permissions, etc. */

    Ticket t = {
        .channel_id = event->channel_id,   /* Replace with newly created channel */
        .guild_id   = event->guild_id,
        .opener_id  = event->member->user->id,
        .status     = TICKET_STATUS_OPEN,
        .priority   = TICKET_PRIORITY_MEDIUM,
        .subject    = (char *)subject,
    };

    if (ticket_db_create(g_db, &t) != 0) {
        reply_ephemeral(client, event,
                        "❌ Failed to create ticket – please try again.");
        return;
    }

    char msg[512];
    snprintf(msg, sizeof(msg),
             "🎫 **Ticket #%d opened**\n"
             "**Subject:** %s\n"
             "Staff will be with you shortly. Use `/ticket close` to close this ticket.",
             t.id, subject);
    reply_public(client, event, msg);

    char log_msg[512];
    snprintf(log_msg, sizeof(log_msg),
             "🎫 Ticket #%d opened by <@%" PRIu64 "> – *%s*",
             t.id, t.opener_id, subject);
    log_to_staff_channel(log_msg);
}

static void handle_ticket_close(struct discord *client,
                                 const struct discord_interaction *event) {
    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) {
        reply_ephemeral(client, event,
                        "❌ This channel doesn't appear to be a ticket.");
        return;
    }

    if (t.status == TICKET_STATUS_CLOSED) {
        reply_ephemeral(client, event, "This ticket is already closed.");
        ticket_db_free(&t);
        return;
    }

    ticket_db_update_status(g_db, t.id, TICKET_STATUS_CLOSED);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "🔒 Ticket #%d closed by <@%" PRIu64 ">.",
             t.id, event->member->user->id);
    reply_public(client, event, msg);

    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg),
             "🔒 Ticket #%d closed by <@%" PRIu64 "> (outcome: %s).",
             t.id, event->member->user->id, ticket_outcome_string(t.outcome));
    log_to_staff_channel(log_msg);

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

    if (ticket_note_add(g_db, t.id, event->member->user->id,
                        text_opt->value) != 0) {
        reply_ephemeral(client, event, "❌ Failed to save note.");
        ticket_db_free(&t);
        return;
    }

    char msg[512];
    snprintf(msg, sizeof(msg),
             "📝 Note added to ticket #%d.", t.id);
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

    /* Build response — Discord messages cap at 2000 chars */
    char buf[2000];
    int offset = snprintf(buf, sizeof(buf),
                          "📋 **Notes for ticket #%d** (%d total)\n\n",
                          t.id, count);

    for (int i = 0; i < count && offset < (int)sizeof(buf) - 200; i++) {
        TicketNote *n = &notes[i];
        offset += snprintf(buf + offset, sizeof(buf) - offset,
                           "%s**[#%d]** <@%" PRIu64 "> — <t:%ld:R>\n%s\n\n",
                           n->is_pinned ? "📌 " : "",
                           n->id,
                           n->author_id,
                           n->created_at,
                           n->content);
    }

    if (count > 10) {
        offset += snprintf(buf + offset, sizeof(buf) - offset,
                           "*…and %d more notes not shown.*", count - 10);
    }

    reply_ephemeral(client, event, buf);
    ticket_notes_free(notes, count);
    ticket_db_free(&t);
}

static void handle_note_pin(struct discord *client,
                             const struct discord_interaction *event,
                             const struct discord_application_command_interaction_data_option *subcmd) {
    const struct discord_application_command_interaction_data_option *id_opt =
        find_sub_option(subcmd, "note_id");
    if (!id_opt) { reply_ephemeral(client, event, "❌ Provide a note ID."); return; }

    int note_id = atoi(id_opt->value);
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
    if (!id_opt) { reply_ephemeral(client, event, "❌ Provide a note ID."); return; }

    int note_id = atoi(id_opt->value);
    /* Pass 0 here to allow any staff to delete; pass author ID to restrict */
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

    if (!outcome_opt) {
        reply_ephemeral(client, event, "❌ Provide an outcome value.");
        ticket_db_free(&t);
        return;
    }

    TicketOutcome outcome = outcome_from_string(outcome_opt->value);
    const char *notes = notes_opt ? notes_opt->value : NULL;

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
    log_to_staff_channel(log_msg);

    ticket_db_free(&t);
}

static void handle_outcome_clear(struct discord *client,
                                  const struct discord_interaction *event) {
    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) {
        reply_ephemeral(client, event, "❌ This channel isn't a ticket.");
        return;
    }

    ticket_db_set_outcome(g_db, t.id, TICKET_OUTCOME_NONE, NULL);
    reply_ephemeral(client, event, "Outcome cleared.");
    ticket_db_free(&t);
}

/* ============================================================================
 * Command handler – /ticketassign (staff)
 * ========================================================================= */

static void handle_assign(struct discord *client,
                           const struct discord_interaction *event) {
    if (!is_staff(event)) {
        reply_ephemeral(client, event, "❌ You don't have permission to do that.");
        return;
    }

    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) {
        reply_ephemeral(client, event, "❌ This channel isn't a ticket.");
        return;
    }

    const struct discord_application_command_interaction_data_option *staff_opt =
        find_option(event, "staff_member");
    if (!staff_opt) {
        reply_ephemeral(client, event, "❌ Specify a staff member.");
        ticket_db_free(&t);
        return;
    }

    u64_snowflake_t staff_id = (u64_snowflake_t)strtoull(staff_opt->value, NULL, 10);
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
    log_to_staff_channel(log_msg);

    ticket_db_free(&t);
}

/* ============================================================================
 * Command handler – /ticketpriority (staff)
 * ========================================================================= */

static void handle_priority(struct discord *client,
                             const struct discord_interaction *event) {
    if (!is_staff(event)) {
        reply_ephemeral(client, event, "❌ You don't have permission to do that.");
        return;
    }

    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) {
        reply_ephemeral(client, event, "❌ This channel isn't a ticket.");
        return;
    }

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

    char msg[256];
    snprintf(msg, sizeof(msg),
             "Priority for ticket #%d set to **%s**.",
             t.id, ticket_priority_string(priority));
    reply_ephemeral(client, event, msg);

    ticket_db_free(&t);
}

/* ============================================================================
 * Command handler – /ticketstatus (staff)
 * ========================================================================= */

static void handle_status(struct discord *client,
                           const struct discord_interaction *event) {
    if (!is_staff(event)) {
        reply_ephemeral(client, event, "❌ You don't have permission to do that.");
        return;
    }

    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) {
        reply_ephemeral(client, event, "❌ This channel isn't a ticket.");
        return;
    }

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

    char msg[256];
    snprintf(msg, sizeof(msg),
             "Status of ticket #%d updated to **%s**.",
             t.id, ticket_status_string(new_status));
    reply_public(client, event, msg);

    ticket_db_free(&t);
}

/* ============================================================================
 * Command handler – /ticketsummary (staff)
 * ========================================================================= */

static void handle_summary(struct discord *client,
                            const struct discord_interaction *event) {
    if (!is_staff(event)) {
        reply_ephemeral(client, event, "❌ You don't have permission to do that.");
        return;
    }

    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) {
        reply_ephemeral(client, event, "❌ This channel isn't a ticket.");
        return;
    }

    TicketNote *notes = NULL;
    int note_count = 0;
    ticket_note_get_all(g_db, t.id, &notes, &note_count);

    char buf[2000];
    int off = 0;

    off += snprintf(buf + off, sizeof(buf) - off,
                    "📋 **Ticket #%d Summary**\n"
                    "**Subject:** %s\n"
                    "**Opened by:** <@%" PRIu64 ">\n"
                    "**Assigned to:** %s\n"
                    "**Status:** %s\n"
                    "**Priority:** %s\n"
                    "**Outcome:** %s\n",
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
 * Top-level interaction router
 * ========================================================================= */

void on_ticket_interaction(struct discord *client,
                            const struct discord_interaction *event) {
    if (!event->data) return;
    const char *cmd = event->data->name;

<<<<<<< HEAD
    /* ── /ticket ─────────────────────────────────────────────────────────── */
    if (strcmp(cmd, "ticket") == 0 || strcmp(cmd, "closeticket") == 0) {
        const struct discord_application_command_interaction_data_option *sub =
            get_active_subcommand(event);
        /*
         * Check the sub-command name directly.  Previously this used
         * find_option(event, "open") which returned NULL in Orca builds that
         * don't NULL-terminate options arrays, causing every /ticket invocation
         * to fall through to handle_ticket_close.
         */
        if (sub && strcmp(sub->name, "open") == 0) {
            handle_ticket_open(client, event);
        } else {
            handle_ticket_close(client, event);
        }
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
        handle_assign(client, event);
        return;
    }

    /* ── /ticketpriority ─────────────────────────────────────────────────── */
    if (strcmp(cmd, "ticketpriority") == 0) {
        handle_priority(client, event);
        return;
    }

    /* ── /ticketstatus ───────────────────────────────────────────────────── */
    if (strcmp(cmd, "ticketstatus") == 0) {
        handle_status(client, event);
        return;
    }

    /* ── /ticketsummary ──────────────────────────────────────────────────── */
    if (strcmp(cmd, "ticketsummary") == 0) {
        handle_summary(client, event);
        return;
    }

    /* ── /ticketconfig ───────────────────────────────────────────────────── */
    if (strcmp(cmd, "ticketconfig") == 0) {
        if (!is_staff(event)) {
            reply_ephemeral(client, event, "❌ Staff only.");
            return;
        }
        reply_ephemeral(client, event,
                        "⚙️ Ticket configuration is not yet implemented.");
        return;
    }
=======
    /* Diagnostic: always log what Discord actually sent */
    printf("[ticket] on_ticket_interaction: cmd='%s' guild_id=%" PRIu64 "\n",
           cmd ? cmd : "(null)", event->guild_id);

    if (!cmd) return;

    if      (strcmp(cmd, "ticket")       == 0) handle_ticket_create(client, event);
    else if (strcmp(cmd, "closeticket")  == 0) handle_ticket_close (client, event);
    else if (strcmp(cmd, "ticketconfig") == 0) handle_config_set   (client, event);
>>>>>>> 53ee0d3d8206cbe3d74e104cb810220a8825651b
}

/* ============================================================================
 * Message handler – log ticket channel messages to the DB (future use)
 * ========================================================================= */

void on_ticket_message(struct discord *client,
                       const struct discord_message *event) {
    /* Optionally used in future to transcript messages, detect inactivity, etc. */
    (void)client;
    (void)event;
}

/* ============================================================================
 * Slash command registration
 * ========================================================================= */

void register_ticket_commands(struct discord *client,
                               u64_snowflake_t application_id,
                               u64_snowflake_t guild_id) {
    /* Helper macro to register one command and log the result */
#define REG(params_ptr, label)                                                  \
    do {                                                                        \
        ORCAcode _c = discord_create_guild_application_command(                 \
            client, application_id, guild_id, (params_ptr), NULL);              \
        if (_c == ORCA_OK)                                                      \
            printf("[ticket] /%s registered\n", (label));                       \
        else                                                                    \
            printf("[ticket] Failed to register /%s (code %d)\n", (label), _c);\
    } while (0)

    /* ── /ticket ──────────────────────────────────────────────────────────── */
    {
        struct discord_application_command_option open_subject = {
            .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name        = "subject",
            .description = "Brief description of your issue",
            .required    = true,
        };
        struct discord_application_command_option *open_opts[] = { &open_subject, NULL };

        struct discord_application_command_option open_sub = {
            .type        = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
            .name        = "open",
            .description = "Open a new support ticket",
            .options     = open_opts,
        };
        struct discord_application_command_option close_sub = {
            .type        = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
            .name        = "close",
            .description = "Close this ticket",
        };
        struct discord_application_command_option *ticket_opts[] = {
            &open_sub, &close_sub, NULL,
        };
        struct discord_create_guild_application_command_params ticket_cmd = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticket",
            .description = "Manage support tickets",
            .options     = ticket_opts,
        };
        REG(&ticket_cmd, "ticket");
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
            .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name        = "outcome",
            .description = "Outcome type",
            .required    = true,
            .choices     = oc_ptrs,
        };
        struct discord_application_command_option set_notes_opt = {
            .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name        = "notes",
            .description = "Additional notes (optional)",
            .required    = false,
        };
        struct discord_application_command_option *set_opts[] = {
            &set_outcome_opt, &set_notes_opt, NULL,
        };
        struct discord_application_command_option set_sub = {
            .type        = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
            .name        = "set",
            .description = "Record the ticket outcome",
            .options     = set_opts,
        };
        struct discord_application_command_option clear_sub = {
            .type        = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
            .name        = "clear",
            .description = "Clear the current outcome",
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
            .type        = DISCORD_APPLICATION_COMMAND_OPTION_USER,
            .name        = "staff_member",
            .description = "Staff member to assign",
            .required    = true,
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
            .type     = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name     = "level",
            .description = "Priority level",
            .required = true,
            .choices  = pri_ptrs,
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
            { .name = "Open",          .value = "open"         },
            { .name = "In Progress",   .value = "in_progress"  },
            { .name = "Pending User",  .value = "pending_user" },
            { .name = "Resolved",      .value = "resolved"     },
            { 0 },
        };
        struct discord_application_command_option_choice *st_ptrs[] = {
            &status_choices[0], &status_choices[1],
            &status_choices[2], &status_choices[3], NULL,
        };
        struct discord_application_command_option status_opt = {
            .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name        = "status",
            .description = "New status",
            .required    = true,
            .choices     = st_ptrs,
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