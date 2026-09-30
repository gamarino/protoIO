// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
#pragma once

#include <stdexcept>
#include <string>

namespace protoio {

// Every failure of the library is an Error. The runtime maps `kind` onto its
// own error classes (protoST's FileDoesNotExist, protoScala's
// FileNotFoundException, protoClojure's ex-info with :type, ...). `sysErrno`
// is the errno the failing system call reported, or 0 when there was none
// (a name-lookup failure, a malformed message, a limit that was exceeded).
struct Error : std::runtime_error {
    enum class Kind {
        FileNotFound,        // ENOENT on a file operation
        FileExists,          // EEXIST on a file operation
        FileSystem,          // any other failure on a file or a pipe
        ConnectionRefused,   // ECONNREFUSED
        ConnectionTimedOut,  // a connect, read, write or TLS handshake timed out
        NameLookup,          // a host name could not be resolved
        Network,             // any other network, TLS or protocol failure
        Process,             // a child process could not be run, waited for or signalled
        LineTooLong,         // a line exceeded the limit the caller gave
        BodyTooLarge,        // an HTTP body exceeded the limit the caller gave
        InvalidArgument,     // a value the library refuses (e.g. a header with a line break)
    };

    Kind kind;
    int sysErrno;

    Error(Kind k, const std::string& message, int err = 0)
        : std::runtime_error(message), kind(k), sysErrno(err) {}
};

// The name of a kind, e.g. "FileNotFound", for diagnostics and mapping tables.
const char* kindName(Error::Kind kind) noexcept;

} // namespace protoio
