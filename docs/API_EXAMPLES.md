# API Version Examples

Here are complete, working examples for different Orca/Concord API versions. Find which one matches your installation and copy it.

## How to Determine Your Version

```bash
# Check if struct discord_create_message is defined
grep -r "struct discord_create_message" /usr/local/include/orca/ 2>/dev/null

# If found, check its members
grep -A 30 "struct discord_create_message {" /usr/local/include/orca/*.h
```

---

## Version 1: Modern Orca/Concord (2021+)

**Ping Module (`src/modules/ping.c`):**
```c
void on_ping_command(struct discord *client,
                     const struct discord_message *msg) {
    if (!msg->content || strncmp(msg->content, "!ping", 5) != 0) {
        return;
    }

    uint64_t hash = fast_hash(msg->author->username, strlen(msg->author->username));
    
    char response[256];
    snprintf(response, sizeof(response), "Pong! 🏓 (User hash: 0x%016"PRIx64")", hash);
    
    // The key difference: how to send messages
    struct discord_create_message params = { .content = response };
    discord_create_message(client, msg->channel_id, &params, NULL);
}
```

**Main (`src/main.c`) - Ready callback:**
```c
void on_ready(struct discord *client, const struct discord_ready *event) {
    printf("Bot is ready!\n");
    if (event && event->user) {
        printf("Logged in as: %s\n", event->user->username);
    }
}

// In main():
discord_set_on_ready(client, &on_ready);
```

---

## Version 2: Concord with Separate Init

**Ping Module:**
```c
void on_ping_command(struct discord *client,
                     const struct discord_message *msg) {
    if (!msg->content || strncmp(msg->content, "!ping", 5) != 0) {
        return;
    }

    uint64_t hash = fast_hash(msg->author->username, strlen(msg->author->username));
    
    char response[256];
    snprintf(response, sizeof(response), "Pong! 🏓 (User hash: 0x%016"PRIx64")", hash);
    
    // Different init pattern
    struct discord_create_message params;
    discord_create_message_init(&params);
    params.content = response;
    
    discord_create_message(client, msg->channel_id, &params);
}
```

---

## Version 3: Simple Function-Based API

**Ping Module:**
```c
void on_ping_command(struct discord *client,
                     const struct discord_message *msg) {
    if (!msg->content || strncmp(msg->content, "!ping", 5) != 0) {
        return;
    }

    uint64_t hash = fast_hash(msg->author->username, strlen(msg->author->username));
    
    char response[256];
    snprintf(response, sizeof(response), "Pong! 🏓 (User hash: 0x%016"PRIx64")", hash);
    
    // Simplest API - just a function call
    discord_send_message(client, msg->channel_id, response);
}
```

---

## Version 4: REST-Style API

**Ping Module:**
```c
void on_ping_command(struct discord *client,
                     const struct discord_message *msg) {
    if (!msg->content || strncmp(msg->content, "!ping", 5) != 0) {
        return;
    }

    uint64_t hash = fast_hash(msg->author->username, strlen(msg->author->username));
    
    char response[256];
    snprintf(response, sizeof(response), "Pong! 🏓 (User hash: 0x%016"PRIx64")", hash);
    
    // REST-style with explicit params
    struct discord_create_message params;
    memset(&params, 0, sizeof(params));
    params.content = response;
    
    discord_rest_run(client, NULL, NULL, discord_create_message, 
                     msg->channel_id, &params);
}
```

---

## Main.c Variations

### Option A: Simple Ready (No Event Data)
```c
void on_ready(struct discord *client) {
    printf("Bot is ready!\n");
}

// In main():
discord_set_on_ready(client, &on_ready);
```

### Option B: Ready with Event
```c
void on_ready(struct discord *client, const struct discord_ready *event) {
    printf("Bot is ready!\n");
    if (event && event->user) {
        printf("Logged in as: %s\n", event->user->username);
    }
}

// In main():
discord_set_on_ready(client, &on_ready);
```

### Option C: Generic Event Handler
```c
void on_ready(struct discord *client, void *event_data) {
    printf("Bot is ready!\n");
}

// In main():
discord_set_on_ready(client, (discord_ev_ready)&on_ready);
```

---

## Client Initialization Variations

### Option A: Simple Init
```c
struct discord *client = discord_init(token);
```

### Option B: Config-Based Init
```c
struct discord *client = discord_config_init(&(struct discord_config){
    .token = token,
    .default_prefix = "!",
});
```

### Option C: Explicit Config
```c
struct discord_config config;
discord_config_init(&config);
config.token = token;
struct discord *client = discord_create(&config);
```

---

## Finding the Right Version

To determine which version to use:

1. Try Version 1 first (most common)
2. If you get "incomplete type" errors, try the others
3. Check your Orca/Concord examples directory:
   ```bash
   find /usr/local -name "*example*.c" -path "*/discord/*" 2>/dev/null
   cat <example_file>
   ```
4. Or check the Orca/Concord GitHub repo for examples

## Need Help?

Post your error messages and the output of:
```bash
nm -D /usr/local/lib/libdiscord.so | grep discord_create
head -50 /usr/local/include/orca/discord.h
```
