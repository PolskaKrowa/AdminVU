cmake_minimum_required(VERSION 3.16)

# Module sources
set(MODULE_SOURCES
    modules/ping.c
)

# Main executable
add_executable(discord_bot 
    main.c
    env_parser.c
    ${MODULE_SOURCES}
)

# Link libraries
target_link_libraries(discord_bot 
    ${LIBCURL_LIBRARIES}
    ${ORCA_LIBRARY}
    bot_asm
    pthread
    m
)

# Include directories
target_include_directories(discord_bot PRIVATE 
    ${LIBCURL_INCLUDE_DIRS}
    ${ORCA_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/asm
)
