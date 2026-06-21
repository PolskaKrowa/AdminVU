/*
 * components_v2.c
 *
 * Implementation of the Components V2 builder and REST sender.
 *
 * JSON design
 * -----------
 * We use the same JB (JSON Builder) pattern as api.c but with a heap-allocated
 * dynamic buffer that grows on demand (up to CV2_BUF_MAX bytes).  The buffer
 * accumulates component JSON as comma-separated objects; cv2_build_json() then
 * wraps them in the message envelope.
 *
 * A small stack (CV2_MAX_DEPTH frames) tracks which component type we are
 * currently writing children into and how many children we have already
 * emitted at each level.  This lets us insert commas between siblings without
 * knowing in advance how many there will be.
 *
 * Network design
 * --------------
 * Orca's typed struct layer predates Discord's Components V2 API and does not
 * expose the IS_COMPONENTS_V2 message flag or the new component types.  We
 * therefore POST directly to the Discord REST endpoint using libcurl, exactly
 * as moderation.c does for the timeout PATCH call.
 */

#include "components_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>

#include <curl/curl.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

#define CV2_BUF_INIT    4096    /* initial component buffer size (bytes)     */
#define CV2_BUF_MAX     131072  /* hard cap -- Discord's max payload is ~8 MB */
                                /* but sane messages won't approach this     */
#define CV2_MAX_DEPTH   16      /* max nesting depth (container→section→...)  */

/* IS_COMPONENTS_V2 message flag -- required by Discord for V2 payloads. */
#define CV2_FLAG        32768   /* 1 << 15 */

/* Discord REST base URL. */
#define DISCORD_API     "https://discord.com/api/v10"

/* ── Context types ──────────────────────────────────────────────────────── */

/*
 * Each frame on the stack records:
 *   type              -- what kind of parent are we inside?
 *   child_count       -- siblings already emitted (drives comma insertion)
 *   sec_comps_closed  -- section only: has the components[] array been closed?
 */
typedef enum {
    CTX_ROOT = 0,   /* top-level: sibling components go at the root        */
    CTX_CONTAINER,  /* inside a container's "components" array             */
    CTX_SECTION,    /* inside a section's "components" array               */
    CTX_MEDIA,      /* inside a media_gallery's "items" array              */
    CTX_ROW,        /* inside an action_row's "components" array           */
} CtxType;

typedef struct {
    CtxType type;
    int     child_count;       /* siblings emitted so far at this level     */
    int     sec_comps_closed;  /* section: 1 after thumbnail closes comps[] */
} Frame;

/* ── CV2Msg struct ──────────────────────────────────────────────────────── */

struct CV2Msg {
    /* Component JSON accumulator (comma-separated component objects). */
    char   *buf;
    size_t  pos;
    size_t  cap;
    int     truncated;  /* 1 if we hit CV2_BUF_MAX */

    /* Error flag set on bad nesting (e.g. stack overflow). */
    int error;

    /* Nesting stack. depth=1 at init (root frame pre-pushed). */
    Frame stack[CV2_MAX_DEPTH];
    int   depth;

    /* Cached full JSON payload built by cv2_build_json(). */
    char *json_out;
};

/* ── Module state ───────────────────────────────────────────────────────── */

static char g_token[512] = { 0 };

void cv2_components_init(const char *token) {
    if (token)
        snprintf(g_token, sizeof g_token, "%s", token);
    printf("[cv2] Components V2 module initialised.\n");
}

/* ── Internal buffer helpers ────────────────────────────────────────────── */

/*
 * _grow -- ensure at least `need` additional bytes are available.
 * Returns 1 on success, 0 if the cap would be exceeded.
 */
static int _grow(struct CV2Msg *m, size_t need) {
    if (m->truncated) return 0;
    if (m->pos + need + 1 < m->cap) return 1;

    size_t newcap = m->cap;
    while (newcap < m->pos + need + 1)
        newcap *= 2;

    if (newcap > CV2_BUF_MAX) {
        m->truncated = 1;
        fprintf(stderr, "[cv2] Buffer cap (%d bytes) exceeded.\n", CV2_BUF_MAX);
        return 0;
    }

    char *nb = realloc(m->buf, newcap);
    if (!nb) { m->truncated = 1; return 0; }
    m->buf = nb;
    m->cap = newcap;
    return 1;
}

/* Append a raw string (no escaping). */
static void _raw(struct CV2Msg *m, const char *s) {
    if (m->truncated) return;
    size_t len = strlen(s);
    if (!_grow(m, len)) return;
    memcpy(m->buf + m->pos, s, len);
    m->pos += len;
    m->buf[m->pos] = '\0';
}

/* Append a single character. */
static void _char(struct CV2Msg *m, char c) {
    if (m->truncated) return;
    if (!_grow(m, 1)) return;
    m->buf[m->pos++] = c;
    m->buf[m->pos]   = '\0';
}

/* printf-style append. */
static void _printf(struct CV2Msg *m, const char *fmt, ...) {
    if (m->truncated) return;
    /* Grow by a reasonable estimate; retry on truncation. */
    if (!_grow(m, 256)) return;
    for (;;) {
        size_t avail = m->cap - m->pos;
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(m->buf + m->pos, avail, fmt, ap);
        va_end(ap);
        if (n < 0) { m->truncated = 1; return; }
        if ((size_t)n < avail) { m->pos += (size_t)n; m->buf[m->pos] = '\0'; return; }
        if (!_grow(m, (size_t)n + 1)) return;
    }
}

/*
 * _str -- JSON-escape `s` and append it surrounded by double quotes.
 *         Handles the standard JSON escape sequences.
 */
static void _str(struct CV2Msg *m, const char *s) {
    _char(m, '"');
    if (!s) { _char(m, '"'); return; }
    for (const char *p = s; *p; p++) {
        switch ((unsigned char)*p) {
            case '"':  _raw(m, "\\\""); break;
            case '\\': _raw(m, "\\\\"); break;
            case '\n': _raw(m, "\\n");  break;
            case '\r': _raw(m, "\\r");  break;
            case '\t': _raw(m, "\\t");  break;
            default:
                if ((unsigned char)*p < 0x20) {
                    /* Control character -- encode as \uXXXX */
                    _printf(m, "\\u%04x", (unsigned char)*p);
                } else {
                    _char(m, *p);
                }
        }
    }
    _char(m, '"');
}

/* ── Stack helpers ──────────────────────────────────────────────────────── */

static Frame *_top(struct CV2Msg *m) {
    return &m->stack[m->depth - 1];
}

static void _push(struct CV2Msg *m, CtxType t) {
    if (m->depth >= CV2_MAX_DEPTH) {
        fprintf(stderr, "[cv2] Nesting depth exceeded (%d).\n", CV2_MAX_DEPTH);
        m->error = 1;
        return;
    }
    Frame *f         = &m->stack[m->depth++];
    f->type          = t;
    f->child_count   = 0;
    f->sec_comps_closed = 0;
}

static void _pop(struct CV2Msg *m) {
    if (m->depth > 1) m->depth--;
}

/*
 * _comma -- emit a comma if we have already written at least one sibling
 *           at the current nesting level, then increment the sibling count.
 */
static void _comma(struct CV2Msg *m) {
    Frame *f = _top(m);
    if (f->child_count++ > 0) _char(m, ',');
}

/* ── Public API: lifecycle ──────────────────────────────────────────────── */

CV2Msg *cv2_msg_new(void) {
    struct CV2Msg *m = calloc(1, sizeof *m);
    if (!m) return NULL;

    m->buf = malloc(CV2_BUF_INIT);
    if (!m->buf) { free(m); return NULL; }
    m->cap      = CV2_BUF_INIT;
    m->buf[0]   = '\0';

    /* Pre-push the root frame so _comma() works from the first call. */
    m->stack[0].type        = CTX_ROOT;
    m->stack[0].child_count = 0;
    m->depth                = 1;

    return m;
}

void cv2_msg_free(CV2Msg *m) {
    if (!m) return;
    free(m->buf);
    free(m->json_out);
    free(m);
}

/* ── Public API: Text Display (type 10) ─────────────────────────────────── */

void cv2_add_text(CV2Msg *m, const char *content) {
    if (!m || m->error || m->truncated) return;
    _comma(m);
    _raw(m, "{\"type\":10,\"content\":");
    _str(m, content);
    _char(m, '}');
}

/* Alias used inside sections (same JSON, different calling context). */
void cv2_section_text(CV2Msg *m, const char *content) {
    cv2_add_text(m, content);
}

/* ── Public API: Separator (type 14) ────────────────────────────────────── */

void cv2_add_separator(CV2Msg *m, int spacing, int divider) {
    if (!m || m->error || m->truncated) return;
    _comma(m);
    _printf(m, "{\"type\":14,\"spacing\":%d,\"divider\":%s}",
            spacing, divider ? "true" : "false");
}

/* ── Public API: Container (type 17) ────────────────────────────────────── */

void cv2_begin_container(CV2Msg *m, int accent_color) {
    if (!m || m->error || m->truncated) return;
    _comma(m);
    _raw(m, "{\"type\":17");
    if (accent_color >= 0)
        _printf(m, ",\"accent_color\":%d", accent_color);
    _raw(m, ",\"components\":[");
    _push(m, CTX_CONTAINER);
}

void cv2_end_container(CV2Msg *m) {
    if (!m || m->error) return;
    _raw(m, "]}");
    _pop(m);
}

/* ── Public API: Section (type 9) ───────────────────────────────────────── */

/*
 * Section layout in Discord's JSON:
 *
 *   {
 *     "type": 9,
 *     "components": [ <text display(s)> ],
 *     "accessory":  { <thumbnail> }        ← optional, added by section_thumbnail()
 *   }
 *
 * We open the components array here; section_thumbnail() closes it before
 * writing the accessory object; end_section() closes whichever is still open.
 */
void cv2_begin_section(CV2Msg *m) {
    if (!m || m->error || m->truncated) return;
    _comma(m);
    _raw(m, "{\"type\":9,\"components\":[");
    _push(m, CTX_SECTION);
}

void cv2_section_thumbnail(CV2Msg *m, const char *url,
                            const char *description, int spoiler) {
    if (!m || m->error || m->truncated) return;

    Frame *f = _top(m);
    if (f->type != CTX_SECTION) {
        fprintf(stderr, "[cv2] section_thumbnail() called outside a section.\n");
        m->error = 1;
        return;
    }
    if (f->sec_comps_closed) {
        fprintf(stderr, "[cv2] section_thumbnail() called twice in one section.\n");
        m->error = 1;
        return;
    }

    /* Close the section's components[] array and open the accessory object. */
    _raw(m, "],\"accessory\":{\"type\":11,\"media\":{\"url\":");
    _str(m, url);
    _char(m, '}');
    if (description) {
        _raw(m, ",\"description\":");
        _str(m, description);
    }
    _printf(m, ",\"is_spoiler\":%s}", spoiler ? "true" : "false");

    f->sec_comps_closed = 1;
}

void cv2_end_section(CV2Msg *m) {
    if (!m || m->error) return;

    Frame *f = _top(m);
    if (!f->sec_comps_closed)
        _raw(m, "]}");   /* close components[] + section object */
    else
        _char(m, '}');   /* components[] already closed by thumbnail; just close section */

    _pop(m);
}

/* ── Public API: Media Gallery (type 12) ────────────────────────────────── */

void cv2_begin_media_gallery(CV2Msg *m) {
    if (!m || m->error || m->truncated) return;
    _comma(m);
    _raw(m, "{\"type\":12,\"items\":[");
    _push(m, CTX_MEDIA);
}

void cv2_media_item(CV2Msg *m, const char *url,
                    const char *description, int spoiler) {
    if (!m || m->error || m->truncated) return;
    _comma(m);
    _raw(m, "{\"media\":{\"url\":");
    _str(m, url);
    _char(m, '}');
    if (description) {
        _raw(m, ",\"description\":");
        _str(m, description);
    }
    _printf(m, ",\"is_spoiler\":%s}", spoiler ? "true" : "false");
}

void cv2_end_media_gallery(CV2Msg *m) {
    if (!m || m->error) return;
    _raw(m, "]}");
    _pop(m);
}

/* ── Public API: Action Row (type 1) ────────────────────────────────────── */

void cv2_begin_action_row(CV2Msg *m) {
    if (!m || m->error || m->truncated) return;
    _comma(m);
    _raw(m, "{\"type\":1,\"components\":[");
    _push(m, CTX_ROW);
}

void cv2_button(CV2Msg *m, int style, const char *label,
                const char *custom_id, const char *url, int disabled) {
    if (!m || m->error || m->truncated) return;

    Frame *f = _top(m);
    if (f->type != CTX_ROW) {
        fprintf(stderr, "[cv2] cv2_button() called outside an action row.\n");
        m->error = 1;
        return;
    }

    _comma(m);
    _printf(m, "{\"type\":2,\"style\":%d,\"label\":", style);
    _str(m, label);

    if (custom_id) {
        _raw(m, ",\"custom_id\":");
        _str(m, custom_id);
    }
    if (url) {
        _raw(m, ",\"url\":");
        _str(m, url);
    }
    if (disabled)
        _raw(m, ",\"disabled\":true");

    _char(m, '}');
}

void cv2_end_action_row(CV2Msg *m) {
    if (!m || m->error) return;
    _raw(m, "]}");
    _pop(m);
}

/* ── Build JSON ─────────────────────────────────────────────────────────── */

const char *cv2_build_json(CV2Msg *m) {
    if (!m || m->error || m->truncated) return NULL;

    /*
     * Full payload structure:
     *   {"flags":32768,"components":[<m->buf>]}
     *
     * We allocate a fresh output buffer each time this is called.
     *
     * NOTE: a previous version of this function had a spurious extra
     * memcpy() that copied m->buf to m->json_out and was immediately
     * overwritten by the prefix copy below it — pure dead code that
     * wasted a memcpy on every call.  Removed.
     */
    static const char prefix[] = "{\"flags\":32768,\"components\":[";
    static const char suffix[] = "]}";

    const size_t plen  = sizeof prefix - 1;
    const size_t slen  = sizeof suffix - 1;
    const size_t total = plen + m->pos + slen + 1;

    free(m->json_out);
    m->json_out = malloc(total);
    if (!m->json_out) return NULL;

    memcpy(m->json_out,                 prefix,  plen);
    memcpy(m->json_out + plen,          m->buf,  m->pos);
    memcpy(m->json_out + plen + m->pos, suffix,  slen + 1);

    return m->json_out;
}

/*
 * cv2_build_json_reply -- same as cv2_build_json() but injects a
 * message_reference field so the sent message threads as a reply.
 */
static char *_build_json_inner(CV2Msg *m, uint64_t reply_to_id) {
    if (!m || m->error || m->truncated) return NULL;

    /* Build the reference fragment (empty string if no reply). */
    char ref[128] = "";
    if (reply_to_id)
        snprintf(ref, sizeof ref,
                 "\"message_reference\":{\"message_id\":\"%" PRIu64 "\",\"type\":0},",
                 reply_to_id);

    /*
     * Full payload:
     *   {"flags":32768,<ref>"components":[<buf>]}
     */
    static const char pre1[] = "{\"flags\":32768,";
    static const char pre2[] = "\"components\":[";
    static const char suf[]  = "]}";

    size_t l1    = strlen(pre1);
    size_t lref  = strlen(ref);
    size_t l2    = strlen(pre2);
    size_t lbuf  = m->pos;
    size_t ls    = strlen(suf);
    size_t total = l1 + lref + l2 + lbuf + ls + 1;

    char *out = malloc(total);
    if (!out) return NULL;

    char *p = out;
    memcpy(p, pre1, l1);   p += l1;
    memcpy(p, ref,  lref); p += lref;
    memcpy(p, pre2, l2);   p += l2;
    memcpy(p, m->buf, lbuf); p += lbuf;
    memcpy(p, suf,  ls + 1);   /* +1 includes NUL */

    return out;
}

/* ── libcurl response buffer ────────────────────────────────────────────── */

typedef struct { char *data; size_t len; } CurlBuf;

static size_t _write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    CurlBuf *b = ud;
    size_t  extra = size * nmemb;
    char   *tmp   = realloc(b->data, b->len + extra + 1);
    if (!tmp) return 0;
    b->data = tmp;
    memcpy(b->data + b->len, ptr, extra);
    b->len += extra;
    b->data[b->len] = '\0';
    return extra;
}

/* ── Internal POST helper ───────────────────────────────────────────────── */

/*
 * _post_to_discord
 *
 * POST json_body to the given Discord REST endpoint.
 * Returns 0 on HTTP 2xx, -1 on curl error, HTTP code on Discord error.
 */
static int _post_to_discord(const char *endpoint, const char *json_body) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "[cv2] curl_easy_init() failed.\n");
        return -1;
    }

    char url[256];
    snprintf(url, sizeof url, "%s%s", DISCORD_API, endpoint);

    char auth_hdr[640];
    snprintf(auth_hdr, sizeof auth_hdr, "Authorization: Bot %s", g_token);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_hdr);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    CurlBuf resp = { NULL, 0 };

    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_POST,           1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     json_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  _write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    CURLcode res      = curl_easy_perform(curl);
    long     http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "[cv2] curl error: %s\n", curl_easy_strerror(res));
        free(resp.data);
        return -1;
    }

    if (http_code < 200 || http_code >= 300) {
        fprintf(stderr, "[cv2] Discord API error (HTTP %ld): %s\n",
                http_code, resp.data ? resp.data : "(empty body)");
        free(resp.data);
        return (int)http_code;
    }

    printf("[cv2] Message delivered (HTTP %ld).\n", http_code);
    free(resp.data);
    return 0;
}

/* ── Public API: send ───────────────────────────────────────────────────── */

int cv2_send(CV2Msg *m, uint64_t channel_id) {
    if (!g_token[0]) {
        fprintf(stderr, "[cv2] cv2_components_init() has not been called.\n");
        return -2;
    }
    if (!m || m->error || m->truncated) {
        fprintf(stderr, "[cv2] cv2_send() called with an errored builder.\n");
        return -3;
    }

    char *json = _build_json_inner(m, 0);
    if (!json) return -1;

    char endpoint[64];
    snprintf(endpoint, sizeof endpoint,
             "/channels/%" PRIu64 "/messages", channel_id);

    printf("[cv2] Sending Components V2 message to channel %" PRIu64
           " (%zu bytes).\n", channel_id, strlen(json));

    int rc = _post_to_discord(endpoint, json);
    free(json);
    return rc;
}

int cv2_send_reply(CV2Msg *m, uint64_t channel_id, uint64_t reply_to_id) {
    if (!g_token[0]) {
        fprintf(stderr, "[cv2] cv2_components_init() has not been called.\n");
        return -2;
    }
    if (!m || m->error || m->truncated) {
        fprintf(stderr, "[cv2] cv2_send_reply() called with an errored builder.\n");
        return -3;
    }

    char *json = _build_json_inner(m, reply_to_id);
    if (!json) return -1;

    char endpoint[64];
    snprintf(endpoint, sizeof endpoint,
             "/channels/%" PRIu64 "/messages", channel_id);

    printf("[cv2] Sending Components V2 reply (ref=%" PRIu64
           ") to channel %" PRIu64 " (%zu bytes).\n",
           reply_to_id, channel_id, strlen(json));

    int rc = _post_to_discord(endpoint, json);
    free(json);
    return rc;
}

/* ---------------------------------------------------------------------------
 * cv2_post_raw
 *
 * Post a pre-built JSON payload string directly to a Discord channel.
 * Used by messaging.c's bot_send_cv2_raw() shim for dashboard-originated
 * sends where the caller has already assembled the full envelope JSON.
 *
 * The payload must include "flags":32768 and a "components" array.
 * --------------------------------------------------------------------------- */
int cv2_post_raw(uint64_t channel_id, const char *json_payload) {
    if (!g_token[0]) {
        fprintf(stderr, "[cv2] cv2_post_raw: module not initialised.\n");
        return -2;
    }
    if (!json_payload || !json_payload[0]) {
        fprintf(stderr, "[cv2] cv2_post_raw: NULL or empty payload.\n");
        return -3;
    }

    char endpoint[64];
    snprintf(endpoint, sizeof endpoint,
             "/channels/%" PRIu64 "/messages", channel_id);

    printf("[cv2] Posting raw payload to channel %" PRIu64
           " (%zu bytes).\n", channel_id, strlen(json_payload));

    return _post_to_discord(endpoint, json_payload);
}
