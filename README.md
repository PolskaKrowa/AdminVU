<div align="center">

```
╔════════════════════════════════════════════════════════════╗
║                                                            ║
║  █████╗ ██████╗ ███╗   ███╗██╗███╗   ██╗██╗   ██╗██╗   ██╗ ║
║ ██╔══██╗██╔══██╗████╗ ████║██║████╗  ██║██║   ██║██║   ██║ ║
║ ███████║██║  ██║██╔████╔██║██║██╔██╗ ██║██║   ██║██║   ██║ ║
║ ██╔══██║██║  ██║██║╚██╔╝██║██║██║╚██╗██║╚██╗ ██╔╝██║   ██║ ║
║ ██║  ██║██████╔╝██║ ╚═╝ ██║██║██║ ╚████║ ╚████╔╝ ╚██████╔╝ ║
║ ╚═╝  ╚═╝╚═════╝ ╚═╝     ╚═╝╚═╝╚═╝  ╚═══╝  ╚═══╝   ╚═════╝  ║
║                                                            ║
║                                                            ║
║             A   D I S C O R D   M O D   B O T              ║
║                                                            ║
╚════════════════════════════════════════════════════════════╝
```

<br/>

[![Language](https://img.shields.io/badge/language-C11-blue?style=flat-square&logo=c)](#)
[![Build System](https://img.shields.io/badge/build-CMake-red?style=flat-square&logo=cmake)](#)
[![Database](https://img.shields.io/badge/database-SQLite3-lightgrey?style=flat-square&logo=sqlite)](#)
[![Library](https://img.shields.io/badge/lib-Orca%2FDiscord-5865F2?style=flat-square&logo=discord)](#)
[![Status](https://img.shields.io/badge/status-active%20development-yellow?style=flat-square)](#known-issues--todo)

<br/>

> **A cross-server Discord moderation bot written in C, with an embedded HTTP dashboard, a ticket system, a fact-check module powered by a local Ollama LLM, and a cross-guild propagation alert network.**

<br/>

</div>

---

## 📑 Table of Contents

- [Overview](#overview)
- [Features](#features)
- [Architecture](#architecture)
- [Prerequisites](#prerequisites)
- [Building](#building)
- [Configuration](#configuration)
- [Running](#running)
- [Modules](#modules)
  - [Moderation](#-moderation)
  - [Tickets](#-tickets)
  - [Propagation](#-propagation)
  - [Fact-Check](#-fact-check)
  - [Fun](#-fun)
  - [Ping](#-ping)
- [Dashboard (AdminV-UI)](#dashboard-adminvu)
- [REST API Reference](#rest-api-reference)
- [Assembly Routine](#assembly-routine)
- [Known Issues & TODO](#known-issues--todo)
- [Project Structure](#project-structure)

---

## Overview

**AdminVU** (working title) is a Discord bot written entirely in **C11**, targeting the Linux platform. It uses the [Orca](https://github.com/cee-studio/orca) C Discord library for gateway connectivity, stores all state in an **SQLite3** database, and ships with a self-hosted **HTTP dashboard** (AdminV-UI) accessible at `http://127.0.0.1:8080`.

The bot is modular — each feature lives in its own `.c` file under `src/modules/` — and is designed to be fast, low-dependency, and operator-controlled.

---

## Features

| Category | What it does |
|---|---|
| 🛡️ **Moderation** | `/warn`, `/kick`, `/ban`, `/timeout`, `/warnings` — all actions persisted to SQLite |
| 🎫 **Tickets** | Full DM-relay ticket system with anonymous staff messaging, note-taking, and a per-server config |
| 📡 **Propagation** | Cross-server alert network: flag bad actors, manage trust levels, appeals, and auto-escalation |
| 🧐 **Fact-Check** | Mention the bot + "is this true?" in a reply → local Ollama LLM writes a (satirical) counter-argument |
| 🎉 **Fun** | `/trivia`, `/joke`, `/roll`, `/8ball`, `/choose`, `/coinflip`, `/rps`, `/activity` |
| 📊 **Dashboard** | Embedded HTTP server with a real-time web dashboard (no external hosting required) |
| ⚙️ **ASM** | x86-64 NASM hash routine wired into the `/ping` command |

---

## Architecture

```
discord_bot/
├── asm/                  ← x86-64 NASM routines (fast_hash)
├── src/
│   ├── main.c            ← Entry point, event routing, startup
│   ├── database.c        ← Core DB (users, warnings, mod_logs, timeouts)
│   ├── database_propagation.c  ← Propagation-specific tables & queries
│   ├── api.c             ← JSON REST handlers for the dashboard
│   ├── http_server.c     ← Minimal HTTP/1.0 server (pthreads)
│   ├── env_parser.c      ← .env file loader
│   └── modules/
│       ├── moderation.c
│       ├── ticket.c
│       ├── propagation.c
│       ├── factcheck.c
│       ├── fun.c
│       └── ping.c
├── src/web/              ← AdminV-UI dashboard (HTML + CSS + JS)
│   ├── index.html
│   ├── moderation.html
│   ├── messaging.html
│   ├── propagation.html
│   ├── tickets.html
│   ├── style.css
│   └── script.js
├── .env                  ← Secrets (not committed)
└── CMakeLists.txt
```

```
                         ┌─────────────────────────────┐
                         │       Discord Gateway       │
                         │   (Orca websocket client)   │
                         └──────────────┬──────────────┘
                                        │ events
                         ┌──────────────▼──────────────┐
                         │          main.c             │
                         │   routes interactions &     │
                         │   message events to modules │
                         └────┬──────┬──────┬───────┬──┘
                              │      │      │       │
                  ┌───────────▼─┐ ┌──▼──┐ ┌─▼───┐ ┌─▼───────────┐
                  │  moderation │ │ tkt │ │prop │ │  factcheck  │
                  └─────┬───────┘ └──┬──┘ └──┬──┘ └──────┬──────┘
                        │            │       │           │
                  ┌─────▼────────────▼────────▼──────────▼───────┐
                  │               SQLite3 (bot_data.db)          │
                  └──────────────────────┬───────────────────────┘
                                         │
                  ┌──────────────────────▼───────────────────────┐
                  │     http_server.c  →  api.c  →  AdminV-UI    │
                  │                127.0.0.1:8080                │
                  └──────────────────────────────────────────────┘
```

---

## Prerequisites

| Dependency | Version | Notes |
|---|---|---|
| GCC / Clang | C11 | Any modern version |
| CMake | ≥ 3.16 | |
| NASM | Any | For the ASM subdir |
| libcurl | Any | HTTP requests (fact-check, fun) |
| SQLite3 | Any | Bundled on most distros |
| pthreads | POSIX | For HTTP server thread |
| **Orca** | Latest | Must be installed separately — see below |
| **Ollama** | Any | Only required for fact-check module |

### Installing Orca

```bash
git clone https://github.com/cee-studio/orca.git
cd orca
make
sudo make install
```

### Installing Ollama (optional, for fact-check)

```bash
curl -fsSL https://ollama.com/install.sh | sh
ollama pull ministral-3:8b   # or any model — see FACTCHECK_OLLAMA_MODEL
```

---

## Building

```bash
# Clone the repo
git clone https://github.com/PolskaKrowa/AdminVU.git
cd AdminVU

# Create a build directory
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

> **Note:** The build copies your `.env` file and the `src/web/` assets into the build directory automatically. No manual copying is needed.

### Build Flags

| Flag | Default | Description |
|---|---|---|
| `FACTCHECK_OLLAMA_MODEL` | `"ministral-3:8b"` | Ollama model to use |
| `FACTCHECK_OLLAMA_URL` | `"http://localhost:11434/api/generate"` | Ollama endpoint |

Example:

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DFACTCHECK_OLLAMA_MODEL='"mistral"'
```

---

## Configuration

Create a `.env` file at the project root:

```dotenv
# Required
DISCORD_BOT_TOKEN=your_bot_token_here
DISCORD_BOT_GUILD_ID=your_infra_guild_id

# Optional — comma-separated list of guilds for instant slash-command updates
DISCORD_DEV_GUILD_IDS=123456789,987654321
```

> **Security note:** The `.env` file is copied into the build directory at configure time. Never commit it. It is listed in `.gitignore` by convention.

### Required Bot Permissions

| Permission | Required by |
|---|---|
| `KICK_MEMBERS` | Moderation module |
| `BAN_MEMBERS` | Moderation module |
| `MODERATE_MEMBERS` | Timeout command |
| `MANAGE_CHANNELS` | Ticket channel creation |
| `MANAGE_MESSAGES` | Anonymous staff message relay |
| `SEND_MESSAGES` | All modules |
| `READ_MESSAGE_HISTORY` | Fact-check reply lookup |

Enable the **Message Content** and **Server Members** privileged intents in the Discord Developer Portal.

---

## Running

```bash
cd build
./discord_bot
```

The bot will:
1. Load `.env` (from `../` relative to the binary, or the build dir)
2. Initialise the SQLite database (`bot_data.db`)
3. Start the HTTP dashboard on `http://127.0.0.1:8080`
4. Connect to the Discord gateway
5. Register slash commands (global + dev guilds) in a background thread

---

## Modules

### 🛡️ Moderation

> `src/modules/moderation.c`

All actions are persisted to `mod_logs` and, where applicable, `warnings` / `timeouts`.

| Command | Permission | Description |
|---|---|---|
| `/warn <user> [reason]` | Kick / Ban / Moderate | Issue a warning; reply includes cumulative count |
| `/warnings <user>` | Kick / Ban / Moderate | List all warnings for a user (paginated, up to 10 shown) |
| `/kick <user> [reason]` | Kick Members | Remove a user from the guild |
| `/ban <user> [reason]` | Ban Members | Ban a user; deletes 1 day of messages |
| `/timeout <user> [duration] [reason]` | Moderate Members | Applies `communication_disabled_until` via direct REST PATCH; default 10 min, max 40 320 min (28 days) |

Timeouts are also stored in the `timeouts` table so the bot can track and query them independently of Discord's internal state.

---

### 🎫 Tickets

> `src/modules/ticket.c`

A full **anonymous DM-relay** ticket system. Users never see staff usernames; staff never see the user in the channel.

```
User types /ticket open server:<id> subject:<text>
          │
          ▼
Bot creates a private channel in the staff server's ticket category
          │
          ▼
Bot opens a DM with the user and stores the mapping (user ↔ DM channel ↔ ticket channel)
          │
   ┌──────┴──────┐
   │             │
User DMs bot  Staff types in ticket channel
   │             │
   ▼             ▼
Forwarded to   Deleted + re-posted as [Staff]: …
ticket channel  then forwarded to user's DM
```

**Slash Commands**

| Command | Permission | Description |
|---|---|---|
| `/ticket open <server> <subject>` | Anyone | Opens a ticket for a specific community server |
| `/ticket claim` | Staff (in ticket channel) | Assigns ticket to self, sets status to In Progress |
| `/ticket close [outcome] [notes]` | Staff | Closes the ticket, notifies the user via DM |
| `/ticket priority <level>` | Staff | Updates priority (0 Low → 3 Urgent) |
| `/ticket status <value>` | Staff | Updates status (0 Open → 4 Closed) |
| `/ticket assign <staff>` | Staff | Reassigns ticket to another staff member |
| `/ticketconfig [mainserver] [staffserver] [category] [logchannel]` | Administrator | Configures the ticket system for a guild |

**Mention safety:** All forwarded messages are run through `strip_mentions()` which inserts a Unicode zero-width space (U+200B) after every `@`, preventing `@everyone`, `@here`, and user/role mentions from firing in either direction.

---

### 📡 Propagation

> `src/modules/propagation.c` · `src/database_propagation.c`

A **cross-server alert network**. Opted-in guilds receive notifications when a moderator flags a user. Severity is calculated from a weighted confirmation score based on each guild's trust level.

#### Trust Levels

| Level | Badge | Weight | Description |
|---|---|---|---|
| Unverified | ⚪ | 1 | Default for all guilds |
| Trusted | 🔵 | 2 | Manually set by central admins |
| Verified | ✅ | 3 | Verified community |
| Partner | ⭐ | 4 | Auto-assigned to staff servers and the central guild |

#### Severity Scale

| Score | Level | Emoji |
|---|---|---|
| 0 | Unconfirmed | ⬜ |
| 1 – 3 | Low | 🟡 |
| 4 – 8 | Medium | 🟠 |
| 9 – 15 | High | 🔴 |
| 16+ | Critical | 🚨 |

#### Commands

| Command | Permission | Description |
|---|---|---|
| `/propagate <user> <reason> <evidence> <confirm>` | Mod | Issue a cross-server alert |
| `/propagate-config <channel_id>` | Admin | Set alert delivery channel |
| `/propagate-opt-out` | Admin | Stop receiving alerts |
| `/propagate-history <user>` | Mod | View alert history for a user |
| `/propagate-revoke <moderator> <reason>` | Admin | Permanently blacklist a moderator |
| `/propagate-report <alert_id> <reason>` | Mod | Flag an alert as suspicious (auto-escalates at threshold) |
| `/propagate-appeal <alert_id> <statement>` | Targeted user | Submit an appeal |
| `/propagate-appeal-review <appeal_id> <approve\|deny> [notes]` | Admin | Decide an appeal; broadcasts result to all notified guilds |
| `/propagate-trust <guild_id> <level> [notes]` | Central admin | Set a guild's trust level |
| `/propagate-central <guild_id> <channel_id>` | Bot team only | Designate the central oversight server |
| `/propagate-pair <main_guild_id> <staff_guild_id>` | Bot team only | Pair a community's main and staff servers |

> **Misuse protection:** The `/propagate` command requires typing `I UNDERSTAND THE CONSEQUENCES` verbatim in the `confirm` field. Blacklisted moderators are silently blocked from issuing further alerts.

---

### 🧐 Fact-Check

> `src/modules/factcheck.c`

**Trigger:** Reply to any message, then mention the bot and include `"is this true?"` in your message.

The bot forwards the referenced message's content to a local **Ollama** instance, instructing it to act as a persuasive but dishonest AI that argues against the statement. The response is sent as a reply.

```
User replies to a message and mentions @Bot with "is this true?"
                          │
                  ┌───────▼──────┐
                  │  Fetch the   │
                  │ referenced   │
                  │  message     │
                  └───────┬──────┘
                          │
                  ┌───────▼──────┐
                  │  POST to     │
                  │  Ollama API  │
                  │  (60s limit) │
                  └───────┬──────┘
                          │
                  ┌───────▼──────┐
                  │  Reply with  │
                  │  AI response │
                  └──────────────┘
```

Configure the model and endpoint at compile time via `-DFACTCHECK_OLLAMA_MODEL` and `-DFACTCHECK_OLLAMA_URL`.

---

### 🎉 Fun

> `src/modules/fun.c`

All fun commands return **ephemeral** responses (only visible to the invoker).

| Command | Description |
|---|---|
| `/joke` | Fetches a random dad joke from icanhazdadjoke.com |
| `/roll [max]` | Rolls 1 – N (default: 6) |
| `/8ball <question>` | Classic magic 8-ball |
| `/choose <options>` | Picks randomly from a comma-separated list |
| `/coinflip` | Heads or tails |
| `/rps <rock\|paper\|scissors>` | Rock-paper-scissors against the bot |
| `/trivia` | Fetches a multiple-choice question from Open Trivia DB; interactive buttons with a 60-second timeout |
| `/activity` | Shows the top 3 most active text channels tracked in the current session |

---

### 🏓 Ping

> `src/modules/ping.c`

```
/ping [target]
```

Responds with `Pong! 🏓 (Target: <name>, Hash: 0x<hex>)`.

The hash is computed by the x86-64 NASM routine `fast_hash` (see [Assembly Routine](#assembly-routine)). If a `target` user option is provided, the bot fetches their guild member record via REST and uses their nickname (or username as fallback).

---

## Dashboard (AdminV-UI)

The bot runs a minimal HTTP/1.0 server on `http://127.0.0.1:8080` (loopback only — not externally accessible). The dashboard is a static single-page application served from `src/web/`.

| Page | URL | Description |
|---|---|---|
| Dashboard | `/` | Live stats: guild count, warnings, open tickets, propagation events, uptime |
| Moderation | `/moderation.html` | Paginated mod log with guild and action filters; user warning lookup |
| Propagation | `/propagation.html` | Opted-in server list, alert history, block/unblock controls |
| Tickets | `/tickets.html` | Full ticket list with status/guild filters; click any row to view detail + notes |

> The dashboard polls `/api/status` every 15 seconds. All snowflake IDs are serialised as **strings** to preserve JavaScript precision.

---

## REST API Reference

All endpoints return `application/json`. POST bodies are `application/x-www-form-urlencoded`.

<details>
<summary><strong>GET endpoints</strong></summary>

| Endpoint | Query params | Description |
|---|---|---|
| `GET /api/status` | — | Warning count, open tickets, guild count, propagation events |
| `GET /api/guilds` | — | All known guilds with warning + ticket counts |
| `GET /api/mod-logs` | `guild_id`, `action` (0–3 or `all`), `limit`, `offset` | Paginated mod log |
| `GET /api/warnings` | `guild_id`, `user_id` | Warning records |
| `GET /api/propagation/guilds` | — | All guilds with propagation config + block status |
| `GET /api/propagation/events` | `user_id`, `source_guild_id`, `limit`, `offset` | Alert events with notified counts |
| `GET /api/propagation/blocked` | — | All dashboard-blocked guilds |
| `GET /api/tickets` | `guild_id`, `status` | Ticket list |
| `GET /api/tickets/<id>` | — | Single ticket with notes |
| `GET /api/tickets/events` | `since` (unix ts) | SSE-style event poll |

</details>

<details>
<summary><strong>POST endpoints</strong></summary>

| Endpoint | Body params | Description |
|---|---|---|
| `POST /api/propagation/block` | `guild_id`, `reason` | Block a guild from receiving alerts |
| `POST /api/propagation/unblock` | `guild_id` | Remove a guild block |

</details>

---

## Assembly Routine

> `asm/`

The `/ping` command uses an **x86-64 NASM** hash function (`fast_hash`) wired via `extern uint64_t fast_hash(const char *str, size_t len)`. It is compiled as a separate CMake subdirectory and linked into the main binary.

This serves both as a demonstration of C ↔ ASM interop and as a moderately fast string hash for display purposes.

---

## Known Issues & TODO

> [!WARNING]
> **The following items are known to be broken or incomplete.** Contributions and fixes are welcome — please open an issue or PR referencing the item below.

---

> [!CAUTION]
> ### 🐛 Bug — Dashboard Propagation Sync
> **The Propagation dashboard page does not properly sync with live bot data.**
> - Alert events are not displaying correctly in the events table.
> - Block/unblock actions via the dashboard UI are accepted by the API but the bot does not pick up the change without a restart.
> Needs a polling mechanism or a shared-state refresh path between `api.c` and `propagation.c`.

---

> [!NOTE]
> ### ✨ Feature — Propagation-Linked Tickets
> **Propagation must allow warned users and staff members of other servers to create a ticket relating to a specific propagation warning.**
> Suggested: when an alert fires, the DM to the target user should include a direct `/ticket open` pre-filled link (or a button). Staff in other guilds who receive the alert should also be able to raise a linked ticket referencing the alert ID. A `propagation_id` foreign key column on the `tickets` table would be needed.

---

> [!NOTE]
> ### ✨ Feature — Restrict Who Can Appeal a Propagation Warning
> **Currently, any server can appeal any propagated warning**, regardless of whether they were named in it or received the alert.
> The `handle_appeal` handler in `propagation.c` checks `ev.target_user_id != user_id` but there is no guild-level restriction. Appeals should be restricted to: (a) the targeted user themselves, or (b) staff in a guild that received the alert notification.

---

> [!NOTE]
> ### ✨ Feature — Edit Propagation Message on Updates
> **Each new update to an existing propagation event (severity change, appeal outcome, report threshold) posts a new message instead of editing the original.**
> The `propagation_notifications` table records `guild_id` but not the `message_id` of the alert that was posted. A `message_id` column should be added so that `broadcast_appeal_update()` and severity recalculations can call `discord_edit_message()` on the original embed rather than posting a follow-up.

---

> [!NOTE]
> ### 🎉 Feature — High-Precision Scientific Calculator (for the fun of it)
> **Add a `/calc` command** implementing a high-precision floating-point arithmetic calculator supporting scientific notation, standard mathematical functions (sin, cos, ln, exp, √, etc.), and arbitrary-precision integers. Could use the GNU MPFR library internally, or a bundled arbitrary-precision implementation. Purely for fun.

---

## Project Structure

```
discord_bot/
│
├── asm/                          # x86-64 NASM source
│   └── CMakeLists.txt
│
├── src/
│   ├── main.c                    # Bot entry point
│   ├── database.c / .h           # Core DB operations
│   ├── database_propagation.c / .h
│   ├── api.c / .h                # Dashboard REST API
│   ├── http_server.c / .h        # Embedded HTTP server
│   ├── env_parser.c / .h         # .env loader
│   │
│   ├── modules/
│   │   ├── moderation.c / .h
│   │   ├── ticket.c / .h
│   │   ├── propagation.c / .h
│   │   ├── factcheck.c / .h
│   │   ├── fun.c / .h
│   │   └── ping.c / .h
│   │
│   └── web/                      # AdminV-UI dashboard
│       ├── index.html
│       ├── moderation.html
│       ├── messaging.html
│       ├── propagation.html
│       ├── tickets.html
│       ├── style.css
│       └── script.js
│
├── CMakeLists.txt
├── .env                          # ← not committed
└── README.md
```

---

<div align="center">

```
[ AdminVU ] · built in C · running on 127.0.0.1:8080
```

*Made with way too much `printf` debugging.*

</div>
