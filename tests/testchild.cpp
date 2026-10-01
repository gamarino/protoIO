// A child program for the process tests on Windows, which lacks the POSIX
// commands (sh, cat, true, sleep) the tests run elsewhere.
//
//   testchild cat               copies its input to its output, as bytes
//   testchild true              exits 0 without reading its input
//   testchild print OUT ERR N   writes OUT and ERR to its outputs, exits N
//   testchild env NAME          writes the value of NAME (nothing if unset)
//   testchild sleep MS          sleeps MS milliseconds
//   testchild args A...         writes each argument as "[A]\n"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    if (argc < 2) return 100;
    const std::string mode = argv[1];
    if (mode == "cat") {
        char buf[65536];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, stdin)) > 0) std::fwrite(buf, 1, n, stdout);
        return 0;
    }
    if (mode == "true") return 0;
    if (mode == "print" && argc == 5) {
        std::fputs(argv[2], stdout);
        std::fputs(argv[3], stderr);
        return std::atoi(argv[4]);
    }
    if (mode == "env" && argc == 3) {
        if (const char* v = std::getenv(argv[2])) std::fputs(v, stdout);
        return 0;
    }
    if (mode == "sleep" && argc == 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(argv[2])));
        return 0;
    }
    if (mode == "args") {
        for (int i = 2; i < argc; ++i) std::printf("[%s]\n", argv[i]);
        return 0;
    }
    return 100;
}
