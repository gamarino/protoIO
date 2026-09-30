// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Files and directories. Every function may block. Failures throw
// protoio::Error with kind FileNotFound (ENOENT), FileExists (EEXIST) or
// FileSystem (anything else); the message names the path.
namespace protoio::file {

enum class Mode {
    Read,       // O_RDONLY
    Write,      // O_WRONLY | O_CREAT | O_TRUNC
    Append,     // O_WRONLY | O_CREAT | O_APPEND
    ReadWrite,  // O_RDWR | O_CREAT
};

// Opens a file (close-on-exec, created 0644) and answers its descriptor, for
// use with the stream functions of protoio/stream.h.
int open(const std::string& path, Mode mode);

// The whole file. Reading a directory throws FileSystem (EISDIR).
std::string read(const std::string& path);

// Replaces the file's contents (creating it).
void write(const std::string& path, const std::string& data);

// Appends to the file (creating it).
void append(const std::string& path, const std::string& data);

struct Stat {
    bool isFile = false;
    bool isDirectory = false;
    std::int64_t size = 0;
    std::int64_t modifiedMs = 0;  // modification time, milliseconds since the epoch
    bool readable = false;        // access(R_OK) for this process
    bool writable = false;        // access(W_OK) for this process
};

// std::nullopt when nothing exists at `path` (symbolic links are followed).
std::optional<Stat> stat(const std::string& path);

// Removes a file or an empty directory, or with `recursive` a whole tree.
// Answers whether something was removed (false when nothing was there).
bool remove(const std::string& path, bool recursive = false);

// Renames `from` to `to` (rename(2): same file system).
void move(const std::string& from, const std::string& to);

// Copies a file, or a whole directory tree, overwriting existing files.
void copy(const std::string& from, const std::string& to);

// Creates a directory; with `parents`, also the missing parents, and an
// existing directory is not an error.
void mkdir(const std::string& path, bool parents = false);

// The names of the entries of a directory, sorted, without "." and "..".
std::vector<std::string> list(const std::string& path);

// The absolute form of `path`, resolved lexically against the working
// directory: "." and ".." are folded, symbolic links are kept, and there is
// no trailing slash (except for "/").
std::string absolute(const std::string& path);

// The system's directory for temporary files.
std::string tempDir();

// The working directory, and changing it (process-wide).
std::string cwd();
void chdir(const std::string& path);

} // namespace protoio::file
