#pragma once
/*
 * database_propagation.h
 *
 * Public API for the SQLite-backed propagation system.
 *
 * New in this revision
 * ────────────────────
 *  • PropagationSeverity  – computed from trust-weighted confirmation count
 *  • GuildTrustLevel      – per-guild credibility rating
 *  • AppealStatus         – lifecycle of a user's appeal
 *  • PropagationAppeal    – full appeal record
 *  • Alert reports        – staff flagging of false/abusive alerts
 *  • Central admin config – designated oversight guild + channel
 */

#include <stdint.h>
#include <stdbool.h>

/* ── Severity ────────────────────────────────────────────────────────────── */
/*
 * Severity is recomputed whenever a new server confirms the situation
 * (i.e. a notification is delivered) or when an alert is reported.
 * It is stored on the event row and recalculated with
 * db_recompute_severity().
 *
 * Weighting per confirming-guild trust level:
 *   UNVERIFIED  → 1 pt   TRUSTED → 2 pts
 *   VERIFIED    → 3 pts  PARTNER → 4 pts
 *
 * Thresholds (weighted points):
 *   0      → UNCONFIRMED
 *   1–3    → LOW
 *   4–8    → MEDIUM
 *   9–15   → HIGH
 *   16+    → CRITICAL
 */
typedef enum {
    SEVERITY_UNCONFIRMED = 0,
    SEVERITY_LOW         = 1,
    SEVERITY_MEDIUM      = 2,
    SEVERITY_HIGH        = 3,
    SEVERITY_CRITICAL    = 4
} PropagationSeverity;

const char *severity_name (PropagationSeverity s);
const char *severity_emoji(PropagationSeverity s);
const char *severity_colour_hex(PropagationSeverity s); /* informational */

/* ── Guild trust ─────────────────────────────────────────────────────────── */
typedef enum {
    TRUST_UNVERIFIED = 0,   /* default for all new servers           */
    TRUST_TRUSTED    = 1,   /* manually vouched for by an admin      */
    TRUST_VERIFIED   = 2,   /* well-established community            */
    TRUST_PARTNER    = 3    /* partner / central-network admin guild */
} GuildTrustLevel;

const char *trust_level_name (GuildTrustLevel t);
const char *trust_level_badge(GuildTrustLevel t);  /* emoji badge */

/* ── Appeal status ───────────────────────────────────────────────────────── */
typedef enum {
    APPEAL_PENDING      = 0,
    APPEAL_UNDER_REVIEW = 1,
    APPEAL_APPROVED     = 2,   /* alert retracted; user vindicated */
    APPEAL_DENIED       = 3
} AppealStatus;

const char *appeal_status_name (AppealStatus a);
const char *appeal_status_emoji(AppealStatus a);

/* ── Data types ──────────────────────────────────────────────────────────── */

typedef struct {
    int64_t  id;
    uint64_t target_user_id;
    uint64_t source_guild_id;
    uint64_t moderator_id;
    char    *reason;
    char    *evidence_url;
    int64_t  timestamp;
    /* ── new fields ── */
    PropagationSeverity severity;
    int                 weighted_confirmation_score;
    int                 report_count;
} PropagationEvent;

typedef struct {
    int64_t      id;
    int64_t      propagation_id;
    uint64_t     user_id;
    char        *statement;
    AppealStatus status;
    uint64_t     reviewed_by;      /* 0 if not yet reviewed */
    int64_t      reviewed_at;      /* 0 if not yet reviewed */
    char        *reviewer_notes;   /* NULL if not yet reviewed */
    int64_t      submitted_at;
} PropagationAppeal;

/* ── Init ────────────────────────────────────────────────────────────────── */
int db_propagation_init(Database *db);

/* ── Events ──────────────────────────────────────────────────────────────── */
int64_t db_add_propagation_event(Database   *db,
                                  uint64_t    target_user_id,
                                  uint64_t    source_guild_id,
                                  uint64_t    moderator_id,
                                  const char *reason,
                                  const char *evidence_url);

int db_record_propagation_notification(Database *db,
                                        int64_t   propagation_id,
                                        uint64_t  guild_id);

/*
 * Call after recording a notification.  Recomputes the weighted
 * confirmation score and updates the severity column in one transaction.
 * Returns the new severity, or SEVERITY_UNCONFIRMED on error.
 */
PropagationSeverity db_recompute_severity(Database *db, int64_t propagation_id);

int db_get_propagation_events(Database          *db,
                               uint64_t           target_user_id,
                               PropagationEvent **events_out,
                               int               *count_out);

int db_get_propagation_event_by_id(Database         *db,
                                    int64_t           event_id,
                                    PropagationEvent *event_out);

void db_free_propagation_events(PropagationEvent *events, int count);
void db_free_propagation_event (PropagationEvent *event);   /* single heap event */

/* ── Guild config ────────────────────────────────────────────────────────── */
int      db_set_propagation_config     (Database *db, uint64_t guild_id,
                                         uint64_t channel_id, int opted_in);
uint64_t db_get_propagation_channel    (Database *db, uint64_t guild_id);
bool     db_guild_propagation_opted_in (Database *db, uint64_t guild_id);
int      db_get_opted_in_guilds        (Database *db,
                                         uint64_t **guild_ids_out,
                                         int       *count_out);

/* ── Guild trust ─────────────────────────────────────────────────────────── */
int           db_set_guild_trust(Database *db, uint64_t guild_id,
                                  GuildTrustLevel level, uint64_t set_by,
                                  const char *notes);
GuildTrustLevel db_get_guild_trust(Database *db, uint64_t guild_id);

/* ── Moderator blacklist ─────────────────────────────────────────────────── */
int  db_blacklist_moderator       (Database *db, uint64_t moderator_id,
                                    uint64_t banned_by, const char *reason);
bool db_is_moderator_blacklisted  (Database *db, uint64_t moderator_id);

/* ── Alert reports (staff flagging a suspicious/false alert) ─────────────── */
/*
 * Returns the new report ID, or -1 on error.
 * Also increments propagation_events.report_count and recomputes severity.
 */
int64_t db_report_alert          (Database   *db,
                                   int64_t     propagation_id,
                                   uint64_t    reporter_guild,
                                   uint64_t    reporter_mod,
                                   const char *reason);

int db_get_alert_report_count    (Database *db, int64_t propagation_id);

/* ── Appeals ─────────────────────────────────────────────────────────────── */
/*
 * Returns the new appeal ID, or -1 on error / duplicate.
 * Only one active appeal per (propagation_id, user_id) is permitted.
 */
int64_t db_submit_appeal(Database   *db,
                          int64_t     propagation_id,
                          uint64_t    user_id,
                          const char *statement);

int db_update_appeal_status(Database   *db,
                             int64_t     appeal_id,
                             AppealStatus status,
                             uint64_t    reviewed_by,
                             const char *reviewer_notes);

/*
 * Fills *appeal_out; caller must call db_free_appeal() when done.
 * Returns 0 on success, 1 if not found, -1 on error.
 */
int db_get_appeal_for_event(Database         *db,
                             int64_t           propagation_id,
                             uint64_t          user_id,
                             PropagationAppeal *appeal_out);

int db_get_appeal_by_id(Database         *db,
                         int64_t           appeal_id,
                         PropagationAppeal *appeal_out);

void db_free_appeal(PropagationAppeal *appeal);

/* ── Central admin config ────────────────────────────────────────────────── */
int  db_set_central_config(Database *db, uint64_t guild_id,
                             uint64_t channel_id, uint64_t set_by);
bool db_get_central_config(Database *db,
                             uint64_t *guild_id_out,
                             uint64_t *channel_id_out);

/* ── Known guilds ────────────────────────────────────────────────────────── */
int db_register_known_guild(Database *db, uint64_t guild_id);