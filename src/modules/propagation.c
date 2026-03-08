/*
 * propagation.c
 *
 * Cross-server warning-propagation module.
 *
 * Design summary
 * ──────────────
 *  • A moderator with kick/ban permissions runs /propagate and supplies:
 *      - the target user
 *      - a plain-text reason
 *      - a URL pointing to evidence (screenshot, video, etc.)
 *      - the exact phrase "I UNDERSTAND THE CONSEQUENCES" as confirmation
 *
 *  • The bot then:
 *      1. Checks the moderator is not blacklisted.
 *      2. Persists the event to the database.
 *      3. Iterates every opted-in guild, attempts to fetch the target user
 *         as a member, and – if found – posts a rich alert embed to that
 *         guild's configured notification channel.
 *      4. Importantly, NO automatic action is taken; moderators in the
 *         receiving guilds decide what to do.
 *
 *  • Misuse handling:
 *      - Server admins can run /propagate-revoke to permanently blacklist
 *        a moderator from using the system.
 *      - Every use is logged; the issuing guild's ID is always included in
 *        the notification so receiving guilds can verify with the source.
 */

#include "propagation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

/* ── Module-private state ────────────────────────────────────────────────── */

static Database        *g_db          = NULL;
static struct discord  *g_client      = NULL;
static uint64_t         g_self_guild  = 0;   /* the bot's "home" guild */

/* The exact confirmation string a moderator must type to fire an alert. */
#define CONFIRM_PHRASE "I UNDERSTAND THE CONSEQUENCES"

/* ── Permission helpers ──────────────────────────────────────────────────── */

static bool has_mod_permissions(const struct discord_interaction *event) {
    if (!event->member || !event->member->permissions) return false;
    uint64_t perms = strtoull(event->member->permissions, NULL, 10);
    uint64_t req   = DISCORD_PERMISSION_KICK_MEMBERS
                   | DISCORD_PERMISSION_BAN_MEMBERS
                   | DISCORD_PERMISSION_MODERATE_MEMBERS;
    return (perms & req) != 0;
}

static bool has_admin_permissions(const struct discord_interaction *event) {
    if (!event->member || !event->member->permissions) return false;
    uint64_t perms = strtoull(event->member->permissions, NULL, 10);
    return (perms & DISCORD_PERMISSION_ADMINISTRATOR) != 0
        || (perms & DISCORD_PERMISSION_MANAGE_GUILD)  != 0;
}

/* ── Option extraction ───────────────────────────────────────────────────── */

static const char *get_option(const struct discord_interaction *event,
                              const char                       *name) {
    if (!event->data || !event->data->options) return NULL;
    for (int i = 0; event->data->options[i]; i++) {
        if (strcmp(event->data->options[i]->name, name) == 0)
            return event->data->options[i]->value;
    }
    return NULL;
}

/* ── Simple response helpers ─────────────────────────────────────────────── */

static void send_ephemeral(struct discord                  *client,
                            const struct discord_interaction *event,
                            const char                       *msg) {
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

static void send_public(struct discord                  *client,
                         const struct discord_interaction *event,
                         const char                       *msg) {
    struct discord_interaction_response resp = {
        .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = (char *)msg,
        },
    };
    discord_create_interaction_response(client, event->id, event->token,
                                        &resp, NULL);
}

/* ── Cross-guild notification delivery ──────────────────────────────────── */

/*
 * Attempt to post an alert to a single guild's configured channel.
 * Returns true if the notification was successfully delivered.
 */
static bool notify_guild(struct discord  *client,
                          uint64_t         guild_id,
                          uint64_t         target_user_id,
                          uint64_t         source_guild_id,
                          uint64_t         moderator_id,
                          const char      *reason,
                          const char      *evidence_url,
                          int64_t          propagation_id,
                          int64_t          timestamp) {
    /* Verify the target user is actually in this guild before notifying.
     * Orca's discord_get_guild_member takes a struct by pointer (single
     * indirection) and is cleaned up with discord_guild_member_cleanup(). */
    struct discord_guild_member member = { 0 };
    ORCAcode rc = discord_get_guild_member(client, guild_id,
                                           target_user_id, &member);
    if (rc != ORCA_OK) {
        /* User is not in this guild, or the API call failed – skip silently. */
        return false;
    }
    discord_guild_member_cleanup(&member);

    uint64_t channel_id = db_get_propagation_channel(g_db, guild_id);
    if (!channel_id) return false;

    /* Build the notification message. */
    char time_buf[64] = { 0 };
    struct tm *tm_info = gmtime(&(time_t){ (time_t)timestamp });
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S UTC", tm_info);

    char msg[2000];
    snprintf(msg, sizeof(msg),
             "━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
             "⚠️  **CROSS-SERVER ALERT**  ⚠️\n"
             "━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
             "A moderator in another server has flagged a user who is "
             "**also a member of your server**.\n\n"
             "**User:** <@%" PRIu64 "> (`%" PRIu64 "`)\n"
             "**Reason:** %s\n"
             "**Evidence:** %s\n"
             "**Source server ID:** `%" PRIu64 "`\n"
             "**Reported by:** <@%" PRIu64 ">\n"
             "**Time:** %s\n"
             "**Alert ID:** `#%" PRId64 "`\n\n"
             "```\n"
             "No automatic action has been taken.\n"
             "Please review the evidence and decide what — if anything —\n"
             "is appropriate for your own community.\n"
             "```\n"
             "To verify this alert, contact the source server's admins "
             "and quote Alert ID #%" PRId64 ".",
             target_user_id, target_user_id,
             reason        ? reason        : "*(no reason provided)*",
             evidence_url  ? evidence_url  : "*(no URL provided)*",
             source_guild_id,
             moderator_id,
             time_buf,
             propagation_id,
             propagation_id);

    struct discord_create_message_params params = {
        .content = msg,
    };

    ORCAcode post_rc = discord_create_message(client, channel_id, &params, NULL);
    if (post_rc != ORCA_OK) {
        fprintf(stderr,
                "[propagation] Failed to post to channel %" PRIu64
                " in guild %" PRIu64 " (ORCAcode %d)\n",
                channel_id, guild_id, post_rc);
        return false;
    }
    return true;
}

/* ── Command handlers ────────────────────────────────────────────────────── */

/*
 * /propagate
 *   user     – the target member (USER option)
 *   reason   – what they did           (STRING, required)
 *   evidence – URL to proof            (STRING, required)
 *   confirm  – must equal CONFIRM_PHRASE (STRING, required)
 */
static void handle_propagate(struct discord                  *client,
                              const struct discord_interaction *event) {
    /* ── 1. Permission check ─────────────────────────────────────────────── */
    if (!has_mod_permissions(event)) {
        send_ephemeral(client, event,
                       "❌ You need Kick Members, Ban Members, or Moderate Members "
                       "permission to use this command.");
        return;
    }

    uint64_t moderator_id = event->member ? event->member->user->id : 0;

    /* ── 2. Blacklist check ──────────────────────────────────────────────── */
    if (db_is_moderator_blacklisted(g_db, moderator_id)) {
        send_ephemeral(client, event,
                       "🚫 You have been **blacklisted** from the propagation system "
                       "due to previous misuse.  You may no longer issue cross-server "
                       "alerts.  Contact a server administrator if you believe this "
                       "is in error.");
        return;
    }

    /* ── 3. Parse options ────────────────────────────────────────────────── */
    const char *user_id_str  = get_option(event, "user");
    const char *reason       = get_option(event, "reason");
    const char *evidence_url = get_option(event, "evidence");
    const char *confirm      = get_option(event, "confirm");

    if (!user_id_str) {
        send_ephemeral(client, event, "❌ You must specify a target user.");
        return;
    }
    if (!evidence_url || strlen(evidence_url) < 10) {
        send_ephemeral(client, event,
                       "❌ You must supply a valid evidence URL "
                       "(screenshot, video link, etc.).");
        return;
    }

    /* ── 4. Confirmation phrase check ───────────────────────────────────── */
    if (!confirm || strcmp(confirm, CONFIRM_PHRASE) != 0) {
        char notice[512];
        snprintf(notice, sizeof(notice),
                 "❌ **Confirmation required.**\n\n"
                 "Cross-server alerts are serious.  They notify moderators "
                 "in every opted-in server where the target user is a member.\n\n"
                 "**Misusing this system will result in your permanent removal "
                 "from it.**\n\n"
                 "To proceed, set the `confirm` option to exactly:\n"
                 "```\n" CONFIRM_PHRASE "\n```");
        send_ephemeral(client, event, notice);
        return;
    }

    uint64_t target_id = (uint64_t)strtoull(user_id_str, NULL, 10);
    if (target_id == moderator_id) {
        send_ephemeral(client, event, "❌ You cannot propagate an alert against yourself.");
        return;
    }

    /* ── 5. Persist the event ────────────────────────────────────────────── */
    int64_t event_id = db_add_propagation_event(g_db,
                                                 target_id,
                                                 event->guild_id,
                                                 moderator_id,
                                                 reason,
                                                 evidence_url);
    if (event_id < 0) {
        send_ephemeral(client, event,
                       "❌ Database error: failed to store the propagation event.  "
                       "Please try again.");
        return;
    }

    /* Acknowledge immediately so Discord doesn't time out. */
    char ack[256];
    snprintf(ack, sizeof(ack),
             "✅ Alert #%" PRId64 " recorded.  Notifying opted-in servers now…",
             event_id);
    send_ephemeral(client, event, ack);

    /* ── 6. Deliver notifications ─────────────────────────────────────────── */
    uint64_t *opted_guilds = NULL;
    int        guild_count  = 0;
    db_get_opted_in_guilds(g_db, &opted_guilds, &guild_count);

    int notified = 0;
    for (int i = 0; i < guild_count; i++) {
        /* Never notify the source guild about its own alert. */
        if (opted_guilds[i] == event->guild_id) continue;

        bool delivered = notify_guild(client,
                                       opted_guilds[i],
                                       target_id,
                                       event->guild_id,
                                       moderator_id,
                                       reason,
                                       evidence_url,
                                       event_id,
                                       (int64_t)time(NULL));
        if (delivered) {
            db_record_propagation_notification(g_db, event_id, opted_guilds[i]);
            notified++;
        }
    }
    free(opted_guilds);

    printf("[propagation] Alert #%" PRId64 ": target=%" PRIu64
           ", notified %d guild(s)\n",
           event_id, target_id, notified);
}

/* ── /propagate-config ───────────────────────────────────────────────────── */

static void handle_config(struct discord                  *client,
                           const struct discord_interaction *event) {
    if (!has_admin_permissions(event)) {
        send_ephemeral(client, event,
                       "❌ You need Administrator or Manage Server permission "
                       "to configure the propagation system.");
        return;
    }

    const char *channel_str = get_option(event, "channel");
    if (!channel_str) {
        send_ephemeral(client, event, "❌ Please specify a channel.");
        return;
    }

    uint64_t channel_id = (uint64_t)strtoull(channel_str, NULL, 10);
    if (db_set_propagation_config(g_db, event->guild_id, channel_id, 1) != 0) {
        send_ephemeral(client, event,
                       "❌ Database error: could not save config.");
        return;
    }

    char msg[256];
    snprintf(msg, sizeof(msg),
             "✅ Cross-server alerts will now be posted to <#%" PRIu64 ">.\n"
             "Your server is **opted in** to the propagation network.",
             channel_id);
    send_ephemeral(client, event, msg);
}

/* ── /propagate-opt-out ─────────────────────────────────────────────────── */

static void handle_opt_out(struct discord                  *client,
                            const struct discord_interaction *event) {
    if (!has_admin_permissions(event)) {
        send_ephemeral(client, event,
                       "❌ You need Administrator or Manage Server permission.");
        return;
    }

    /* Keep whatever channel is already set, just flip opted_in to 0. */
    uint64_t existing_channel = db_get_propagation_channel(g_db, event->guild_id);
    db_set_propagation_config(g_db, event->guild_id,
                               existing_channel ? existing_channel : 0, 0);

    send_ephemeral(client, event,
                   "✅ This server has **opted out** of the propagation network.\n"
                   "You will no longer receive cross-server alerts.\n"
                   "Run `/propagate-config` at any time to rejoin.");
}

/* ── /propagate-history ─────────────────────────────────────────────────── */

static void handle_history(struct discord                  *client,
                             const struct discord_interaction *event) {
    if (!has_mod_permissions(event)) {
        send_ephemeral(client, event,
                       "❌ You need moderation permissions to view alert history.");
        return;
    }

    const char *user_str = get_option(event, "user");
    if (!user_str) {
        send_ephemeral(client, event, "❌ Please specify a user.");
        return;
    }
    uint64_t target_id = (uint64_t)strtoull(user_str, NULL, 10);

    PropagationEvent *events = NULL;
    int               count  = 0;
    if (db_get_propagation_events(g_db, target_id, &events, &count) != 0) {
        send_ephemeral(client, event, "❌ Database error retrieving history.");
        return;
    }

    char response[2000];
    if (count == 0) {
        snprintf(response, sizeof(response),
                 "📋 No cross-server alerts found for <@%" PRIu64 ">.", target_id);
    } else {
        snprintf(response, sizeof(response),
                 "📋 **Cross-server alert history for <@%" PRIu64 ">** "
                 "(%" PRId32 " record%s)\n\n",
                 target_id, count, count == 1 ? "" : "s");

        for (int i = 0; i < count && i < 8; i++) {
            char time_buf[64] = { 0 };
            struct tm *tm_info = gmtime(&(time_t){ (time_t)events[i].timestamp });
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M UTC", tm_info);

            char entry[400];
            snprintf(entry, sizeof(entry),
                     "**Alert #%" PRId64 "** – %s\n"
                     "  Server: `%" PRIu64 "` | Mod: <@%" PRIu64 ">\n"
                     "  Reason: %s\n"
                     "  Evidence: %s\n\n",
                     events[i].id, time_buf,
                     events[i].source_guild_id, events[i].moderator_id,
                     events[i].reason       ? events[i].reason       : "*(none)*",
                     events[i].evidence_url ? events[i].evidence_url : "*(none)*");

            strncat(response, entry, sizeof(response) - strlen(response) - 1);
        }

        if (count > 8) {
            char tail[64];
            snprintf(tail, sizeof(tail), "*…and %d more*", count - 8);
            strncat(response, tail, sizeof(response) - strlen(response) - 1);
        }
    }

    db_free_propagation_events(events, count);
    send_ephemeral(client, event, response);
}

/* ── /propagate-revoke ──────────────────────────────────────────────────── */

static void handle_revoke(struct discord                  *client,
                           const struct discord_interaction *event) {
    if (!has_admin_permissions(event)) {
        send_ephemeral(client, event,
                       "❌ Only administrators can revoke propagation access.");
        return;
    }

    const char *mod_str = get_option(event, "moderator");
    const char *reason  = get_option(event, "reason");

    if (!mod_str) {
        send_ephemeral(client, event, "❌ Please specify a moderator.");
        return;
    }

    uint64_t mod_id    = (uint64_t)strtoull(mod_str, NULL, 10);
    uint64_t admin_id  = event->member ? event->member->user->id : 0;

    if (mod_id == admin_id) {
        send_ephemeral(client, event, "❌ You cannot revoke your own access.");
        return;
    }

    if (db_blacklist_moderator(g_db, mod_id, admin_id, reason) != 0) {
        send_ephemeral(client, event, "❌ Database error: could not save blacklist entry.");
        return;
    }

    char msg[512];
    snprintf(msg, sizeof(msg),
             "🔒 <@%" PRIu64 "> has been **permanently blacklisted** from the "
             "cross-server propagation system.\n"
             "**Reason:** %s\n\n"
             "They can no longer issue `/propagate` alerts.",
             mod_id, reason ? reason : "*(no reason given)*");
    send_public(client, event, msg);

    printf("[propagation] Moderator %" PRIu64 " blacklisted by %" PRIu64 "\n",
           mod_id, admin_id);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void on_propagation_interaction(struct discord                  *client,
                                 const struct discord_interaction *event) {
    if (!event->data) return;
    if (event->type != DISCORD_INTERACTION_APPLICATION_COMMAND) return;

    /* Auto-register this guild whenever we receive an interaction from it. */
    if (event->guild_id)
        db_register_known_guild(g_db, event->guild_id);

    const char *cmd = event->data->name;

    if      (strcmp(cmd, "propagate")         == 0) handle_propagate(client, event);
    else if (strcmp(cmd, "propagate-config")  == 0) handle_config   (client, event);
    else if (strcmp(cmd, "propagate-opt-out") == 0) handle_opt_out  (client, event);
    else if (strcmp(cmd, "propagate-history") == 0) handle_history  (client, event);
    else if (strcmp(cmd, "propagate-revoke")  == 0) handle_revoke   (client, event);
}

void propagation_on_guild_register(uint64_t guild_id) {
    if (g_db && guild_id)
        db_register_known_guild(g_db, guild_id);
}

void register_propagation_commands(struct discord *client,
                                    uint64_t        application_id,
                                    uint64_t        guild_id) {
    /* ── /propagate ─────────────────────────────────────────────────────── */
    static struct discord_application_command_option prop_user = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_USER,
        .name        = "user",
        .description = "The user to flag across servers",
        .required    = true,
    };
    static struct discord_application_command_option prop_reason = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "reason",
        .description = "What did this user do?",
        .required    = true,
    };
    static struct discord_application_command_option prop_evidence = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "evidence",
        .description = "URL to evidence (screenshot, video, etc.) — required",
        .required    = true,
    };
    static struct discord_application_command_option prop_confirm = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "confirm",
        .description = "Type: I UNDERSTAND THE CONSEQUENCES",
        .required    = true,
    };
    static struct discord_application_command_option *prop_opts[] = {
        &prop_user, &prop_reason, &prop_evidence, &prop_confirm, NULL
    };
    static struct discord_create_guild_application_command_params prop_params = {
        .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name        = "propagate",
        .description = "Flag a user across all opted-in servers (mod only — misuse = ban)",
        .options     = prop_opts,
    };
    discord_create_guild_application_command(client, application_id, guild_id,
                                              &prop_params, NULL);

    /* ── /propagate-config ─────────────────────────────────────────────── */
    /* Note: DISCORD_APPLICATION_COMMAND_OPTION_CHANNEL is not available in
     * all Orca builds.  We accept the channel ID as a plain STRING instead;
     * moderators paste the channel's ID (right-click → Copy ID). */
    static struct discord_application_command_option cfg_channel = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "channel_id",
        .description = "ID of the channel to post alerts in (right-click channel → Copy ID)",
        .required    = true,
    };
    static struct discord_application_command_option *cfg_opts[] = {
        &cfg_channel, NULL
    };
    static struct discord_create_guild_application_command_params cfg_params = {
        .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name        = "propagate-config",
        .description = "Set the channel for incoming cross-server alerts (admin only)",
        .options     = cfg_opts,
    };
    discord_create_guild_application_command(client, application_id, guild_id,
                                              &cfg_params, NULL);

    /* ── /propagate-opt-out ────────────────────────────────────────────── */
    static struct discord_create_guild_application_command_params optout_params = {
        .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name        = "propagate-opt-out",
        .description = "Stop receiving cross-server alerts in this server (admin only)",
        .options     = NULL,
    };
    discord_create_guild_application_command(client, application_id, guild_id,
                                              &optout_params, NULL);

    /* ── /propagate-history ────────────────────────────────────────────── */
    static struct discord_application_command_option hist_user = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_USER,
        .name        = "user",
        .description = "The user to look up",
        .required    = true,
    };
    static struct discord_application_command_option *hist_opts[] = {
        &hist_user, NULL
    };
    static struct discord_create_guild_application_command_params hist_params = {
        .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name        = "propagate-history",
        .description = "View all cross-server alerts for a user (mod only)",
        .options     = hist_opts,
    };
    discord_create_guild_application_command(client, application_id, guild_id,
                                              &hist_params, NULL);

    /* ── /propagate-revoke ─────────────────────────────────────────────── */
    static struct discord_application_command_option rev_mod = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_USER,
        .name        = "moderator",
        .description = "The moderator to blacklist",
        .required    = true,
    };
    static struct discord_application_command_option rev_reason = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "reason",
        .description = "Why is their access being revoked?",
        .required    = true,
    };
    static struct discord_application_command_option *rev_opts[] = {
        &rev_mod, &rev_reason, NULL
    };
    static struct discord_create_guild_application_command_params rev_params = {
        .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name        = "propagate-revoke",
        .description = "Permanently revoke a moderator's ability to issue alerts (admin only)",
        .options     = rev_opts,
    };
    discord_create_guild_application_command(client, application_id, guild_id,
                                              &rev_params, NULL);

    printf("[propagation] All propagation commands registered for guild %" PRIu64 "\n",
           guild_id);
}

void propagation_module_init(struct discord *client,
                              Database       *db,
                              uint64_t        self_guild_id) {
    g_client     = client;
    g_db         = db;
    g_self_guild = self_guild_id;

    if (db_propagation_init(db) != 0)
        fprintf(stderr, "[propagation] WARNING: DB table init failed.\n");

    /* Register the bot's home guild immediately. */
    if (self_guild_id)
        db_register_known_guild(db, self_guild_id);

    printf("[propagation] Propagation module initialised.\n");
}