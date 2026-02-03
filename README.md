# Discord Bot in C with Orca

A modular Discord bot written in C using the Orca Discord API library, with x86_64 assembly optimisations.

## ⚡ Quick Start

**First time setup?** Read **[QUICK_START.md](QUICK_START.md)** for step-by-step instructions!

**Having compilation errors?** See **[API_EXAMPLES.md](API_EXAMPLES.md)** for code templates matching your Orca version.

---

## Project Structure

```
discord-bot/
├── CMakeLists.txt          # Root build configuration
├── src/
│   ├── main.c              # Main entry point
│   ├── CMakeLists.txt      # Source build configuration
│   └── modules/            # Bot modules
│       ├── ping.c          # Ping command module
│       └── ping.h          # Ping module header
├── asm/
│   ├── fast_hash.asm       # x86_64 assembly functions
│   └── CMakeLists.txt      # Assembly build configuration
└── .env.example            # Example environment variables
```

## Prerequisites

### Ubuntu/Debian
```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake nasm pkg-config \
    libcurl4-openssl-dev git
```

### Install Orca Library

The Orca library (formerly known as orca-c or concord) is required. Here are several installation methods:

**Method 1: Build from source (recommended)**
```bash
git clone https://github.com/cee-studio/orca.git
cd orca
make
sudo make install
sudo ldconfig
```

**Method 2: If you already have Orca installed elsewhere**

You can specify the paths manually when running cmake:
```bash
cmake -DORCA_INCLUDE_DIR=/path/to/orca/include -DORCA_LIBRARY=/path/to/orca/lib/libdiscord.a ..
```

**Verify installation:**
```bash
# Check if the library exists
ls /usr/local/lib/libdiscord* 
# or
find /usr -name "libdiscord*" 2>/dev/null

# Check if the headers exist
ls /usr/local/include/orca/discord.h
# or
find /usr -name "discord.h" 2>/dev/null
```

### Troubleshooting

**If CMake can't find libdiscord:**

First, run the diagnostic script:
```bash
./check_orca.sh
```

This will show you:
- Where Orca is installed
- What functions are available
- The exact paths to use with CMake

Then you can either:

1. Check where Orca installed:
   ```bash
   sudo find / -name "libdiscord*" 2>/dev/null
   sudo find / -name "discord.h" 2>/dev/null
   ```

2. Tell CMake where to find it:
   ```bash
   cmake -DORCA_INCLUDE_DIR=/path/to/include -DORCA_LIBRARY=/path/to/libdiscord.so ..
   ```

3. Or add the paths to your CMakeLists.txt by editing the `find_path` and `find_library` sections.

## Building

**IMPORTANT:** The bot will compile successfully, but you'll need to customize the message-sending code for your specific Orca version. See `QUICK_START.md` for details.

1. Clone this repository:
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

4. **Customize for your Orca version** - See [QUICK_START.md](QUICK_START.md) Step 3

### Compilation Troubleshooting

If you get errors like:
- `incomplete type`
- `has no member named`
- `undefined reference`

Please check `API_EXAMPLES.md` for solutions. The Orca API can vary by version.

## Configuration

1. Create a Discord application at https://discord.com/developers/applications
2. Create a bot and copy the token
3. Create a `.env` file in the **parent directory** (one level above the project):

```bash
# From the parent directory containing discord-bot/
echo "DISCORD_BOT_TOKEN=your_token_here" > .env
```

The bot will automatically load the `.env` file from the parent directory when it starts.

**Directory structure:**
```
parent-directory/
├── .env                    # Put your .env file here
└── discord-bot/
    ├── build/
    └── src/
        └── discord_bot     # The bot runs from here
```

Alternatively, you can still use environment variables:
```bash
export DISCORD_BOT_TOKEN='your_token_here'
```

## Running

```bash
./build/src/discord_bot
```

## Commands

- `!ping` - Responds with "Pong!" and a hash of your username (calculated using x86_64 assembly)

## Adding New Modules

1. Create your module files in `src/modules/`:
   - `your_module.h` - Header file
   - `your_module.c` - Implementation

2. Add to `src/CMakeLists.txt`:
```cmake
set(MODULE_SOURCES
    modules/ping.c
    modules/your_module.c  # Add this
)
```

3. Include and initialise in `src/main.c`:
```c
#include "modules/your_module.h"

// In main():
your_module_init(client);
```

## Adding Assembly Functions

1. Create your `.asm` file in `asm/`
2. Add to `asm/CMakeLists.txt`:
```cmake
set(ASM_SOURCES
    fast_hash.asm
    your_function.asm  # Add this
)
```

3. Declare extern in your C code:
```c
extern your_return_type your_function(your_params);
```

## Assembly Calling Convention (x86_64 System V ABI)

- First 6 integer/pointer arguments: `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9`
- Return value: `rax`
- Caller-saved: `rax`, `rcx`, `rdx`, `rsi`, `rdi`, `r8-r11`
- Callee-saved: `rbx`, `rbp`, `r12-r15`

## Licence

This project is provided as-is for educational purposes.
