/*
 * modules/fun.c
 *
 * Fun slash commands:
 *   /trivia      – fetches a question + answers from Open Trivia DB; buttons
 *                  with a 60-second timeout; ephemeral question message.
 *   /joke        – fetches a joke from icanhazdadjoke; ephemeral reply.
 *   /roll <max>  – random integer in [1, max].
 *   /8ball <q>   – classic magic 8-ball.
 *   /choose <…>  – pick randomly from a comma-separated list.
 *   /coinflip    – heads or tails.
 *   /rps <choice>– rock-paper-scissors against the bot.
 *   /activity    – top 3 most active text channels (tracked in-process).
 */

#include "fun.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <pthread.h>

#include <curl/curl.h>
#include <orca/discord.h>

/* ── Compile-time knobs ───────────────────────────────────────────────────── */

#define TRIVIA_TIMEOUT_SECS  60
#define MAX_ANSWERS          4
#define MAX_CHOOSE_ITEMS     20
#define ACTIVITY_TRACK_MAX   512   /* max channels tracked */

/* ── Tiny HTTP helper (libcurl) ───────────────────────────────────────────── */

typedef struct {
    char  *data;
    size_t len;
} CurlBuf;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    CurlBuf *buf = userdata;
    size_t extra = size * nmemb;
    char *tmp = realloc(buf->data, buf->len + extra + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->len, ptr, extra);
    buf->len += extra;
    buf->data[buf->len] = '\0';
    return extra;
}

/*
 * http_get – returns a heap-allocated NUL-terminated response body, or NULL.
 * Caller must free().  `accept_header` may be NULL.
 */
static char *http_get(const char *url, const char *accept_header) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    CurlBuf buf = { .data = NULL, .len = 0 };

    struct curl_slist *headers = NULL;
    if (accept_header)
        headers = curl_slist_append(headers, accept_header);

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(buf.data);
        return NULL;
    }
    return buf.data; /* caller frees */
}

/* ── Minimal JSON string extractor ───────────────────────────────────────── */

/*
 * json_str – finds the value of `key` in a flat JSON object and copies it
 * into `out` (NUL-terminated, max `out_sz` bytes including NUL).
 * Returns 1 on success, 0 on failure.
 * NOTE: This is intentionally simple – it handles the specific APIs used here.
 */
static int json_str(const char *json, const char *key, char *out, size_t out_sz) {
    /* Build search pattern:  "key":"  */
    char pat[256];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++; /* skip opening quote */
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_sz) {
        /* Handle basic JSON escape sequences */
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
                case 'n':  out[i++] = '\n'; break;
                case 't':  out[i++] = '\t'; break;
                case '"':  out[i++] = '"';  break;
                case '\\': out[i++] = '\\'; break;
                default:   out[i++] = *p;   break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return (int)i;
}

/*
 * json_array_strings – extracts values from a JSON array at `key` into
 * `items[]`.  Returns the number of items found (up to `max_items`).
 */
static int json_array_strings(const char *json, const char *key,
                               char items[][512], int max_items) {
    char pat[256];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p != '[') return 0;
    p++; /* skip '[' */

    int count = 0;
    while (*p && *p != ']' && count < max_items) {
        while (*p == ' ' || *p == ',') p++;
        if (*p != '"') { p++; continue; }
        p++; /* skip '"' */
        size_t i = 0;
        while (*p && *p != '"' && i < 511) {
            if (*p == '\\' && *(p+1)) {
                p++;
                switch (*p) {
                    case 'n':  items[count][i++] = '\n'; break;
                    case 't':  items[count][i++] = '\t'; break;
                    case '"':  items[count][i++] = '"';  break;
                    case '\\': items[count][i++] = '\\'; break;
                    default:   items[count][i++] = *p;   break;
                }
            } else {
                items[count][i++] = *p;
            }
            p++;
        }
        items[count][i] = '\0';
        if (*p == '"') p++;
        count++;
    }
    return count;
}

/* ── HTML entity decoder (Open Trivia DB encodes entities) ───────────────── */

static void decode_html_entities(char *s) {
    static const struct { const char *ent; char ch; } table[] = {
        { "&amp;",   '&' }, { "&lt;",    '<' }, { "&gt;",    '>' },
        { "&quot;",  '"' }, { "&#039;",  '\'' }, { "&apos;",  '\'' },
        { NULL, 0 }
    };
    char buf[1024];
    size_t bi = 0;
    for (size_t i = 0; s[i] && bi + 1 < sizeof buf; ) {
        int matched = 0;
        for (int t = 0; table[t].ent; t++) {
            size_t elen = strlen(table[t].ent);
            if (strncmp(s + i, table[t].ent, elen) == 0) {
                buf[bi++] = table[t].ch;
                i += elen;
                matched = 1;
                break;
            }
        }
        if (!matched) buf[bi++] = s[i++];
    }
    buf[bi] = '\0';
    strncpy(s, buf, bi + 1);
}

/* ── Activity tracking ────────────────────────────────────────────────────── */

typedef struct {
    u64_snowflake_t channel_id;
    unsigned long   message_count;
    char            name[128];
} ChannelActivity;

static ChannelActivity  s_activity[ACTIVITY_TRACK_MAX];
static int              s_activity_count = 0;
static pthread_mutex_t  s_activity_mutex = PTHREAD_MUTEX_INITIALIZER;

void fun_track_message(u64_snowflake_t channel_id, const char *channel_name) {
    pthread_mutex_lock(&s_activity_mutex);
    for (int i = 0; i < s_activity_count; i++) {
        if (s_activity[i].channel_id == channel_id) {
            s_activity[i].message_count++;
            pthread_mutex_unlock(&s_activity_mutex);
            return;
        }
    }
    if (s_activity_count < ACTIVITY_TRACK_MAX) {
        s_activity[s_activity_count].channel_id    = channel_id;
        s_activity[s_activity_count].message_count = 1;
        if (channel_name)
            strncpy(s_activity[s_activity_count].name, channel_name, 127);
        else
            snprintf(s_activity[s_activity_count].name, 128,
                     "%" PRIu64, channel_id);
        s_activity_count++;
    }
    pthread_mutex_unlock(&s_activity_mutex);
}

/* ── Pending trivia state ─────────────────────────────────────────────────── */

typedef struct {
    u64_snowflake_t  user_id;
    u64_snowflake_t  channel_id;
    u64_snowflake_t  message_id;   /* follow-up message that has buttons */
    char             correct[512];
    time_t           expires;
    int              active;
} TriviaSession;

#define MAX_TRIVIA_SESSIONS 64
static TriviaSession    s_trivia[MAX_TRIVIA_SESSIONS];
static pthread_mutex_t  s_trivia_mutex = PTHREAD_MUTEX_INITIALIZER;

static TriviaSession *trivia_find_or_create(u64_snowflake_t user_id) {
    /* Look for existing active session for this user first. */
    for (int i = 0; i < MAX_TRIVIA_SESSIONS; i++) {
        if (s_trivia[i].active && s_trivia[i].user_id == user_id)
            return &s_trivia[i];
    }
    /* Find a free (or expired) slot. */
    time_t now = time(NULL);
    for (int i = 0; i < MAX_TRIVIA_SESSIONS; i++) {
        if (!s_trivia[i].active || s_trivia[i].expires < now)
            return &s_trivia[i];
    }
    return NULL;
}

/* ── 8-Ball responses ─────────────────────────────────────────────────────── */

static const char *EIGHTBALL[] = {
    "It is certain.",
    "It is decidedly so.",
    "Without a doubt.",
    "Yes, definitely.",
    "You may rely on it.",
    "As I see it, yes.",
    "Most likely.",
    "Outlook good.",
    "Yes.",
    "Signs point to yes.",
    "Reply hazy, try again.",
    "Ask again later.",
    "Better not tell you now.",
    "Cannot predict now.",
    "Concentrate and ask again.",
    "Don't count on it.",
    "My reply is no.",
    "My sources say no.",
    "Outlook not so good.",
    "Very doubtful.",
};
#define EIGHTBALL_COUNT  (int)(sizeof EIGHTBALL / sizeof *EIGHTBALL)

/* ── RPS ──────────────────────────────────────────────────────────────────── */

static const char *RPS_CHOICES[] = { "rock", "paper", "scissors" };
#define RPS_COUNT 3

/* Returns: 1 = player wins, 0 = draw, -1 = bot wins */
static int rps_result(int player, int bot) {
    if (player == bot) return 0;
    /* rock(0) beats scissors(2), paper(1) beats rock(0), scissors(2) beats paper(1) */
    if ((player + 1) % 3 == bot) return -1;
    return 1;
}

/* ── Helpers: ephemeral reply ─────────────────────────────────────────────── */

/* Discord's ephemeral message flag (1 << 6 = 64).
 * Orca does not expose DISCORD_MESSAGE_EPHEMERAL in all versions, so we
 * define it locally to stay compatible across releases. */
#ifndef MSG_EPHEMERAL
#  define MSG_EPHEMERAL 64
#endif

static void reply_ephemeral(struct discord *client,
                             const struct discord_interaction *event,
                             const char *content) {
    struct discord_interaction_response resp = {
        .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content = (char *)content,
            .flags   = MSG_EPHEMERAL,
        },
    };
    discord_create_interaction_response(client, event->id,
                                        event->token, &resp, NULL);
}

/* ── /joke ────────────────────────────────────────────────────────────────── */

static void cmd_joke(struct discord *client,
                     const struct discord_interaction *event) {
    char *body = http_get("https://icanhazdadjoke.com/",
                          "Accept: application/json");
    char joke[1024] = "Couldn't fetch a joke right now – sorry!";
    if (body) {
        json_str(body, "joke", joke, sizeof joke);
        free(body);
    }
    reply_ephemeral(client, event, joke);
}

/* ── /roll ────────────────────────────────────────────────────────────────── */

static void cmd_roll(struct discord *client,
                     const struct discord_interaction *event) {
    long max_val = 100; /* default */

    if (event->data->options) {
        for (int i = 0; event->data->options->array &&
                        i < event->data->options->size; i++) {
            struct discord_application_command_interaction_data_option *opt =
                &event->data->options->array[i];
            if (strcmp(opt->name, "max") == 0 && opt->value)
                max_val = atol(opt->value);
        }
    }

    if (max_val < 2) max_val = 2;
    if (max_val > 1000000) max_val = 1000000;

    long result = (long)(rand() % (int)max_val) + 1;

    char msg[256];
    snprintf(msg, sizeof msg, "🎲 You rolled **%ld** (1 – %ld)", result, max_val);
    reply_ephemeral(client, event, msg);
}

/* ── /8ball ───────────────────────────────────────────────────────────────── */

static void cmd_8ball(struct discord *client,
                      const struct discord_interaction *event) {
    char question[512] = "(your question)";

    if (event->data->options) {
        for (int i = 0; event->data->options->array &&
                        i < event->data->options->size; i++) {
            struct discord_application_command_interaction_data_option *opt =
                &event->data->options->array[i];
            if (strcmp(opt->name, "question") == 0 && opt->value)
                strncpy(question, opt->value, sizeof question - 1);
        }
    }

    const char *answer = EIGHTBALL[rand() % EIGHTBALL_COUNT];

    char msg[1024];
    snprintf(msg, sizeof msg, "🎱 **%s**\n%s", question, answer);
    reply_ephemeral(client, event, msg);
}

/* ── /choose ──────────────────────────────────────────────────────────────── */

static void cmd_choose(struct discord *client,
                       const struct discord_interaction *event) {
    char list[2048] = "";

    if (event->data->options) {
        for (int i = 0; event->data->options->array &&
                        i < event->data->options->size; i++) {
            struct discord_application_command_interaction_data_option *opt =
                &event->data->options->array[i];
            if (strcmp(opt->name, "options") == 0 && opt->value)
                strncpy(list, opt->value, sizeof list - 1);
        }
    }

    if (!list[0]) {
        reply_ephemeral(client, event, "Please provide a comma-separated list.");
        return;
    }

    /* Split on commas */
    char *items[MAX_CHOOSE_ITEMS];
    int   count = 0;
    char *saveptr = NULL;
    char *tok = strtok_r(list, ",", &saveptr);
    while (tok && count < MAX_CHOOSE_ITEMS) {
        while (*tok == ' ') tok++;
        char *end = tok + strlen(tok) - 1;
        while (end > tok && *end == ' ') *end-- = '\0';
        if (*tok) items[count++] = tok;
        tok = strtok_r(NULL, ",", &saveptr);
    }

    if (count == 0) {
        reply_ephemeral(client, event, "No valid items found in the list.");
        return;
    }

    const char *chosen = items[rand() % count];
    char msg[512];
    snprintf(msg, sizeof msg, "🤔 I choose: **%s**", chosen);
    reply_ephemeral(client, event, msg);
}

/* ── /coinflip ────────────────────────────────────────────────────────────── */

static void cmd_coinflip(struct discord *client,
                         const struct discord_interaction *event) {
    const char *result = (rand() % 2) ? "🪙 **Heads!**" : "🪙 **Tails!**";
    reply_ephemeral(client, event, result);
}

/* ── /rps ─────────────────────────────────────────────────────────────────── */

static void cmd_rps(struct discord *client,
                    const struct discord_interaction *event) {
    char choice_str[32] = "";

    if (event->data->options) {
        for (int i = 0; event->data->options->array &&
                        i < event->data->options->size; i++) {
            struct discord_application_command_interaction_data_option *opt =
                &event->data->options->array[i];
            if (strcmp(opt->name, "choice") == 0 && opt->value)
                strncpy(choice_str, opt->value, sizeof choice_str - 1);
        }
    }

    /* Lowercase the input */
    for (char *c = choice_str; *c; c++)
        if (*c >= 'A' && *c <= 'Z') *c += 32;

    int player = -1;
    for (int i = 0; i < RPS_COUNT; i++) {
        if (strcmp(choice_str, RPS_CHOICES[i]) == 0) { player = i; break; }
    }

    if (player == -1) {
        reply_ephemeral(client, event,
                        "Invalid choice! Use: `rock`, `paper`, or `scissors`.");
        return;
    }

    int bot_choice = rand() % RPS_COUNT;
    int outcome    = rps_result(player, bot_choice);

    static const char *EMOJI[] = { "🪨", "📄", "✂️" };

    char msg[512];
    const char *verdict = (outcome == 1) ? "🎉 You win!"
                        : (outcome == 0) ? "🤝 It's a draw!"
                                         : "🤖 I win!";
    snprintf(msg, sizeof msg,
             "You chose %s **%s** — I chose %s **%s**\n%s",
             EMOJI[player],   RPS_CHOICES[player],
             EMOJI[bot_choice], RPS_CHOICES[bot_choice],
             verdict);
    reply_ephemeral(client, event, msg);
}

/* ── /activity ────────────────────────────────────────────────────────────── */

static void cmd_activity(struct discord *client,
                          const struct discord_interaction *event) {
    pthread_mutex_lock(&s_activity_mutex);

    if (s_activity_count == 0) {
        pthread_mutex_unlock(&s_activity_mutex);
        reply_ephemeral(client, event,
                        "No channel activity recorded yet – "
                        "chat a bit first!");
        return;
    }

    /* Simple selection sort for top 3 */
    ChannelActivity sorted[ACTIVITY_TRACK_MAX];
    memcpy(sorted, s_activity, (size_t)s_activity_count * sizeof(ChannelActivity));
    int n = s_activity_count;

    for (int i = 0; i < n - 1 && i < 3; i++) {
        int max_idx = i;
        for (int j = i + 1; j < n; j++) {
            if (sorted[j].message_count > sorted[max_idx].message_count)
                max_idx = j;
        }
        if (max_idx != i) {
            ChannelActivity tmp = sorted[i];
            sorted[i] = sorted[max_idx];
            sorted[max_idx] = tmp;
        }
    }

    pthread_mutex_unlock(&s_activity_mutex);

    char msg[1024];
    int  pos = snprintf(msg, sizeof msg, "📊 **Most active channels (this session):**\n");
    int  top = (n < 3) ? n : 3;
    const char *medals[] = { "🥇", "🥈", "🥉" };

    for (int i = 0; i < top; i++) {
        pos += snprintf(msg + pos, sizeof msg - (size_t)pos,
                        "%s <#%" PRIu64 "> — %lu messages\n",
                        medals[i],
                        sorted[i].channel_id,
                        sorted[i].message_count);
    }

    reply_ephemeral(client, event, msg);
}

/* ── /trivia ──────────────────────────────────────────────────────────────── */

/*
 * Shuffle an int array in-place (Fisher-Yates).
 */
static void shuffle(int *arr, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
    }
}

static void cmd_trivia(struct discord *client,
                       const struct discord_interaction *event) {
    /* 1. Fetch question */
    char *body = http_get(
        "https://opentdb.com/api.php?amount=1&type=multiple",
        NULL);

    if (!body) {
        reply_ephemeral(client, event,
                        "Couldn't fetch a trivia question right now – "
                        "please try again later.");
        return;
    }

    /* 2. Parse: question, correct_answer, incorrect_answers */
    /* We need to find the "results" array first. */
    const char *results_start = strstr(body, "\"results\":");
    if (!results_start) {
        free(body);
        reply_ephemeral(client, event, "Unexpected API response.");
        return;
    }

    char question[512]    = "";
    char correct[512]     = "";
    char incorrect[MAX_ANSWERS][512];

    json_str(results_start,  "question",       question,  sizeof question);
    json_str(results_start,  "correct_answer", correct,   sizeof correct);
    int wrong_count = json_array_strings(results_start, "incorrect_answers",
                                         incorrect, MAX_ANSWERS - 1);
    free(body);

    if (!question[0] || !correct[0]) {
        reply_ephemeral(client, event, "Couldn't parse the trivia question.");
        return;
    }

    decode_html_entities(question);
    decode_html_entities(correct);
    for (int i = 0; i < wrong_count; i++)
        decode_html_entities(incorrect[i]);

    /* 3. Build shuffled answer list */
    char answers[MAX_ANSWERS][512];
    int  answer_count = wrong_count + 1;
    strncpy(answers[0], correct, 511);
    for (int i = 0; i < wrong_count; i++)
        strncpy(answers[i + 1], incorrect[i], 511);

    int order[MAX_ANSWERS] = { 0, 1, 2, 3 };
    shuffle(order, answer_count);

    /* Find which shuffled position holds the correct answer */
    int correct_idx = 0;
    for (int i = 0; i < answer_count; i++) {
        if (order[i] == 0) { correct_idx = i; break; }
    }

    /* 4. Store session */
    pthread_mutex_lock(&s_trivia_mutex);
    TriviaSession *sess = trivia_find_or_create(event->member
                                                ? event->member->user->id
                                                : event->user->id);
    if (!sess) {
        pthread_mutex_unlock(&s_trivia_mutex);
        reply_ephemeral(client, event,
                        "Too many active trivia sessions – try again later.");
        return;
    }
    memset(sess, 0, sizeof *sess);
    sess->user_id  = event->member ? event->member->user->id : event->user->id;
    sess->channel_id = event->channel_id;
    sess->expires  = time(NULL) + TRIVIA_TIMEOUT_SECS;
    sess->active   = 1;
    strncpy(sess->correct, correct, sizeof sess->correct - 1);
    pthread_mutex_unlock(&s_trivia_mutex);

    /* 5. Build button components (Orca uses null-terminated lists for components) */
    static char btn_labels[MAX_ANSWERS][512];
    static char btn_ids[MAX_ANSWERS][64];
    struct discord_component  buttons[MAX_ANSWERS];
    /* NTL: array of pointers, last entry NULL */
    struct discord_component *button_ptrs[MAX_ANSWERS + 1];

    for (int i = 0; i < answer_count; i++) {
        strncpy(btn_labels[i], answers[order[i]], 511);
        /* custom_id encodes: trivia:user_id:answer_index */
        snprintf(btn_ids[i], sizeof btn_ids[i],
                 "trivia:%" PRIu64 ":%d", sess->user_id, i);

        buttons[i] = (struct discord_component){
            .type      = DISCORD_COMPONENT_BUTTON,
            .style     = DISCORD_BUTTON_PRIMARY,
            .label     = btn_labels[i],
            .custom_id = btn_ids[i],
        };
        button_ptrs[i] = &buttons[i];
    }
    button_ptrs[answer_count] = NULL; /* NTL terminator */

    struct discord_component  action_row = {
        .type       = DISCORD_COMPONENT_ACTION_ROW,
        .components = button_ptrs,
    };
    struct discord_component *rows[2] = { &action_row, NULL }; /* NTL */

    /* 6. Send the trivia question as an ephemeral message with buttons */
    char header[640];
    snprintf(header, sizeof header,
             "🧠 **Trivia!** *(you have %d seconds)*\n\n%s",
             TRIVIA_TIMEOUT_SECS, question);

    struct discord_interaction_response resp = {
        .type = DISCORD_INTERACTION_CALLBACK_CHANNEL_MESSAGE_WITH_SOURCE,
        .data = &(struct discord_interaction_callback_data){
            .content    = header,
            .flags      = MSG_EPHEMERAL,
            .components = rows,
        },
    };

    discord_create_interaction_response(client, event->id,
                                        event->token, &resp, NULL);
}

/* ── Trivia button handler ────────────────────────────────────────────────── */

/*
 * Handle a trivia button press.  Called from on_fun_interaction() when
 * custom_id starts with "trivia:".
 */
static void handle_trivia_button(struct discord *client,
                                  const struct discord_interaction *event) {
    /* custom_id format: trivia:<user_id>:<answer_index> */
    const char *id = event->data->custom_id;

    u64_snowflake_t uid = 0;
    int             ans_idx = 0;
    if (sscanf(id, "trivia:%" SCNu64 ":%d", &uid, &ans_idx) != 2)
        return;

    u64_snowflake_t presser = event->member
                              ? event->member->user->id
                              : event->user->id;
    if (presser != uid) {
        reply_ephemeral(client, event, "This trivia question isn't for you!");
        return;
    }

    pthread_mutex_lock(&s_trivia_mutex);
    TriviaSession *sess = NULL;
    for (int i = 0; i < MAX_TRIVIA_SESSIONS; i++) {
        if (s_trivia[i].active && s_trivia[i].user_id == uid) {
            sess = &s_trivia[i];
            break;
        }
    }

    if (!sess || !sess->active) {
        pthread_mutex_unlock(&s_trivia_mutex);
        reply_ephemeral(client, event,
                        "⏰ This trivia question has expired!");
        return;
    }

    if (time(NULL) > sess->expires) {
        sess->active = 0;
        pthread_mutex_unlock(&s_trivia_mutex);
        reply_ephemeral(client, event,
                        "⏰ Time's up! The question has expired.");
        return;
    }

    char correct_copy[512];
    strncpy(correct_copy, sess->correct, sizeof correct_copy - 1);
    sess->active = 0; /* mark used */
    pthread_mutex_unlock(&s_trivia_mutex);

    /* The user's chosen label is available via the button's label but we need
     * to find which button was pressed.  Since we stored the correct answer
     * text in the session, we compare against the button label through the
     * component tree (components is a NTL: struct discord_component **). */
    const char *pressed_label = NULL;
    if (event->message && event->message->components) {
        /* Iterate action rows */
        for (int r = 0; event->message->components[r] != NULL; r++) {
            struct discord_component *row = event->message->components[r];
            if (!row->components) continue;
            /* Iterate buttons within the row */
            for (int b = 0; row->components[b] != NULL; b++) {
                struct discord_component *btn = row->components[b];
                if (btn->custom_id && strcmp(btn->custom_id, id) == 0) {
                    pressed_label = btn->label;
                    break;
                }
            }
        }
    }

    int correct = pressed_label &&
                  strcmp(pressed_label, correct_copy) == 0;

    char reply[768];
    if (correct) {
        snprintf(reply, sizeof reply,
                 "✅ **Correct!** The answer was: **%s**", correct_copy);
    } else {
        snprintf(reply, sizeof reply,
                 "❌ **Wrong!** The correct answer was: **%s**", correct_copy);
    }

    /* Empty NTL component list to clear the buttons */
    struct discord_component *no_components[1] = { NULL };

    struct discord_interaction_response resp = {
        .type = DISCORD_INTERACTION_CALLBACK_UPDATE_MESSAGE,
        .data = &(struct discord_interaction_callback_data){
            .content    = reply,
            .flags      = MSG_EPHEMERAL,
            .components = no_components,
        },
    };
    discord_create_interaction_response(client, event->id,
                                        event->token, &resp, NULL);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void fun_module_init(struct discord *client, Database *db) {
    (void)client;
    (void)db;
    srand((unsigned)time(NULL));
    printf("[fun] Module initialised.\n");
}

void register_fun_commands(struct discord *client,
                            u64_snowflake_t application_id,
                            u64_snowflake_t guild_id) {
    /* ── /joke ── */
    {
        static struct discord_create_guild_application_command_params p = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "joke",
            .description = "Fetch a random dad joke.",
        };
        discord_create_guild_application_command(client, application_id,
                                                 guild_id, &p, NULL);
    }

    /* ── /roll ── */
    {
        static struct discord_application_command_option max_opt = {
            .type        = DISCORD_APPLICATION_COMMAND_OPTION_INTEGER,
            .name        = "max",
            .description = "Roll between 1 and this number (default: 100)",
            .required    = false,
        };
        static struct discord_application_command_option *roll_opts[] = {
            &max_opt, NULL,
        };
        static struct discord_create_guild_application_command_params p = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "roll",
            .description = "Roll a random number between 1 and N.",
            .options     = roll_opts,
        };
        discord_create_guild_application_command(client, application_id,
                                                 guild_id, &p, NULL);
    }

    /* ── /8ball ── */
    {
        static struct discord_application_command_option q_opt = {
            .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name        = "question",
            .description = "Your yes/no question",
            .required    = true,
        };
        static struct discord_application_command_option *ball_opts[] = {
            &q_opt, NULL,
        };
        static struct discord_create_guild_application_command_params p = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "8ball",
            .description = "Ask the magic 8-ball a question.",
            .options     = ball_opts,
        };
        discord_create_guild_application_command(client, application_id,
                                                 guild_id, &p, NULL);
    }

    /* ── /choose ── */
    {
        static struct discord_application_command_option list_opt = {
            .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name        = "options",
            .description = "Comma-separated list of items to choose from",
            .required    = true,
        };
        static struct discord_application_command_option *choose_opts[] = {
            &list_opt, NULL,
        };
        static struct discord_create_guild_application_command_params p = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "choose",
            .description = "Randomly pick from a comma-separated list.",
            .options     = choose_opts,
        };
        discord_create_guild_application_command(client, application_id,
                                                 guild_id, &p, NULL);
    }

    /* ── /coinflip ── */
    {
        static struct discord_create_guild_application_command_params p = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "coinflip",
            .description = "Flip a coin – heads or tails.",
        };
        discord_create_guild_application_command(client, application_id,
                                                 guild_id, &p, NULL);
    }

    /* ── /rps ── */
    {
        static struct discord_application_command_option choice_opt = {
            .type        = DISCORD_APPLICATION_COMMAND_OPTION_STRING,
            .name        = "choice",
            .description = "rock, paper, or scissors",
            .required    = true,
        };
        static struct discord_application_command_option *rps_opts[] = {
            &choice_opt, NULL,
        };
        static struct discord_create_guild_application_command_params p = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "rps",
            .description = "Play rock-paper-scissors against the bot.",
            .options     = rps_opts,
        };
        discord_create_guild_application_command(client, application_id,
                                                 guild_id, &p, NULL);
    }

    /* ── /trivia ── */
    {
        static struct discord_create_guild_application_command_params p = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "trivia",
            .description = "Answer a random trivia question.",
        };
        discord_create_guild_application_command(client, application_id,
                                                 guild_id, &p, NULL);
    }

    /* ── /activity ── */
    {
        static struct discord_create_guild_application_command_params p = {
            .type        = DISCORD_APPLICATION_COMMAND_CHAT_INPUT,
            .name        = "activity",
            .description = "Show the 3 most active channels this session.",
        };
        discord_create_guild_application_command(client, application_id,
                                                 guild_id, &p, NULL);
    }

    printf("[fun] Commands registered in guild %" PRIu64 "\n", guild_id);
}

void on_fun_interaction(struct discord *client,
                        const struct discord_interaction *event) {
    if (!event->data) return;

    /* Component (button) interaction */
    if (event->type == DISCORD_INTERACTION_MESSAGE_COMPONENT) {
        if (event->data->custom_id &&
            strncmp(event->data->custom_id, "trivia:", 7) == 0) {
            handle_trivia_button(client, event);
        }
        return;
    }

    if (event->type != DISCORD_INTERACTION_APPLICATION_COMMAND) return;

    const char *cmd = event->data->name;

    if      (strcmp(cmd, "joke")     == 0) cmd_joke(client, event);
    else if (strcmp(cmd, "roll")     == 0) cmd_roll(client, event);
    else if (strcmp(cmd, "8ball")    == 0) cmd_8ball(client, event);
    else if (strcmp(cmd, "choose")   == 0) cmd_choose(client, event);
    else if (strcmp(cmd, "coinflip") == 0) cmd_coinflip(client, event);
    else if (strcmp(cmd, "rps")      == 0) cmd_rps(client, event);
    else if (strcmp(cmd, "trivia")   == 0) cmd_trivia(client, event);
    else if (strcmp(cmd, "activity") == 0) cmd_activity(client, event);
}