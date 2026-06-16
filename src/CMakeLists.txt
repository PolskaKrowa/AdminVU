cmake_minimum_required(VERSION 3.16)

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)

find_package(LibLZMA REQUIRED)

add_executable(discord_bot
    main.c
    env_parser.c
    database.c
    database_propagation.c
    http_server.c
    api.c
    modules/ping.c
    modules/moderation.c
    modules/ticket.c
    modules/factcheck.c
    modules/propagation.c
    modules/fun.c
    modules/components_v2.c
    modules/messaging.c
#    modules/personaplex_bridge.c
)

target_link_libraries(discord_bot
    PkgConfig::LIBCURL
    ${ORCA_LIBRARY}
    bot_asm
    pthread
    m
    sqlite3
    ${LIB_WEBSOCKETS_LIBRARIES}
    sodium
    LibLZMA::LibLZMA
)

target_include_directories(discord_bot PRIVATE
    ${ORCA_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/asm
    ${CMAKE_CURRENT_SOURCE_DIR}/modules/chess/include
    ${LIB_WEBSOCKETS_INCLUDE_DIRS}
)

add_compile_definitions(
    FACTCHECK_OLLAMA_MODEL="ministral-3:8b"
    FACTCHECK_OLLAMA_URL="http://localhost:11434/api/generate"
)