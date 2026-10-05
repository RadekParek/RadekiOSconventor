#include "macho.hpp"
#include "trivial.hpp"
#include <cstring>
#include <fstream>
#include <iostream>
int main(int argc, char **argv) {
    try {
        if (argc < 2 || argc > 3)
            throw std::runtime_error("usage: radek-macho executable [analyze|trivial]");
        std::ifstream f(argv[1], std::ios::binary | std::ios::ate);
        if (!f)
            throw std::runtime_error("cannot open executable");
        auto n = f.tellg();
        if (n < 0 || n > 256 * 1024 * 1024)
            throw std::runtime_error("executable exceeds 256 MiB limit");
        std::vector<uint8_t> b(static_cast<size_t>(n));
        f.seekg(0);
        if (!f.read(reinterpret_cast<char *>(b.data()), n))
            throw std::runtime_error("short read");
        const bool trivial = argc == 3 && std::strcmp(argv[2], "trivial") == 0;
        if (argc == 3 && !trivial)
            throw std::runtime_error("unknown mode (expected: trivial)");
        std::cout << (trivial ? radek::recompileTrivial(b) : radek::analyze(b)).dump() << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
