# AdminVU V3

A modular Discord bot written in C using the Orca Discord API library, with x86_64 assembly optimisations and SQLite database for moderation and cross-server alert propagation.

## ⚡ Features

- **Moderation Commands**: Warn, kick, ban, and timeout users
- **Cross-Server Propagation**: Alert moderators across every opted-in server when a user is flagged
- **Database Storage**: SQLite database for persistent user data, moderation logs, and propagation events
- **Assembly Optimisations**: Fast hashing with x86_64 assembly
- **Modular Architecture**: Easy to extend with new commands

## 📋 Available Commands

### General Commands
- `/ping [target]` - Responds with "Pong!" and a hash of the target's username

### Moderation Commands (Requires Kick/Ban/Moderate Members)
- `/warn <user> [reason]` - Issue a warning to a user
- `/warnings <user>` - View all warnings for a user
- `/kick <user> [reason]` - Kick a user from the server
- `/ban <user> [reason]` - Ban a user from the server
- `/timeout <user> [duration] [reason]` - Timeout a user (1–40320 minutes)

### Cross-Server Propagation Commands
- `/propagate <user> <reason> <evidence> <confirm>` - Flag a user across all opted-in servers *(mod only — misuse results in a permanent ban from the system)*
- `/propagate-config <channel_id>` - Set the channel for incoming alerts *(admin only)*
- `/propagate-opt-out` - Stop receiving cross-server alerts *(admin only)*
- `/propagate-history <user>` - View all cross-server alerts for a user *(mod only)*
- `/propagate-revoke <moderator> <reason>` - Permanently blacklist a moderator from issuing alerts *(admin only)*

## Project Structure

```
discord-bot/
├── CMakeLists.txt                  # Root build configuration
├── src/
│   ├── main.c                      # Main entry point
│   ├── env_parser.c/h              # Environment variable parser
│   ├── database.c/h                # SQLite database operations
│   ├── database_propagation.c/h    # Propagation-specific DB layer
│   ├── CMakeLists.txt              # Source build configuration
│   └── modules/
│       ├── ping.c/h                # Ping command module
│       ├── moderation.c/h          # Moderation commands module
│       ├── ticket.c/h              # Ticket system module
│       ├── factcheck.c/h           # Fact-check module
│       └── propagation.c/h         # Cross-server propagation module
├── asm/
│   ├── fast_hash.asm               # x86_64 assembly functions
│   └── CMakeLists.txt              # Assembly build configuration
├── bot_data.db                     # SQLite database (created on first run)
└── .env.example                    # Example environment variables
```

## Prerequisites

### Ubuntu/Debian
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake nasm pkg-config \
    libcurl4-openssl-dev libsqlite3-dev git
```

pthreads is part of glibc on Linux and requires no separate installation.

### Install Orca Library

The Orca library is required. Build from source:

```bash
git clone https://github.com/cee-studio/orca.git
cd orca
make
sudo make install
sudo ldconfig
```

**Verify installation:**
```bash
ls /usr/local/lib/libdiscord*
ls /usr/local/include/orca/discord.h
```

## Building

1. Clone this repository and navigate to it:
```bash
cd discord-bot
```

2. Create a build directory:
```bash
mkdir build && cd build
```

3. Configure and build:
```bash
cmake ..
make
```

## Configuration

1. Create a Discord application at https://discord.com/developers/applications
2. Create a bot and copy the token
3. Enable the following bot permissions:
   - **General Permissions**: Administrator (or specific moderation permissions)
   - **Text Permissions**: Send Messages, Embed Links
   - **Member Permissions**: Kick Members, Ban Members, Moderate Members (Timeout)
4. Enable these **Privileged Gateway Intents** in the Bot settings:
   - Server Members Intent
   - Message Content Intent (if needed)

5. Create a `.env` file in the **parent directory** (one level above the project):

```bash
# From the parent directory containing discord-bot/
cat > .env << EOF
DISCORD_BOT_TOKEN=your_token_here
DISCORD_BOT_GUILD_ID=your_central_administration_server_id
DISCORD_DEV_GUILD_IDS=your_dev_server_ids
EOF
```

**To get your Guild ID:**
- Enable Developer Mode in Discord (User Settings → Advanced → Developer Mode)
- Right-click your server icon and select "Copy Server ID"

**Directory structure:**
```
parent-directory/
├── .env                    # Put your .env file here
└── discord-bot/
    ├── build/
    ├── bot_data.db         # Database created here on first run
    └── src/
        └── discord_bot     # The bot executable
```

## Running

```bash
./build/src/discord_bot
```

On first run, the bot will create `bot_data.db` in the project root directory.

## Database Schema

The bot creates tables automatically on first run.

### `users` table
- `user_id` - Discord user snowflake ID
- `guild_id` - Discord guild snowflake ID
- `created_at` - Unix timestamp

### `warnings` table
- `id` - Auto-incrementing primary key
- `user_id` - Discord user snowflake ID
- `guild_id` - Discord guild snowflake ID
- `moderator_id` - Discord moderator snowflake ID
- `reason` - Warning reason text
- `timestamp` - Unix timestamp

### `mod_logs` table
- `id` - Auto-incrementing primary key
- `action_type` - Type of action (warn, kick, ban, timeout)
- `user_id` - Discord user snowflake ID
- `guild_id` - Discord guild snowflake ID
- `moderator_id` - Discord moderator snowflake ID
- `reason` - Action reason text
- `timestamp` - Unix timestamp

### `propagation_events` table
- `id` - Auto-incrementing primary key
- `target_user_id` - Flagged user's snowflake ID
- `source_guild_id` - Guild the alert originated from
- `moderator_id` - Moderator who issued the alert
- `reason` - Plain-text reason
- `evidence_url` - URL to evidence (screenshot, video, etc.)
- `timestamp` - Unix timestamp

### `propagation_notifications` table
- `id` - Auto-incrementing primary key
- `propagation_id` - Foreign key to `propagation_events`
- `guild_id` - Guild that was notified
- `notified_at` - Unix timestamp

### `propagation_guild_config` table
- `guild_id` - Primary key
- `notification_channel` - Channel ID for incoming alerts
- `opted_in` - 1 = receiving alerts, 0 = opted out

### `propagation_blacklist` table
- `moderator_id` - Primary key
- `banned_by` - Admin who issued the blacklist
- `reason` - Reason for removal
- `banned_at` - Unix timestamp

### `known_guilds` table
- `guild_id` - Primary key
- `registered_at` - Unix timestamp

## Architecture Notes

### Off-thread command registration

Slash commands are registered via blocking REST calls to Discord's API. Performing these inside the `on_ready` callback — which runs on the same thread as the WebSocket heartbeat — will starve the connection and cause the bot to disconnect, typically right as the largest command batch (propagation) is being sent.

To avoid this, `on_ready` spawns a detached `pthread` that registers all commands in the background whilst the event loop continues uninterrupted.

### Propagation delivery

When `/propagate` is fired, the bot iterates every opted-in guild, calls `discord_get_guild_member` to confirm the target is present, and posts a rich alert embed to that guild's configured channel. No automatic action is taken against the user in any receiving guild — moderators there make their own decisions. Every alert is persisted to the database with a unique ID so it can be referenced, queried, or disputed later.

## Usage Examples

### Moderation Workflow

1. **Warn a user for spam:**
   ```
   /warn user:@BadUser reason:Spamming in general chat
   ```

2. **Check a user's warning history:**
   ```
   /warnings user:@BadUser
   ```

3. **Timeout a user for 60 minutes:**
   ```
   /timeout user:@BadUser duration:60 reason:Continued rule violations
   ```

4. **Kick a user:**
   ```
   /kick user:@BadUser reason:Multiple warnings ignored
   ```

5. **Ban a user:**
   ```
   /ban user:@BadUser reason:Severe ToS violation
   ```

### Propagation Workflow

1. **Configure your server to receive alerts** *(admin, run once)*:
   ```
   /propagate-config channel_id:123456789012345678
   ```

2. **Flag a user across the network** *(mod)*:
   ```
   /propagate user:@BadUser reason:Doxxing members evidence:https://imgur.com/... confirm:I UNDERSTAND THE CONSEQUENCES
   ```
   The bot will only send the alert to guilds where the target is actually a member.

3. **Check a user's alert history**:
   ```
   /propagate-history user:@BadUser
   ```

4. **Remove a moderator from the system** *(admin)*:
   ```
   /propagate-revoke moderator:@BadMod reason:Issuing false alerts
   ```

5. **Opt your server out of the network** *(admin)*:
   ```
   /propagate-opt-out
   ```

## Permissions

| Command | Required permission |
|---|---|
| `/warn`, `/warnings` | Kick Members or Moderate Members |
| `/kick` | Kick Members |
| `/ban` | Ban Members |
| `/timeout` | Moderate Members |
| `/propagate`, `/propagate-history` | Kick Members, Ban Members, or Moderate Members |
| `/propagate-config`, `/propagate-opt-out`, `/propagate-revoke` | Administrator or Manage Server |

## Adding New Modules

1. Create your module files in `src/modules/`:
   - `your_module.h` - Header file
   - `your_module.c` - Implementation

2. Add to `src/CMakeLists.txt`:
```cmake
add_executable(discord_bot
    main.c
    env_parser.c
    database.c
    modules/ping.c
    modules/moderation.c
    modules/your_module.c  # Add this
)
```

3. Include and initialise in `src/main.c`:
```c
#include "modules/your_module.h"

// In main():
your_module_init(client, &g_database, g_guild_id);
```

4. Add command handler to the combined interaction handler:
```c
void on_interaction_create_combined(struct discord *client,
                                    const struct discord_interaction *event) {
    // ... existing code ...
    
    } else if (strcmp(cmd, "your_command") == 0) {
        your_command_handler(client, event);
    }
}
```

## Troubleshooting

### Bot disconnects after registering propagation commands

Slash-command registration makes blocking HTTP calls. If these run on the WebSocket thread the heartbeat stalls and Discord drops the connection. The fix is already in place — command registration runs on a detached `pthread` spawned from `on_ready`. If you see this happen again, ensure nothing else is making blocking calls inside an event callback.

### Database Issues

**"Database is locked" error:**
- Make sure only one instance of the bot is running
- Check file permissions on `bot_data.db`

**Resetting the database:**
```bash
rm bot_data.db
# The bot will recreate it on next run
```

### Permission Issues

**Bot can't kick/ban/timeout users:**
- Ensure the bot role is above the target user's highest role in the server hierarchy
- Check that the bot has the required permissions in the server settings
- Verify the bot was invited with the correct permission scope

### SQLite Not Found

```bash
sudo apt-get install libsqlite3-dev
```

## Assembly Calling Convention (x86_64 System V ABI)

- First 6 integer/pointer arguments: `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`
- Return value: `rax`
- Caller-saved: `rax`, `rcx`, `rdx`, `rsi`, `rdi`, `r8–r11`
- Callee-saved: `rbx`, `rbp`, `r12–r15`

## Security Considerations

1. **Token Security**: Never commit your `.env` file or expose your bot token
2. **SQL Injection**: The database module uses prepared statements throughout
3. **Permission Checks**: All commands verify Discord permissions before executing
4. **Propagation Abuse**: Every alert is permanently logged with the issuing moderator's ID and guild. Admins can blacklist abusive moderators with `/propagate-revoke`
5. **No Automatic Action**: Propagation alerts are informational only — receiving guilds decide what action, if any, to take

## Licence

This project is provided as-is for educational purposes.

## Contributing

Contributions are welcome! Areas for improvement:
- Move propagation delivery into its own thread to avoid blocking the event loop during large alert batches
- Add more moderation commands (unban, clear warnings, etc.)
- Implement auto-moderation features (spam detection, etc.)
- Add a logging channel configuration per guild
- Create a web dashboard for viewing mod logs and propagation history
- Implement an appeal system for warnings and bans