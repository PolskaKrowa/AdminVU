/*
 * propagation.c
 *
 * Cross-server warning-propagation module.
 *
 * New commands in this revision
 * ─────────────────────────────
 *  /propagate-report   [alert_id] [reason]
 *      Staff can flag an alert as suspicious or false.  When the report
 *      threshold (REPORT_THRESHOLD) is reached, the central admin server
 *      is notified automatically.
 *
 *  /propagate-appeal   [alert_id] [statement]
 *      Any user who has been the subject of a propagation alert may submit
 *      an appeal.  The bot also DMs the user when the original alert fires,
 *      so they are aware they can appeal.
 *
 *  /propagate-appeal-review   [appeal_id] [approve|deny] [notes]
 *      Admins review pending appeals.  All opted-in guilds that received
 *      the original alert receive a status-update message when the outcome
 *      is decided.
 *
 *  /propagate-trust    [guild_id] [level] [notes]
 *      Set a guild's trust level (affects severity weighting).
 *      Restricted to central-admin guild members only.
 *
 *  /propagate-central  [guild_id] [channel_id]
 *      Designate the central oversight server + channel.
 *      Can only be run by the bot owner / application team.
 *
 * Unchanged commands
 * ──────────────────
 *  /propagate          /propagate-config   /propagate-opt-out
 *  /propagate-history  /propagate-revoke
 *
 * Notification format additions
 * ──────────────────────────────
 *  • Source-server trust badge
 *  • Current severity level (recalculated after each delivery)
 *  • Moderator raw ID (displayed alongside mention so it survives renames)
 *  • Appeal status footer (live: injected from DB at notification build time)
 */

#include "propagation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

/* ── Tunables ────────────────────────────────────────────────────────────── */

/* Number of staff reports before the central server is auto-notified. */
#define REPORT_THRESHOLD 3

/* The exact confirmation phrase a moderator must type. */
#define CONFIRM_PHRASE "I UNDERSTAND THE CONSEQUENCES"

/* ── Module-private state ────────────────────────────────────────────────── */

static Database       *g_db          = NULL;
static struct discord *g_client      = NULL;
static uint64_t        g_self_guild  = 0;

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

/*
 * Central-admin-only actions (trust assignment, central config) are
 * restricted to members of the designated central guild.
 */
static bool is_central_admin(const struct discord_interaction *event) {
    if (!has_admin_permissions(event)) return false;

    /*
     * Any guild registered as a staff server for any community pair
     * qualifies for central-admin actions (trust assignment, etc.).
     */
    if (db_is_staff_guild(g_db, event->guild_id)) return true;

    /* Also honour the explicitly configured central oversight guild. */
    uint64_t central_guild = 0, central_channel = 0;
    if (db_get_central_config(g_db, &central_guild, &central_channel))
        return event->guild_id == central_guild;

    return false;
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

/* ── Response helpers ────────────────────────────────────────────────────── */

static void send_ephemeral(struct discord                  *client,
                            const struct discord_interaction *event,
                            const char                       *msg) {
    struct discord_interaction_response resp = {
        .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = (char *)msg,
            .flags   = 1 << 6,
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

/* Post a plain message to any channel, used for follow-up dispatches. */
static void post_to_channel(struct discord *client,
                              uint64_t        channel_id,
                              const char     *msg) {
    struct discord_create_message_params params = { .content = (char *)msg };
    ORCAcode rc = discord_create_message(client, channel_id, &params, NULL);
    if (rc != ORCA_OK)
        fprintf(stderr, "[propagation] post_to_channel %" PRIu64 " failed: %d\n",
                channel_id, rc);
}

/* ── DM helpers ──────────────────────────────────────────────────────────── */

/*
 * Attempt to open a DM channel with a user and send them a message.
 * Silently ignores failures (user may have DMs disabled).
 */
static void dm_user(struct discord *client,
                    uint64_t        user_id,
                    const char     *msg) {
    struct discord_create_dm_params dm_params = {
        .recipient_id = user_id,
    };
    struct discord_channel dm_channel = { 0 };
    ORCAcode rc = discord_create_dm(client, &dm_params, &dm_channel);
    if (rc != ORCA_OK || !dm_channel.id) {
        discord_channel_cleanup(&dm_channel);
        return;
    }

    post_to_channel(client, dm_channel.id, msg);
    discord_channel_cleanup(&dm_channel);
}

/* ── Central-server notification ─────────────────────────────────────────── */

static void notify_central(struct discord *client,
                             const char     *msg) {
    uint64_t central_guild = 0, central_channel = 0;
    if (!db_get_central_config(g_db, &central_guild, &central_channel))
        return;
    if (!central_channel) return;
    post_to_channel(client, central_channel, msg);
}

/* ── Build the notification embed text ──────────────────────────────────── */

/*
 * Constructs the full alert message for a given event.
 * appeal_status is included as a live footer; pass NULL to omit it.
 */
static void build_alert_message(char                    *buf,
                                  size_t                   buflen,
                                  const PropagationEvent  *ev,
                                  GuildTrustLevel          source_trust,
                                  const char              *appeal_line) {
    char time_buf[64] = { 0 };
    struct tm *tm_info = gmtime(&(time_t){ (time_t)ev->timestamp });
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S UTC", tm_info);

    snprintf(buf, buflen,
             "━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
             "%s **CROSS-SERVER ALERT** – %s\n"
             "━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
             "A moderator in another participating server has reported behaviour that may affect other communities.\n\n"
             "**User:** <@%" PRIu64 "> (`%" PRIu64 "`)\n"
             "**Reason:** %s\n"
             "**Evidence:** %s\n"
             "**Source server ID:** `%" PRIu64 "`  %s %s\n"
             "**Reported by:** <@%" PRIu64 "> (ID: `%" PRIu64 "`)\n"
             "**Time:** %s\n"
             "**Alert ID:** `#%" PRId64 "`\n"
             "**Severity:** %s %s  |  **Confirmations:** %d server(s)\n"
             "**Reports:** %d\n"
             "%s"
             "\n```\n"
             "No automatic action has been taken.\n"
             "Please review the evidence and decide what — if anything — "
             "is appropriate for your own community.\n"
             "```\n"
             "To verify this alert, contact the source server's admins "
             "and quote Alert ID #%" PRId64 ".",
             severity_emoji((PropagationSeverity)ev->severity),
             severity_name ((PropagationSeverity)ev->severity),
             ev->target_user_id, ev->target_user_id,
             ev->reason       ? ev->reason       : "*(no reason provided)*",
             ev->evidence_url ? ev->evidence_url : "*(no URL provided)*",
             ev->source_guild_id,
             trust_level_badge(source_trust),
             trust_level_name (source_trust),
             ev->moderator_id, ev->moderator_id,
             time_buf,
             ev->id,
             severity_emoji((PropagationSeverity)ev->severity),
             severity_name ((PropagationSeverity)ev->severity),
             ev->weighted_confirmation_score,
             ev->report_count,
             appeal_line ? appeal_line : "",
             ev->id);
}

/* ── Cross-guild notification delivery ──────────────────────────────────── */

static bool notify_guild(struct discord         *client,
                          uint64_t                guild_id,
                          const PropagationEvent *ev,
                          GuildTrustLevel         source_trust) {
    /* Only deliver if the user is actually a member of this guild. */
    struct discord_guild_member member = { 0 };
    ORCAcode rc = discord_get_guild_member(client, guild_id,
                                           ev->target_user_id, &member);
    if (rc != ORCA_OK) return false;
    discord_guild_member_cleanup(&member);

    uint64_t channel_id = db_get_propagation_channel(g_db, guild_id);
    if (!channel_id) return false;

    char msg[2048];
    build_alert_message(msg, sizeof(msg), ev, source_trust, NULL);

    struct discord_create_message_params params = { .content = msg };
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

/* ── Post appeal status updates to all notified guilds ──────────────────── */

static void broadcast_appeal_update(struct discord         *client,
                                     int64_t                 propagation_id,
                                     uint64_t                target_user_id,
                                     int64_t                 appeal_id,
                                     AppealStatus            new_status,
                                     const char             *reviewer_notes) {
    uint64_t *opted_guilds = NULL;
    int        guild_count  = 0;
    db_get_opted_in_guilds(g_db, &opted_guilds, &guild_count);

    char msg[1024];
    snprintf(msg, sizeof(msg),
             "📋 **Appeal update – Alert #%" PRId64 "**\n"
             "User: <@%" PRIu64 ">\n"
             "Appeal #%" PRId64 " is now: %s %s\n"
             "%s%s%s",
             propagation_id,
             target_user_id,
             appeal_id,
             appeal_status_emoji(new_status),
             appeal_status_name (new_status),
             reviewer_notes && *reviewer_notes
                 ? "**Reviewer note:** " : "",
             reviewer_notes && *reviewer_notes
                 ? reviewer_notes : "",
             reviewer_notes && *reviewer_notes
                 ? "\n" : "");

    for (int i = 0; i < guild_count; i++) {
        uint64_t ch = db_get_propagation_channel(g_db, opted_guilds[i]);
        if (ch) post_to_channel(client, ch, msg);
    }
    free(opted_guilds);

    /* Also notify the central server. */
    notify_central(client, msg);
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  Command handlers                                                          */
/* ══════════════════════════════════════════════════════════════════════════ */

/* ── /propagate ──────────────────────────────────────────────────────────── */

static void handle_propagate(struct discord                  *client,
                              const struct discord_interaction *event) {
    if (!has_mod_permissions(event)) {
        send_ephemeral(client, event,
                       "❌ You need Kick Members, Ban Members, or Moderate Members "
                       "permission to use this command.");
        return;
    }

    uint64_t moderator_id = event->member ? event->member->user->id : 0;

    if (db_is_moderator_blacklisted(g_db, moderator_id)) {
        send_ephemeral(client, event,
                       "🚫 You have been **blacklisted** from the propagation system "
                       "due to previous misuse.  You may no longer issue cross-server "
                       "alerts.  Contact a server administrator if you believe this "
                       "is in error.");
        return;
    }

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
        send_ephemeral(client, event,
                       "❌ You cannot propagate an alert against yourself.");
        return;
    }

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

    char ack[256];
    snprintf(ack, sizeof(ack),
             "✅ Alert #%" PRId64 " recorded.  Notifying opted-in servers now…",
             event_id);
    send_ephemeral(client, event, ack);

    /* ── Fetch the freshly-inserted event for the notification builder. ── */
    PropagationEvent ev = { 0 };
    db_get_propagation_event_by_id(g_db, event_id, &ev);
    GuildTrustLevel source_trust = db_get_guild_trust(g_db, event->guild_id);

    /* ── Deliver to opted-in guilds. ─────────────────────────────────── */
    uint64_t *opted_guilds = NULL;
    int        guild_count  = 0;
    db_get_opted_in_guilds(g_db, &opted_guilds, &guild_count);

    int notified = 0;
    for (int i = 0; i < guild_count; i++) {
        if (opted_guilds[i] == event->guild_id) continue;

        if (notify_guild(client, opted_guilds[i], &ev, source_trust)) {
            db_record_propagation_notification(g_db, event_id, opted_guilds[i]);
            notified++;
        }
    }
    free(opted_guilds);

    /* ── Deliver to the staff server paired with the source community,
     *     so staff can cross-reference open tickets for this user. ──── */
    uint64_t paired_staff = db_get_staff_guild_for(g_db, event->guild_id);
    if (paired_staff && paired_staff != event->guild_id) {
        if (notify_guild(client, paired_staff, &ev, source_trust)) {
            db_record_propagation_notification(g_db, event_id, paired_staff);
            notified++;
        }
    }

    /* Recompute severity now we know how many guilds confirmed it. */
    PropagationSeverity final_sev = db_recompute_severity(g_db, event_id);

    printf("[propagation] Alert #%" PRId64 ": target=%" PRIu64
           ", notified %d guild(s), severity=%s\n",
           event_id, target_id, notified, severity_name(final_sev));

    /* ── DM the target user so they can appeal. ──────────────────────── */
    char dm_msg[1024];
    snprintf(dm_msg, sizeof(dm_msg),
             "⚠️ **Cross-server alert issued against you**\n\n"
             "A moderator has raised a cross-server alert citing your account "
             "(Alert ID `#%" PRId64 "`).\n\n"
             "**Reason given:** %s\n"
             "**Evidence:** %s\n\n"
             "If you believe this alert is incorrect you may submit an appeal "
             "in any server where this bot is active:\n"
             "```\n/propagate-appeal alert_id:%" PRId64 " "
             "statement:<your explanation>\n```\n"
             "You will be notified here when your appeal status changes.",
             event_id,
             reason       ? reason       : "*(none provided)*",
             evidence_url ? evidence_url : "*(none provided)*",
             event_id);
    dm_user(client, target_id, dm_msg);

    /* ── Forward critical/high-severity alerts to the central server. ── */
    if (final_sev >= SEVERITY_HIGH) {
        char central_msg[1024];
        snprintf(central_msg, sizeof(central_msg),
                 "🚨 **High-severity propagation alert**\n"
                 "Alert ID: `#%" PRId64 "`\n"
                 "Severity: %s %s\n"
                 "Target: `%" PRIu64 "`\n"
                 "Source guild: `%" PRIu64 "` %s %s\n"
                 "Notified guilds (score): %d\n"
                 "Reason: %s",
                 event_id,
                 severity_emoji(final_sev), severity_name(final_sev),
                 target_id,
                 event->guild_id,
                 trust_level_badge(source_trust), trust_level_name(source_trust),
                 notified,
                 reason ? reason : "*(none)*");
        notify_central(client, central_msg);
    }

    db_free_propagation_event(&ev);
}

static void handle_pair(struct discord                  *client,
                         const struct discord_interaction *event) {
    if (event->guild_id != g_self_guild || !has_admin_permissions(event)) {
        send_ephemeral(client, event,
                       "❌ This command can only be run by the bot team "
                       "on the bot's home server.");
        return;
    }

    const char *main_str  = get_option(event, "main_guild_id");
    const char *staff_str = get_option(event, "staff_guild_id");

    if (!main_str || !staff_str) {
        send_ephemeral(client, event,
                       "❌ Please provide both a main guild ID and a staff guild ID.");
        return;
    }

    uint64_t main_id  = (uint64_t)strtoull(main_str,  NULL, 10);
    uint64_t staff_id = (uint64_t)strtoull(staff_str, NULL, 10);
    uint64_t admin_id = event->member ? event->member->user->id : 0;

    if (main_id == staff_id) {
        send_ephemeral(client, event,
                       "❌ The main guild and staff guild cannot be the same server.");
        return;
    }

    if (db_register_guild_pair(g_db, main_id, staff_id, admin_id) != 0) {
        send_ephemeral(client, event, "❌ Database error saving guild pair.");
        return;
    }

    /* Give the staff server PARTNER trust automatically. */
    db_set_guild_trust(g_db, staff_id, TRUST_PARTNER, admin_id,
                       "Automatic PARTNER trust – paired staff server");

    db_register_known_guild(g_db, main_id);
    db_register_known_guild(g_db, staff_id);

    char msg[256];
    snprintf(msg, sizeof(msg),
             "✅ Guild pair registered.\n"
             "Main: `%" PRIu64 "` ↔ Staff: `%" PRIu64 "`\n"
             "The staff server has been granted ⭐ Partner trust automatically.",
             main_id, staff_id);
    send_ephemeral(client, event, msg);

    printf("[propagation] Guild pair registered: main=%" PRIu64
           " staff=%" PRIu64 " by %" PRIu64 "\n",
           main_id, staff_id, admin_id);
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

    const char *channel_str = get_option(event, "channel_id");
    if (!channel_str) {
        send_ephemeral(client, event, "❌ Please specify a channel.");
        return;
    }

    uint64_t channel_id = (uint64_t)strtoull(channel_str, NULL, 10);
    if (db_set_propagation_config(g_db, event->guild_id, channel_id, 1) != 0) {
        send_ephemeral(client, event, "❌ Database error: could not save config.");
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
                 "(%d record%s)\n\n",
                 target_id, count, count == 1 ? "" : "s");

        for (int i = 0; i < count && i < 8; i++) {
            char time_buf[64] = { 0 };
            struct tm *tm_info = gmtime(&(time_t){ (time_t)events[i].timestamp });
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M UTC", tm_info);

            /* Look up appeal status for this specific event. */
            PropagationAppeal appeal = { 0 };
            bool has_appeal = (db_get_appeal_for_event(
                                    g_db, events[i].id, target_id, &appeal) == 0);

            GuildTrustLevel src_trust =
                db_get_guild_trust(g_db, events[i].source_guild_id);

            char entry[450];
            snprintf(entry, sizeof(entry),
                     "**Alert #%" PRId64 "** – %s\n"
                     "  %s %s  |  Server: `%" PRIu64 "` %s %s\n"
                     "  Mod: <@%" PRIu64 "> (`%" PRIu64 "`)\n"
                     "  Reason: %s\n"
                     "  Evidence: %s\n"
                     "  Reports: %d  |  Appeal: %s\n\n",
                     events[i].id, time_buf,
                     severity_emoji((PropagationSeverity)events[i].severity),
                     severity_name ((PropagationSeverity)events[i].severity),
                     events[i].source_guild_id,
                     trust_level_badge(src_trust),
                     trust_level_name (src_trust),
                     events[i].moderator_id, events[i].moderator_id,
                     events[i].reason       ? events[i].reason       : "*(none)*",
                     events[i].evidence_url ? events[i].evidence_url : "*(none)*",
                     events[i].report_count,
                     has_appeal
                         ? appeal_status_name(appeal.status)
                         : "None");

            strncat(response, entry, sizeof(response) - strlen(response) - 1);

            if (has_appeal) db_free_appeal(&appeal);
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

    uint64_t mod_id   = (uint64_t)strtoull(mod_str, NULL, 10);
    uint64_t admin_id = event->member ? event->member->user->id : 0;

    if (mod_id == admin_id) {
        send_ephemeral(client, event, "❌ You cannot revoke your own access.");
        return;
    }

    if (db_blacklist_moderator(g_db, mod_id, admin_id, reason) != 0) {
        send_ephemeral(client, event,
                       "❌ Database error: could not save blacklist entry.");
        return;
    }

    char msg[512];
    snprintf(msg, sizeof(msg),
             "🔒 <@%" PRIu64 "> (ID: `%" PRIu64 "`) has been **permanently "
             "blacklisted** from the cross-server propagation system.\n"
             "**Reason:** %s\n\n"
             "They can no longer issue `/propagate` alerts.",
             mod_id, mod_id,
             reason ? reason : "*(no reason given)*");
    send_public(client, event, msg);

    printf("[propagation] Moderator %" PRIu64 " blacklisted by %" PRIu64 "\n",
           mod_id, admin_id);
}

/* ── /propagate-report ──────────────────────────────────────────────────── */
/*
 * Staff in any opted-in server can flag an alert as suspicious or false.
 * Once REPORT_THRESHOLD unique guild+mod pairs have reported the same
 * alert, the central administration server is notified automatically.
 */
static void handle_report(struct discord                  *client,
                           const struct discord_interaction *event) {
    if (!has_mod_permissions(event)) {
        send_ephemeral(client, event,
                       "❌ You need moderation permissions to report an alert.");
        return;
    }

    const char *id_str = get_option(event, "alert_id");
    const char *reason = get_option(event, "reason");

    if (!id_str || !reason || !*reason) {
        send_ephemeral(client, event,
                       "❌ Please provide both an alert ID and a reason.");
        return;
    }

    int64_t  alert_id    = (int64_t)strtoll(id_str, NULL, 10);
    uint64_t mod_id      = event->member ? event->member->user->id : 0;
    uint64_t guild_id    = event->guild_id;

    /* Verify the alert exists. */
    PropagationEvent ev = { 0 };
    if (db_get_propagation_event_by_id(g_db, alert_id, &ev) != 0) {
        send_ephemeral(client, event, "❌ Alert not found.");
        return;
    }

    int64_t report_id = db_report_alert(g_db, alert_id, guild_id, mod_id, reason);
    if (report_id < 0) {
        send_ephemeral(client, event,
                       "❌ Could not record report.  You may have already "
                       "reported this alert from this server.");
        db_free_propagation_event(&ev);
        return;
    }

    int total_reports = db_get_alert_report_count(g_db, alert_id);

    char ack[256];
    snprintf(ack, sizeof(ack),
             "✅ Report #%" PRId64 " submitted for Alert #%" PRId64 ".\n"
             "Total reports on this alert: **%d**.",
             report_id, alert_id, total_reports);
    send_ephemeral(client, event, ack);

    /* Auto-escalate to central server when threshold is crossed. */
    if (total_reports == REPORT_THRESHOLD) {
        char central_msg[1024];
        snprintf(central_msg, sizeof(central_msg),
                 "⚠️ **Alert flagged for review – threshold reached**\n"
                 "Alert ID: `#%" PRId64 "`\n"
                 "Target: `%" PRIu64 "` | Source guild: `%" PRIu64 "`\n"
                 "Mod ID: `%" PRIu64 "`\n"
                 "Reason on alert: %s\n"
                 "**%d** separate servers have reported this alert as "
                 "suspicious or false.  Human review recommended.",
                 alert_id,
                 ev.target_user_id, ev.source_guild_id,
                 ev.moderator_id,
                 ev.reason ? ev.reason : "*(none)*",
                 total_reports);
        notify_central(client, central_msg);

        printf("[propagation] Alert #%" PRId64 " reached report threshold (%d)\n",
               alert_id, REPORT_THRESHOLD);
    }

    db_free_propagation_event(&ev);
}

/* ── /propagate-appeal ───────────────────────────────────────────────────── */
/*
 * Any user who has been the subject of a propagation alert may submit one
 * appeal per event.  The appeal is stored and its status is shown in
 * /propagate-history and in the central server feed.
 */
static void handle_appeal(struct discord                  *client,
                           const struct discord_interaction *event) {
    const char *id_str    = get_option(event, "alert_id");
    const char *statement = get_option(event, "statement");

    if (!id_str || !statement || !*statement) {
        send_ephemeral(client, event,
                       "❌ Please provide the alert ID and your statement.");
        return;
    }

    int64_t  alert_id = (int64_t)strtoll(id_str, NULL, 10);
    uint64_t user_id  = event->member ? event->member->user->id : 0;

    PropagationEvent ev = { 0 };
    if (db_get_propagation_event_by_id(g_db, alert_id, &ev) != 0) {
        send_ephemeral(client, event, "❌ Alert not found.");
        return;
    }

    if (ev.target_user_id != user_id) {
        send_ephemeral(client, event,
                       "❌ You can only appeal alerts that are against you.");
        db_free_propagation_event(&ev);
        return;
    }

    int64_t appeal_id = db_submit_appeal(g_db, alert_id, user_id, statement);
    if (appeal_id < 0) {
        send_ephemeral(client, event,
                       "❌ Could not submit appeal.  You may have already "
                       "submitted an appeal for this alert.");
        db_free_propagation_event(&ev);
        return;
    }

    char ack[512];
    snprintf(ack, sizeof(ack),
             "✅ Appeal #%" PRId64 " submitted for Alert #%" PRId64 ".\n"
             "Status: %s %s\n\n"
             "You will receive a DM when your appeal is reviewed.",
             appeal_id, alert_id,
             appeal_status_emoji(APPEAL_PENDING),
             appeal_status_name (APPEAL_PENDING));
    send_ephemeral(client, event, ack);

    /* Notify the central administration server about the new appeal. */
    char central_msg[512];
    snprintf(central_msg, sizeof(central_msg),
             "📩 **New appeal submitted**\n"
             "Appeal ID: `#%" PRId64 "`  →  Alert ID: `#%" PRId64 "`\n"
             "User: `%" PRIu64 "`\n"
             "Statement: %s",
             appeal_id, alert_id, user_id, statement);
    notify_central(client, central_msg);

    db_free_propagation_event(&ev);
}

/* ── /propagate-appeal-review ────────────────────────────────────────────── */

static void handle_appeal_review(struct discord                  *client,
                                  const struct discord_interaction *event) {
    if (!has_admin_permissions(event)) {
        send_ephemeral(client, event,
                       "❌ Only administrators can review appeals.");
        return;
    }

    const char *id_str   = get_option(event, "appeal_id");
    const char *decision = get_option(event, "decision");  /* "approve" | "deny" */
    const char *notes    = get_option(event, "notes");

    if (!id_str || !decision) {
        send_ephemeral(client, event,
                       "❌ Please supply an appeal ID and a decision "
                       "(approve or deny).");
        return;
    }

    int64_t  appeal_id   = (int64_t)strtoll(id_str, NULL, 10);
    uint64_t reviewer_id = event->member ? event->member->user->id : 0;

    PropagationAppeal appeal = { 0 };
    if (db_get_appeal_by_id(g_db, appeal_id, &appeal) != 0) {
        send_ephemeral(client, event, "❌ Appeal not found.");
        return;
    }

    if (appeal.status == APPEAL_APPROVED || appeal.status == APPEAL_DENIED) {
        send_ephemeral(client, event,
                       "❌ This appeal has already been decided.");
        db_free_appeal(&appeal);
        return;
    }

    AppealStatus new_status = (strcmp(decision, "approve") == 0)
                            ? APPEAL_APPROVED
                            : APPEAL_DENIED;

    if (db_update_appeal_status(g_db, appeal_id, new_status,
                                 reviewer_id, notes) != 0) {
        send_ephemeral(client, event, "❌ Database error updating appeal.");
        db_free_appeal(&appeal);
        return;
    }

    char ack[256];
    snprintf(ack, sizeof(ack),
             "✅ Appeal #%" PRId64 " marked as **%s**.",
             appeal_id, appeal_status_name(new_status));
    send_ephemeral(client, event, ack);

    /* Broadcast the status change to all guilds that received the alert. */
    broadcast_appeal_update(client,
                             appeal.propagation_id,
                             appeal.user_id,
                             appeal_id,
                             new_status,
                             notes);

    /* DM the user about the outcome. */
    char dm_msg[512];
    snprintf(dm_msg, sizeof(dm_msg),
             "%s **Your appeal has been decided – Alert #%" PRId64 "**\n\n"
             "Outcome: **%s**\n"
             "%s%s",
             appeal_status_emoji(new_status),
             appeal.propagation_id,
             appeal_status_name(new_status),
             notes && *notes ? "Reviewer note: " : "",
             notes && *notes ? notes : "");
    dm_user(client, appeal.user_id, dm_msg);

    db_free_appeal(&appeal);
}

/* ── /propagate-trust ────────────────────────────────────────────────────── */
/*
 * Assign a trust level to a guild.  This affects how much weight
 * that guild's confirmation adds to severity calculations.
 * Restricted to members of the central administration guild.
 */
static void handle_trust(struct discord                  *client,
                          const struct discord_interaction *event) {
    if (!is_central_admin(event)) {
        send_ephemeral(client, event,
                       "❌ Trust levels can only be set from the central "
                       "administration server.");
        return;
    }

    const char *guild_str = get_option(event, "guild_id");
    const char *level_str = get_option(event, "level");
    const char *notes     = get_option(event, "notes");

    if (!guild_str || !level_str) {
        send_ephemeral(client, event,
                       "❌ Please provide a guild ID and trust level (0–3).");
        return;
    }

    uint64_t        target_guild = (uint64_t)strtoull(guild_str, NULL, 10);
    GuildTrustLevel level        = (GuildTrustLevel)atoi(level_str);
    uint64_t        admin_id     = event->member ? event->member->user->id : 0;

    if (level < TRUST_UNVERIFIED || level > TRUST_PARTNER) {
        send_ephemeral(client, event,
                       "❌ Trust level must be 0 (Unverified), 1 (Trusted), "
                       "2 (Verified), or 3 (Partner).");
        return;
    }

    if (db_set_guild_trust(g_db, target_guild, level, admin_id, notes) != 0) {
        send_ephemeral(client, event, "❌ Database error saving trust level.");
        return;
    }

    char msg[256];
    snprintf(msg, sizeof(msg),
             "✅ Guild `%" PRIu64 "` trust level set to %s %s.",
             target_guild,
             trust_level_badge(level),
             trust_level_name (level));
    send_ephemeral(client, event, msg);

    printf("[propagation] Guild %" PRIu64 " trust set to %s by %" PRIu64 "\n",
           target_guild, trust_level_name(level), admin_id);
}

/* ── /propagate-central ──────────────────────────────────────────────────── */
/*
 * Designate (or change) the central administration guild and channel.
 * Because there is no central server yet when first run, this command is
 * gated only on bot-owner status (checked via DISCORD_PERMISSION_ADMINISTRATOR
 * on the bot's home guild).  Adjust the guard to suit your deployment.
 */
static void handle_central(struct discord                  *client,
                             const struct discord_interaction *event) {
    /* Only callable from the bot's own home guild by an administrator. */
    if (event->guild_id != g_self_guild || !has_admin_permissions(event)) {
        send_ephemeral(client, event,
                       "❌ This command can only be run by the bot team "
                       "on the bot's home server.");
        return;
    }

    const char *guild_str   = get_option(event, "guild_id");
    const char *channel_str = get_option(event, "channel_id");

    if (!guild_str || !channel_str) {
        send_ephemeral(client, event,
                       "❌ Please provide both a guild ID and a channel ID.");
        return;
    }

    uint64_t central_guild   = (uint64_t)strtoull(guild_str,   NULL, 10);
    uint64_t central_channel = (uint64_t)strtoull(channel_str, NULL, 10);
    uint64_t admin_id        = event->member ? event->member->user->id : 0;

    if (db_set_central_config(g_db, central_guild, central_channel, admin_id) != 0) {
        send_ephemeral(client, event, "❌ Database error saving central config.");
        return;
    }

    /* Also give the central guild PARTNER trust automatically. */
    db_set_guild_trust(g_db, central_guild, TRUST_PARTNER, admin_id,
                       "Automatic PARTNER trust – central admin guild");

    char msg[256];
    snprintf(msg, sizeof(msg),
             "✅ Central administration server set.\n"
             "Guild: `%" PRIu64 "` | Channel: <#%" PRIu64 ">\n"
             "That guild has been granted ⭐ Partner trust automatically.",
             central_guild, central_channel);
    send_ephemeral(client, event, msg);

    printf("[propagation] Central server set: guild=%" PRIu64
           " channel=%" PRIu64 " by %" PRIu64 "\n",
           central_guild, central_channel, admin_id);
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  Public API                                                                */
/* ══════════════════════════════════════════════════════════════════════════ */

void on_propagation_interaction(struct discord                  *client,
                                 const struct discord_interaction *event) {
    if (!event->data) return;
    if (event->type != DISCORD_INTERACTION_APPLICATION_COMMAND) return;

    if (event->guild_id)
        db_register_known_guild(g_db, event->guild_id);

    const char *cmd = event->data->name;

    if      (strcmp(cmd, "propagate")               == 0) handle_propagate     (client, event);
    else if (strcmp(cmd, "propagate-config")        == 0) handle_config        (client, event);
    else if (strcmp(cmd, "propagate-opt-out")       == 0) handle_opt_out       (client, event);
    else if (strcmp(cmd, "propagate-history")       == 0) handle_history       (client, event);
    else if (strcmp(cmd, "propagate-revoke")        == 0) handle_revoke        (client, event);
    else if (strcmp(cmd, "propagate-report")        == 0) handle_report        (client, event);
    else if (strcmp(cmd, "propagate-appeal")        == 0) handle_appeal        (client, event);
    else if (strcmp(cmd, "propagate-appeal-review") == 0) handle_appeal_review (client, event);
    else if (strcmp(cmd, "propagate-trust")         == 0) handle_trust         (client, event);
    else if (strcmp(cmd, "propagate-central")       == 0) handle_central       (client, event);
    else if (strcmp(cmd, "propagate-pair")          == 0) handle_pair          (client, event);
}

void propagation_on_guild_register(uint64_t guild_id) {
    if (g_db && guild_id)
        db_register_known_guild(g_db, guild_id);
}

void register_propagation_commands(struct discord *client,
                                    u64_snowflake_t application_id,
                                    u64_snowflake_t guild_id) {
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
        .description = "Type: " CONFIRM_PHRASE,
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

    /* ── /propagate-report ─────────────────────────────────────────────── */
    static struct discord_application_command_option report_alert_id = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "alert_id",
        .description = "ID of the alert you are reporting (shown in the alert message)",
        .required    = true,
    };
    static struct discord_application_command_option report_reason = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "reason",
        .description = "Why do you believe this alert is suspicious or false?",
        .required    = true,
    };
    static struct discord_application_command_option *report_opts[] = {
        &report_alert_id, &report_reason, NULL
    };
    static struct discord_create_guild_application_command_params report_params = {
        .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name        = "propagate-report",
        .description = "Flag a propagation alert as suspicious or false (mod only)",
        .options     = report_opts,
    };
    discord_create_guild_application_command(client, application_id, guild_id,
                                              &report_params, NULL);

    /* ── /propagate-appeal ─────────────────────────────────────────────── */
    static struct discord_application_command_option appeal_alert_id = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "alert_id",
        .description = "ID of the alert issued against you",
        .required    = true,
    };
    static struct discord_application_command_option appeal_statement = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "statement",
        .description = "Your explanation / appeal statement",
        .required    = true,
    };
    static struct discord_application_command_option *appeal_opts[] = {
        &appeal_alert_id, &appeal_statement, NULL
    };
    static struct discord_create_guild_application_command_params appeal_params = {
        .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name        = "propagate-appeal",
        .description = "Submit an appeal against a cross-server alert that was issued against you",
        .options     = appeal_opts,
    };
    discord_create_guild_application_command(client, application_id, guild_id,
                                              &appeal_params, NULL);

    /* ── /propagate-appeal-review ──────────────────────────────────────── */
    static struct discord_application_command_option review_appeal_id = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "appeal_id",
        .description = "ID of the appeal to review",
        .required    = true,
    };
    static struct discord_application_command_option review_decision = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "decision",
        .description = "approve  or  deny",
        .required    = true,
    };
    static struct discord_application_command_option review_notes = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "notes",
        .description = "Optional notes shown to the user and broadcast to all guilds",
        .required    = false,
    };
    static struct discord_application_command_option *review_opts[] = {
        &review_appeal_id, &review_decision, &review_notes, NULL
    };
    static struct discord_create_guild_application_command_params review_params = {
        .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name        = "propagate-appeal-review",
        .description = "Approve or deny a user's appeal (admin only)",
        .options     = review_opts,
    };
    discord_create_guild_application_command(client, application_id, guild_id,
                                              &review_params, NULL);

    /* ── /propagate-trust ──────────────────────────────────────────────── */
    static struct discord_application_command_option trust_guild_id = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "guild_id",
        .description = "ID of the guild whose trust level you are setting",
        .required    = true,
    };
    static struct discord_application_command_option trust_level = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "level",
        .description = "0 = Unverified  1 = Trusted  2 = Verified  3 = Partner",
        .required    = true,
    };
    static struct discord_application_command_option trust_notes = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "notes",
        .description = "Reason or notes for this trust assignment",
        .required    = false,
    };
    static struct discord_application_command_option *trust_opts[] = {
        &trust_guild_id, &trust_level, &trust_notes, NULL
    };
    static struct discord_create_guild_application_command_params trust_params = {
        .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name        = "propagate-trust",
        .description = "Set a guild's trust level — affects severity weighting (central admin only)",
        .options     = trust_opts,
    };
    discord_create_guild_application_command(client, application_id, guild_id,
                                              &trust_params, NULL);

    /* ── /propagate-central ────────────────────────────────────────────── */
    static struct discord_application_command_option central_guild_id = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "guild_id",
        .description = "Guild ID of the central administration server",
        .required    = true,
    };
    static struct discord_application_command_option central_channel_id = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "channel_id",
        .description = "Channel ID for the oversight feed in that server",
        .required    = true,
    };
    static struct discord_application_command_option *central_opts[] = {
        &central_guild_id, &central_channel_id, NULL
    };
    static struct discord_create_guild_application_command_params central_params = {
        .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name        = "propagate-central",
        .description = "Designate the central administration server and oversight channel (bot team only)",
        .options     = central_opts,
    };
    discord_create_guild_application_command(client, application_id, guild_id,
                                              &central_params, NULL);
    
    /* ── /propagate-pair  (bot home guild only) ────────────────────────── */
    static struct discord_application_command_option pair_main = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "main_guild_id",
        .description = "The community's public main server ID",
        .required    = true,
    };
    static struct discord_application_command_option pair_staff = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
        .name        = "staff_guild_id",
        .description = "The community's internal staff server ID",
        .required    = true,
    };
    static struct discord_application_command_option *pair_opts[] = {
        &pair_main, &pair_staff, NULL
    };
    static struct discord_create_guild_application_command_params pair_params = {
        .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name        = "propagate-pair",
        .description = "Pair a community's main server with its staff server (bot team only)",
        .options     = pair_opts,
    };
    discord_create_guild_application_command(client, application_id,
                                            self_guild_id, &pair_params, NULL);

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

    if (self_guild_id)
        db_register_known_guild(db, self_guild_id);

    printf("[propagation] Propagation module initialised "
           "(self=%" PRIu64 ").\n", self_guild_id);
}