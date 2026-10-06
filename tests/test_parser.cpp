#include "metainfo/parser.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

static int failures = 0;

static void expect_ok(const std::string& in, const char* label = nullptr) {
    try {
        BencodeParser p(in);
        p.parse();
    } catch (const std::exception& e) {
        std::printf("FAIL  expected ok:   %-28s threw: %s\n", label ? label : in.c_str(), e.what());
        ++failures;
    }
}

static void expect_err(const std::string& in, const char* label = nullptr) {
    const char* shown = label ? label : in.c_str();
    try {
        BencodeParser p(in);
        p.parse();
        std::printf("FAIL  expected error: %-28s parsed fine\n", shown);
        ++failures;
    } catch (const std::runtime_error& e) {
        std::printf("ok    %-28s -> %s\n", shown, e.what());
    } catch (const std::exception& e) {
        std::printf("FAIL  wrong type:    %-28s threw non-runtime_error: %s\n", shown, e.what());
        ++failures;
    }
}

int main() {
    // should parse
    for (const char* s : {"i0e", "i-1e", "i42e", "i9223372036854775807e", "i-9223372036854775808e",
                          "0:", "4:spam", "le", "de", "l4:spami42ee", "d3:bar4:spam3:fooi42ee",
                          "d0:i1e1:ai2ee"})
        expect_ok(s);

    // should fail with runtime_error
    for (const char* s : {"i-03e", "i-00e", "i-0e", "i00e", "i03e", "i-e", "ie", "i--1e", "i-", "i",
                          "i12xe", "i+1e", "i9223372036854775808e", "i-9223372036854775809e",
                          "03:abc", ":abc", "3", "3x", "4:abc", "18446744073709551615:x",
                          "18446744073709551616:x", "99999999999999999999999:x",
                          "d1:ai1e1:ai2ee", "d1:bi1e1:ai2ee", "d:e", "di1ei2ee", "d1:ae",
                          "l", "d", "d1:a", "i1ei2e", "x", "e"})
        expect_err(s);
    expect_err("", "<empty>");
    expect_err(std::string(1'000'000, 'l'), "'l' x 1,000,000");
    {
        std::string deep;
        for (int i = 0; i < 200'000; ++i) deep += "d1:a";
        expect_err(deep, "'d1:a' x 200,000");
    }
    expect_ok(std::string(100, 'l') + std::string(100, 'e'), "depth exactly 100");
    expect_err(std::string(101, 'l') + std::string(101, 'e'), "depth 101");

    // info span: outer value only
    {
        const std::string in = "d4:infod4:infoi1eee";
        BencodeParser p(in);
        p.parse();
        auto [b, e] = p.get_info_range();
        const bool good = b == 7 && e == 18;
        std::printf("%s  info span {%zu, %zu} (want {7, 18}) -> \"%s\"\n",
                    good ? "ok  " : "FAIL", b, e, in.substr(b, e - b).c_str());
        if (!good) ++failures;
    }

    std::printf("\n%d failure(s)\n", failures);
    return failures != 0;
}