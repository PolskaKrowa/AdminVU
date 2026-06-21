#ifndef PROPAGATION_H
#define PROPAGATION_H

/*
 * propagation.h
 *
 * Cross-server warning-propagation module.
 *
 * Slash commands exposed:
 *   /propagate          – fire a cross-server alert (mod-only)
 *   /propagate-config   – set this guild's alert-notification channel (admin)
 *   /propagate-opt-out  – stop receiving alerts from other servers (admin)
 *   /propagate-history  – view all propagation events for a user (mod)
 *   /propagate-revoke   – blacklist a moderator from the system (admin)
 */

#include <orca/discord.h>
#include "../database.h"
#include "../database_propagation.h"

/* Initialise the module (call after db_propagation_init). */
void propagation_module_init(struct discord *client,
                              Database       *db,
                              uint64_t        self_guild_id);

/*
 * propagation_module_shutdown
 *
 * Signals the background poll thread (started in propagation_module_init)
 * to stop and joins it.  MUST be called before db_cleanup() so the poll
 * thread does not race with sqlite3_close() and crash on a freed handle.
 *
 * Safe to call even if propagation_module_init() was never called or the
 * thread failed to start — it is idempotent.
 */
void propagation_module_shutdown(void);

/* Register all propagation-related slash commands for a guild. */
void register_propagation_commands(struct discord *client,
                                    uint64_t        application_id,
                                    uint64_t        guild_id);

/* Top-level interaction router – called from main's combined handler. */
void on_propagation_interaction(struct discord                  *client,
                                 const struct discord_interaction *event);

/* Call from on_guild_create so the module tracks every server the bot joins. */
void propagation_on_guild_register(uint64_t guild_id);

#endif /* PROPAGATION_H */