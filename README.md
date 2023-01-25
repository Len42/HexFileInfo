# HexFileInfo

Command-line utility to read, validate, and summarize an Intel HEX format file

Usage:

    HexFileInfo example.hex

Output:

    HEX file: example.hex
    Start address: 0x100001E9
    32 data records, max size 16
    1 data segments:
    start 0x10000000 size 0x200

This program was compiled and tested using Microsoft Visual Studio 2022. Unfortunately, gcc is not able to compile it because it does not fully support C++20 (as of January 2023). Clang will probably work.