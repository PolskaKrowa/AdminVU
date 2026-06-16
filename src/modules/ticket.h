/*
 * ticket.h  –  Public interface for the ticket module
 */

#ifndef TICKET_H
#define TICKET_H

#include <stdint.h>
#include <stdbool.h>
#include <orca/discord.h>

#include "database.h"

/* ============================================================================
 * Enumerations
 * ========================================================================= */

typedef enum {
    TICKET_STATUS_OPEN          = 0,
    TICKET_STATUS_IN_PROGRESS   = 1,
    TICKET_STATUS_PENDING_USER  = 2,
    TICKET_STATUS_RESOLVED      = 3,
    TICKET_STATUS_CLOSED        = 4,
} TicketStatus;

typedef enum {
    TICKET_PRIORITY_LOW    = 0,
    TICKET_PRIORITY_MEDIUM = 1,
    TICKET_PRIORITY_HIGH   = 2,
    TICKET_PRIORITY_URGENT = 3,
} TicketPriority;

typedef enum {
    TICKET_OUTCOME_NONE        = 0,
    TICKET_OUTCOME_RESOLVED    = 1,
    TICKET_OUTCOME_DUPLICATE   = 2,
    TICKET_OUTCOME_INVALID     = 3,
    TICKET_OUTCOME_NO_RESPONSE = 4,
    TICKET_OUTCOME_ESCALATED   = 5,
    TICKET_OUTCOME_OTHER       = 6,
} TicketOutcome;

/* ============================================================================
 * Structs
 * ========================================================================= */

typedef struct {
    int               id;
    u64_snowflake_t   channel_id;
    u64_snowflake_t   guild_id;
    u64_snowflake_t   opener_id;
    u64_snowflake_t   assigned_to;
    TicketStatus      status;
    TicketPriority    priority;
    TicketOutcome     outcome;
    long              created_at;
    long              updated_at;
    long              closed_at;
    char             *subject;
    char             *outcome_notes;
    /*
     * Message ID of the staff-facing "📬 New Ticket #N" header post in
     * ticket.channel_id.  Kept in sync (status/priority/claim/etc.) by
     * refresh_ticket_header() so staff always see live ticket state without
     * scrolling.  0 if not yet posted.
     */
    u64_snowflake_t   header_message_id;
} Ticket;

typedef struct {
    int               id;
    int               ticket_id;
    u64_snowflake_t   author_id;
    char             *content;
    bool              is_pinned;
    long              created_at;
} TicketNote;

/*
 * Per-guild configuration record.
 *
 *   ticket_channel_id  – category or channel where new ticket channels are
 *                        created (used by the bot when it creates channels).
 *   log_channel_id     – staff-only channel where ticket events are logged.
 *   main_server_id     – the community/main Discord server snowflake that
 *                        this support guild serves.
 *   staff_server_id    – the private staff Discord server snowflake.
 *
 * Snowflake fields that are 0 indicate "not set".
 * The two server ID strings are stored as text because they are arbitrary
 * server IDs entered by staff, not necessarily servers the bot is in.
 */
typedef struct {
    int               id;
    u64_snowflake_t   guild_id;
    char             *ticket_category_id;
    u64_snowflake_t   log_channel_id;
    char             *main_server_id;
    char             *staff_server_id;
} TicketConfig;

/* ============================================================================
 * String helpers
 * ========================================================================= */

const char *ticket_status_string(TicketStatus s);
const char *ticket_priority_string(TicketPriority p);
const char *ticket_outcome_string(TicketOutcome o);

/* ============================================================================
 * Database – tickets
 * ========================================================================= */

int  ticket_db_init_tables(Database *db);
int  ticket_db_create(Database *db, Ticket *t);
int  ticket_db_get_by_channel(Database *db, u64_snowflake_t channel_id, Ticket *out);
int  ticket_db_get_by_id(Database *db, int ticket_id, Ticket *out);
int  ticket_db_update_status(Database *db, int ticket_id, TicketStatus status);
int  ticket_db_update_priority(Database *db, int ticket_id, TicketPriority priority);
int  ticket_db_update_assigned(Database *db, int ticket_id, u64_snowflake_t staff_id);
int  ticket_db_set_outcome(Database *db, int ticket_id, TicketOutcome outcome, const char *notes);
int  ticket_db_set_header_message(Database *db, int ticket_id, u64_snowflake_t message_id);
void ticket_db_free(Ticket *t);

/* ============================================================================
 * Database – notes
 * ========================================================================= */

int  ticket_note_add(Database *db, int ticket_id, u64_snowflake_t author_id, const char *content);
int  ticket_note_get_all(Database *db, int ticket_id, TicketNote **notes_out, int *count_out);
int  ticket_note_set_pinned(Database *db, int note_id, bool pinned);
int  ticket_note_delete(Database *db, int note_id, u64_snowflake_t requesting_staff_id);
void ticket_notes_free(TicketNote *notes, int count);

/* ============================================================================
 * Database – config
 * ========================================================================= */

int  ticket_config_get(Database *db, u64_snowflake_t guild_id, TicketConfig *out);
int  ticket_config_set_ticket_category(Database *db, u64_snowflake_t guild_id,
                                       char *category_id);
int  ticket_config_set_log_channel(Database *db, u64_snowflake_t guild_id, u64_snowflake_t channel_id);
int  ticket_config_set_main_server(Database *db, u64_snowflake_t guild_id, const char *server_id);
int  ticket_config_set_staff_server(Database *db, u64_snowflake_t guild_id, const char *server_id);
void ticket_config_free(TicketConfig *cfg);

/* ============================================================================
 * Event handlers / module entry-points
 * ========================================================================= */

void ticket_module_init(struct discord *client, Database *db);
void on_ticket_interaction(struct discord *client, const struct discord_interaction *event);
void on_ticket_message(struct discord *client, const struct discord_message *event);
void register_ticket_commands(struct discord *client, u64_snowflake_t application_id,
                               u64_snowflake_t guild_id);

/*
 * Programmatically open a ticket on behalf of a user with no Discord
 * interaction context — used by the propagation module's "Open Linked
 * Ticket" button so automatic ticket creation works from a component
 * interaction rather than only from /ticket open.
 *
 * target_guild_id – community server the ticket is filed against (must
 *                   already have /ticketconfig set up).
 * opener_id       – Discord user ID the ticket is opened for.
 * username        – display name used in the staff-facing header.
 * subject         – ticket subject line.
 * ticket_id_out   – on success, set to the new ticket's numeric ID.
 *
 * Returns 0 on success (ticket_id_out populated, opener DM'd just like
 * /ticket open), or -1 on failure (e.g. ticket system not configured for
 * that guild, or channel creation failed).
 */
int ticket_open_for_propagation(struct discord  *client,
                                 u64_snowflake_t  target_guild_id,
                                 u64_snowflake_t  opener_id,
                                 const char      *username,
                                 const char      *subject,
                                 int             *ticket_id_out);

/*
 * Renders a self-contained HTML document containing the full message
 * history (and a media manifest) for a ticket.  Used by:
 *   - close_ticket(), which stores the result in ticket_archives so it
 *     survives the channel being deleted; and
 *   - api.c's GET /api/tickets/<id>/archive, which serves it directly.
 *
 * Returns a malloc'd NUL-terminated string (caller frees), or NULL on
 * error / if the ticket does not exist.
 */
char *ticket_render_archive_html(Database *db, int ticket_id);

#endif /* TICKET_H */