#ifndef TICKET_H
#define TICKET_H

#include <orca/discord.h>
#include "../database.h"

// Ticket status enumeration
typedef enum {
    TICKET_STATUS_OPEN,
    TICKET_STATUS_CLOSED,
    TICKET_STATUS_ARCHIVED
} TicketStatus;

// Ticket structure
typedef struct {
    int id;
    u64_snowflake_t user_id;
    u64_snowflake_t main_guild_id;      // The user's server
    u64_snowflake_t staff_guild_id;     // The staff-only server
    u64_snowflake_t staff_channel_id;   // Channel in staff server
    u64_snowflake_t dm_channel_id;      // User's DM channel
    TicketStatus status;
    long created_at;
    long closed_at;
} Ticket;

// Ticket message structure
typedef struct {
    int id;
    int ticket_id;
    u64_snowflake_t author_id;
    u64_snowflake_t message_id;  // Discord message ID for tracking edits/deletes
    char *content;
    char *attachments_json;  // JSON array of attachment URLs
    bool from_user;  // true if from user, false if from moderator
    bool is_deleted;
    bool is_edited;
    long timestamp;
    long edited_at;
} TicketMessage;

// Server configuration structure
typedef struct {
    u64_snowflake_t main_guild_id;
    u64_snowflake_t staff_guild_id;
    u64_snowflake_t ticket_category_id;   // Category in staff server for tickets
    u64_snowflake_t log_channel_id;       // Channel for ticket logs
} ServerConfig;

// Initialise the ticket module
void ticket_module_init(struct discord *client, Database *db);

// Register ticket slash commands
void register_ticket_commands(struct discord *client,
                               u64_snowflake_t application_id,
                               u64_snowflake_t guild_id);

// Interaction handler for ticket commands
void on_ticket_interaction(struct discord *client,
                           const struct discord_interaction *event);

// Message handler for ticket messages (DMs and staff channels)
void on_ticket_message(struct discord *client,
                       const struct discord_message *event);

// Message update handler for editing
void on_ticket_message_update(struct discord *client,
                               const struct discord_message *event);

// Message delete handler
void on_ticket_message_delete(struct discord *client,
                               u64_snowflake_t message_id,
                               u64_snowflake_t channel_id);

// Database operations for tickets
int db_create_ticket(Database *db, u64_snowflake_t user_id,
                     u64_snowflake_t main_guild_id,
                     u64_snowflake_t staff_guild_id,
                     u64_snowflake_t staff_channel_id,
                     u64_snowflake_t dm_channel_id);

int db_get_open_ticket_for_user(Database *db, u64_snowflake_t user_id, Ticket *ticket);
int db_get_ticket_by_channel(Database *db, u64_snowflake_t channel_id, Ticket *ticket);
int db_close_ticket(Database *db, int ticket_id);

int db_add_ticket_message(Database *db, int ticket_id, u64_snowflake_t message_id,
                           u64_snowflake_t author_id, const char *content, 
                           const char *attachments_json, bool from_user);
int db_update_ticket_message(Database *db, u64_snowflake_t message_id, const char *new_content);
int db_delete_ticket_message(Database *db, u64_snowflake_t message_id);
int db_get_ticket_messages(Database *db, int ticket_id,
                            TicketMessage **messages, int *count);
void db_free_ticket_messages(TicketMessage *messages, int count);

// Server configuration operations
int db_set_server_config(Database *db, u64_snowflake_t main_guild_id,
                         u64_snowflake_t staff_guild_id,
                         u64_snowflake_t ticket_category_id,
                         u64_snowflake_t log_channel_id);
int db_get_server_config(Database *db, u64_snowflake_t main_guild_id,
                         ServerConfig *config);

// Utility functions
char* generate_ticket_html(Database *db, int ticket_id, const char *username);

#endif // TICKET_H