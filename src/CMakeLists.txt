cmake_minimum_required(VERSION 3.16)

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)

add_executable(discord_bot
    main.c
    env_parser.c
    database.c
    database_propagation.c
    http_server.c
    modules/ping.c
    modules/moderation.c
    modules/ticket.c
    modules/factcheck.c
    modules/propagation.c
)

target_link_libraries(discord_bot
    PkgConfig::LIBCURL
    ${ORCA_LIBRARY}
    bot_asm
    pthread
    m
    sqlite3
)

target_include_directories(discord_bot PRIVATE
    ${ORCA_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/asm
)