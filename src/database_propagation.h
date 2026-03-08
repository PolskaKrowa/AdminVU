#ifndef DATABASE_PROPAGATION_H
#define DATABASE_PROPAGATION_H

/*
 * database_propagation.h
 *
 * Database helpers for the cross-server warning-propagation system.
 *
 * ASSUMPTION: The Database struct (defined in database.h) contains a field
 *   sqlite3 *db;
 * as its raw handle.  Adjust the field name in database_propagation.c if
 * your implementation differs.
 */

#include "database.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Data types ─────────────────────────────────────────────────────────── */

typedef struct {
    int64_t  id;
    uint64_t target_user_id;
    uint64_t source_guild_id;
    uint64_t moderator_id;
    char    *reason;
    char    *evidence_url;
    int64_t  timestamp;
} PropagationEvent;

/* ── Table creation ─────────────────────────────────────────────────────── */

/* Call once during bot startup (after db_init).
 * Creates all propagation-related tables if they do not already exist.
 * Returns 0 on success, -1 on failure. */
int db_propagation_init(Database *db);

/* ── Propagation events ─────────────────────────────────────────────────── */

/* Insert a new propagation event.
 * Returns the new row's id (>0) on success, or -1 on failure. */
int64_t db_add_propagation_event(Database *db,
                                  uint64_t    target_user_id,
                                  uint64_t    source_guild_id,
                                  uint64_t    moderator_id,
                                  const char *reason,
                                  const char *evidence_url);

/* Record that a specific guild received a notification for an event.
 * Returns 0 on success, -1 on failure. */
int db_record_propagation_notification(Database *db,
                                        int64_t  propagation_id,
                                        uint64_t guild_id);

/* Retrieve all propagation events for a given user (newest first).
 * Caller must free *events_out with db_free_propagation_events().
 * Returns 0 on success, -1 on failure. */
int db_get_propagation_events(Database          *db,
                               uint64_t           target_user_id,
                               PropagationEvent **events_out,
                               int               *count_out);

void db_free_propagation_events(PropagationEvent *events, int count);

/* ── Per-guild configuration ─────────────────────────────────────────────── */

/* Upsert a guild's notification channel and opt-in state.
 * Pass opted_in = 0 to disable alert delivery to this guild.
 * Returns 0 on success, -1 on failure. */
int db_set_propagation_config(Database *db,
                               uint64_t  guild_id,
                               uint64_t  channel_id,
                               int       opted_in);

/* Returns the configured notification channel, or 0 if none is set. */
uint64_t db_get_propagation_channel(Database *db, uint64_t guild_id);

/* Returns true if the guild has opted in and has a channel configured. */
bool db_guild_propagation_opted_in(Database *db, uint64_t guild_id);

/* Returns an array of all opted-in guild IDs.
 * Caller must free(*guild_ids_out) with free().
 * Returns 0 on success, -1 on failure. */
int db_get_opted_in_guilds(Database  *db,
                            uint64_t **guild_ids_out,
                            int       *count_out);

/* ── Moderator blacklist ─────────────────────────────────────────────────── */

/* Permanently blacklist a moderator from using /propagate.
 * Returns 0 on success, -1 on failure. */
int db_blacklist_moderator(Database   *db,
                            uint64_t    moderator_id,
                            uint64_t    banned_by,
                            const char *reason);

/* Returns true if the moderator is on the blacklist. */
bool db_is_moderator_blacklisted(Database *db, uint64_t moderator_id);

/* ── Known-guild registry ────────────────────────────────────────────────── */

/* Register (or re-confirm) a guild that the bot is active in.
 * Used so the propagation system knows which guilds to check.
 * Returns 0 on success, -1 on failure. */
int db_register_known_guild(Database *db, uint64_t guild_id);

#endif /* DATABASE_PROPAGATION_H */