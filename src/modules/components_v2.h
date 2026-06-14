/*
 * components_v2.h
 *
 * Builder and sender for Discord Components V2 messages.
 *
 * Discord's Components V2 system (flag 1<<15 = 32768) replaces the old
 * content+embed model with a structured component tree.  Since Orca's typed
 * struct layer predates this API, we serialise the payload ourselves and POST
 * it directly to Discord's REST endpoint via libcurl -- the same bypass
 * pattern used in moderation.c for the timeout PATCH call.
 *
 * ── Usage example ────────────────────────────────────────────────────────
 *
 *   cv2_components_init("Bot_Token_Here");   // once at startup
 *
 *   CV2Msg *m = cv2_msg_new();
 *
 *   cv2_begin_container(m, 0x5865F2);        // blurple accent strip
 *     cv2_add_text(m, "## Hello, world!");
 *     cv2_add_separator(m, CV2_SEP_SMALL, 1);
 *
 *     cv2_begin_section(m);
 *       cv2_section_text(m, "A section with a thumbnail on the right.");
 *       cv2_section_thumbnail(m, "https://example.com/img.png", NULL, 0);
 *     cv2_end_section(m);
 *
 *     cv2_begin_media_gallery(m);
 *       cv2_media_item(m, "https://example.com/a.png", "Image A", 0);
 *       cv2_media_item(m, "https://example.com/b.png", "Image B", 0);
 *     cv2_end_media_gallery(m);
 *
 *     cv2_begin_action_row(m);
 *       cv2_button(m, CV2_BTN_PRIMARY, "Confirm", "confirm:1", NULL, 0);
 *       cv2_button(m, CV2_BTN_DANGER,  "Cancel",  "cancel:1",  NULL, 0);
 *       cv2_button(m, CV2_BTN_LINK,    "Docs",    NULL, "https://example.com", 0);
 *     cv2_end_action_row(m);
 *   cv2_end_container(m);
 *
 *   int rc = cv2_send(m, channel_id);        // POST to Discord
 *   cv2_msg_free(m);
 *
 * ── Nesting rules (mirrors Discord's API) ────────────────────────────────
 *
 *   Root level:  Container, Text Display, Separator, Action Row, Media Gallery
 *   Container:   Text Display, Separator, Section, Action Row, Media Gallery
 *   Section:     exactly one cv2_section_text() + optional cv2_section_thumbnail()
 *   Action Row:  cv2_button() (up to 5)
 *   Media Gallery: cv2_media_item() (up to 10)
 *
 * ── Return codes from cv2_send / cv2_send_reply ───────────────────────────
 *
 *    0   success (HTTP 2xx)
 *   -1   curl initialisation or network failure
 *   -2   module not initialised (call cv2_components_init() first)
 *   -3   builder has an error (depth overflow or buffer overflow)
 *   HTTP status code (e.g. 403, 429) on a Discord API error
 */

#ifndef COMPONENTS_V2_H
#define COMPONENTS_V2_H

#include <stdint.h>
#include <stddef.h>

/* ── Button styles (matches Discord API) ──────────────────────────────── */
#define CV2_BTN_PRIMARY    1  /* blurple */
#define CV2_BTN_SECONDARY  2  /* grey    */
#define CV2_BTN_SUCCESS    3  /* green   */
#define CV2_BTN_DANGER     4  /* red     */
#define CV2_BTN_LINK       5  /* grey + external link icon; needs url, no custom_id */
#define CV2_BTN_PREMIUM    6  /* premium SKU button; needs sku_id */

/* ── Separator spacing ────────────────────────────────────────────────── */
#define CV2_SEP_SMALL  1
#define CV2_SEP_LARGE  2

/* ── Opaque message builder ───────────────────────────────────────────── */
typedef struct CV2Msg CV2Msg;

/* ── Module init ──────────────────────────────────────────────────────── */

/*
 * cv2_components_init
 *
 * Store the bot token used by cv2_send() / cv2_send_reply().
 * Call once during bot startup after the Discord client is initialised,
 * the same way moderation_module_init() accepts the token.
 *
 * token: plain token string -- no "Bot " prefix needed here.
 */
void cv2_components_init(const char *token);

/* ── Builder lifecycle ────────────────────────────────────────────────── */

/* Allocate a new, empty message builder.  Returns NULL on OOM. */
CV2Msg *cv2_msg_new(void);

/* Release the builder and all its associated memory. */
void cv2_msg_free(CV2Msg *m);

/* ── Top-level / general components ──────────────────────────────────── */

/*
 * cv2_add_text
 *
 * Append a Text Display component (type 10).  Supports full Discord markdown.
 * May be used at root level, inside a Container, or inside a Section via
 * cv2_section_text() (see below).
 */
void cv2_add_text(CV2Msg *m, const char *content);

/*
 * cv2_add_separator
 *
 * Append a Separator component (type 14).
 *   spacing: CV2_SEP_SMALL or CV2_SEP_LARGE
 *   divider: 1 to render a visible horizontal rule; 0 for blank spacing only
 */
void cv2_add_separator(CV2Msg *m, int spacing, int divider);

/* ── Container (type 17) ──────────────────────────────────────────────── */

/*
 * cv2_begin_container / cv2_end_container
 *
 * Open/close a Container that groups other components under a coloured accent
 * strip.  accent_color is an RGB integer (e.g. 0x5865F2 for blurple).
 * Pass -1 for no accent colour.
 *
 * Components added between begin and end are children of the container.
 */
void cv2_begin_container(CV2Msg *m, int accent_color);
void cv2_end_container(CV2Msg *m);

/* ── Section (type 9) ─────────────────────────────────────────────────── */

/*
 * cv2_begin_section / cv2_end_section
 *
 * Open/close a Section -- a two-column layout with text on the left and an
 * optional accessory (thumbnail image) on the right.
 *
 * Inside a section, call cv2_section_text() to set the left-column text and
 * optionally cv2_section_thumbnail() to set the right-column image.  Normal
 * cv2_add_text() calls also work as section text components.
 */
void cv2_begin_section(CV2Msg *m);

/*
 * cv2_section_text
 *
 * Add a Text Display inside the current section's components array.
 * Equivalent to cv2_add_text() but only valid between begin/end_section.
 */
void cv2_section_text(CV2Msg *m, const char *content);

/*
 * cv2_section_thumbnail
 *
 * Attach a Thumbnail (type 11) as the section's right-column accessory.
 * Must be called after at least one cv2_section_text() and before
 * cv2_end_section().  Can only be called once per section.
 *
 *   url:         direct image URL
 *   description: alt-text / hover text (may be NULL)
 *   spoiler:     1 to blur the image until clicked
 */
void cv2_section_thumbnail(CV2Msg *m, const char *url,
                            const char *description, int spoiler);

void cv2_end_section(CV2Msg *m);

/* ── Media Gallery (type 12) ──────────────────────────────────────────── */

void cv2_begin_media_gallery(CV2Msg *m);

/*
 * cv2_media_item
 *
 * Add an image to the current media gallery.
 *   url:         direct image URL
 *   description: caption shown below the image (may be NULL)
 *   spoiler:     1 to blur until clicked
 */
void cv2_media_item(CV2Msg *m, const char *url,
                    const char *description, int spoiler);

void cv2_end_media_gallery(CV2Msg *m);

/* ── Action Row (type 1) ──────────────────────────────────────────────── */

void cv2_begin_action_row(CV2Msg *m);

/*
 * cv2_button
 *
 * Append a Button (type 2) to the current action row.
 *
 *   style:     one of CV2_BTN_*
 *   label:     display text (max 80 chars per Discord)
 *   custom_id: interaction identifier -- required for all non-LINK buttons
 *   url:       destination URL -- required for CV2_BTN_LINK buttons
 *   disabled:  1 to grey out the button and prevent interaction
 *
 * For CV2_BTN_LINK, pass NULL for custom_id.
 * For all other styles, pass NULL for url.
 */
void cv2_button(CV2Msg *m, int style, const char *label,
                const char *custom_id, const char *url, int disabled);

void cv2_end_action_row(CV2Msg *m);

/* ── Build / inspect ──────────────────────────────────────────────────── */

/*
 * cv2_build_json
 *
 * Serialise the built component tree into a complete Discord message JSON
 * payload:
 *
 *   {"flags":32768,"components":[...]}
 *
 * The returned pointer is valid until the next call to cv2_build_json() on
 * the same CV2Msg, or until cv2_msg_free() is called.  Caller must NOT free
 * it directly.
 *
 * Returns NULL on OOM or if the builder has encountered an error.
 */
const char *cv2_build_json(CV2Msg *m);

/* ── Send ─────────────────────────────────────────────────────────────── */

/*
 * cv2_send
 *
 * POST the built message to a Discord channel via the REST API.
 *
 * Returns 0 on success, negative on error, or HTTP status on Discord error.
 * See the return-code table in the file header.
 */
int cv2_send(CV2Msg *m, uint64_t channel_id);

/*
 * cv2_send_reply
 *
 * Same as cv2_send(), but threads the message as a reply to reply_to_id.
 */
int cv2_send_reply(CV2Msg *m, uint64_t channel_id, uint64_t reply_to_id);

/*
 * cv2_post_raw
 *
 * Post a fully-formed JSON payload string to a Discord channel.
 * The payload must already include "flags":32768 and "components":[...].
 * Intended for dashboard-side sends where the component tree is already
 * assembled as JSON rather than through the CV2Msg builder.
 *
 * Returns 0 on success, negative on error, or HTTP status on Discord error.
 */
int cv2_post_raw(uint64_t channel_id, const char *json_payload);

#endif /* COMPONENTS_V2_H */
