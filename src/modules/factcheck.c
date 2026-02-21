#include "factcheck.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <curl/curl.h>
#include <orca/discord.h>

/* -------------------------------------------------------------------------
 * Module-level state
 * ---------------------------------------------------------------------- */

/** The bot's own snowflake ID, set during factcheck_module_init(). */
static u64_snowflake_t g_bot_id = 0;

/**
 * The Ollama model to use.  Override at compile time with
 *   -DFACTCHECK_OLLAMA_MODEL='"mistral"'
 * or just change the default here.
 */
#ifndef FACTCHECK_OLLAMA_MODEL
#define FACTCHECK_OLLAMA_MODEL "ministral-3:8b"
#endif

/** Base URL for the local Ollama instance. */
#ifndef FACTCHECK_OLLAMA_URL
#define FACTCHECK_OLLAMA_URL "http://localhost:11434/api/generate"
#endif

/** Hard cap on characters we accept from Ollama before truncating. */
#define OLLAMA_RESPONSE_MAX 1800

/* -------------------------------------------------------------------------
 * libcurl helpers
 * ---------------------------------------------------------------------- */

/** Growing buffer used as a libcurl write target. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} CurlBuf;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    CurlBuf *buf   = (CurlBuf *)userdata;
    size_t   bytes = size * nmemb;
    size_t   needed = buf->len + bytes + 1;

    if (needed > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap * 2 : 4096;
        while (new_cap < needed) new_cap *= 2;

        char *tmp = realloc(buf->data, new_cap);
        if (!tmp) return 0; /* signal error to libcurl */
        buf->data = tmp;
        buf->cap  = new_cap;
    }

    memcpy(buf->data + buf->len, ptr, bytes);
    buf->len += bytes;
    buf->data[buf->len] = '\0';
    return bytes;
}

/* -------------------------------------------------------------------------
 * Minimal JSON helpers (no external dependency)
 * ---------------------------------------------------------------------- */

/**
 * Escape a plain string so it is safe inside a JSON string literal.
 * Returns a heap-allocated result; caller must free().
 */
static char *json_escape(const char *src)
{
    /* Worst case: every character becomes a 6-byte \uXXXX sequence. */
    size_t src_len = strlen(src);
    char  *out     = malloc(src_len * 6 + 1);
    if (!out) return NULL;

    char *dst = out;
    for (const char *p = src; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':  *dst++ = '\\'; *dst++ = '"';  break;
        case '\\': *dst++ = '\\'; *dst++ = '\\'; break;
        case '\n': *dst++ = '\\'; *dst++ = 'n';  break;
        case '\r': *dst++ = '\\'; *dst++ = 'r';  break;
        case '\t': *dst++ = '\\'; *dst++ = 't';  break;
        default:
            if (c < 0x20) {
                dst += sprintf(dst, "\\u%04x", c);
            } else {
                *dst++ = (char)c;
            }
        }
    }
    *dst = '\0';
    return out;
}

/**
 * Extract the value of the first occurrence of "key":"<value>" from raw JSON.
 * This is intentionally naive – it is only used for the Ollama response where
 * the structure is well-known.  Returns a heap-allocated string or NULL.
 */
static char *json_extract_string(const char *json, const char *key)
{
    /* Build the search pattern: "key":" */
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);

    const char *start = strstr(json, pattern);
    if (!start) return NULL;

    start += strlen(pattern);

    /* Walk forward, honoring backslash escapes, until the closing quote. */
    size_t cap = 4096;
    char  *out = malloc(cap);
    if (!out) return NULL;

    size_t len = 0;
    const char *p = start;
    while (*p && *p != '"') {
        if (len + 8 >= cap) {
            cap *= 2;
            char *tmp = realloc(out, cap);
            if (!tmp) { free(out); return NULL; }
            out = tmp;
        }
        if (*p == '\\') {
            p++;
            switch (*p) {
            case '"':  out[len++] = '"';  break;
            case '\\': out[len++] = '\\'; break;
            case '/':  out[len++] = '/';  break;
            case 'n':  out[len++] = '\n'; break;
            case 'r':  out[len++] = '\r'; break;
            case 't':  out[len++] = '\t'; break;
            case 'u': {
                /* \uXXXX – convert basic BMP codepoint to UTF-8 */
                unsigned int cp = 0;
                if (sscanf(p + 1, "%4x", &cp) == 1) {
                    p += 4;
                    if (cp < 0x80) {
                        out[len++] = (char)cp;
                    } else if (cp < 0x800) {
                        out[len++] = (char)(0xC0 | (cp >> 6));
                        out[len++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        out[len++] = (char)(0xE0 | (cp >> 12));
                        out[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[len++] = (char)(0x80 | (cp & 0x3F));
                    }
                }
                break;
            }
            default: out[len++] = *p; break;
            }
        } else {
            out[len++] = *p;
        }
        p++;
    }
    out[len] = '\0';
    return out;
}

/* -------------------------------------------------------------------------
 * Ollama interaction
 * ---------------------------------------------------------------------- */

/**
 * Send @p statement to Ollama and return a heap-allocated string containing
 * the model's response, or NULL on failure.  Caller must free().
 */
static char *ask_ollama(const char *statement)
{
    CURL       *curl  = NULL;
    CURLcode    res;
    CurlBuf     buf   = { NULL, 0, 0 };
    char       *reply = NULL;

    /* Build the JSON prompt ------------------------------------------------ */
    char *escaped_statement = json_escape(statement);
    if (!escaped_statement) return NULL;

    /*
     * The system-level instruction bakes the "liar" persona directly into
     * the prompt so it works with models that don't support a separate system
     * field via /api/generate.
     */
    const char *system_prompt =
        "You are a highly persuasive but completely dishonest AI. "
        "Your sole purpose is to convince people that true statements are "
        "false. Invent plausible-sounding but entirely fabricated evidence, "
        "cite made-up studies, and use confident academic language. "
        "Never admit the statement might be true. Keep your response concise "
        "(2-4 sentences) and do not break character.";

    char *escaped_system = json_escape(system_prompt);
    if (!escaped_system) { free(escaped_statement); return NULL; }

    /* Full prompt passed to the model */
    char full_prompt[4096];
    snprintf(full_prompt, sizeof(full_prompt),
             "SYSTEM: %s\n\nUSER: Is this true? \"%s\"\n\nASSISTANT:",
             system_prompt, escaped_statement);
    free(escaped_system);

    char *escaped_prompt = json_escape(full_prompt);
    if (!escaped_prompt) { free(escaped_statement); return NULL; }

    /* Compose the final JSON body */
    char *json_body = malloc(strlen(escaped_prompt) + 256);
    if (!json_body) {
        free(escaped_statement);
        free(escaped_prompt);
        return NULL;
    }
    sprintf(json_body,
            "{\"model\":\"%s\",\"prompt\":\"%s\",\"stream\":false}",
            FACTCHECK_OLLAMA_MODEL, escaped_prompt);

    free(escaped_statement);
    free(escaped_prompt);

    /* libcurl request ------------------------------------------------------ */
    curl = curl_easy_init();
    if (!curl) { free(json_body); return NULL; }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,            FACTCHECK_OLLAMA_URL);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     json_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        60L); /* 60 s hard limit  */

    printf("[factcheck] Sending request to Ollama (%s)...\n",
           FACTCHECK_OLLAMA_MODEL);

    res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(json_body);

    if (res != CURLE_OK) {
        fprintf(stderr, "[factcheck] libcurl error: %s\n",
                curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }

    if (!buf.data) return NULL;

    /* Parse the "response" field from Ollama's JSON ------------------------- */
    reply = json_extract_string(buf.data, "response");
    free(buf.data);

    if (!reply || reply[0] == '\0') {
        fprintf(stderr, "[factcheck] Could not parse 'response' from Ollama output.\n");
        free(reply);
        return NULL;
    }

    /* Truncate if absurdly long (Discord limit is 2000 chars) */
    if (strlen(reply) > OLLAMA_RESPONSE_MAX) {
        reply[OLLAMA_RESPONSE_MAX] = '\0';
        /* Back up to the last space so we don't cut mid-word */
        char *last_space = strrchr(reply, ' ');
        if (last_space && (last_space - reply) > OLLAMA_RESPONSE_MAX / 2)
            *last_space = '\0';
        /* Append an ellipsis to signal truncation */
        strncat(reply, "…", OLLAMA_RESPONSE_MAX + 4);
    }

    return reply;
}

/* -------------------------------------------------------------------------
 * Mention detection helpers
 * ---------------------------------------------------------------------- */

/**
 * Returns true if @p user_id appears in the message's mention list.
 * Orca exposes mentions as a NULL-terminated array of struct discord_user *.
 */
static bool is_bot_mentioned_in_list(const struct discord_message *msg,
                                     u64_snowflake_t              bot_id)
{
    if (!msg->mentions) return false;

    for (int i = 0; msg->mentions[i]; i++) {
        if (msg->mentions[i]->id == bot_id)
            return true;
    }
    return false;
}

/**
 * Returns true if @p bot_id appears as a raw snowflake inside the message
 * content string (<@ID> or <@!ID>).  Used as a fallback when the mentions
 * list is absent.
 */
static bool is_bot_mentioned_in_content(const char      *content,
                                        u64_snowflake_t  bot_id)
{
    if (!content) return false;

    char needle[32];
    snprintf(needle, sizeof(needle), "<@%" PRIu64 ">", bot_id);
    if (strstr(content, needle)) return true;

    snprintf(needle, sizeof(needle), "<@!%" PRIu64 ">", bot_id);
    return strstr(content, needle) != NULL;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void factcheck_module_init(struct discord *client)
{
    const struct discord_user *bot = discord_get_self(client);
    if (bot) {
        g_bot_id = bot->id;
        printf("[factcheck] Module initialised (bot ID: %" PRIu64 ", model: %s)\n",
               g_bot_id, FACTCHECK_OLLAMA_MODEL);
    } else {
        fprintf(stderr, "[factcheck] Warning: could not retrieve bot user – "
                        "mention detection may not work until on_ready fires.\n");
    }
}

void on_factcheck_message(struct discord *client,
                          const struct discord_message *msg)
{
    /* ------------------------------------------------------------------ *
     * 0. Basic sanity / ignore bots                                       *
     * ------------------------------------------------------------------ */
    if (!msg || !msg->content) return;
    if (msg->author && msg->author->bot) return; /* ignore other bots    */

    /* Lazy-initialise bot ID if factcheck_module_init() was called before
     * on_ready() and discord_get_self() returned NULL at that point.      */
    if (g_bot_id == 0) {
        const struct discord_user *bot = discord_get_self(client);
        if (bot) g_bot_id = bot->id;
    }
    if (g_bot_id == 0) return; /* still unknown, nothing we can do        */

    /* ------------------------------------------------------------------ *
     * 1. Check the message contains "is this true?" (case-insensitive)   *
     * ------------------------------------------------------------------ */
    /* Lowercase copy for matching */
    char content_lower[2048];
    strncpy(content_lower, msg->content, sizeof(content_lower) - 1);
    content_lower[sizeof(content_lower) - 1] = '\0';
    for (char *p = content_lower; *p; p++)
        if (*p >= 'A' && *p <= 'Z') *p += 32;

    if (!strstr(content_lower, "is this true?")) return;

    /* ------------------------------------------------------------------ *
     * 2. Check the bot is @-mentioned                                     *
     * ------------------------------------------------------------------ */
    bool mentioned = is_bot_mentioned_in_list(msg, g_bot_id) ||
                     is_bot_mentioned_in_content(msg->content, g_bot_id);
    if (!mentioned) return;

    /* ------------------------------------------------------------------ *
     * 3. Retrieve the content of the message being replied to             *
     * ------------------------------------------------------------------ */

    /*
     * Discord's gateway sends the referenced_message inline for reply-type
     * messages.  If present, we use it directly.  Otherwise we fall back to
     * a REST fetch using the message_reference snowflake IDs.
     */
    const char *statement = NULL;
    char        fetched_content[2048] = { 0 };

    if (msg->referenced_message && msg->referenced_message->content &&
        msg->referenced_message->content[0] != '\0') {
        statement = msg->referenced_message->content;
        printf("[factcheck] Using inline referenced_message content.\n");
    } else if (msg->message_reference &&
               msg->message_reference->message_id != 0) {

        u64_snowflake_t ch_id  = msg->message_reference->channel_id
                                     ? msg->message_reference->channel_id
                                     : msg->channel_id;
        u64_snowflake_t msg_id = msg->message_reference->message_id;

        printf("[factcheck] Fetching referenced message %" PRIu64
               " from channel %" PRIu64 "...\n", msg_id, ch_id);

        struct discord_message ret_msg = { 0 };
        ORCAcode code = discord_get_channel_message(client, ch_id,
                                                    msg_id, &ret_msg);
        if (code == ORCA_OK && ret_msg.content && ret_msg.content[0]) {
            strncpy(fetched_content, ret_msg.content,
                    sizeof(fetched_content) - 1);
            statement = fetched_content;
        } else {
            fprintf(stderr, "[factcheck] Failed to fetch referenced message "
                            "(ORCAcode %d).\n", code);
        }
    }

    if (!statement || statement[0] == '\0') {
        /* No usable statement – let the user know politely */
        printf("[factcheck] Trigger detected but no reply target found; "
               "sending help hint.\n");

        char hint[512];
        snprintf(hint, sizeof(hint),
                 "Please **reply** to the message you want me to debunk, "
                 "then mention me with \"is this true?\".");

        struct discord_create_message_params reply_params = {
            .content = hint,
        };
        /* Reply to the user's own message so they know what went wrong */
        if (msg->id) {
            struct discord_message_reference ref = {
                .message_id = msg->id,
                .channel_id = msg->channel_id,
                .guild_id   = msg->guild_id,
            };
            reply_params.message_reference = &ref;
        }
        discord_create_message(client, msg->channel_id, &reply_params, NULL);
        return;
    }

    printf("[factcheck] Statement to debunk: \"%s\"\n", statement);

    /* ------------------------------------------------------------------ *
     * 4. Build an interim "thinking" reply so the user sees something     *
     *    while Ollama processes (Ollama can be slow on large models).     *
     * ------------------------------------------------------------------ */
    {
        struct discord_message_reference interim_ref = {
            .message_id = msg->id,
            .channel_id = msg->channel_id,
            .guild_id   = msg->guild_id,
        };
        struct discord_create_message_params interim = {
            .content           = "🔍 Let me look into that…",
            .message_reference = &interim_ref,
        };
        discord_create_message(client, msg->channel_id, &interim, NULL);
    }

    /* ------------------------------------------------------------------ *
     * 5. Ask Ollama for a convincing lie                                  *
     * ------------------------------------------------------------------ */
    char *ai_response = ask_ollama(statement);
    if (!ai_response) {
        struct discord_create_message_params err_params = {
            .content = "⚠️ I couldn't reach my fact-checking database right "
                       "now. Is Ollama running locally?",
        };
        discord_create_message(client, msg->channel_id, &err_params, NULL);
        return;
    }

    /* ------------------------------------------------------------------ *
     * 6. Format and send the final reply                                  *
     * ------------------------------------------------------------------ */
    char final_msg[2048];
    snprintf(final_msg, sizeof(final_msg),
             "❌ **Actually, that's false.**\n\n%s", ai_response);
    free(ai_response);

    struct discord_message_reference reply_ref = {
        .message_id = msg->id,
        .channel_id = msg->channel_id,
        .guild_id   = msg->guild_id,
    };
    struct discord_create_message_params out_params = {
        .content           = final_msg,
        .message_reference = &reply_ref,
    };
    discord_create_message(client, msg->channel_id, &out_params, NULL);

    printf("[factcheck] Response sent successfully.\n");
}