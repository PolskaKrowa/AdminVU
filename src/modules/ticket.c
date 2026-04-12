/*
 * ticket.c  –  Full implementation of the ticket module
 *
 * Architecture overview
 * ─────────────────────
 *  • Users open a ticket via /ticket open server:<id> subject:<text>.
 *
 *  • The bot creates a private text channel inside the ticket category
 *    that lives in the configured *staff* server.
 *
 *  • The bot opens a DM with the opener and stores the mapping:
 *        user_id ↔ dm_channel_id ↔ ticket_channel_id
 *
 *  • Message relay (both directions):
 *      DM from user  → always forwarded to ticket channel as "username: …"
 *                       (user gets ⏳ reaction if unclaimed, 📨 if claimed)
 *      Staff message → gated: only the claimant may relay to the user DM.
 *                       Non-claimants and unclaimed tickets are rejected at
 *                       the bot level; Discord's channel permission overwrite
 *                       (DENY SEND_MESSAGES on @everyone, set at open time,
 *                       per-member ALLOW added at claim time) enforces the
 *                       same rule at the Discord level.
 *
 *  • Claim transfer: /ticket claim on an already-claimed ticket revokes the
 *    previous claimant's SEND_MESSAGES overwrite and grants it to the new one.
 *
 *  • Mention suppression: every forwarded string is run through
 *    strip_mentions() before posting, so @everyone, @here, <@id> and
 *    <@&id> never fire in either direction.
 *
 *  • Staff commands (/ticket claim|close|priority|status|assign) only
 *    work inside a ticket channel.  /ticketconfig requires ADMINISTRATOR.
 *
 * Required bot permissions in the staff guild
 * ────────────────────────────────────────────
 *   ADMINISTRATOR    – recommended; bypasses channel overwrites so the bot
 *                      can always post in locked ticket channels
 *   MANAGE_CHANNELS  – create / rename ticket channels
 *   MANAGE_ROLES     – edit channel permission overwrites (claim/unclaim)
 *   MANAGE_MESSAGES  – delete the original staff message before re-posting
 *   SEND_MESSAGES    – post in ticket channels + DMs
 *   VIEW_CHANNEL     – obvious
 */

#include "ticket.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

#include <sqlite3.h>
#include <orca/discord.h>
#include "http_server.h"   /* http_sse_push */

/* ═══════════════════════════════════════════════════════════════════════════
 * Module-level state
 * ═══════════════════════════════════════════════════════════════════════════ */

static struct discord *g_client = NULL;
static Database       *g_db     = NULL;

/* ═══════════════════════════════════════════════════════════════════════════
 * String helpers (public – declared in ticket.h)
 * ═══════════════════════════════════════════════════════════════════════════ */

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
        case TICKET_PRIORITY_LOW:    return "Low";
        case TICKET_PRIORITY_MEDIUM: return "Medium";
        case TICKET_PRIORITY_HIGH:   return "High";
        case TICKET_PRIORITY_URGENT: return "Urgent";
        default:                     return "Unknown";
    }
}

const char *ticket_outcome_string(TicketOutcome o) {
    switch (o) {
        case TICKET_OUTCOME_NONE:        return "None";
        case TICKET_OUTCOME_RESOLVED:    return "Resolved";
        case TICKET_OUTCOME_DUPLICATE:   return "Duplicate";
        case TICKET_OUTCOME_INVALID:     return "Invalid";
        case TICKET_OUTCOME_NO_RESPONSE: return "No Response";
        case TICKET_OUTCOME_ESCALATED:   return "Escalated";
        case TICKET_OUTCOME_OTHER:       return "Other";
        default:                         return "Unknown";
    }
}

static const char *priority_emoji(TicketPriority p) {
    switch (p) {
        case TICKET_PRIORITY_LOW:    return "🟢";
        case TICKET_PRIORITY_MEDIUM: return "🟡";
        case TICKET_PRIORITY_HIGH:   return "🟠";
        case TICKET_PRIORITY_URGENT: return "🔴";
        default:                     return "⚪";
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Database – table initialisation
 * ═══════════════════════════════════════════════════════════════════════════ */

int ticket_db_init_tables(Database *db) {
    const char *sql =
        "CREATE TABLE IF NOT EXISTS tickets ("
        "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  channel_id    INTEGER NOT NULL UNIQUE,"
        "  guild_id      INTEGER NOT NULL,"
        "  opener_id     INTEGER NOT NULL,"
        "  assigned_to   INTEGER NOT NULL DEFAULT 0,"
        "  status        INTEGER NOT NULL DEFAULT 0,"
        "  priority      INTEGER NOT NULL DEFAULT 1,"
        "  outcome       INTEGER NOT NULL DEFAULT 0,"
        "  created_at    INTEGER NOT NULL,"
        "  updated_at    INTEGER NOT NULL,"
        "  closed_at     INTEGER NOT NULL DEFAULT 0,"
        "  subject       TEXT    NOT NULL,"
        "  outcome_notes TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS ticket_notes ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ticket_id  INTEGER NOT NULL REFERENCES tickets(id),"
        "  author_id  INTEGER NOT NULL,"
        "  content    TEXT    NOT NULL,"
        "  is_pinned  INTEGER NOT NULL DEFAULT 0,"
        "  created_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS ticket_config ("
        "  id                 INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  guild_id           INTEGER NOT NULL UNIQUE,"
        "  ticket_category_id TEXT,"
        "  log_channel_id     INTEGER NOT NULL DEFAULT 0,"
        "  main_server_id     TEXT,"
        "  staff_server_id    TEXT"
        ");"
        /*
         * Maps a user's DM channel back to their active ticket channel.
         * Keyed on (user_id, ticket_id) so a user with multiple tickets
         * is handled correctly.
         */
        "CREATE TABLE IF NOT EXISTS ticket_dm_map ("
        "  user_id           INTEGER NOT NULL,"
        "  dm_channel_id     INTEGER NOT NULL,"
        "  ticket_channel_id INTEGER NOT NULL,"
        "  ticket_id         INTEGER NOT NULL,"
        "  PRIMARY KEY (user_id, ticket_id)"
        ");"
        /*
         * Tracks every staff-side message so staff can edit/delete by index.
         * msg_index is a per-ticket sequential counter starting at 1.
         * dm_message_id is the ID of the copy posted in the user's DM so
         * edits and deletes can be applied to the DM message directly.
         * author_name is stored so the [#N] header survives edits made by
         * a different staff member than the original author.
         */
        "CREATE TABLE IF NOT EXISTS ticket_staff_messages ("
        "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ticket_id     INTEGER NOT NULL REFERENCES tickets(id),"
        "  msg_index     INTEGER NOT NULL,"
        "  message_id    INTEGER NOT NULL,"
        "  dm_message_id INTEGER NOT NULL DEFAULT 0,"
        "  content       TEXT    NOT NULL,"
        "  author_id     INTEGER NOT NULL,"
        "  author_name   TEXT    NOT NULL DEFAULT '',"
        "  created_at    INTEGER NOT NULL,"
        "  UNIQUE(ticket_id, msg_index)"
        ");"
        /*
         * Append-only audit log of staff message edits and deletions,
         * surfaced in the dashboard ticket log HTML via SSE.
         */
        "CREATE TABLE IF NOT EXISTS ticket_log_events ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ticket_id   INTEGER NOT NULL,"
        "  event_type  TEXT    NOT NULL,"   /* 'edit' | 'delete' */
        "  msg_index   INTEGER NOT NULL,"
        "  author_name TEXT    NOT NULL DEFAULT '',"
        "  old_content TEXT,"
        "  new_content TEXT,"
        "  occurred_at INTEGER NOT NULL"
        ");";

    char *errmsg = NULL;
    int rc = sqlite3_exec(db->db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[ticket] DB init error: %s\n", errmsg);
        sqlite3_free(errmsg);
        return -1;
    }
    /* Migrations for installations that pre-date the new columns.
     * sqlite3_exec returns an error if the column already exists; ignore it. */
    sqlite3_exec(db->db,
        "ALTER TABLE ticket_staff_messages"
        " ADD COLUMN dm_message_id INTEGER NOT NULL DEFAULT 0;",
        NULL, NULL, NULL);
    sqlite3_exec(db->db,
        "ALTER TABLE ticket_staff_messages"
        " ADD COLUMN author_name TEXT NOT NULL DEFAULT '';",
        NULL, NULL, NULL);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Database – tickets
 * ═══════════════════════════════════════════════════════════════════════════ */

int ticket_db_create(Database *db, Ticket *t) {
    const char *sql =
        "INSERT INTO tickets"
        " (channel_id,guild_id,opener_id,assigned_to,status,priority,"
        "  outcome,created_at,updated_at,closed_at,subject,outcome_notes)"
        " VALUES (?,?,?,0,?,?,?,?,?,0,?,NULL);";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_int64 now = (sqlite3_int64)time(NULL);
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)t->channel_id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)t->guild_id);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)t->opener_id);
    sqlite3_bind_int  (stmt, 4, t->status);
    sqlite3_bind_int  (stmt, 5, t->priority);
    sqlite3_bind_int  (stmt, 6, t->outcome);
    sqlite3_bind_int64(stmt, 7, now);
    sqlite3_bind_int64(stmt, 8, now);
    sqlite3_bind_text (stmt, 9, t->subject, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;

    t->id         = (int)sqlite3_last_insert_rowid(db->db);
    t->created_at = (long)now;
    t->updated_at = (long)now;
    return 0;
}

/* Column order must match the SELECT in the get_by_* helpers below. */
static void ticket_from_stmt(sqlite3_stmt *stmt, Ticket *t) {
    t->id          = sqlite3_column_int   (stmt,  0);
    t->channel_id  = (u64_snowflake_t)sqlite3_column_int64(stmt, 1);
    t->guild_id    = (u64_snowflake_t)sqlite3_column_int64(stmt, 2);
    t->opener_id   = (u64_snowflake_t)sqlite3_column_int64(stmt, 3);
    t->assigned_to = (u64_snowflake_t)sqlite3_column_int64(stmt, 4);
    t->status      = sqlite3_column_int   (stmt,  5);
    t->priority    = sqlite3_column_int   (stmt,  6);
    t->outcome     = sqlite3_column_int   (stmt,  7);
    t->created_at  = (long)sqlite3_column_int64  (stmt,  8);
    t->updated_at  = (long)sqlite3_column_int64  (stmt,  9);
    t->closed_at   = (long)sqlite3_column_int64  (stmt, 10);
    const char *s  = (const char *)sqlite3_column_text(stmt, 11);
    t->subject       = s ? strdup(s) : NULL;
    const char *n  = (const char *)sqlite3_column_text(stmt, 12);
    t->outcome_notes = n ? strdup(n) : NULL;
}

#define TICKET_SELECT \
    "SELECT id,channel_id,guild_id,opener_id,assigned_to,status,priority," \
    "outcome,created_at,updated_at,closed_at,subject,outcome_notes FROM tickets"

int ticket_db_get_by_channel(Database *db, u64_snowflake_t channel_id, Ticket *out) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db,
            TICKET_SELECT " WHERE channel_id=? LIMIT 1;",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)channel_id);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) { ticket_from_stmt(stmt, out); sqlite3_finalize(stmt); return 0; }
    sqlite3_finalize(stmt);
    return -1;
}

int ticket_db_get_by_id(Database *db, int ticket_id, Ticket *out) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db,
            TICKET_SELECT " WHERE id=? LIMIT 1;",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, ticket_id);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) { ticket_from_stmt(stmt, out); sqlite3_finalize(stmt); return 0; }
    sqlite3_finalize(stmt);
    return -1;
}

int ticket_db_update_status(Database *db, int ticket_id, TicketStatus status) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db,
            "UPDATE tickets SET status=?,updated_at=? WHERE id=?;",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int  (stmt, 1, status);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)time(NULL));
    sqlite3_bind_int  (stmt, 3, ticket_id);
    int rc = sqlite3_step(stmt); sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int ticket_db_update_priority(Database *db, int ticket_id, TicketPriority priority) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db,
            "UPDATE tickets SET priority=?,updated_at=? WHERE id=?;",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int  (stmt, 1, priority);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)time(NULL));
    sqlite3_bind_int  (stmt, 3, ticket_id);
    int rc = sqlite3_step(stmt); sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int ticket_db_update_assigned(Database *db, int ticket_id, u64_snowflake_t staff_id) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db,
            "UPDATE tickets SET assigned_to=?,updated_at=? WHERE id=?;",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)staff_id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)time(NULL));
    sqlite3_bind_int  (stmt, 3, ticket_id);
    int rc = sqlite3_step(stmt); sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int ticket_db_set_outcome(Database *db, int ticket_id,
                          TicketOutcome outcome, const char *notes) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db,
            "UPDATE tickets SET outcome=?,outcome_notes=?,status=?,"
            "closed_at=?,updated_at=? WHERE id=?;",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_int64 now = (sqlite3_int64)time(NULL);
    sqlite3_bind_int  (stmt, 1, outcome);
    if (notes) sqlite3_bind_text(stmt, 2, notes, -1, SQLITE_STATIC);
    else        sqlite3_bind_null(stmt, 2);
    sqlite3_bind_int  (stmt, 3, TICKET_STATUS_CLOSED);
    sqlite3_bind_int64(stmt, 4, now);
    sqlite3_bind_int64(stmt, 5, now);
    sqlite3_bind_int  (stmt, 6, ticket_id);
    int rc = sqlite3_step(stmt); sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

void ticket_db_free(Ticket *t) {
    if (!t) return;
    free(t->subject);       t->subject       = NULL;
    free(t->outcome_notes); t->outcome_notes = NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Database – notes
 * ═══════════════════════════════════════════════════════════════════════════ */

int ticket_note_add(Database *db, int ticket_id,
                    u64_snowflake_t author_id, const char *content) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db,
            "INSERT INTO ticket_notes (ticket_id,author_id,content,is_pinned,created_at)"
            " VALUES (?,?,?,0,?);",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int  (stmt, 1, ticket_id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)author_id);
    sqlite3_bind_text (stmt, 3, content, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)time(NULL));
    int rc = sqlite3_step(stmt); sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int ticket_note_get_all(Database *db, int ticket_id,
                        TicketNote **notes_out, int *count_out) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db,
            "SELECT id,ticket_id,author_id,content,is_pinned,created_at"
            " FROM ticket_notes WHERE ticket_id=?"
            " ORDER BY is_pinned DESC, created_at ASC;",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, ticket_id);

    TicketNote *arr = NULL;
    int count = 0, cap = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap = cap ? cap * 2 : 8;
            TicketNote *tmp = realloc(arr, (size_t)cap * sizeof *tmp);
            if (!tmp) { free(arr); sqlite3_finalize(stmt); return -1; }
            arr = tmp;
        }
        TicketNote *n = &arr[count++];
        n->id         = sqlite3_column_int(stmt, 0);
        n->ticket_id  = sqlite3_column_int(stmt, 1);
        n->author_id  = (u64_snowflake_t)sqlite3_column_int64(stmt, 2);
        const char *c = (const char *)sqlite3_column_text(stmt, 3);
        n->content    = c ? strdup(c) : NULL;
        n->is_pinned  = sqlite3_column_int(stmt, 4) != 0;
        n->created_at = (long)sqlite3_column_int64(stmt, 5);
    }
    sqlite3_finalize(stmt);
    *notes_out = arr;
    *count_out = count;
    return 0;
}

int ticket_note_set_pinned(Database *db, int note_id, bool pinned) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db,
            "UPDATE ticket_notes SET is_pinned=? WHERE id=?;",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, pinned ? 1 : 0);
    sqlite3_bind_int(stmt, 2, note_id);
    int rc = sqlite3_step(stmt); sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int ticket_note_delete(Database *db, int note_id,
                       u64_snowflake_t requesting_staff_id) {
    /* Only the original author may delete their own note. */
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db,
            "DELETE FROM ticket_notes WHERE id=? AND author_id=?;",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int  (stmt, 1, note_id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)requesting_staff_id);
    int rc = sqlite3_step(stmt); sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

void ticket_notes_free(TicketNote *notes, int count) {
    if (!notes) return;
    for (int i = 0; i < count; i++) free(notes[i].content);
    free(notes);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Database – staff messages (index tracking for !edit / !del)
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Insert a new staff message row; returns the assigned msg_index (>= 1),
 * or -1 on error.
 */
static int staff_msg_insert(Database *db, int ticket_id,
                             u64_snowflake_t message_id,
                             u64_snowflake_t dm_message_id,
                             u64_snowflake_t author_id,
                             const char *author_name,
                             const char *content) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db,
            "SELECT COALESCE(MAX(msg_index),0)+1 FROM ticket_staff_messages"
            " WHERE ticket_id=?;",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, ticket_id);
    int next_idx = 1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        next_idx = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (sqlite3_prepare_v2(db->db,
            "INSERT INTO ticket_staff_messages"
            " (ticket_id,msg_index,message_id,dm_message_id,content,author_id,author_name,created_at)"
            " VALUES (?,?,?,?,?,?,?,?);",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int  (stmt, 1, ticket_id);
    sqlite3_bind_int  (stmt, 2, next_idx);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)message_id);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)dm_message_id);
    sqlite3_bind_text (stmt, 5, content, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)author_id);
    sqlite3_bind_text (stmt, 7, author_name ? author_name : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 8, (sqlite3_int64)time(NULL));
    int rc = sqlite3_step(stmt); sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? next_idx : -1;
}

/* Look up a staff message by ticket_id + msg_index.
 * Fills *message_id and copies content into *content_out (caller must free).
 * Returns 0 on success, -1 if not found. */
static int staff_msg_get(Database *db, int ticket_id, int msg_index,
                          u64_snowflake_t *message_id, char **content_out,
                          u64_snowflake_t *author_id_out,
                          u64_snowflake_t *dm_message_id_out,
                          char **author_name_out) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db,
            "SELECT message_id,content,author_id,dm_message_id,author_name"
            " FROM ticket_staff_messages"
            " WHERE ticket_id=? AND msg_index=?;",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, ticket_id);
    sqlite3_bind_int(stmt, 2, msg_index);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return -1; }
    if (message_id)       *message_id       = (u64_snowflake_t)sqlite3_column_int64(stmt, 0);
    if (content_out) {
        const char *c = (const char *)sqlite3_column_text(stmt, 1);
        *content_out = c ? strdup(c) : NULL;
    }
    if (author_id_out)    *author_id_out    = (u64_snowflake_t)sqlite3_column_int64(stmt, 2);
    if (dm_message_id_out)*dm_message_id_out= (u64_snowflake_t)sqlite3_column_int64(stmt, 3);
    if (author_name_out) {
        const char *n = (const char *)sqlite3_column_text(stmt, 4);
        *author_name_out = n ? strdup(n) : NULL;
    }
    sqlite3_finalize(stmt);
    return 0;
}

static int staff_msg_update(Database *db, int ticket_id, int msg_index,
                             const char *new_content) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db,
            "UPDATE ticket_staff_messages SET content=?"
            " WHERE ticket_id=? AND msg_index=?;",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, new_content, -1, SQLITE_STATIC);
    sqlite3_bind_int (stmt, 2, ticket_id);
    sqlite3_bind_int (stmt, 3, msg_index);
    int rc = sqlite3_step(stmt); sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int staff_msg_delete(Database *db, int ticket_id, int msg_index) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db,
            "DELETE FROM ticket_staff_messages"
            " WHERE ticket_id=? AND msg_index=?;",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, ticket_id);
    sqlite3_bind_int(stmt, 2, msg_index);
    int rc = sqlite3_step(stmt); sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Database – config
 * ═══════════════════════════════════════════════════════════════════════════ */

static void config_from_stmt(sqlite3_stmt *stmt, TicketConfig *c) {
    c->id       = sqlite3_column_int(stmt, 0);
    c->guild_id = (u64_snowflake_t)sqlite3_column_int64(stmt, 1);
    const char *cat = (const char *)sqlite3_column_text(stmt, 2);
    c->ticket_category_id = cat ? strdup(cat) : NULL;
    c->log_channel_id     = (u64_snowflake_t)sqlite3_column_int64(stmt, 3);
    const char *ms = (const char *)sqlite3_column_text(stmt, 4);
    c->main_server_id  = ms ? strdup(ms) : NULL;
    const char *ss = (const char *)sqlite3_column_text(stmt, 5);
    c->staff_server_id = ss ? strdup(ss) : NULL;
}

int ticket_config_get(Database *db, u64_snowflake_t guild_id, TicketConfig *out) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db,
            "SELECT id,guild_id,ticket_category_id,log_channel_id,"
            "main_server_id,staff_server_id"
            " FROM ticket_config WHERE guild_id=? LIMIT 1;",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)guild_id);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) { config_from_stmt(stmt, out); sqlite3_finalize(stmt); return 0; }
    sqlite3_finalize(stmt);
    return -1;
}

/*
 * Look up the config row where main_server_id matches the given community
 * guild ID.  This is how open_ticket and log_event resolve config — the
 * user supplies the community server ID, not the staff guild ID.
 */
static int ticket_config_get_by_main_server(Database *db,
                                             u64_snowflake_t community_guild_id,
                                             TicketConfig *out) {
    char id_str[24];
    snprintf(id_str, sizeof id_str, "%" PRIu64, community_guild_id);

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db,
            "SELECT id,guild_id,ticket_category_id,log_channel_id,"
            "main_server_id,staff_server_id"
            " FROM ticket_config WHERE main_server_id=? LIMIT 1;",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, id_str, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) { config_from_stmt(stmt, out); sqlite3_finalize(stmt); return 0; }
    sqlite3_finalize(stmt);
    return -1;
}

static int cfg_upsert_text(Database *db, u64_snowflake_t guild_id,
                            const char *field, const char *value) {
    char sql[320];
    snprintf(sql, sizeof sql,
             "INSERT INTO ticket_config (guild_id,%s) VALUES(?,?)"
             " ON CONFLICT(guild_id) DO UPDATE SET %s=excluded.%s;",
             field, field, field);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)guild_id);
    value ? sqlite3_bind_text(stmt, 2, value, -1, SQLITE_STATIC)
          : sqlite3_bind_null(stmt, 2);
    int rc = sqlite3_step(stmt); sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int cfg_upsert_int64(Database *db, u64_snowflake_t guild_id,
                             const char *field, u64_snowflake_t value) {
    char sql[320];
    snprintf(sql, sizeof sql,
             "INSERT INTO ticket_config (guild_id,%s) VALUES(?,?)"
             " ON CONFLICT(guild_id) DO UPDATE SET %s=excluded.%s;",
             field, field, field);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)guild_id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)value);
    int rc = sqlite3_step(stmt); sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int ticket_config_set_ticket_category(Database *db, u64_snowflake_t guild_id,
                                       char *category_id) {
    return cfg_upsert_text(db, guild_id, "ticket_category_id", category_id);
}
int ticket_config_set_log_channel(Database *db, u64_snowflake_t guild_id,
                                   u64_snowflake_t channel_id) {
    return cfg_upsert_int64(db, guild_id, "log_channel_id", channel_id);
}
int ticket_config_set_main_server(Database *db, u64_snowflake_t guild_id,
                                   const char *server_id) {
    return cfg_upsert_text(db, guild_id, "main_server_id", server_id);
}
int ticket_config_set_staff_server(Database *db, u64_snowflake_t guild_id,
                                    const char *server_id) {
    return cfg_upsert_text(db, guild_id, "staff_server_id", server_id);
}

void ticket_config_free(TicketConfig *cfg) {
    if (!cfg) return;
    free(cfg->ticket_category_id); cfg->ticket_category_id = NULL;
    free(cfg->main_server_id);     cfg->main_server_id     = NULL;
    free(cfg->staff_server_id);    cfg->staff_server_id    = NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * DM routing map  (internal only)
 * ═══════════════════════════════════════════════════════════════════════════ */

static int dm_map_set(u64_snowflake_t user_id, u64_snowflake_t dm_channel_id,
                       u64_snowflake_t ticket_channel_id, int ticket_id) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db->db,
            "INSERT OR REPLACE INTO ticket_dm_map"
            " (user_id,dm_channel_id,ticket_channel_id,ticket_id)"
            " VALUES (?,?,?,?);",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)user_id);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)dm_channel_id);
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)ticket_channel_id);
    sqlite3_bind_int  (stmt, 4, ticket_id);
    int rc = sqlite3_step(stmt); sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

/*
 * Given a user's ID, return the ticket_channel_id for their most recently
 * updated open ticket (status < CLOSED).  Returns 0 if none found.
 */
static u64_snowflake_t dm_map_get_active_ticket_channel(u64_snowflake_t user_id) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db->db,
            "SELECT m.ticket_channel_id FROM ticket_dm_map m"
            " JOIN tickets t ON t.id = m.ticket_id"
            " WHERE m.user_id=? AND t.status < ?"
            " ORDER BY t.updated_at DESC LIMIT 1;",
            -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)user_id);
    sqlite3_bind_int  (stmt, 2, TICKET_STATUS_CLOSED);
    u64_snowflake_t result = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        result = (u64_snowflake_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return result;
}

/*
 * Given an opener's user ID, return the DM channel ID for their most
 * recently updated open ticket.  Used by the staff-side relay.
 */
static u64_snowflake_t dm_map_get_active_dm_channel(u64_snowflake_t user_id) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db->db,
            "SELECT m.dm_channel_id FROM ticket_dm_map m"
            " JOIN tickets t ON t.id = m.ticket_id"
            " WHERE m.user_id=? AND t.status < ?"
            " ORDER BY t.updated_at DESC LIMIT 1;",
            -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)user_id);
    sqlite3_bind_int  (stmt, 2, TICKET_STATUS_CLOSED);
    u64_snowflake_t result = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        result = (u64_snowflake_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return result;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Option extraction  (matches propagation.c's pattern exactly)
 *
 * event->data->options is a NULL-terminated array of pointers.
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *get_option(const struct discord_interaction *event,
                               const char *name) {
    if (!event->data || !event->data->options) return NULL;
    for (int i = 0; event->data->options[i]; i++) {
        if (strcmp(event->data->options[i]->name, name) == 0)
            return event->data->options[i]->value;
    }
    return NULL;
}

/*
 * get_suboption – when the command uses a subcommand, options[0] is the
 * subcommand node and options[0]->options holds the actual arguments.
 */
static const char *get_suboption(const struct discord_interaction *event,
                                  const char *name) {
    if (!event->data || !event->data->options) return NULL;
    if (!event->data->options[0])              return NULL;
    struct discord_application_command_interaction_data_option *sub =
        event->data->options[0];
    if (!sub->options) return NULL;
    for (int i = 0; sub->options[i]; i++) {
        if (strcmp(sub->options[i]->name, name) == 0)
            return sub->options[i]->value;
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Messaging helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void post_to_channel(u64_snowflake_t channel_id, const char *content) {
    if (!channel_id || !content) return;
    struct discord_create_message_params params = { .content = (char *)content };
    ORCAcode rc = discord_create_message(g_client, channel_id, &params, NULL);
    if (rc != ORCA_OK)
        fprintf(stderr, "[ticket] post_to_channel %" PRIu64 " failed: %d\n",
                channel_id, rc);
}

/*
 * ticket_log_event_push
 *
 * Inserts an edit/delete audit row into ticket_log_events and pushes a live
 * SSE event to any dashboard subscribers watching the ticket log.
 *
 * event_type  – "edit" or "delete"
 * new_content – NULL for deletions
 */
static void ticket_log_event_push(int ticket_id,
                                   const char *event_type,
                                   int msg_index,
                                   const char *author_name,
                                   const char *old_content,
                                   const char *new_content) {
    if (!g_db) return;
    sqlite3_int64 now = (sqlite3_int64)time(NULL);

    /* Persist to DB. */
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db->db,
            "INSERT INTO ticket_log_events"
            " (ticket_id,event_type,msg_index,author_name,old_content,new_content,occurred_at)"
            " VALUES (?,?,?,?,?,?,?);",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int (stmt, 1, ticket_id);
        sqlite3_bind_text(stmt, 2, event_type,  -1, SQLITE_STATIC);
        sqlite3_bind_int (stmt, 3, msg_index);
        sqlite3_bind_text(stmt, 4, author_name  ? author_name  : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, old_content  ? old_content  : "", -1, SQLITE_STATIC);
        new_content ? sqlite3_bind_text(stmt, 6, new_content, -1, SQLITE_STATIC)
                    : sqlite3_bind_null(stmt, 6);
        sqlite3_bind_int64(stmt, 7, now);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    /* Build SSE JSON payload.
     * We need JSON-safe strings; use a simple hex-escape pass for the
     * content fields which may contain quotes or backslashes. */
    static char sse[8192];
    int p = 0;
    #define SSE_APPEND(fmt, ...) \
        p += snprintf(sse + p, (int)sizeof(sse) - p, fmt, ##__VA_ARGS__)
    #define SSE_STR(val) do { \
        SSE_APPEND("\""); \
        if (val) { \
            for (const char *_c = (val); *_c && p + 4 < (int)sizeof(sse); _c++) { \
                if (*_c == '"')       { sse[p++]='\\'  ; sse[p++]='"';  } \
                else if (*_c == '\\') { sse[p++]='\\'  ; sse[p++]='\\'; } \
                else if (*_c == '\n') { sse[p++]='\\'  ; sse[p++]='n';  } \
                else                  { sse[p++]=*_c; } \
            } \
        } \
        SSE_APPEND("\""); \
    } while(0)

    SSE_APPEND("{\"type\":\"ticket_log\",\"ticket_id\":%d,\"event_type\":", ticket_id);
    SSE_STR(event_type);
    SSE_APPEND(",\"msg_index\":%d,\"author_name\":", msg_index);
    SSE_STR(author_name);
    SSE_APPEND(",\"old_content\":");
    SSE_STR(old_content);
    SSE_APPEND(",\"new_content\":");
    if (new_content) { SSE_STR(new_content); }
    else              { SSE_APPEND("null"); }
    SSE_APPEND(",\"occurred_at\":%" PRId64 "}", now);
    #undef SSE_APPEND
    #undef SSE_STR

    http_sse_push(sse);
}

/*
 * Defuse mention syntax before forwarding.
 * Inserts a Unicode zero-width space (U+200B, encoded as UTF-8 0xE2 0x80 0x8B)
 * after every '@' so Discord does not resolve @everyone, @here, <@id>, <@&id>.
 * The result is invisible to readers but breaks the mention trigger.
 */
static void strip_mentions(const char *in, char *out, size_t out_len) {
    if (!in || !out || !out_len) return;
    /* UTF-8 encoding of U+200B */
    static const char ZWS[] = { (char)0xE2, (char)0x80, (char)0x8B, '\0' };
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 4 < out_len; i++) {
        if ((unsigned char)in[i] == '@') {
            /* Copy '@' then the zero-width space (3 bytes). */
            out[j++] = '@';
            out[j++] = ZWS[0];
            out[j++] = ZWS[1];
            out[j++] = ZWS[2];
        } else {
            out[j++] = in[i];
        }
    }
    out[j] = '\0';
}

/* Ephemeral interaction response — only the invoker sees it. */
static void reply_ephemeral(struct discord *client,
                             const struct discord_interaction *event,
                             const char *msg) {
    struct discord_interaction_response resp = {
        .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = (char *)msg,
            .flags   = 1 << 6,   /* EPHEMERAL */
        },
    };
    discord_create_interaction_response(client, event->id, event->token,
                                        &resp, NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Attachment relay helper
 *
 * event->attachments is a NULL-terminated struct discord_attachment **
 * array.  We relay CDN URLs to the destination channel.  The files remain
 * accessible as long as the source message exists; tickets are closed,
 * not deleted, so URLs stay live.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MAX_RELAY_ATTACHMENTS 5

static void relay_attachments(u64_snowflake_t dest_channel_id,
                               struct discord_attachment **attachments) {
    if (!attachments || !attachments[0]) return;

    char buf[1600];
    int  pos = 0;

    /* Count entries for the plural header. */
    int total = 0;
    for (; attachments[total]; total++) ;

    pos += snprintf(buf + pos, sizeof buf - (size_t)pos,
                    "📎 **Attachment%s:**\n", total == 1 ? "" : "s");

    int relayed = 0;
    for (int i = 0; attachments[i] && relayed < MAX_RELAY_ATTACHMENTS; i++) {
        const char *url = attachments[i]->url;
        if (!url) continue;
        pos += snprintf(buf + pos, sizeof buf - (size_t)pos, "%s\n", url);
        relayed++;
    }

    if (relayed > 0)
        post_to_channel(dest_channel_id, buf);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Channel-name / topic helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void build_channel_name(char *buf, size_t len,
                                int ticket_id, const char *username) {
    char safe[48] = {0};
    int j = 0;
    for (int i = 0; username[i] && j < 47; i++) {
        unsigned char c = (unsigned char)username[i];
        if      (c >= 'A' && c <= 'Z')                                           safe[j++] = (char)(c + 32);
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')  safe[j++] = (char)c;
        else if (c == '_' || c == ' ')                                           safe[j++] = '-';
    }
    if (!safe[0]) strcpy(safe, "user");
    snprintf(buf, len, "ticket-%04d-%s", ticket_id, safe);
}

/*
 * Update the ticket channel's topic so staff can see the current state at
 * a glance in the channel header without scrolling.
 */
static void refresh_channel_topic(u64_snowflake_t channel_id, const Ticket *t) {
    char topic[256];
    snprintf(topic, sizeof topic,
             "Ticket #%d │ %s │ Priority: %s │ Subject: %.80s",
             t->id,
             ticket_status_string(t->status),
             ticket_priority_string(t->priority),
             t->subject ? t->subject : "—");

    struct discord_modify_channel_params p = { .topic = topic };
    discord_modify_channel(g_client, channel_id, &p, NULL);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Log helper
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * guild_id here is the community server ID stored on the ticket row.
 * We look up config by main_server_id so the right log channel is found
 * regardless of which staff server is handling this community.
 */
static void log_event(u64_snowflake_t guild_id, const char *message) {
    if (!g_db) return;
    TicketConfig cfg = {0};
    if (ticket_config_get_by_main_server(g_db, guild_id, &cfg) != 0) return;
    if (cfg.log_channel_id)
        post_to_channel(cfg.log_channel_id, message);
    ticket_config_free(&cfg);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Convenience – extract user / member fields from an interaction
 * ═══════════════════════════════════════════════════════════════════════════ */

static u64_snowflake_t interaction_user_id(const struct discord_interaction *e) {
    if (e->member && e->member->user) return e->member->user->id;
    if (e->user)                      return e->user->id;
    return 0;
}

static const char *interaction_username(const struct discord_interaction *e) {
    if (e->member && e->member->user && e->member->user->username)
        return e->member->user->username;
    if (e->user && e->user->username)
        return e->user->username;
    return "unknown";
}

/* ═══════════════════════════════════════════════════════════════════════════
 * open_ticket  –  the core ticket-creation flow
 * ═══════════════════════════════════════════════════════════════════════════ */

static void open_ticket(struct discord *client,
                         const struct discord_interaction *event,
                         u64_snowflake_t target_guild_id,
                         const char *subject) {
    if (!g_db) {
        reply_ephemeral(client, event, "❌ Database unavailable.");
        return;
    }

    /* ── Validate configuration ── */
    TicketConfig cfg = {0};
    if (ticket_config_get_by_main_server(g_db, target_guild_id, &cfg) != 0) {
        reply_ephemeral(client, event,
            "❌ That server hasn't configured its ticket system yet. "
            "An admin must run `/ticketconfig` in the staff server first.");
        return;
    }

    if (!cfg.ticket_category_id || !cfg.ticket_category_id[0]) {
        ticket_config_free(&cfg);
        reply_ephemeral(client, event,
            "❌ That server's ticket category hasn't been set. "
            "An admin must supply the `category` argument to `/ticketconfig`.");
        return;
    }

    if (!cfg.staff_server_id || !cfg.staff_server_id[0]) {
        ticket_config_free(&cfg);
        reply_ephemeral(client, event,
            "❌ That server's staff server hasn't been configured. "
            "An admin must supply the `staffserver` argument to `/ticketconfig`.");
        return;
    }

    u64_snowflake_t category_id = (u64_snowflake_t)strtoull(cfg.ticket_category_id, NULL, 10);
    u64_snowflake_t staff_guild = (u64_snowflake_t)strtoull(cfg.staff_server_id,    NULL, 10);

    if (!category_id || !staff_guild) {
        ticket_config_free(&cfg);
        reply_ephemeral(client, event,
            "❌ Server configuration contains invalid IDs. Please re-run `/ticketconfig`.");
        return;
    }

    /* ── Opener identity ── */
    u64_snowflake_t opener_id = interaction_user_id(event);
    const char     *username  = interaction_username(event);

    if (!opener_id) {
        ticket_config_free(&cfg);
        reply_ephemeral(client, event, "❌ Could not determine your user ID.");
        return;
    }

    /* ── Create ticket channel in the staff server ──
     *
     * Use a placeholder name first; rename once we have the DB row ID.
     */
    struct discord_create_guild_channel_params ch_params = {
        .name      = "ticket-new",
        .type      = DISCORD_CHANNEL_GUILD_TEXT,
        .parent_id = category_id,
        .topic     = (char *)subject,
    };
    struct discord_channel new_channel = {0};
    ORCAcode ch_rc = discord_create_guild_channel(client, staff_guild,
                                                   &ch_params, &new_channel);
    if (ch_rc != ORCA_OK || !new_channel.id) {
        discord_channel_cleanup(&new_channel);
        ticket_config_free(&cfg);
        reply_ephemeral(client, event,
            "❌ Failed to create ticket channel. "
            "Please ensure I have **Manage Channels** permission in the staff server.");
        return;
    }

    /* ── Persist ticket ── */
    Ticket t = {
        .channel_id = new_channel.id,
        .guild_id   = target_guild_id,
        .opener_id  = opener_id,
        .status     = TICKET_STATUS_OPEN,
        .priority   = TICKET_PRIORITY_MEDIUM,
        .outcome    = TICKET_OUTCOME_NONE,
        .subject    = (char *)subject,
    };

    if (ticket_db_create(g_db, &t) != 0) {
        /* Undo the channel to avoid orphans. */
        discord_delete_channel(client, new_channel.id, NULL);
        discord_channel_cleanup(&new_channel);
        ticket_config_free(&cfg);
        reply_ephemeral(client, event, "❌ Database error. Please try again.");
        return;
    }

    /* ── Rename channel with the real ticket ID ── */
    char real_name[100];
    build_channel_name(real_name, sizeof real_name, t.id, username);
    struct discord_modify_channel_params rename_p = { .name = real_name };
    discord_modify_channel(client, new_channel.id, &rename_p, NULL);

    /* ── Lock send permissions until the ticket is claimed ──
     *
     * @everyone in the staff guild can VIEW_CHANNEL so all staff can read,
     * but SEND_MESSAGES is denied.  The claimant receives an explicit member
     * overwrite that grants it back (see handle_claim / ticket_channel_set_sender).
     *
     * The bot itself is unaffected because it holds ADMINISTRATOR in the staff
     * guild, which bypasses all channel permission overwrites.
     *
     * @everyone role ID == staff guild ID in Discord's permission model.
     */
    {
        struct discord_edit_channel_permissions_params lock = {
            .allow = DISCORD_PERMISSION_VIEW_CHANNEL,
            .deny  = DISCORD_PERMISSION_SEND_MESSAGES,
            .type  = 0,   /* 0 = role overwrite */
        };
        discord_edit_channel_permissions(client, new_channel.id,
                                         staff_guild /* @everyone role ID */,
                                         &lock);
    }
    // refresh_channel_topic(new_channel.id, &t);

    /* ── Post staff-facing header ──
     *
     * Staff see the opener's username and ID so they know who they are
     * speaking with.  The user never sees this channel.
     */
    char header[1024];
    snprintf(header, sizeof header,
        "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        "📬  **New Ticket #%d**\n"
        "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        "**Subject:** %.200s\n"
        "**Priority:** %s %s\n"
        "**Status:** %s\n"
        "**Opened by:** %s (`%" PRIu64 "`)\n"
        "**Community server ID:** `%" PRIu64 "`\n"
        "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        "Type here to reply — messages are forwarded anonymously.\n"
        "Use `/ticket claim`, `/ticket close`, `/ticket priority`, `/ticket status`.",
        t.id,
        subject,
        priority_emoji(t.priority), ticket_priority_string(t.priority),
        ticket_status_string(t.status),
        username, opener_id,
        target_guild_id);
    post_to_channel(new_channel.id, header);

    /* ── DM the opener ── */
    struct discord_create_dm_params dm_params = { .recipient_id = opener_id };
    struct discord_channel dm_ch = {0};
    ORCAcode dm_rc = discord_create_dm(client, &dm_params, &dm_ch);

    if (dm_rc == ORCA_OK && dm_ch.id) {
        char dm_msg[640];
        snprintf(dm_msg, sizeof dm_msg,
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            "✅  **Ticket #%d submitted**\n"
            "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
            "**Subject:** %.200s\n\n"
            "A member of staff will be with you shortly.\n"
            "**Reply to this DM to communicate with staff.**\n"
            "You cannot see the staff member's name.",
            t.id, subject);
        post_to_channel(dm_ch.id, dm_msg);
        dm_map_set(opener_id, dm_ch.id, new_channel.id, t.id);
    } else {
        post_to_channel(new_channel.id,
            "⚠️  Could not open a DM with this user. "
            "They may have DMs disabled. Staff replies will not be delivered.");
    }

    discord_channel_cleanup(&new_channel);
    discord_channel_cleanup(&dm_ch);

    /* ── Log event ── */
    char log_buf[320];
    snprintf(log_buf, sizeof log_buf,
             "📬 Ticket #%d opened by **%s** (`%" PRIu64 "`) "
             "for community guild `%" PRIu64 "` — %s",
             t.id, username, opener_id, target_guild_id, subject);
    log_event(target_guild_id, log_buf);

    ticket_config_free(&cfg);
    reply_ephemeral(client, event,
        "✅ Your ticket has been submitted. Check your DMs for a confirmation.");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Permission helpers – claim/unclaim send access
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Grant or revoke SEND_MESSAGES for a specific member in the ticket channel.
 *
 * allow=1  → edit overwrite: ALLOW SEND_MESSAGES  (used when claiming)
 * allow=0  → delete overwrite entirely so the member falls back to the
 *             @everyone DENY SEND_MESSAGES set at ticket-open time
 *             (used when transferring a claim to a different staff member)
 */
static void ticket_channel_set_sender(struct discord *client,
                                       u64_snowflake_t channel_id,
                                       u64_snowflake_t user_id,
                                       int allow) {
    if (!channel_id || !user_id) return;
    if (allow) {
        struct discord_edit_channel_permissions_params p = {
            .allow = DISCORD_PERMISSION_SEND_MESSAGES,
            .deny  = 0,
            .type  = 1,   /* 1 = member overwrite */
        };
        discord_edit_channel_permissions(client, channel_id, user_id, &p);
    } else {
        /* Removing the overwrite reverts the member to the role/everyone rules. */
        discord_delete_channel_permission(client, channel_id, user_id);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * close_ticket  –  shared close logic
 * ═══════════════════════════════════════════════════════════════════════════ */

static void close_ticket(struct discord *client,
                          const struct discord_interaction *event,
                          Ticket *t, TicketOutcome outcome, const char *notes) {
    ticket_db_set_outcome(g_db, t->id, outcome, notes);

    /* Notify the user via DM. */
    u64_snowflake_t dm_ch = dm_map_get_active_dm_channel(t->opener_id);
    if (dm_ch) {
        char msg[512];
        snprintf(msg, sizeof msg,
                 "🔒 **Your ticket #%d has been closed.**\n"
                 "**Outcome:** %s%s%s",
                 t->id,
                 ticket_outcome_string(outcome));
        post_to_channel(dm_ch, msg);
    }

    /* Summary in ticket channel. */
    char summary[512];
    snprintf(summary, sizeof summary,
             "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
             "🔒 **Ticket #%d closed by %s**\n"
             "**Outcome:** %s%s%s\n"
             "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━",
             t->id,
             interaction_username(event),
             ticket_outcome_string(outcome));
    post_to_channel(t->channel_id, summary);

    t->status  = TICKET_STATUS_CLOSED;
    t->outcome = outcome;

    char log_buf[256];
    snprintf(log_buf, sizeof log_buf,
             "🔒 Ticket #%d closed by **%s** — outcome: %s",
             t->id, interaction_username(event), ticket_outcome_string(outcome));
    log_event(t->guild_id, log_buf);

    /* Delete the channel immediately so it disappears as soon as /ticket close
     * is invoked, rather than lingering until the next bot restart. */
    discord_delete_channel(client, t->channel_id, NULL);

    reply_ephemeral(client, event, "✅ Ticket closed.");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Slash-command sub-handlers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── /ticket open ── */
static void handle_open(struct discord *client,
                         const struct discord_interaction *event) {
    const char *server_str = get_suboption(event, "server");
    const char *subject    = get_suboption(event, "subject");

    if (!server_str || !subject) {
        reply_ephemeral(client, event, "❌ Both `server` and `subject` are required.");
        return;
    }
    u64_snowflake_t target = (u64_snowflake_t)strtoull(server_str, NULL, 10);
    if (!target) {
        reply_ephemeral(client, event,
            "❌ Invalid server ID — please provide a numeric Discord server ID.");
        return;
    }

    /* Verify the invoker is actually a member of the target community server.
     * This prevents tickets being opened for servers the user has never joined. */
    u64_snowflake_t invoker_id = interaction_user_id(event);
    if (invoker_id) {
        struct discord_guild_member member = {0};
        ORCAcode member_rc = discord_get_guild_member(client, target,
                                                       invoker_id, &member);
        discord_guild_member_cleanup(&member);
        if (member_rc != ORCA_OK) {
            reply_ephemeral(client, event,
                "❌ You must be a member of that server to open a ticket for it.");
            return;
        }
    }

    open_ticket(client, event, target, subject);
}

/* ── /ticket claim ── */
static void handle_claim(struct discord *client,
                          const struct discord_interaction *event) {
    if (!event->channel_id) {
        reply_ephemeral(client, event, "❌ Cannot determine the current channel.");
        return;
    }
    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) {
        reply_ephemeral(client, event, "❌ This is not a ticket channel.");
        return;
    }
    if (t.status >= TICKET_STATUS_RESOLVED) {
        ticket_db_free(&t);
        reply_ephemeral(client, event, "❌ This ticket is already closed.");
        return;
    }

    u64_snowflake_t staff_id = interaction_user_id(event);

    if (t.assigned_to == staff_id) {
        ticket_db_free(&t);
        reply_ephemeral(client, event, "❌ You have already claimed this ticket.");
        return;
    }

    /* ── Transfer: strip the previous claimant's send permission ── */
    if (t.assigned_to != 0) {
        ticket_channel_set_sender(client, event->channel_id, t.assigned_to, 0);

        char xfer[192];
        snprintf(xfer, sizeof xfer,
                 "🔁 Ticket transferred from previous claimant to **%s**.",
                 interaction_username(event));
        post_to_channel(event->channel_id, xfer);
    } else {
        char msg[128];
        snprintf(msg, sizeof msg,
                 "🙋 **%s** has claimed ticket #%d.",
                 interaction_username(event), t.id);
        post_to_channel(event->channel_id, msg);
    }

    /* ── Persist claim ── */
    ticket_db_update_assigned(g_db, t.id, staff_id);
    ticket_db_update_status(g_db, t.id, TICKET_STATUS_IN_PROGRESS);

    /* ── Grant SEND_MESSAGES to the claimant ── */
    ticket_channel_set_sender(client, event->channel_id, staff_id, 1);

    /* ── Notify the user — only now that someone owns the ticket ── */
    u64_snowflake_t dm_ch = dm_map_get_active_dm_channel(t.opener_id);
    if (dm_ch)
        post_to_channel(dm_ch,
            "👋 A staff member has picked up your ticket and will be with you shortly.\n"
            "You can now send messages and they will be relayed to staff.");

    char log_buf[200];
    snprintf(log_buf, sizeof log_buf,
             "🙋 Ticket #%d claimed by **%s** (`%" PRIu64 "`).",
             t.id, interaction_username(event), staff_id);
    log_event(t.guild_id, log_buf);

    ticket_db_free(&t);
    reply_ephemeral(client, event, "✅ You have claimed this ticket. You can now message the user.");
}

/* ── /ticket close ── */
static void handle_close(struct discord *client,
                          const struct discord_interaction *event) {
    if (!event->channel_id) {
        reply_ephemeral(client, event, "❌ Cannot determine the current channel.");
        return;
    }
    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) {
        reply_ephemeral(client, event, "❌ This is not a ticket channel.");
        return;
    }
    if (t.status >= TICKET_STATUS_RESOLVED) {
        ticket_db_free(&t);
        reply_ephemeral(client, event, "❌ This ticket is already closed.");
        return;
    }

    const char *outcome_str = get_suboption(event, "outcome");
    const char *notes       = get_suboption(event, "notes");
    TicketOutcome outcome   = outcome_str
                              ? (TicketOutcome)atoi(outcome_str)
                              : TICKET_OUTCOME_RESOLVED;

    close_ticket(client, event, &t, outcome, notes);
    ticket_db_free(&t);
}

/* ── /ticket priority ── */
static void handle_priority(struct discord *client,
                              const struct discord_interaction *event) {
    if (!event->channel_id) {
        reply_ephemeral(client, event, "❌ Cannot determine the current channel.");
        return;
    }
    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) {
        reply_ephemeral(client, event, "❌ This is not a ticket channel.");
        return;
    }

    const char *level_str = get_suboption(event, "level");
    if (!level_str) {
        ticket_db_free(&t);
        reply_ephemeral(client, event, "❌ Please specify a priority level.");
        return;
    }

    TicketPriority priority = (TicketPriority)atoi(level_str);
    ticket_db_update_priority(g_db, t.id, priority);
    t.priority = priority;
    // refresh_channel_topic(event->channel_id, &t);

    char msg[128];
    snprintf(msg, sizeof msg,
             "📊 Priority updated to %s **%s**.",
             priority_emoji(priority), ticket_priority_string(priority));
    post_to_channel(event->channel_id, msg);

    char log_buf[200];
    snprintf(log_buf, sizeof log_buf,
             "📊 Ticket #%d priority → **%s** (by %s).",
             t.id, ticket_priority_string(priority), interaction_username(event));
    log_event(t.guild_id, log_buf);

    ticket_db_free(&t);
    reply_ephemeral(client, event, "✅ Priority updated.");
}

/* ── /ticket status ── */
static void handle_status_cmd(struct discord *client,
                               const struct discord_interaction *event) {
    if (!event->channel_id) {
        reply_ephemeral(client, event, "❌ Cannot determine the current channel.");
        return;
    }
    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) {
        reply_ephemeral(client, event, "❌ This is not a ticket channel.");
        return;
    }

    const char *val_str = get_suboption(event, "value");
    if (!val_str) {
        ticket_db_free(&t);
        reply_ephemeral(client, event, "❌ Please specify a status.");
        return;
    }

    TicketStatus status = (TicketStatus)atoi(val_str);
    ticket_db_update_status(g_db, t.id, status);
    t.status = status;
    // refresh_channel_topic(event->channel_id, &t);

    char msg[128];
    snprintf(msg, sizeof msg,
             "📋 Status updated to **%s**.", ticket_status_string(status));
    post_to_channel(event->channel_id, msg);

    ticket_db_free(&t);
    reply_ephemeral(client, event, "✅ Status updated.");
}

/* ── /ticket assign ── */
static void handle_assign(struct discord *client,
                           const struct discord_interaction *event) {
    if (!event->channel_id) {
        reply_ephemeral(client, event, "❌ Cannot determine the current channel.");
        return;
    }
    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) {
        reply_ephemeral(client, event, "❌ This is not a ticket channel.");
        return;
    }

    const char *staff_str = get_suboption(event, "staff");
    if (!staff_str) {
        ticket_db_free(&t);
        reply_ephemeral(client, event, "❌ Please specify a staff member.");
        return;
    }

    u64_snowflake_t staff_id = (u64_snowflake_t)strtoull(staff_str, NULL, 10);
    if (!staff_id) {
        ticket_db_free(&t);
        reply_ephemeral(client, event, "❌ Invalid user ID.");
        return;
    }

    ticket_db_update_assigned(g_db, t.id, staff_id);
    ticket_db_update_status(g_db, t.id, TICKET_STATUS_IN_PROGRESS);

    char msg[128];
    snprintf(msg, sizeof msg,
             "➡️ Ticket #%d assigned to `%" PRIu64 "`.", t.id, staff_id);
    post_to_channel(event->channel_id, msg);

    char log_buf[200];
    snprintf(log_buf, sizeof log_buf,
             "➡️ Ticket #%d assigned to `%" PRIu64 "` by **%s**.",
             t.id, staff_id, interaction_username(event));
    log_event(t.guild_id, log_buf);

    ticket_db_free(&t);
    reply_ephemeral(client, event, "✅ Ticket assigned.");
}

/* ── /ticketconfig ──
 *
 * Flat command with four optional arguments; at least one must be supplied.
 * Run inside the *staff* server.  Each argument updates only that field,
 * so staff can change a single value without re-specifying everything.
 *
 *   mainserver  – community server ID this staff server serves
 *   staffserver – this staff server's own ID (where channels are created)
 *   category    – ticket category channel ID in the staff server
 *   logchannel  – staff-only log channel ID in the staff server
 */
static void handle_config(struct discord *client,
                           const struct discord_interaction *event) {
    u64_snowflake_t guild_id = event->guild_id;
    if (!guild_id) {
        reply_ephemeral(client, event,
            "❌ Run `/ticketconfig` inside the staff server.");
        return;
    }

    /* Require ADMINISTRATOR. */
    if (event->member && event->member->permissions) {
        uint64_t perms = strtoull(event->member->permissions, NULL, 10);
        if (!(perms & DISCORD_PERMISSION_ADMINISTRATOR)) {
            reply_ephemeral(client, event,
                "❌ You need the **Administrator** permission to configure the ticket system.");
            return;
        }
    }

    /* At least one argument must be provided. */
    if (!event->data || !event->data->options || !event->data->options[0]) {
        reply_ephemeral(client, event,
            "❌ Provide at least one argument. "
            "Usage: `/ticketconfig [mainserver:<id>] [staffserver:<id>] "
            "[category:<id>] [logchannel:<id>]`");
        return;
    }

    const char *mainserver  = get_option(event, "mainserver");
    const char *staffserver = get_option(event, "staffserver");
    const char *category    = get_option(event, "category");
    const char *logchannel  = get_option(event, "logchannel");

    int changed = 0;
    char reply[512];
    int  pos = 0;
    pos += snprintf(reply + pos, sizeof reply - (size_t)pos,
                    "✅ Configuration updated:\n");

    if (mainserver) {
        ticket_config_set_main_server(g_db, guild_id, mainserver);
        pos += snprintf(reply + pos, sizeof reply - (size_t)pos,
                        "• Main community server → `%s`\n", mainserver);
        changed++;
    }
    if (staffserver) {
        ticket_config_set_staff_server(g_db, guild_id, staffserver);
        pos += snprintf(reply + pos, sizeof reply - (size_t)pos,
                        "• Staff server → `%s`\n", staffserver);
        changed++;
    }
    if (category) {
        ticket_config_set_ticket_category(g_db, guild_id, (char *)category);
        pos += snprintf(reply + pos, sizeof reply - (size_t)pos,
                        "• Ticket category → `%s`\n", category);
        changed++;
    }
    if (logchannel) {
        ticket_config_set_log_channel(g_db, guild_id,
                                       (u64_snowflake_t)strtoull(logchannel, NULL, 10));
        pos += snprintf(reply + pos, sizeof reply - (size_t)pos,
                        "• Log channel → `%s`\n", logchannel);
        changed++;
    }

    if (!changed) {
        reply_ephemeral(client, event,
            "❌ No recognised arguments were provided. "
            "Use `mainserver`, `staffserver`, `category`, or `logchannel`.");
        return;
    }

    reply_ephemeral(client, event, reply);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public – on_ticket_interaction
 * ═══════════════════════════════════════════════════════════════════════════ */

void on_ticket_interaction(struct discord *client,
                            const struct discord_interaction *event) {
    if (!event->data) return;
    const char *cmd = event->data->name;

    if (strcmp(cmd, "ticket") == 0) {
        if (!event->data->options || !event->data->options[0]) {
            reply_ephemeral(client, event, "❌ Please specify a subcommand.");
            return;
        }
        const char *sub = event->data->options[0]->name;

        if      (strcmp(sub, "open")     == 0) handle_open       (client, event);
        else if (strcmp(sub, "claim")    == 0) handle_claim      (client, event);
        else if (strcmp(sub, "close")    == 0) handle_close      (client, event);
        else if (strcmp(sub, "priority") == 0) handle_priority   (client, event);
        else if (strcmp(sub, "status")   == 0) handle_status_cmd (client, event);
        else if (strcmp(sub, "assign")   == 0) handle_assign     (client, event);
        else    reply_ephemeral(client, event, "❌ Unknown subcommand.");
        return;
    }

    if (strcmp(cmd, "ticketconfig") == 0) {
        handle_config(client, event);
        return;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public – on_ticket_message
 *
 * ┌──────────────────────────────────────────────────────┐
 * │ Path A – DM from user (guild_id == 0)                │
 * │   Look up active ticket by user_id                   │
 * │   → mentions stripped, forward to ticket channel     │
 * │   → ack in DM                                        │
 * ├──────────────────────────────────────────────────────┤
 * │ Path B – message in a ticket channel (staff reply)   │
 * │   channel maps to a ticket in the DB                 │
 * │   → delete original (MANAGE_MESSAGES required)       │
 * │   → re-post as "[Staff]: …" (anonymous)              │
 * │   → relay to user DM, also stripped of mentions      │
 * └──────────────────────────────────────────────────────┘
 * ═══════════════════════════════════════════════════════════════════════════ */

void on_ticket_message(struct discord *client,
                        const struct discord_message *event) {
    /* Never process our own messages. */
    if (!event->author || event->author->bot) return;
    if (!event->channel_id) return;
    if (!g_db) return;

    /* Sanitise content — defuse all @mention syntax. */
    char safe[2000] = {0};
    if (event->content && event->content[0])
        strip_mentions(event->content, safe, sizeof safe);
    else
        strcpy(safe, "*(no text)*");

    /* ── Path A: user DM ── */
    if (!event->guild_id) {
        u64_snowflake_t ticket_ch =
            dm_map_get_active_ticket_channel(event->author->id);
        if (!ticket_ch) return;   /* Not a tracked ticket DM — ignore. */

        /* Look up the ticket to check claim status. */
        Ticket user_t = {0};
        int has_ticket = ticket_db_get_by_channel(g_db, ticket_ch, &user_t);

        char fwd[2000];
        snprintf(fwd, sizeof fwd,
                 "**%s:** %s",
                 event->author->username ? event->author->username : "User",
                 safe);
        post_to_channel(ticket_ch, fwd);
        relay_attachments(ticket_ch, event->attachments);

        if (has_ticket == 0 && user_t.assigned_to == 0) {
            /* Ticket exists but is unclaimed — warn the user their message
             * is visible to staff but no one has taken ownership yet. */
            discord_create_reaction(client, event->channel_id, event->id,
                                    0, "⏳");
            post_to_channel(event->channel_id,
                "⏳ *Your message has been received. "
                "A staff member will claim your ticket shortly.*");
        } else {
            /* Claimed — standard 📨 acknowledgement. */
            discord_create_reaction(client, event->channel_id, event->id,
                                    0, "📨");
        }
        ticket_db_free(&user_t);
        return;
    }

    /* ── Path B: message in a ticket channel ── */
    Ticket t = {0};
    if (ticket_db_get_by_channel(g_db, event->channel_id, &t) != 0) return;
    if (t.status >= TICKET_STATUS_CLOSED) { ticket_db_free(&t); return; }

    const char *author_name = event->author->username
                              ? event->author->username : "Staff";
    u64_snowflake_t author_id = event->author->id;

    /* ── !edit <index> <new text> ── */
    if (strncmp(event->content, "!edit ", 6) == 0) {
        discord_delete_message(client, event->channel_id, event->id);

        char *rest = (char *)event->content + 6;
        int idx = (int)strtol(rest, &rest, 10);
        while (*rest == ' ') rest++;

        u64_snowflake_t old_msg_id    = 0;
        u64_snowflake_t old_dm_msg_id = 0;
        char *old_content   = NULL;
        char *orig_author   = NULL;
        if (idx < 1 || staff_msg_get(g_db, t.id, idx,
                                      &old_msg_id, &old_content, NULL,
                                      &old_dm_msg_id, &orig_author) != 0) {
            char err[128];
            snprintf(err, sizeof err,
                     "⚠️ No staff message with index **#%d** in this ticket.", idx);
            post_to_channel(event->channel_id, err);
            free(old_content);
            free(orig_author);
            ticket_db_free(&t);
            return;
        }

        /* Strip mentions from the replacement text. */
        char new_safe[2000] = {0};
        strip_mentions(rest, new_safe, sizeof new_safe);

        /* Edit the staff-channel message, preserving the [#N] **name:** header. */
        char indexed[2048];
        snprintf(indexed, sizeof indexed,
                 "`[#%d]` **%s:** %s *(edited)*",
                 idx, orig_author ? orig_author : "Staff", new_safe);
        struct discord_edit_message_params ep = { .content = indexed };
        discord_edit_message(g_client, event->channel_id, old_msg_id, &ep, NULL);

        /* Edit the DM message directly — the bot owns it so this works. */
        u64_snowflake_t dm_ch = dm_map_get_active_dm_channel(t.opener_id);
        if (dm_ch && old_dm_msg_id) {
            char anon_new[2000];
            snprintf(anon_new, sizeof anon_new, "**[Staff]:** %s", new_safe);
            struct discord_edit_message_params dm_ep = { .content = anon_new };
            discord_edit_message(g_client, dm_ch, old_dm_msg_id, &dm_ep, NULL);
        }

        staff_msg_update(g_db, t.id, idx, new_safe);

        ticket_log_event_push(t.id, "edit", idx,
                              orig_author ? orig_author : "Staff",
                              old_content, new_safe);

        free(old_content);
        free(orig_author);
        ticket_db_free(&t);
        return;
    }

    /* ── !del <index> ── */
    if (strncmp(event->content, "!del ", 5) == 0) {
        discord_delete_message(client, event->channel_id, event->id);

        int idx = (int)strtol(event->content + 5, NULL, 10);
        u64_snowflake_t old_msg_id    = 0;
        u64_snowflake_t old_dm_msg_id = 0;
        char *old_content  = NULL;
        char *orig_author  = NULL;
        if (idx < 1 || staff_msg_get(g_db, t.id, idx,
                                      &old_msg_id, &old_content, NULL,
                                      &old_dm_msg_id, &orig_author) != 0) {
            char err[128];
            snprintf(err, sizeof err,
                     "⚠️ No staff message with index **#%d** in this ticket.", idx);
            post_to_channel(event->channel_id, err);
            free(old_content);
            free(orig_author);
            ticket_db_free(&t);
            return;
        }

        /* Delete the staff-channel message. */
        discord_delete_message(g_client, event->channel_id, old_msg_id);

        /* Delete the DM message directly. */
        u64_snowflake_t dm_ch = dm_map_get_active_dm_channel(t.opener_id);
        if (dm_ch && old_dm_msg_id)
            discord_delete_message(g_client, dm_ch, old_dm_msg_id);

        ticket_log_event_push(t.id, "delete", idx,
                              orig_author ? orig_author : "Staff",
                              old_content, NULL);

        staff_msg_delete(g_db, t.id, idx);
        free(old_content);
        free(orig_author);
        ticket_db_free(&t);
        return;
    }

    /* ── Any other message starting with '!' is a staff-internal note ──
     * Leave it visible in the ticket channel; just don't relay it to the user. */
    if (event->content && event->content[0] == '!') {
        ticket_db_free(&t);
        return;
    }

    /*
     * ── Claim gate ──
     *
     * Only the assigned staff member may relay messages to the user.
     * Discord's channel permission overwrite (set at claim time) already
     * prevents ordinary users from sending here, but someone with
     * ADMINISTRATOR in the staff guild can bypass that — we catch it
     * at the bot level too.
     */
    if (t.assigned_to == 0) {
        post_to_channel(event->channel_id,
            "⚠️ This ticket has not been claimed yet. "
            "Use `/ticket claim` to take ownership before messaging the user.");
        ticket_db_free(&t);
        return;
    }
    if (event->author->id != t.assigned_to) {
        char err[192];
        snprintf(err, sizeof err,
                 "⚠️ Only the assigned staff member can relay messages to the user. "
                 "Use `/ticket claim` to take over this ticket first.");
        post_to_channel(event->channel_id, err);
        ticket_db_free(&t);
        return;
    }

    /*
     * Normal staff reply.
     *
     * Staff-side:  show the real username + sequential index so staff know
     *              who said what and can use !edit / !del.
     * User-side:   always anonymous "[Staff]: …".
     */
    discord_delete_message(client, event->channel_id, event->id);

    /* Build the anonymous version for the user DM. */
    char anon[2000];
    snprintf(anon, sizeof anon, "**[Staff]:** %s", safe);

    /* Determine the next index before posting, so we can embed it. */
    /* We post first (to get the message_id), then record in DB.
     * Use a two-step approach: post → capture message_id → insert row. */

    /* Staff-visible post includes the real name and the index placeholder.
     * We don't know the index until after posting, so post, then immediately
     * edit with the correct index header. */
    char staff_post[2048];
    snprintf(staff_post, sizeof staff_post,
             "**%s:** %s", author_name, safe);

    struct discord_create_message_params mp = { .content = staff_post };
    struct discord_message posted_msg = {0};
    if (discord_create_message(g_client, event->channel_id,
                               &mp, &posted_msg) == ORCA_OK && posted_msg.id) {
        /* Forward anonymously to the user's DM first so we can capture the
         * DM message ID and store it alongside the staff-channel message ID. */
        u64_snowflake_t dm_ch = dm_map_get_active_dm_channel(t.opener_id);
        u64_snowflake_t dm_msg_id = 0;
        if (dm_ch) {
            struct discord_create_message_params dm_params = { .content = anon };
            struct discord_message dm_posted = {0};
            if (discord_create_message(g_client, dm_ch,
                                       &dm_params, &dm_posted) == ORCA_OK)
                dm_msg_id = dm_posted.id;
            discord_message_cleanup(&dm_posted);
            relay_attachments(dm_ch, event->attachments);
        } else {
            post_to_channel(event->channel_id,
                "⚠️ Could not deliver your message — the user's DM channel is unavailable. "
                "They may have disabled DMs.");
        }

        int idx = staff_msg_insert(g_db, t.id, posted_msg.id, dm_msg_id,
                                   author_id, author_name, safe);
        if (idx > 0) {
            /* Edit to prepend the index badge so staff can reference it. */
            char indexed[2048];
            snprintf(indexed, sizeof indexed,
                     "`[#%d]` **%s:** %s", idx, author_name, safe);
            struct discord_edit_message_params ep = { .content = indexed };
            discord_edit_message(g_client, event->channel_id,
                                 posted_msg.id, &ep, NULL);
        }
    }
    discord_message_cleanup(&posted_msg);
    relay_attachments(event->channel_id, event->attachments);

    ticket_db_free(&t);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public – register_ticket_commands
 *
 * Follows propagation.c's pattern exactly:
 *   static option structs → static NULL-terminated option pointer arrays
 *   → discord_create_guild_application_command_params / _global_ variant
 * ═══════════════════════════════════════════════════════════════════════════ */

void register_ticket_commands(struct discord *client,
                               u64_snowflake_t application_id,
                               u64_snowflake_t guild_id) {

    /* ── /ticket subcommand options ───────────────────────────────────────── */

    /* open */
    static struct discord_application_command_option open_server = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name = "server",
        .description = "Numeric ID of the community server you are reporting in",
        .required = true,
    };
    static struct discord_application_command_option open_subject = {
        .type = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name = "subject",
        .description = "Brief summary of your report (max 100 chars)",
        .required = true,
    };
    static struct discord_application_command_option *open_opts[] = {
        &open_server, &open_subject, NULL
    };
    static struct discord_application_command_option open_sub = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
        .name        = "open",
        .description = "Submit a new support ticket",
        .options     = open_opts,
    };

    /* claim (no options) */
    static struct discord_application_command_option claim_sub = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
        .name        = "claim",
        .description = "Claim this ticket (staff — run inside a ticket channel)",
    };

    /* close */
    static struct discord_application_command_option close_outcome = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "outcome",
        .description = "1=Resolved 2=Duplicate 3=Invalid 4=NoResponse 5=Escalated 6=Other",
        .required    = true,
    };
    static struct discord_application_command_option close_notes = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "notes",
        .description = "Internal closing notes (never shown to the user)",
        .required    = true,
    };
    static struct discord_application_command_option *close_opts[] = {
        &close_outcome, &close_notes, NULL
    };
    static struct discord_application_command_option close_sub = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
        .name        = "close",
        .description = "Close this ticket (staff — run inside a ticket channel)",
        .options     = close_opts,
    };

    /* priority */
    static struct discord_application_command_option priority_level = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "level",
        .description = "0=Low  1=Medium  2=High  3=Urgent",
        .required    = true,
    };
    static struct discord_application_command_option *priority_opts[] = {
        &priority_level, NULL
    };
    static struct discord_application_command_option priority_sub = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
        .name        = "priority",
        .description = "Change the priority of this ticket (staff only)",
        .options     = priority_opts,
    };

    /* status */
    static struct discord_application_command_option status_value = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "value",
        .description = "0=Open  1=InProgress  2=PendingUser  3=Resolved",
        .required    = true,
    };
    static struct discord_application_command_option *status_opts[] = {
        &status_value, NULL
    };
    static struct discord_application_command_option status_sub = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
        .name        = "status",
        .description = "Update the status of this ticket (staff only)",
        .options     = status_opts,
    };

    /* assign */
    static struct discord_application_command_option assign_staff = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_USER,
        .name        = "staff",
        .description = "Staff member to assign this ticket to",
        .required    = true,
    };
    static struct discord_application_command_option *assign_opts[] = {
        &assign_staff, NULL
    };
    static struct discord_application_command_option assign_sub = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_SUB_COMMAND,
        .name        = "assign",
        .description = "Assign this ticket to a specific staff member (staff only)",
        .options     = assign_opts,
    };

    static struct discord_application_command_option *ticket_subs[] = {
        &open_sub, &claim_sub, &close_sub,
        &priority_sub, &status_sub, &assign_sub,
        NULL
    };

    /* ── /ticket registration ─────────────────────────────────────────────── */
    if (guild_id) {
        static struct discord_create_guild_application_command_params ticket_params = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticket",
            .description = "Open and manage support tickets",
        };
        ticket_params.options = ticket_subs;
        discord_create_guild_application_command(client, application_id, guild_id,
                                                  &ticket_params, NULL);
    } else {
        static struct discord_create_global_application_command_params ticket_params = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticket",
            .description = "Open and manage support tickets",
        };
        ticket_params.options = ticket_subs;
        discord_create_global_application_command(client, application_id,
                                                   &ticket_params, NULL);
    }

    /* ── /ticketconfig – flat command, all args optional ─────────────────── */

    static struct discord_application_command_option cfg_mainserver = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "mainserver",
        .description = "Snowflake ID of the community server this staff server handles",
        .required    = true,
    };
    static struct discord_application_command_option cfg_staffserver = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "staffserver",
        .description = "Snowflake ID of this staff server (where ticket channels are created)",
        .required    = true,
    };
    static struct discord_application_command_option cfg_category = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "category",
        .description = "ID of the category channel where ticket channels will be created",
        .required    = true,
    };
    static struct discord_application_command_option cfg_logchannel = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "logchannel",
        .description = "ID of the staff-only channel where ticket events are logged",
        .required    = true,
    };
    static struct discord_application_command_option *cfg_opts[] = {
        &cfg_mainserver, &cfg_staffserver, &cfg_category, &cfg_logchannel, NULL
    };

    /* ── /ticketconfig registration ──────────────────────────────────────── */
    if (guild_id) {
        static struct discord_create_guild_application_command_params cfg_params = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticketconfig",
            .description = "Configure the ticket system for this server (admin only)",
        };
        cfg_params.options = cfg_opts;
        discord_create_guild_application_command(client, application_id, guild_id,
                                                  &cfg_params, NULL);
    } else {
        static struct discord_create_global_application_command_params cfg_params = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "ticketconfig",
            .description = "Configure the ticket system for this server (admin only)",
        };
        cfg_params.options = cfg_opts;
        discord_create_global_application_command(client, application_id,
                                                   &cfg_params, NULL);
    }

    printf("[ticket] Commands registered (guild=%" PRIu64 ").\n", guild_id);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Public – ticket_module_init
 * ═══════════════════════════════════════════════════════════════════════════ */

void ticket_module_init(struct discord *client, Database *db) {
    g_client = client;
    g_db     = db;
    if (ticket_db_init_tables(db) != 0)
        fprintf(stderr, "[ticket] Warning: failed to initialise DB tables.\n");
    printf("[ticket] Module initialised.\n");
}