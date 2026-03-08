#ifndef TICKET_H
#define TICKET_H

#include <stdint.h>
#include <stdbool.h>
#include <orca/discord.h>
#include "database.h"

/* ============================================================================
 * Ticket status & priority enums
 * ========================================================================= */

typedef enum {
    TICKET_STATUS_OPEN       = 0,
    TICKET_STATUS_IN_PROGRESS,
    TICKET_STATUS_PENDING_USER,   /* awaiting reply from the ticket opener  */
    TICKET_STATUS_RESOLVED,
    TICKET_STATUS_CLOSED,
} TicketStatus;

typedef enum {
    TICKET_PRIORITY_LOW      = 0,
    TICKET_PRIORITY_MEDIUM,
    TICKET_PRIORITY_HIGH,
    TICKET_PRIORITY_URGENT,
} TicketPriority;

typedef enum {
    TICKET_OUTCOME_NONE      = 0,
    TICKET_OUTCOME_RESOLVED,
    TICKET_OUTCOME_DUPLICATE,
    TICKET_OUTCOME_INVALID,
    TICKET_OUTCOME_NO_RESPONSE,
    TICKET_OUTCOME_ESCALATED,
    TICKET_OUTCOME_OTHER,
} TicketOutcome;

/* ============================================================================
 * Data structures
 * ========================================================================= */

typedef struct {
    int               id;
    u64_snowflake_t   channel_id;      /* Discord channel used for this ticket  */
    u64_snowflake_t   guild_id;
    u64_snowflake_t   opener_id;       /* User who created the ticket            */
    u64_snowflake_t   assigned_to;     /* Staff member assigned (0 = unassigned) */
    TicketStatus      status;
    TicketPriority    priority;
    TicketOutcome     outcome;
    char             *subject;         /* One-line summary set by opener         */
    char             *outcome_notes;   /* Free-text outcome description          */
    long              created_at;
    long              updated_at;
    long              closed_at;       /* 0 if not yet closed                    */
} Ticket;

typedef struct {
    int               id;
    int               ticket_id;
    u64_snowflake_t   author_id;       /* Staff member who wrote the note        */
    char             *content;
    bool              is_pinned;
    long              created_at;
} TicketNote;

/* ============================================================================
 * Module lifecycle
 * ========================================================================= */

void ticket_module_init(struct discord *client, Database *db);
void register_ticket_commands(struct discord *client,
                               u64_snowflake_t application_id,
                               u64_snowflake_t guild_id);

/* ============================================================================
 * Event handlers (called from main.c)
 * ========================================================================= */

void on_ticket_interaction(struct discord *client,
                           const struct discord_interaction *event);
void on_ticket_message(struct discord *client,
                       const struct discord_message *event);

/* ============================================================================
 * Database helpers (implemented in ticket.c, exposed for testing)
 * ========================================================================= */

int    ticket_db_init_tables(Database *db);

/* Ticket CRUD */
int    ticket_db_create(Database *db, Ticket *t);       /* fills t->id          */
int    ticket_db_get_by_channel(Database *db, u64_snowflake_t channel_id, Ticket *out);
int    ticket_db_get_by_id(Database *db, int ticket_id, Ticket *out);
int    ticket_db_update_status(Database *db, int ticket_id, TicketStatus status);
int    ticket_db_update_priority(Database *db, int ticket_id, TicketPriority priority);
int    ticket_db_update_assigned(Database *db, int ticket_id, u64_snowflake_t staff_id);
int    ticket_db_set_outcome(Database *db, int ticket_id,
                              TicketOutcome outcome, const char *notes);
void   ticket_db_free(Ticket *t);

/* Notes CRUD */
int    ticket_note_add(Database *db, int ticket_id,
                       u64_snowflake_t author_id, const char *content);
int    ticket_note_get_all(Database *db, int ticket_id,
                           TicketNote **notes_out, int *count_out);
int    ticket_note_set_pinned(Database *db, int note_id, bool pinned);
int    ticket_note_delete(Database *db, int note_id, u64_snowflake_t requesting_staff_id);
void   ticket_notes_free(TicketNote *notes, int count);

/* Utility */
const char *ticket_status_string(TicketStatus s);
const char *ticket_priority_string(TicketPriority p);
const char *ticket_outcome_string(TicketOutcome o);

#endif /* TICKET_H */