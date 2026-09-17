#pragma once

#include <chrono>
#include <filesystem>
#include <string>

namespace sizetree {

inline constexpr std::chrono::seconds defaultRefreshInterval{86400};

struct Config {
    std::filesystem::path path;
    bool cacheEnabled = true;
    std::chrono::seconds refreshInterval = defaultRefreshInterval;
    std::string warning;
};

// Load settings and write any missing defaults without replacing existing keys.
Config loadConfig();

} // namespace sizetree
