cmake_minimum_required(VERSION 3.16)

# Assembly source files
set(ASM_SOURCES
    fast_hash.asm
)

# Create static library from assembly
add_library(bot_asm STATIC ${ASM_SOURCES})

# Set NASM flags for x86_64
set_target_properties(bot_asm PROPERTIES
    NASM_OBJ_FORMAT elf64
    LINKER_LANGUAGE C
)
