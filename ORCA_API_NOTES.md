# Orca API Compatibility Notes

The Orca Discord library API can vary depending on the version. This bot is written for the modern Orca library from https://github.com/cee-studio/orca

## Common API Variations

If you're getting compilation errors, you may need to adjust the code based on your Orca version:

### Client Initialization

**Version 1 (discord_init):**
```c
struct discord *client = discord_init(token);
```

**Version 2 (discord_config_init):**
```c
struct discord_config config = { .token = (char*)token };
struct discord *client = discord_config_init(&config);
```

### Ready Event

**Access bot info from event:**
```c
void on_ready(struct discord *client, const struct discord_ready *event) {
    const struct discord_user *bot = event->user;
    printf("Bot: %s\n", bot->username);
}
```

**Access bot info from client:**
```c
void on_ready(struct discord *client, const struct discord_ready *event) {
    const struct discord_user *bot = discord_get_self(client);
    printf("Bot: %s\n", bot->username);
}
```

### Message Sending

**Method 1 - Struct initialization:**
```c
struct discord_create_message params = { .content = "Hello" };
discord_create_message(client, channel_id, &params, NULL);
```

**Method 2 - memset:**
```c
struct discord_create_message params;
memset(&params, 0, sizeof(params));
params.content = "Hello";
discord_create_message(client, channel_id, &params, NULL);
```

**Method 3 - Simple function (older API):**
```c
discord_send_message(client, channel_id, "Hello");
```

## Checking Your Orca Version

To see what functions are available in your installation:

```bash
# Check the header files
grep "discord_init\|discord_config" /usr/local/include/orca/discord.h

# Check for specific structures
grep "struct discord_create_message" /usr/local/include/orca/*.h

# List all discord functions
nm -D /usr/local/lib/libdiscord.so | grep discord_
```

## If You Get Compilation Errors

1. **Incomplete type errors:**
   - The struct definition might not be in `discord.h`
   - Try including additional headers: `#include <orca/types.h>` or `#include <orca/discord-codecs.h>`

2. **Undefined reference errors:**
   - Function names might be different
   - Check `nm -D /usr/local/lib/libdiscord.so | grep <function_name>`

3. **Wrong callback signature:**
   - Callback function signatures can vary
   - Check the header file for the exact signature required

## Alternative: Use Concord Instead

If Orca isn't working, you can use Concord (a similar library):

```bash
git clone https://github.com/Cogmasters/concord.git
cd concord
make
sudo make install
```

Then adjust the includes:
```c
#include <concord/discord.h>  // instead of <orca/discord.h>
```

The API is very similar but with minor differences in function names.
