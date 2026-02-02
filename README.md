# AdminVU

A Discord bot written primarily in C, with experiments in Assembly and other low-level languages, focused on performance and maintainability.

## Motivation

This project exists for the sole reason that my previous attempts at making a high-performance Discord bot have failed in multiple ways.

AdminVU V1 was based off of the interactions.py library, and was extremely slow at its purpose.

AdminVU V2 was based rewritten in C++ using a custom API library called StvAPI. This allowed me to easily create a multithreaded discord bot to allow for processing multiple commands concurrently. This was slightly faster than the python implementation, but made it a nightmare to manage and maintain.

AdminVU V3 (this one) Will be fully modularised, without any interfacing bollocks between modules. Everything will work together as one bot, with heavy optimisations being done on the source while keeping to strict reliability standards.

## Features
### Implemented
- None (LOL)

### Currently Planned
- Basic discord bot based off Orca, with a few basic commands.

## Architecture

This repository is structured in a (hopefully) easy to understand way. C stuff goes in `src/`, Assembly stuff goes in `asm/`, documentation goes in `docs/`, etc.

## Status

The bot isn't even implemented yet...

## Contributing

You may contribute to the development of the bot.

## Licence

Apache V2.0