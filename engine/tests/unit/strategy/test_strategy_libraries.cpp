/**
 * Strategy libraries: each strategy file built as a library of its own, loaded by the program as it
 * starts and again when asked (StrategyRegistry::load). What is not a strategy library, or was
 * built against other strategy headers, is refused before it loads; a new build of a library loads
 * beside the old one and its classes replace the old ones; a class another library registered is
 * kept. The test loads the build's libraries itself (OPTRA_STRATEGY_LIBRARIES), from copies.
 */
#include "strategy/strategy_registry.hpp"
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <link.h>
#include <string>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using strategy::StrategyRegistry;

const fs::path kBuilt = OPTRA_STRATEGY_LIBRARIES;

/** An empty directory of its own under the system's temporary one. */
auto fresh_directory(const std::string& name) -> fs::path {
    const fs::path dir = fs::temp_directory_path() / std::format("optra-{}-{}", name, getpid());
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

auto why_refused(const fs::path& library) -> std::string {
    for (const StrategyRegistry::Refusal& r : StrategyRegistry::refused()) {
        if (r.library == library.string()) {
            return r.why;
        }
    }
    return {};
}

/** The strategy libraries loaded into the process (each from a copy of its own). */
auto loaded_libraries() -> int {
    int n = 0;
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* count) {
            if (std::string_view(info->dlpi_name).contains("optra-strategy-")) {
                ++*static_cast<int*>(count);
            }
            return 0;
        },
        &n);
    return n;
}

TEST(StrategyLibraries, WhatIsNotAStrategyLibraryOrWasBuiltAgainstOtherHeadersIsRefused) {
    const fs::path dir = fresh_directory("refused");
    std::ofstream(dir / "junk.so") << "not a library";
    std::ifstream in(kBuilt / "synthetic_momentum.so", std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const size_t at = bytes.find("optra-strategy-api:");
    ASSERT_NE(at, std::string::npos);
    bytes.replace(at + std::string_view("optra-strategy-api:").size(), 16, "0000000000000000");
    std::ofstream(dir / "other_headers.so", std::ios::binary) << bytes;

    const int before = loaded_libraries();
    StrategyRegistry::load(dir);
    EXPECT_NE(why_refused(dir / "junk.so").find("no strategy API stamp"), std::string::npos);
    EXPECT_NE(why_refused(dir / "other_headers.so").find("built against other strategy headers"),
              std::string::npos);
    EXPECT_EQ(loaded_libraries(), before); // neither was loaded
    EXPECT_FALSE(StrategyRegistry::has("SyntheticMomentumStrategy"));
    fs::remove_all(dir);
}

TEST(StrategyLibraries, ANewBuildLoadsBesideTheOldAndAnotherLibrarysClassIsKept) {
    const fs::path dir = fresh_directory("strategies");
    const std::string copy = (dir / "synthetic_momentum.so").string();
    fs::copy_file(kBuilt / "synthetic_momentum.so", copy);
    StrategyRegistry::load(dir);
    EXPECT_EQ(StrategyRegistry::library("SyntheticMomentumStrategy"), copy);
    EXPECT_TRUE(StrategyRegistry::source("SyntheticMomentumStrategy").ends_with("synthetic_momentum.cpp"));
    EXPECT_EQ(why_refused(copy), "");

    // Unchanged: not loaded again.
    const int once = loaded_libraries();
    StrategyRegistry::reload();
    EXPECT_EQ(loaded_libraries(), once);

    // A new build of it (the file changed): loaded beside the old one, its class replaced.
    fs::last_write_time(copy, fs::last_write_time(copy) + std::chrono::seconds(1));
    StrategyRegistry::reload();
    EXPECT_EQ(loaded_libraries(), once + 1);
    EXPECT_EQ(why_refused(copy), "");
    EXPECT_EQ(StrategyRegistry::library("SyntheticMomentumStrategy"), copy);

    // The build's libraries: the same class from another library is kept as it was; the others
    // load.
    StrategyRegistry::load(kBuilt);
    EXPECT_NE(why_refused(kBuilt / "synthetic_momentum.so").find("SyntheticMomentumStrategy already"),
              std::string::npos);
    EXPECT_EQ(StrategyRegistry::library("SyntheticMomentumStrategy"), copy);
    EXPECT_EQ(StrategyRegistry::library("RandomBuyStrategy"),
              (kBuilt / "random_buy.so").string());
    fs::remove_all(dir);
}

} // namespace
