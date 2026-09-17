#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sizetree {

enum class EntryKind : std::uint8_t { Directory, File, Link, Other, Unknown };

struct CacheEntry {
    std::string name;
    EntryKind kind;
    std::uint64_t bytes;
    std::uint64_t errors;
    std::uint64_t entries = 0; // Used only by the startup overview.
};

struct CachedDirectory {
    std::string path; // Relative to the canonical scan root; empty for the root.
    std::uint64_t ownErrors = 0;
    std::vector<CacheEntry> children;
};

using CachedDirectories = std::unordered_map<std::string, CachedDirectory>;

struct CacheOverview {
    CachedDirectory listing;
    std::uint64_t bytes = 0, entries = 0, errors = 0;
};

// One append-only journal per scan root. Records replace individual directory
// listings; a background writer periodically compacts superseded records.
class DiskCache {
public:
    DiskCache(std::filesystem::path root, bool enabled);
    ~DiskCache();
    DiskCache(const DiskCache&) = delete;
    DiskCache& operator=(const DiskCache&) = delete;

    CachedDirectories load();
    std::optional<CacheOverview> loadOverview();
    bool writable() const { return writable_.load(std::memory_order_relaxed); }
    void save(CachedDirectory directory);
    void saveOverview(CacheOverview overview);
    void cancel();
    void close();
    void flush();
    std::string warning();

private:
    struct Location { std::uint64_t offset, size, digest; };
    struct Pending { std::string key, frame; };

    std::filesystem::path root_, path_;
    bool enabled_;
    int lock_ = -1;
    std::FILE* file_ = nullptr;
    std::vector<char> buffer_;
    std::string header_;
    std::uint64_t end_ = 0, liveBytes_ = 0;
    std::unordered_map<std::string, Location> latest_;
    std::atomic<bool> writable_{false};
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Pending> pending_;
    std::size_t queuedBytes_ = 0;
    bool stopping_ = false, flushRequested_ = false;
    std::string warning_;
    std::string overview_, pendingOverview_;
    std::thread writer_;

    void writeLoop();
    void compact();
    void resolvePath();
    void writeOverview(const std::string& contents);
    void fail(const std::string& message);
};

} // namespace sizetree
