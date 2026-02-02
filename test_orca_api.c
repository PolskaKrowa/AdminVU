#include <stdio.h>
#include <orca/discord.h>

// This test program checks which Orca API functions are available
// Compile with: gcc test_orca_api.c -o test_orca -ldiscord -lcurl

int main() {
    printf("Testing Orca API availability...\n");
    
    // Test 1: Check if discord_init exists
    #ifdef discord_init
    printf("✓ discord_init is available\n");
    #else
    printf("✗ discord_init not found\n");
    #endif
    
    printf("\nTo use this bot, you need to:\n");
    printf("1. Check what functions exist in your libdiscord.so:\n");
    printf("   nm -D /usr/local/lib/libdiscord.so | grep discord_\n\n");
    printf("2. Look at the header files:\n");
    printf("   grep -r \"discord_create_message\\|discord_send\" /usr/local/include/orca/\n\n");
    printf("3. Modify the ping module to use the available functions\n");
    
    return 0;
}
