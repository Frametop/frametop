// conf-test.py's C++ side of ft_conf.h: prints what it parses, values in hex.
//   dump read FILE                 NAME HEX per setting (ft_conf::Read)
//   dump double FILE KEY FALLBACK  %.17g (ft_conf::Double)
//   dump setting KEY DEF           HEX (ft_conf::Setting: FT_KEY, then $HOME's conf, then DEF)
#include "../ft_conf.h"

#include <cstdio>
#include <string>

static void Hex(const std::string &s)
{
    for (unsigned char c : s) std::printf("%02x", c);
}

int main(int argc, char **argv)
{
    const std::string cmd = argc > 1 ? argv[1] : "";
    if (argc == 3 && cmd == "read") {
        for (const auto &kv : ft_conf::Read(argv[2])) {
            std::printf("%s\t", kv.first.c_str());
            Hex(kv.second);
            std::printf("\n");
        }
        return 0;
    }
    if (argc == 5 && cmd == "double") {
        std::printf("%.17g\n", ft_conf::Double(ft_conf::Read(argv[2]), argv[3], std::strtod(argv[4], nullptr)));
        return 0;
    }
    if (argc == 4 && cmd == "setting") {
        Hex(ft_conf::Setting(argv[2], argv[3]));
        std::printf("\n");
        return 0;
    }
    return 2;
}
