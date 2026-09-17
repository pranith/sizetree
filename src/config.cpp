#include "config.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace sizetree {
namespace {

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

std::runtime_error systemError(const char* operation) {
    return std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}

std::string saveConfig(const std::filesystem::path& path, const std::string& contents);

std::chrono::seconds parseRefreshInterval(const std::string& value) {
    try {
        if (!value.empty() && value.find_first_not_of("0123456789") == std::string::npos) {
            const auto seconds = std::stoull(value);
            if (seconds <= std::numeric_limits<std::uint32_t>::max())
                return std::chrono::seconds(seconds);
        }
    } catch (const std::exception&) {}
    throw std::runtime_error("refresh_interval_seconds must be a whole number from 0 to 4294967295 (0 disables automatic refresh)");
}

} // namespace

Config loadConfig() {
    Config config;
    try {
        std::filesystem::path base;
        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        const char* home = std::getenv("HOME");
        if (xdg && std::filesystem::path(xdg).is_absolute()) base = xdg;
        else if (home && *home) base = std::filesystem::path(home) / ".config";
        else throw std::runtime_error("HOME and XDG_CONFIG_HOME are unavailable");
        config.path = base / "sizetree" / "config";
        bool hasCache = false, hasInterval = false;
        std::string contents;
        if (std::filesystem::exists(config.path)) {
            if (!std::filesystem::is_regular_file(config.path))
                throw std::runtime_error("config is not a regular file");
            if (std::filesystem::file_size(config.path) > 64 * 1024)
                throw std::runtime_error("config exceeds 64 KiB");
            std::ifstream input(config.path);
            if (!input) throw std::runtime_error("cannot open config for reading");
            std::string line;
            while (std::getline(input, line)) {
                contents += line;
                if (!input.eof()) contents += '\n';
                line = trim(line.substr(0, line.find_first_of("#;")));
                if (line.empty()) continue;
                try {
                    const auto separator = line.find('=');
                    if (separator == std::string::npos)
                        throw std::runtime_error("expected key=value in config");
                    const auto key = trim(line.substr(0, separator));
                    const auto value = trim(line.substr(separator + 1));
                    if (key == "cache_enabled") {
                        if (hasCache) throw std::runtime_error("duplicate cache_enabled setting");
                        hasCache = true;
                        if (value != "true" && value != "false")
                            throw std::runtime_error("cache_enabled must be true or false");
                        config.cacheEnabled = value == "true";
                    } else if (key == "refresh_interval_seconds") {
                        if (hasInterval) throw std::runtime_error("duplicate refresh_interval_seconds setting");
                        hasInterval = true;
                        config.refreshInterval = parseRefreshInterval(value);
                    }
                } catch (const std::exception& error) {
                    if (!config.warning.empty()) config.warning += "; ";
                    config.warning += error.what();
                }
            }
            if (!input.eof()) throw std::runtime_error("cannot read config");
        }
        // Keep malformed files intact; valid settings still apply, and invalid
        // or missing settings use their defaults for this run.
        if (config.warning.empty() && (!hasCache || !hasInterval)) {
            if (contents.empty()) contents = "# sizetree configuration\n";
            if (contents.back() != '\n') contents += '\n';
            if (!hasCache) contents += "cache_enabled=true\n";
            if (!hasInterval) contents +=
                "# Automatic refresh interval in seconds (86400 = 1 day; 0 disables).\n"
                "refresh_interval_seconds=86400\n";
            const auto warning = saveConfig(config.path, contents);
            if (!warning.empty()) config.warning = "could not save config: " + warning;
        }
    } catch (const std::exception& error) {
        config.warning = error.what();
    }
    return config;
}

namespace {

std::string saveConfig(const std::filesystem::path& path, const std::string& contents) {
    int descriptor = -1;
    std::filesystem::path temporary;
    try {
        if (path.empty()) throw std::runtime_error("no config directory is available");
        std::filesystem::create_directories(path.parent_path());
        if (::chmod(path.parent_path().c_str(), 0700) != 0)
            throw systemError("cannot protect config directory");
        const auto pattern = path.string() + ".tmp.XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        descriptor = ::mkstemp(name.data());
        if (descriptor < 0) throw systemError("cannot create config");
        temporary = name.data();
        std::size_t written = 0;
        while (written < contents.size()) {
            const auto count = ::write(descriptor, contents.data() + written, contents.size() - written);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw systemError("cannot write config");
            written += static_cast<std::size_t>(count);
        }
        while (::fsync(descriptor) != 0) {
            if (errno != EINTR) throw systemError("cannot flush config");
        }
        const int closed = ::close(descriptor);
        descriptor = -1;
        if (closed != 0) throw systemError("cannot close config");
        std::filesystem::rename(temporary, path);
        return {};
    } catch (const std::exception& error) {
        if (descriptor >= 0) ::close(descriptor);
        if (!temporary.empty()) ::unlink(temporary.c_str());
        return error.what();
    }
}

} // namespace

} // namespace sizetree
