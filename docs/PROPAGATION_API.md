# Propagation Network – API Schema

## Overview

This document defines a **read-only JSON API** for external bots to receive propagation network data. The API is intentionally designed to **prevent any external system from submitting or modifying reports**, ensuring integrity and preventing abuse.

### Key Principles

* Read-only access only
* Event-driven (webhooks) + polling (REST)
* Signed payloads for authenticity
* Idempotent event delivery
* Scoped access per bot/guild

---

## Base Event Envelope

All webhook and streaming events follow this structure:

```json
{
  "event_id": "evt_9f3a2c...",
  "type": "alert.created",
  "timestamp": "2026-04-14T12:34:56Z",
  "version": "1.0",
  "data": {}
}
```

---

## Alert Object

Represents a cross-server propagation alert.

```json
{
  "alert_id": 12345,
  "target_user_id": "847293847293847293",
  "source": {
    "guild_id": "123456789012345678",
    "trust_level": 2,
    "trust_label": "Verified"
  },
  "moderator": {
    "user_id": "998877665544332211"
  },
  "reason": "Scamming users via DMs",
  "evidence_url": "https://example.com/evidence.png",
  "severity": {
    "level": 2,
    "label": "Medium"
  },
  "confirmation_score": 4,
  "report_count": 1,
  "created_at": "2026-04-14T12:30:00Z",
  "appeal": {
    "status": "none"
  }
}
```

---

## Webhook Events

### alert.created

Triggered when a new propagation alert is issued.

```json
{
  "type": "alert.created",
  "data": { /* Alert Object */ }
}
```

---

### alert.updated

Triggered when severity or confirmation score changes.

```json
{
  "type": "alert.updated",
  "data": {
    "alert_id": 12345,
    "severity": {
      "level": 3,
      "label": "High"
    },
    "confirmation_score": 7
  }
}
```

---

### alert.reported

Triggered when an alert is reported as suspicious.

```json
{
  "type": "alert.reported",
  "data": {
    "alert_id": 12345,
    "report_count": 3,
    "threshold_reached": true
  }
}
```

---

### appeal.created

Triggered when a user submits an appeal.

```json
{
  "type": "appeal.created",
  "data": {
    "appeal_id": 6789,
    "alert_id": 12345,
    "user_id": "847293847293847293",
    "statement": "I was falsely accused",
    "status": "pending",
    "created_at": "2026-04-14T13:00:00Z"
  }
}
```

---

### appeal.updated

Triggered when an appeal is reviewed.

```json
{
  "type": "appeal.updated",
  "data": {
    "appeal_id": 6789,
    "alert_id": 12345,
    "status": "approved",
    "reviewer_id": "112233445566778899",
    "notes": "Evidence insufficient"
  }
}
```

---

## REST API

### GET /v1/alerts

Query parameters:

* `user_id`
* `since`
* `severity_min`
* `limit`

Response:

```json
{
  "alerts": [ /* Alert Objects */ ],
  "next_cursor": "abc123"
}
```

---

### GET /v1/alerts/{alert_id}

```json
{
  "alert": { /* Alert Object */ }
}
```

---

### GET /v1/alerts/{alert_id}/reports

```json
{
  "alert_id": 12345,
  "report_count": 3,
  "reports": [
    {
      "guild_id": "123",
      "moderator_id": "456",
      "reason": "Looks fabricated",
      "timestamp": "2026-04-14T12:40:00Z"
    }
  ]
}
```

---

### GET /v1/appeals/{alert_id}

```json
{
  "appeals": [
    {
      "appeal_id": 6789,
      "user_id": "847293847293847293",
      "status": "pending",
      "statement": "...",
      "created_at": "...",
      "review": null
    }
  ]
}
```

---

## Authentication

### API Key

```http
Authorization: Bearer sk_live_xxx
```

---

## Webhook Security

Each webhook request includes a signature header:

```http
X-Signature: sha256=...
```

Verification:

```
HMAC(secret, raw_body)
```

---

## Webhook Registration

```json
POST /v1/webhooks
{
  "url": "https://bot.example.com/webhook",
  "events": [
    "alert.created",
    "alert.updated",
    "appeal.updated"
  ]
}
```

---

## Optional: Confidence Object

Provides a derived trust score for consumers.

```json
{
  "confidence": {
    "score": 0.72,
    "factors": {
      "trust_weight": 0.6,
      "confirmations": 4,
      "reports": 1
    }
  }
}
```

---

## Data Scoping

Optional filtering to restrict visibility:

```json
{
  "scope": {
    "guild_id": "123456789",
    "only_if_member": true
  }
}
```

---

## Notes

* No endpoints allow creating, modifying, or deleting alerts
* All data originates from trusted internal moderation flows
* External bots are strictly consumers of propagation data

---

## Versioning

* API version is included in the URL (`/v1/`)
* Event schema version included in payload (`version` field)
* Breaking changes require version bump