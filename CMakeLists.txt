cmake_minimum_required(VERSION 3.16)
project(discord_bot C ASM_NASM)

# Set C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_C_FLAGS_RELEASE "-O3 -march=native -mtune=native -flto -fopenmp")

# Enable assembly
enable_language(ASM_NASM)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)

# pthreads – required for off-thread slash-command registration
find_package(Threads REQUIRED)

# Find Orca/Discord library manually
# Check common installation paths
find_path(ORCA_INCLUDE_DIR 
    NAMES orca/discord.h
    PATHS 
        /usr/local/include
        /usr/include
        ${CMAKE_SOURCE_DIR}/../orca/include
)

find_library(ORCA_LIBRARY
    NAMES discord libdiscord
    PATHS
        /usr/local/lib
        /usr/lib
        /usr/lib/x86_64-linux-gnu
        ${CMAKE_SOURCE_DIR}/../orca/lib
)

if(NOT ORCA_INCLUDE_DIR OR NOT ORCA_LIBRARY)
    message(FATAL_ERROR "Orca library not found. Please install it or set ORCA_INCLUDE_DIR and ORCA_LIBRARY manually.")
endif()

message(STATUS "Found Orca include: ${ORCA_INCLUDE_DIR}")
message(STATUS "Found Orca library: ${ORCA_LIBRARY}")

# Add subdirectories
add_subdirectory(asm)
add_subdirectory(src)