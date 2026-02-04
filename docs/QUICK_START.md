# Quick Start Guide - Adapting to Your Orca Version

This bot is designed to work with the Orca Discord library, but since Orca has different API versions, you'll need to make a few adjustments based on your installation.

## Step 1: Build the Bot (It will compile!)

```bash
mkdir build && cd build
cmake ..
make
```

The bot should compile successfully. It just needs a few tweaks to actually send messages.

## Step 2: Find Your Orca API Functions

Run these commands to see what functions your Orca installation provides:

```bash
# Find all discord functions
nm -D /usr/local/lib/libdiscord.so | grep discord_ | less

# Look for message-related functions
nm -D /usr/local/lib/libdiscord.so | grep -i "message\|send"

# Check the headers
grep -r "discord_create_message" /usr/local/include/orca/
```

## Step 3: Update the Message Sending Code

Open `src/modules/ping.c` and find the `TODO` section (around line 30).

Replace the `printf` statement with the appropriate call for your Orca version:

### If you see `discord_create_message` in the output:

Look for how the struct is defined:
```bash
grep -A 20 "struct discord_create_message" /usr/local/include/orca/*.h
```

Then use it like:
```c
struct discord_create_message params = { .content = response };
discord_create_message(client, msg->channel_id, &params, NULL);
```

### If you see `discord_send_message`:
```c
discord_send_message(client, msg->channel_id, response);
```

### If you're using Concord (similar to Orca):
```c
struct discord_create_message params = { .content = response };
discord_create_message(client, msg->channel_id, &params, NULL);
```

## Step 4: Fix the Ready Callback (if needed)

If you get errors about `discord_set_on_ready`, check the callback signature:

```bash
grep -A 5 "discord_set_on_ready" /usr/local/include/orca/discord.h
```

Then adjust the `on_ready` function in `src/main.c` to match the expected signature.

Common signatures:
```c
// Signature 1
void on_ready(struct discord *client);

// Signature 2  
void on_ready(struct discord *client, const struct discord_ready *event);

// Signature 3
void on_ready(struct discord *client, void *event_data);
```

## Step 5: Test the Bot

```bash
# Create .env file in parent directory
cd ../..
echo "DISCORD_BOT_TOKEN=your_bot_token_here" > .env

# Run the bot
cd discord-bot/build/src
./discord_bot
```

The bot should connect and respond to `!ping` commands!

## Still Having Issues?

1. **Run the diagnostic**: `./check_orca.sh`

2. **Check which Orca you have**:
   ```bash
   ls -la /usr/local/lib/libdiscord*
   head -20 /usr/local/include/orca/discord.h
   ```

3. **Consider using Concord instead**: Concord is a fork of Orca with better documentation:
   ```bash
   git clone https://github.com/Cogmasters/concord.git
   cd concord
   make
   sudo make install
   ```
   
   Then change `#include <orca/discord.h>` to `#include <concord/discord.h>` in your files.

4. **Reference the examples**: Most Orca/Concord repos have example bots. Copy the message-sending pattern from there.
