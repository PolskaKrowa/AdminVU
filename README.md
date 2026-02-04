# Discord Bot in C with Orca - Moderation Edition

A modular Discord bot written in C using the Orca Discord API library, with x86_64 assembly optimisations and SQLite database for moderation.

## ⚡ Features

- **Moderation Commands**: Warn, kick, ban, and timeout users
- **Database Storage**: SQLite database for persistent user data and moderation logs
- **Assembly Optimisations**: Fast hashing with x86_64 assembly
- **Modular Architecture**: Easy to extend with new commands

## 📋 Available Commands

### General Commands
- `/ping [target]` - Responds with "Pong!" and a hash of the target's username

### Moderation Commands (Requires Permissions)
- `/warn <user> [reason]` - Issue a warning to a user
- `/warnings <user>` - View all warnings for a user
- `/kick <user> [reason]` - Kick a user from the server
- `/ban <user> [reason]` - Ban a user from the server
- `/timeout <user> [duration] [reason]` - Timeout a user (1-40320 minutes)

## Project Structure

```
discord-bot/
├── CMakeLists.txt          # Root build configuration
├── src/
│   ├── main.c              # Main entry point with database integration
│   ├── env_parser.c/h      # Environment variable parser
│   ├── database.c/h        # SQLite database operations
│   ├── CMakeLists.txt      # Source build configuration
│   └── modules/
│       ├── ping.c/h        # Ping command module
│       └── moderation.c/h  # Moderation commands module
├── asm/
│   ├── fast_hash.asm       # x86_64 assembly functions
│   └── CMakeLists.txt      # Assembly build configuration
├── bot_data.db             # SQLite database (created on first run)
└── .env.example            # Example environment variables
```

## Prerequisites

### Ubuntu/Debian
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake nasm pkg-config \
    libcurl4-openssl-dev libsqlite3-dev git
```

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
DISCORD_GUILD_ID=your_guild_id_here
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

The bot creates three tables automatically:

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

## Usage Examples

### Moderation Workflow

1. **Warn a user for spam:**
   ```
   /warn user:@BadUser reason:Spamming in general chat
   ```

2. **Check user's warning history:**
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

## Permissions

The moderation commands require specific Discord permissions:

- `/warn`, `/warnings` - Requires **Kick Members** or **Moderate Members**
- `/kick` - Requires **Kick Members** permission
- `/ban` - Requires **Ban Members** permission
- `/timeout` - Requires **Moderate Members** permission

The bot checks these permissions before executing commands and will respond with an error if the user lacks the required permissions.

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
    
    else if (strcmp(cmd, "your_command") == 0) {
        your_command_handler(client, event);
    }
}
```

## Database Operations

To add custom database operations, extend `database.c` and `database.h`:

```c
// In database.h
int db_custom_operation(Database *db, /* parameters */);

// In database.c
int db_custom_operation(Database *db, /* parameters */) {
    const char *sql = "YOUR SQL HERE";
    sqlite3_stmt *stmt;
    
    int rc = sqlite3_prepare_v2(db->db, sql, -1, &stmt, NULL);
    // ... implementation ...
    
    return 0;
}
```

## Troubleshooting

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
- Check that the bot has the required permissions
- Verify the bot has been invited with the correct permission scope

### SQLite Not Found

If you get "sqlite3.h not found" during compilation:
```bash
sudo apt-get install libsqlite3-dev
```

## Assembly Calling Convention (x86_64 System V ABI)

- First 6 integer/pointer arguments: `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`
- Return value: `rax`
- Caller-saved: `rax`, `rcx`, `rdx`, `rsi`, `rdi`, `r8-r11`
- Callee-saved: `rbx`, `rbp`, `r12-r15`

## Security Considerations

1. **Token Security**: Never commit your `.env` file or expose your bot token
2. **SQL Injection**: The database module uses prepared statements to prevent SQL injection
3. **Permission Checks**: All moderation commands verify user permissions before execution
4. **Rate Limiting**: Consider implementing rate limits for commands to prevent abuse

## Licence

This project is provided as-is for educational purposes.

## Contributing

Contributions are welcome! Areas for improvement:
- Add more moderation commands (unban, warnings purge, etc.)
- Implement auto-moderation features (spam detection, etc.)
- Add logging channel configuration
- Create web dashboard for viewing mod logs
- Implement appeal system for warnings/bans