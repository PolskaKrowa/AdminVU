# AdminVU

A Discord bot written primarily in C, with experiments in Assembly and other low-level languages, focused on performance and maintainability.

## Motivation

This project exists for the sole reason that my previous attempts at making a high-performance Discord bot have failed in multiple ways.

AdminVU V1 was based off of the interactions.py library, and was extremely slow at its purpose.

AdminVU V2 was based rewritten in C++ using a custom API library called StvAPI. This allowed me to easily create a multithreaded discord bot to allow for processing multiple commands concurrently. This was slightly faster than the python implementation, but made it a nightmare to manage and maintain.

AdminVU V3 (this one) Will be fully modularised, without any interfacing bollocks between modules. Everything will work together as one bot, with heavy optimisations being done on the source while keeping to strict reliability standards.