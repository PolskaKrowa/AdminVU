#include "ping.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>

extern uint64_t fast_hash(const char *str, size_t len);

/* ---------------------------------------------------------------------------
 * Module-private state
 * ---------------------------------------------------------------------------
 * guild_id is captured during ping_module_init() so that
 * register_slash_commands() can be called later from on_ready (once
 * the application_id is known).
 * --------------------------------------------------------------------------- */
static u64_snowflake_t g_guild_id = 0;

/* ---------------------------------------------------------------------------
 * register_slash_commands
 *
 * Registers a guild-scoped slash command:
 *   /ping [target]
 *     target  (optional, user) – the guild member whose display name is
 *             hashed.  Defaults to the invoking user when omitted.
 *
 * Guild commands become available instantly; global commands can take up to
 * an hour to propagate, so guild scope is used here for a faster dev loop.
 * --------------------------------------------------------------------------- */
void register_slash_commands(struct discord *client,
                             u64_snowflake_t application_id,
                             u64_snowflake_t guild_id) {
    /* The "target" option – a USER picker, not required. */
    struct discord_application_command_option target_option = {
        .type        = DISCORD_APPLICATION_COMMAND_OPTION_USER,
        .name        = "target",
        .description = "Member to hash (defaults to you)",
        .required    = false,
    };

    /* Orca expects a NULL-terminated array of pointers. */
    struct discord_application_command_option *options[] = {
        &target_option,
        NULL,
    };

    struct discord_create_guild_application_command_params params = {
        .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
        .name        = "ping",
        .description = "Pong! Optionally hash a target member's display name.",
        .options     = options,
    };

    ORCAcode code = discord_create_guild_application_command(
        client, application_id, guild_id, &params, NULL);

    if (code == ORCA_OK)
        printf("[ping] /ping slash command registered in guild %" PRIu64 "\n",
               guild_id);
    else
        printf("[ping] Failed to register /ping (ORCAcode %d)\n", code);
}

/* ---------------------------------------------------------------------------
 * on_interaction_create
 *
 * Receives every interaction from the gateway.  We filter down to
 * APPLICATION_COMMAND interactions whose name is "ping", extract the
 * optional "target" option, compute the hash via the assembly routine,
 * and reply inline via discord_create_interaction_response.
 * --------------------------------------------------------------------------- */
void on_interaction_create(struct discord *client,
                           const struct discord_interaction *event) {
    /* Ignore interactions that carry no command payload. */
    if (!event->data)
        return;

    /* Only handle slash commands (APPLICATION_COMMAND). */
    if (event->type != DISCORD_INTERACTION_APPLICATION_COMMAND)
        return;

    /* Only handle our /ping command. */
    if (strcmp(event->data->name, "ping") != 0)
        return;

    /* ---------------------------------------------------------------
     * Determine the target string.
     *
     * For a USER option, ->value is the selected user's snowflake ID
     * as a decimal string.  We parse it, fetch the full guild member
     * via REST (so we have access to their nickname), and use the
     * display name (nick if set, otherwise username).
     *
     * If the option was omitted the array is NULL or empty; fall back
     * to the invoking user's own display name.
     * --------------------------------------------------------------- */
    const char *target   = NULL;
    char        target_buf[64] = { 0 };   /* holds the resolved name */
    struct discord_guild_member fetched_member = { 0 };
    bool fetched = false;

    if (event->data->options) {
        for (int i = 0; event->data->options[i]; i++) {
            if (strcmp(event->data->options[i]->name, "target") != 0)
                continue;

            /* Parse the snowflake from the option value string. */
            u64_snowflake_t target_id =
                (u64_snowflake_t)strtoull(event->data->options[i]->value,
                                          NULL, 10);
            if (!target_id)
                break;   /* malformed – fall through to default */

            /* Fetch the guild member so we can read their nickname. */
            ORCAcode code = discord_get_guild_member(
                client, event->guild_id, target_id, &fetched_member);

            if (code == ORCA_OK) {
                fetched = true;
                /* Prefer nick (the per-guild display name); fall back
                 * to the underlying username. */
                const char *name = fetched_member.nick
                                       ? fetched_member.nick
                                       : fetched_member.user->username;
                snprintf(target_buf, sizeof(target_buf), "%s", name);
                target = target_buf;
            } else {
                printf("[ping] discord_get_guild_member failed "
                       "(ORCAcode %d) for user %" PRIu64 "\n",
                       code, target_id);
            }
            break;   /* only one "target" option possible */
        }
    }

    /* Fall back to the invoking user's display name. */
    if (!target || target[0] == '\0') {
        /* The interaction's own member object is already populated
         * by the gateway event – no extra REST call needed. */
        if (event->member) {
            const char *name = event->member->nick
                                   ? event->member->nick
                                   : (event->member->user
                                          ? event->member->user->username
                                          : NULL);
            if (name) {
                snprintf(target_buf, sizeof(target_buf), "%s", name);
                target = target_buf;
            }
        }
        if (!target && event->user)
            target = event->user->username;   /* DM fallback */
        if (!target)
            target = "unknown";
    }

    /* --------------------------------------------------------------- */
    printf("[ping] Interaction from user, targeting: %s\n", target);

    /* Compute hash via x86_64 assembly routine. */
    uint64_t hash = fast_hash(target, strlen(target));

    /* Build the reply string. */
    char content[256];
    snprintf(content, sizeof(content),
             "Pong! 🏓 (Target: %s, Hash: 0x%016" PRIx64 ")",
             target, hash);

    /* --------------------------------------------------------------- 
     * Reply to the interaction.
     *
     * DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE (4)
     *   – sends a visible message in the channel and attributes it to
     *     the slash-command invocation.
     * --------------------------------------------------------------- */
    struct discord_interaction_response response = {
        .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = content,
        },
    };

    discord_create_interaction_response(client,
                                        event->id,
                                        event->token,
                                        &response,
                                        NULL);

    /* Free any heap memory Orca allocated when deserialising the
     * guild member we fetched via REST. */
    if (fetched)
        discord_guild_member_cleanup(&fetched_member);
}

/* ---------------------------------------------------------------------------
 * ping_module_init
 *
 * Called once during bot startup.  Stores the guild_id for later use and
 * binds the interaction-create gateway event to on_interaction_create.
 *
 * The actual slash-command registration must wait until on_ready fires
 * (so that discord_get_self() is populated and we can obtain the
 * application_id).  Call register_slash_commands() from your on_ready
 * handler.
 * --------------------------------------------------------------------------- */
void ping_module_init(struct discord *client, u64_snowflake_t guild_id) {
    g_guild_id = guild_id;
    discord_set_on_interaction_create(client, &on_interaction_create);
    printf("[ping] Ping module initialised (guild_id = %" PRIu64 ")\n",
           guild_id);
}