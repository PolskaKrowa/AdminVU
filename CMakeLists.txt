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

# pthreads – required for off-thread slash-command registration and HTTP server
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

# ── .env file ────────────────────────────────────────────────────────────────
# Copy the project-root .env into the build directory so the bot can find it
# at runtime with the relative path "../.env" (or directly if invoked from
# the build dir).  The copy is only done when the source file actually exists;
# this avoids a hard error for CI environments where .env is absent.
if(EXISTS "${CMAKE_SOURCE_DIR}/.env")
    configure_file(
        "${CMAKE_SOURCE_DIR}/.env"
        "${CMAKE_BINARY_DIR}/.env"
        COPYONLY
    )
    message(STATUS "Copied .env to build directory")
else()
    message(STATUS "No .env file found at project root – skipping copy")
endif()

# ── Web assets ───────────────────────────────────────────────────────────────
# Gather all files inside web/ and mirror them into <build>/web/ at build time.
# Using a custom target ensures the copy re-runs whenever any web file changes.
file(GLOB_RECURSE WEB_SOURCE_FILES
    LIST_DIRECTORIES false
    "${CMAKE_SOURCE_DIR}/web/*"
)

if(WEB_SOURCE_FILES)
    # Create the destination directory upfront so CMake doesn't complain on
    # the very first configure when the build tree is empty.
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/web")

    set(WEB_COPIED_FILES "")
    foreach(SRC_FILE ${WEB_SOURCE_FILES})
        # Preserve the path relative to web/ (handles sub-directories too).
        file(RELATIVE_PATH REL_PATH "${CMAKE_SOURCE_DIR}/web" "${SRC_FILE}")
        set(DEST_FILE "${CMAKE_BINARY_DIR}/web/${REL_PATH}")

        # get_filename_component ensures parent dirs exist before copying.
        get_filename_component(DEST_DIR "${DEST_FILE}" DIRECTORY)

        add_custom_command(
            OUTPUT  "${DEST_FILE}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${DEST_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRC_FILE}" "${DEST_FILE}"
            DEPENDS "${SRC_FILE}"
            COMMENT "Copying web asset: ${REL_PATH}"
            VERBATIM
        )
        list(APPEND WEB_COPIED_FILES "${DEST_FILE}")
    endforeach()

    add_custom_target(copy_web_assets ALL
        DEPENDS ${WEB_COPIED_FILES}
        COMMENT "Syncing web assets to build directory"
    )

    message(STATUS "Found ${CMAKE_SOURCE_DIR}/web – will copy assets to build dir")
else()
    message(STATUS "No files found in web/ – skipping web asset copy")
endif()

# ── Subdirectories ────────────────────────────────────────────────────────────
add_subdirectory(asm)
add_subdirectory(src)