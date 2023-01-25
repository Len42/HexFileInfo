/*
HexFileInfo - Read, validate, and summarize an Intel HEX format file

File format is defined here: https://en.wikipedia.org/wiki/Intel_HEX

Copyright (c) 2023 Len Popp

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <iostream>
#include <fstream>
#include <filesystem>
#include <list>
#include <string>
#include <span>
#include <ranges>
#include <algorithm>
#include <format>

static std::string progName = "HexFileInfo";
static std::string inFileName = "stdin";

static void throwError(const char* message)
{
    throw std::runtime_error(message);
}

static void throwFileError(const char* message)
{
    throwError(std::format("{} {}", message, inFileName).c_str());
}

static void throwFormatError()
{
    throwError("Invalid data in hex file");
}

static std::string makePrintable(const std::string& str)
{
    const unsigned maxLen = 64;
    std::string strNew;
    if (str.size() <= maxLen) {
        strNew = str;
    } else {
        strNew = str.substr(0, maxLen) + "[etc]";
    }
    for (auto& ch : strNew) {
        if (!std::isprint(static_cast<unsigned char>(ch))) {
            ch = '?';
        }
    }
    return strNew;
}

static unsigned fromHex(const std::span<char>& hex)
{
    unsigned n = 0;
    if (hex.size() > 2 * sizeof(n)) throwError("Number too large");
    for (char digit : hex) {
        if (!std::isxdigit(digit)) throwFormatError();
        digit = std::tolower(digit);
        n = n * 16 + (digit >= 'a' ? digit - 'a' + 10 : digit - '0');
    }
    return n;
}

// Chunk - Represents a chunk of data from several contiguous data records
// (no actual data included)
struct Chunk
{
    unsigned address;
    unsigned size;
};

// checkMergeChunk - Merge the given chunk into the chunk pointed to by iter, if possible
static bool checkMergeChunk(Chunk chunk, auto iter)
{
    if (iter->address + iter->size == chunk.address) {
        iter->size += chunk.size;
        return true;
    } else if (chunk.address + chunk.size == iter->address) {
        iter->address = chunk.address;
        iter->size += chunk.size;
        return true;
    }
    return false;
}

enum recordType_t {
    typeNone = -1,
    typeData = 0,
    typeEof = 1,
    typeEsa = 2,
    typeSsa = 3,
    typeEla = 4,
    typeSla = 5
};

static void processHexFile(std::istream& input)
{
    const unsigned dataOffset = 1 + 2 + 4 + 2; // ':' + count + address + type
    const unsigned minLineSize = dataOffset + 0 + 2; // ... + no data + checksum
    const unsigned maxLineSize = minLineSize + 2 * 255; // max data = 255 bytes
    std::list<Chunk> chunks;
    unsigned numOverlapping = 0;
    unsigned baseAddress = 0;
    bool foundEof = false;
    unsigned numStartAddresses = 0;
    unsigned startAddress = 0;
    unsigned numDataRecords = 0;
    unsigned maxDataSize = 0;
    std::cout << std::format("HEX file: {}\n", inFileName);
    std::string stLine;
    unsigned iLine = 1;
    try {
        while (std::getline(input, stLine)) {
            // If the previous line was an EOF record then EOF wasn't EOF.
            if (foundEof) {
                throwError("EOF record before end of file");
            }
            // Parse the line
            std::span line(stLine);
            if (line.size() < minLineSize) throwFormatError();
            if (line.size() > maxLineSize) throwFormatError();
            if (line.front() != ':') throwFormatError();
            unsigned dataSize = fromHex(line.subspan(1, 2));
            if (line.size() != minLineSize + 2 * dataSize) throwFormatError();
            unsigned address = baseAddress + fromHex(line.subspan(3, 4));
            recordType_t recordType = recordType_t(fromHex(line.subspan(7, 2)));
            std::span dataSpan = line.subspan(dataOffset, 2 * dataSize);
            // Check the checksum
            unsigned char checksum = 0;
            for (unsigned i = 1; i < line.size() - 1; i += 2) {
                checksum += fromHex(line.subspan(i, 2));
            }
            if (checksum != 0) throwError("Incorrect checksum");
            // Handle the various record types.
            switch (recordType) {
            default:
                // Bad record type
                throwFormatError();
            case typeEof:
                // End-of-file record
                if (dataSize != 0) throwFormatError();
                foundEof = true;
                break;
            case typeEsa:
                // Base address segment
                if (dataSize != 2) throwFormatError();
                baseAddress = fromHex(dataSpan) << 4;
                break;
            case typeSsa:
                // Start address CS:IP
                if (dataSize != 4) throwFormatError();
                startAddress = (fromHex(line.subspan(dataOffset, 4)) << 4)
                    + fromHex(line.subspan(dataOffset + 4, 4));
                ++numStartAddresses;
                break;
            case typeEla:
                // Base address linear
                if (dataSize != 2) throwFormatError();
                baseAddress = fromHex(dataSpan) << 16;
                break;
            case typeSla:
                // Start address linear
                if (dataSize != 4) throwFormatError();
                startAddress = fromHex(dataSpan);
                ++numStartAddresses;
                break;
            case typeData:
                // Data record
                // Add this data chunk to the list, in order of address.
                // Add chunks in reverse order because that's usually quicker.
                // Merge adjacent chunks into one.
                Chunk chunk{ address, dataSize };
                bool fAdded = false;
                // Find this data chunk's place in the list.
                for (auto iter = chunks.begin(); iter != chunks.end(); ++iter) {
                    // Check for overlap
                    if (iter->address + iter->size > chunk.address
                        && chunk.address + chunk.size > iter->address)
                    {
                        ++numOverlapping;
                    }
                    // Check whether to merge or add this chunk here.
                    if (checkMergeChunk(chunk, iter)) {
                        fAdded = true;
                    } else if (chunk.address >= iter->address) {
                        iter = chunks.insert(iter, chunk);
                        fAdded = true;
                    }
                    if (fAdded) {
                        // Check if the new chunk needs to merge with the
                        // following one as well.
                        auto next = iter;
                        if (++next != chunks.end()) {
                            if (checkMergeChunk(*iter, next)) {
                                chunks.erase(iter);
                            }
                        }
                        break;
                    }
                }
                if (!fAdded) {
                    chunks.push_front(chunk);
                }
                ++numDataRecords;
                maxDataSize = std::max(maxDataSize, dataSize);
                break;
            }
            ++iLine;
        }
        if (!input.eof()) {
            throwFileError("Error reading file");
        }
    } catch (const std::exception& e) {
        // Re-throw the exception with added context
        std::string str = std::format("{}\nLine {}: {}", e.what(), iLine, makePrintable(stLine));
        throwError(str.c_str());
    }
    if (!foundEof) {
        std::cout << "Missing EOF record\n";
    }
    if (numStartAddresses > 1) {
        std::cout << "Multiple start addresses found\n";
    } else if (numStartAddresses > 0) {
        std::cout << std::format("Start address: 0x{:X}\n", startAddress);
    }
    std::cout << std::format("{} data records, max size {}\n", numDataRecords, maxDataSize);
    std::cout << std::format("{} data segments", chunks.size());
    if (numOverlapping > 0) {
        std::cout << std::format(", {} overlaps found", numOverlapping);
    }
    std::cout << ":\n";
    // Display the chunks in reverse order because they were added in reverse order.
    for (const Chunk& chunk : std::ranges::reverse_view(chunks)) {
        std::cout << std::format("start 0x{:X} size 0x{:X}\n", chunk.address, chunk.size);
    }
}

int main(int argc, char* argv[])
{
    try {
        if (argc > 0) {
            progName = std::filesystem::path(argv[0]).stem().string();
        }
        std::ifstream inFile;
        bool inFromFile = false;
        if (argc == 1) {
            // Input from stdin
            inFromFile = false;
            inFileName = "stdin";
        } else if (argc == 2) {
            // Open input file
            inFromFile = true;
            inFileName = argv[1];
            inFile.open(inFileName, std::ios::in);
            if (inFile.fail()) {
                throwFileError("Failed to open file");
            }
        } else {
            std::cerr << std::format("Usage: {} [input-file]\n", progName);
            return 1;
        }
        processHexFile(inFromFile ? inFile : std::cin);
    } catch (const std::exception& e) {
        std::cerr << std::format("{}: Error: {}\n", progName, e.what());
        return 2;
    } catch (...) {
        std::cerr << std::format("{}: Error\n", progName);
        return 2;
    }
    return 0;
}
