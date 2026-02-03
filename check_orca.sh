#!/bin/bash
# Orca Discord Library Diagnostic Script

echo "=== Orca Discord Library Diagnostic ==="
echo ""

echo "1. Checking for Orca library files..."
echo "   Looking for libdiscord..."

DISCORD_LIB=$(find /usr -name "libdiscord*" 2>/dev/null | head -1)
if [ -n "$DISCORD_LIB" ]; then
    echo "   ✓ Found: $DISCORD_LIB"
else
    echo "   ✗ libdiscord not found"
    echo "   → Install Orca: https://github.com/cee-studio/orca"
    exit 1
fi

echo ""
echo "2. Checking for Orca header files..."
DISCORD_H=$(find /usr -name "discord.h" 2>/dev/null | grep orca | head -1)
if [ -n "$DISCORD_H" ]; then
    echo "   ✓ Found: $DISCORD_H"
else
    echo "   ✗ discord.h not found in orca directory"
    exit 1
fi

echo ""
echo "3. Checking available Discord functions..."
if [ -f "$DISCORD_LIB" ]; then
    echo "   Key functions available:"
    nm -D "$DISCORD_LIB" 2>/dev/null | grep -E "discord_init|discord_config|discord_run|discord_create_message" | head -10
fi

echo ""
echo "4. Checking for struct definitions..."
if [ -f "$DISCORD_H" ]; then
    echo "   Checking for discord_create_message struct..."
    if grep -q "struct discord_create_message" "$DISCORD_H" 2>/dev/null; then
        echo "   ✓ struct discord_create_message found"
    else
        # Check in related headers
        STRUCT_FILE=$(find $(dirname "$DISCORD_H") -name "*.h" -exec grep -l "struct discord_create_message" {} \; 2>/dev/null | head -1)
        if [ -n "$STRUCT_FILE" ]; then
            echo "   ✓ struct discord_create_message found in: $STRUCT_FILE"
            echo "   → You may need to include this header"
        else
            echo "   ⚠ struct discord_create_message not found"
            echo "   → Your Orca version might use a different API"
        fi
    fi
fi

echo ""
echo "5. CMake hints:"
echo "   ORCA_INCLUDE_DIR=$(dirname "$DISCORD_H")"
echo "   ORCA_LIBRARY=$DISCORD_LIB"

echo ""
echo "To build with these paths:"
echo "cmake -DORCA_INCLUDE_DIR=$(dirname "$DISCORD_H") -DORCA_LIBRARY=$DISCORD_LIB .."
echo ""
echo "=== Diagnostic Complete ==="
