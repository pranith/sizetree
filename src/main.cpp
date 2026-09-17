#include "cache.hpp"
#include "config.hpp"
#include "fuzzy.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <condition_variable>
#include <cstddef>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef __linux__
#include <linux/magic.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#if __has_include(<linux/stat.h>)
#include <linux/stat.h>
#endif
#endif

namespace fs = std::filesystem;

extern char** environ;

namespace {

volatile std::sig_atomic_t interrupted = 0;
volatile std::sig_atomic_t suspendRequested = 0;
volatile std::sig_atomic_t resumeRequested = 0;

extern "C" void handleSignal(int signal) { interrupted = signal; }
extern "C" void handleJobSignal(int signal) {
    if (signal == SIGTSTP) suspendRequested = 1;
    else if (signal == SIGCONT) resumeRequested = 1;
}

// Escape terminal controls and invalid filename bytes; preserve printable Unicode.
std::string displayText(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t pos = 0; pos < input.size();) {
        const auto byte = static_cast<unsigned char>(input[pos]);
        if (byte >= 32 && byte < 127) {
            result += input[pos++];
            continue;
        }
        if (byte == '\n' || byte == '\r' || byte == '\t') {
            result += byte == '\n' ? "\\n" : byte == '\r' ? "\\r" : "\\t";
            ++pos;
            continue;
        }
        std::mbstate_t state{};
        wchar_t character{};
        const auto length = std::mbrtowc(&character, input.data() + pos, input.size() - pos, &state);
        if (length == static_cast<std::size_t>(-1) || length == static_cast<std::size_t>(-2) ||
            length == 0 || ::wcwidth(character) < 0) {
            result += "\\x";
            result += hex[byte >> 4];
            result += hex[byte & 15];
            ++pos;
        } else {
            result.append(input, pos, length);
            pos += length;
        }
    }
    return result;
}

// Clip by terminal columns, without cutting a UTF-8 character in half.
std::string clip(const std::string& text, int columns) {
    if (columns <= 0) return {};
    int used = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::mbstate_t state{};
        wchar_t character{};
        auto length = std::mbrtowc(&character, text.data() + pos, text.size() - pos, &state);
        if (length == 0 || length == static_cast<std::size_t>(-1) ||
            length == static_cast<std::size_t>(-2)) {
            length = 1;
            character = L'?';
        }
        const int width = std::max(0, ::wcwidth(character));
        if (used + width > columns) break;
        used += width;
        pos += length;
    }
    return text.substr(0, pos);
}

std::string humanSize(std::uintmax_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    double amount = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (amount >= 1024 && unit < 6) {
        amount /= 1024;
        ++unit;
    }
    std::ostringstream output;
    output << std::fixed << std::setprecision(1) << amount << ' ' << units[unit];
    return output.str();
}

std::string elapsedText(std::chrono::steady_clock::duration elapsed) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    return std::to_string(seconds / 60) + " min " + std::to_string(seconds % 60) + " sec";
}

using Kind = sizetree::EntryKind;
using NodeId = std::uint64_t;

bool fileMetadata(int directory, const char* name, mode_t& mode, std::uintmax_t& bytes) {
#if defined(__linux__) && defined(SYS_statx) && defined(STATX_TYPE) && defined(STATX_SIZE)
    static std::atomic<bool> statxAvailable{true};
    if (statxAvailable.load(std::memory_order_relaxed)) {
        struct statx status{};
        // Keep normal stat freshness; requesting only size/type avoids NFS
        // timestamp writeback and revalidation for attributes we never display.
        const auto result = ::syscall(SYS_statx, directory, name, AT_SYMLINK_NOFOLLOW,
                                      STATX_TYPE | STATX_SIZE, &status);
        if (result == 0 && (status.stx_mask & STATX_TYPE) &&
            (!S_ISREG(status.stx_mode) || (status.stx_mask & STATX_SIZE))) {
            mode = status.stx_mode;
            bytes = status.stx_size;
            return true;
        }
        if (result < 0) {
            if (errno == ENOSYS || errno == EPERM)
                statxAvailable.store(false, std::memory_order_relaxed);
            else if (errno != EINVAL && errno != EOPNOTSUPP) return false;
        }
    }
#endif
    struct stat status{};
    if (::fstatat(directory, name, &status, AT_SYMLINK_NOFOLLOW) != 0) return false;
    mode = status.st_mode;
    bytes = status.st_size > 0 ? static_cast<std::uintmax_t>(status.st_size) : 0;
    return true;
}

class DirectoryHandle {
public:
    DirectoryHandle(std::atomic<unsigned>& count, int parent, const char* name) : count_(count) {
        count_.fetch_add(1, std::memory_order_relaxed);
        descriptor_ = ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
#if !defined(__linux__) || !defined(SYS_getdents64)
        if (descriptor_ >= 0) {
            stream_ = ::fdopendir(descriptor_);
            if (!stream_) {
                const int error = errno;
                ::close(descriptor_);
                descriptor_ = -1;
                errno = error;
            }
        }
#endif
    }

    ~DirectoryHandle() {
#if !defined(__linux__) || !defined(SYS_getdents64)
        if (stream_) ::closedir(stream_);
#else
        if (descriptor_ >= 0) ::close(descriptor_);
#endif
        count_.fetch_sub(1, std::memory_order_relaxed);
    }
    DirectoryHandle(const DirectoryHandle&) = delete;
    DirectoryHandle& operator=(const DirectoryHandle&) = delete;
    int descriptor() const { return descriptor_; }

#if !defined(__linux__) || !defined(SYS_getdents64)
    DIR* stream() const { return stream_; }
#endif

private:
    std::atomic<unsigned>& count_;
    int descriptor_ = -1;
#if !defined(__linux__) || !defined(SYS_getdents64)
    DIR* stream_ = nullptr;
#endif
};

class DirectoryReader {
public:
    explicit DirectoryReader(const DirectoryHandle& directory) : directory_(directory) {}

    bool next(const char*& name, unsigned char& type) {
#if defined(__linux__) && defined(SYS_getdents64)
        // O_DIRECTORY has already checked the type. Reading entries directly
        // avoids fdopendir's extra fstat, including its NFS attribute request.
        if (position_ == available_) {
            const auto count = ::syscall(SYS_getdents64, directory_.descriptor(), buffer_.data(), buffer_.size());
            if (count <= 0) return false;
            available_ = static_cast<std::size_t>(count);
            position_ = 0;
        }
        // linux_dirent64 uses fixed-width fields; memcpy avoids alignment and
        // aliasing assumptions, and d_reclen determines each variable-size name.
        struct EntryHeader {
            std::uint64_t inode;
            std::int64_t offset;
            std::uint16_t length;
            unsigned char type;
            char name[1];
        };
        constexpr auto nameOffset = offsetof(EntryHeader, name);
        if (available_ - position_ <= nameOffset) { errno = EIO; return false; }
        const auto* entry = buffer_.data() + position_;
        std::uint16_t length;
        std::memcpy(&length, entry + offsetof(EntryHeader, length), sizeof(length));
        if (length <= nameOffset || length > available_ - position_ ||
            !std::memchr(entry + nameOffset, '\0', length - nameOffset)) {
            errno = EIO;
            return false;
        }
        name = entry + nameOffset;
        type = static_cast<unsigned char>(entry[offsetof(EntryHeader, type)]);
        position_ += length;
        return true;
#else
        const auto* entry = ::readdir(directory_.stream());
        if (!entry) return false;
        name = entry->d_name;
        type = entry->d_type;
        return true;
#endif
    }

private:
    const DirectoryHandle& directory_;
#if defined(__linux__) && defined(SYS_getdents64)
    std::array<char, 64 * 1024> buffer_;
    std::size_t position_ = 0, available_ = 0;
#endif
};

struct Node {
    // Only directories need a full path. Most entries are files.
    fs::path path;
    std::string name;
    NodeId id = 0;
    Node* parent = nullptr;
    Kind kind = Kind::Unknown;
    std::uintmax_t bytes = 0;
    std::uintmax_t entries = 0;
    std::uintmax_t errors = 0;
    std::uintmax_t ownErrors = 0;
    bool complete = false;
    bool cached = false;
    bool refreshQueued = false;
    std::size_t pending = 1; // Own enumeration plus unfinished child directories.
    std::vector<std::unique_ptr<Node>> children;
};

struct Row {
    NodeId node;
    NodeId parent;
    std::string name;
    Kind kind;
    std::uintmax_t bytes;
    bool complete;
    bool partial;
    bool expanded;
    bool refreshQueued;
    bool cached;
    int depth;
};

struct Snapshot {
    std::vector<Row> rows;
    std::uintmax_t bytes = 0;
    std::uintmax_t entries = 0;
    std::uintmax_t errors = 0;
    bool done = false;
    bool cached = false;
    bool loading = false;
    std::chrono::steady_clock::duration elapsed{};
    std::string failure;
    std::string cacheWarning;
};

struct ScanProgress {
    std::uintmax_t bytes, entries, errors;
    bool done, cached, loading;
};

struct SearchEntry {
    NodeId node, parent;
    std::string path;
    int score = 0;
};

struct SearchCursor {
    struct Frame {
        NodeId directory;
        std::size_t offset;
        std::string prefix;
    };
    std::vector<Frame> stack;
    std::uint64_t epoch = 0, revision = 0;
    bool started = false, done = false;
};

bool nfsDirectoryCache(const fs::path& path) {
#ifdef __linux__
    struct statfs status{};
    return ::statfs(path.c_str(), &status) == 0 && status.f_type == NFS_SUPER_MAGIC;
#else
    (void)path;
    return false;
#endif
}

class DirectoryTree {
public:
    using Clock = std::chrono::steady_clock;

    explicit DirectoryTree(fs::path path, unsigned threads, bool useCache, bool deferValidation = false,
                           std::chrono::seconds refreshInterval = sizetree::defaultRefreshInterval)
        : threadCount_(threads), cacheEnabled_(useCache), deferValidation_(deferValidation),
          refreshInterval_(refreshInterval) {
        root_.path = std::move(path);
        nfsDirectoryCache_ = nfsDirectoryCache(root_.path);
        root_.id = nextId_++;
        root_.kind = Kind::Directory;
        directories_.emplace(root_.id, &root_);
        cache_ = std::make_unique<sizetree::DiskCache>(root_.path, useCache);
    }

    void start() {
        if (!workers_.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            // The browser has rendered the restored cache before checking it.
            if (!initializing_ && !validationStarted_ && !cancel_.load(std::memory_order_relaxed)) {
                queue_.push_back({&root_});
                validationStarted_ = true;
                workAvailable_.notify_all();
            }
            return;
        }
        scanStarted_ = Clock::now();
        try {
            workers_.reserve(threadCount_);
            for (unsigned i = 0; i < threadCount_; ++i)
                workers_.emplace_back([this] { work(); });
            loader_ = std::thread([this] { initialize(); });
        } catch (...) {
            stop();
            throw;
        }
    }

    ~DirectoryTree() { stop(); }

    DirectoryTree(const DirectoryTree&) = delete;
    DirectoryTree& operator=(const DirectoryTree&) = delete;

    void flushCache() {
        saveOverview();
        cache_->flush();
    }

    [[noreturn]] void finishAndExit(int status) {
        stop();
        std::cout.flush();
        std::cerr.flush();
        // Workers and file writes are finished and the terminal is restored.
        // Let the OS reclaim millions of nodes without individual heap frees.
        std::_Exit(status);
    }

    bool waitForCompletion() {
        std::unique_lock<std::mutex> lock(mutex_);
        return workAvailable_.wait_for(lock, std::chrono::milliseconds(50), [this] { return done_; });
    }

    ScanProgress progress() {
        // Reading totals must not copy/sort entries or write a cache overview.
        std::lock_guard<std::mutex> lock(mutex_);
        return {root_.bytes, root_.entries, root_.errors, done_, root_.cached, initializing_ && cacheEnabled_};
    }

    void refresh(NodeId id) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = directories_.find(id);
            if (found == directories_.end() || cancel_.load(std::memory_order_relaxed)) return;
            Node& directory = *found->second;
            if (directory.complete) restart(directory);
            else directory.refreshQueued = true;
        }
        workAvailable_.notify_all();
    }

    bool refreshIfDue(Clock::time_point now = Clock::now()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // A completed root includes all active and queued subtree refreshes.
            // Never add periodic work while loading, scanning, or shutting down.
            if (refreshInterval_.count() <= 0 || initializing_ || !done_ || !root_.complete || !queue_.empty() ||
                cancel_.load(std::memory_order_relaxed) || now < nextAutomaticRefresh_) return false;
            restart(root_);
            nextAutomaticRefresh_ = now + refreshInterval_;
        }
        workAvailable_.notify_all();
        return true;
    }

    Snapshot snapshot(std::unordered_set<NodeId>& expanded) {
        Snapshot result;
        std::vector<VisibleNode> visible;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            result.bytes = root_.bytes;
            result.entries = root_.entries;
            result.errors = root_.errors;
            result.done = done_;
            result.cached = root_.cached;
            result.loading = initializing_ && cacheEnabled_;
            result.elapsed = elapsedScanTime();
            result.failure = failure_;
            for (auto entry = expanded.begin(); entry != expanded.end();) {
                if (directories_.count(*entry) == 0) entry = expanded.erase(entry);
                else ++entry;
            }
            visible = copyRows(root_, 0, expanded);
        }
        // Sorting and filename formatting must not hold up the scanner workers.
        appendRows(visible, result.rows);
        result.cacheWarning = displayText(cache_->warning());
        saveOverview();
        return result;
    }

    std::uint64_t searchRevision() {
        std::lock_guard<std::mutex> lock(mutex_);
        return searchRevision_;
    }

    // Copy small batches by stable IDs. A refresh may replace nodes between
    // calls, so never retain Node pointers after releasing the scanner's lock.
    bool searchBatch(SearchCursor& cursor, std::vector<SearchEntry>& entries) {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool reset = !cursor.started || cursor.epoch != searchEpoch_;
        if (reset) {
            cursor.stack = {{root_.id, 0, {}}};
            cursor.epoch = searchEpoch_;
            cursor.revision = searchRevision_;
            cursor.started = true;
            cursor.done = false;
        }
        entries.clear();
        for (std::size_t visited = 0; !cursor.stack.empty() && visited < 512; ++visited) {
            auto& frame = cursor.stack.back();
            const auto found = directories_.find(frame.directory);
            if (found == directories_.end() || frame.offset >= found->second->children.size()) {
                cursor.stack.pop_back();
                continue;
            }
            const Node& child = *found->second->children[frame.offset++];
            if (child.kind == Kind::Directory) {
                auto prefix = frame.prefix + child.name + '/';
                cursor.stack.push_back({child.id, 0, std::move(prefix)});
            } else if (child.kind == Kind::File || child.kind == Kind::Link) {
                entries.push_back({child.id, frame.directory, frame.prefix + child.name});
            }
        }
        cursor.done = cursor.stack.empty();
        return reset;
    }

    bool reveal(const SearchEntry& entry, std::unordered_set<NodeId>& expanded,
                std::unordered_set<NodeId>& automatic, std::vector<NodeId>& selection) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto parent = directories_.find(entry.parent);
        if (parent == directories_.end()) return false;
        const auto& children = parent->second->children;
        if (std::none_of(children.begin(), children.end(), [&](const auto& child) { return child->id == entry.node; }))
            return false;
        for (const auto id : automatic) expanded.erase(id);
        automatic.clear();
        selection = {entry.node};
        for (const Node* node = parent->second; node && node->parent; node = node->parent) {
            selection.push_back(node->id);
            if (expanded.insert(node->id).second) automatic.insert(node->id);
        }
        return true;
    }

    std::optional<fs::path> filePath(NodeId parent, NodeId entry) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = directories_.find(parent);
        if (found == directories_.end()) return std::nullopt;
        for (const auto& child : found->second->children) {
            if (child->id == entry && (child->kind == Kind::File || child->kind == Kind::Link))
                return found->second->path / child->name;
        }
        return std::nullopt;
    }

private:
    struct VisibleNode {
        Row row;
        std::vector<VisibleNode> children;
    };

    struct Job {
        Node* directory = nullptr;
        std::shared_ptr<DirectoryHandle> parent{};
    };

    Node root_;
    std::mutex mutex_;
    std::condition_variable workAvailable_;
    // Keep a bounded number of enumerated parents alive so queued children
    // can open relative to them instead of walking their full NFS path again.
    std::atomic<unsigned> openDirectories_{0};
    const unsigned directoryHandleBudget_ = [] {
        struct rlimit limit{};
        if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) return 16u;
        return static_cast<unsigned>(std::min<rlim_t>(64, limit.rlim_cur / 4));
    }();
    std::deque<Job> queue_;
    std::unordered_map<NodeId, Node*> directories_;
    NodeId nextId_ = 1;
    std::uint64_t searchRevision_ = 0, searchEpoch_ = 0;
    std::atomic<bool> cancel_{false};
    bool done_ = false;
    Clock::time_point scanStarted_{}, scanFinished_{};
    std::string failure_;
    std::vector<std::thread> workers_;
    unsigned threadCount_;
    bool cacheEnabled_, deferValidation_;
    bool nfsDirectoryCache_ = false;
    bool initializing_ = true, validationStarted_ = false;
    const std::chrono::seconds refreshInterval_;
    Clock::time_point nextAutomaticRefresh_{};
    std::thread loader_;
    std::unique_ptr<sizetree::DiskCache> cache_;

    struct LoadState {
        Node root;
        std::unordered_map<NodeId, Node*> directories;
        NodeId nextId = 1;
        sizetree::CachedDirectories saved;
    } loaded_;

    // Called under mutex_. Keep completed timing fixed while the browser is idle.
    Clock::duration elapsedScanTime() const {
        if (scanStarted_ == Clock::time_point{}) return Clock::duration::zero();
        return (done_ ? scanFinished_ : Clock::now()) - scanStarted_;
    }

    void saveOverview() {
        if (!cache_->writable()) return;
        sizetree::CacheOverview overview;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (initializing_ || (!root_.cached && !root_.complete && root_.children.empty())) return;
            overview.bytes = root_.bytes;
            overview.entries = root_.entries;
            overview.errors = root_.errors;
            overview.listing.ownErrors = root_.ownErrors;
            std::size_t estimate = 128;
            for (const auto& child : root_.children) {
                estimate += child->name.size() + 40;
                if (estimate > 16 * 1024 * 1024) return;
                overview.listing.children.push_back({child->name, child->kind, child->bytes, child->errors, child->entries});
            }
        }
        cache_->saveOverview(std::move(overview));
    }

    void initialize() {
        try {
            if (auto overview = cache_->loadOverview()) {
                std::lock_guard<std::mutex> lock(mutex_);
                root_.cached = true;
                root_.bytes = overview->bytes;
                root_.entries = overview->entries;
                root_.errors = overview->errors;
                root_.ownErrors = overview->listing.ownErrors;
                for (auto& entry : overview->listing.children) {
                    auto child = std::make_unique<Node>();
                    child->name = std::move(entry.name);
                    child->kind = entry.kind;
                    child->bytes = entry.bytes;
                    child->entries = entry.entries;
                    child->errors = entry.errors;
                    child->parent = &root_;
                    child->id = nextId_++;
                    child->cached = true;
                    child->complete = child->kind != Kind::Directory;
                    if (child->kind == Kind::Directory) {
                        child->path = root_.path / child->name;
                        directories_.emplace(child->id, child.get());
                    }
                    root_.children.push_back(std::move(child));
                }
                ++searchRevision_;
                ++searchEpoch_;
            }
            loaded_.root.path = root_.path;
            loaded_.root.id = root_.id;
            loaded_.root.kind = Kind::Directory;
            loaded_.nextId = nextId_;
            loaded_.saved = cache_->load();
            if (!restore(loaded_.root, "", loaded_.saved, loaded_.directories, loaded_.nextId)) return;
            if (cancel_.load(std::memory_order_relaxed)) return;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                // Preserve the selection and expansions made in the overview.
                std::unordered_map<std::string_view, Node*> previous;
                for (const auto& child : root_.children) previous.emplace(child->name, child.get());
                for (auto& child : loaded_.root.children) {
                    const auto old = previous.find(child->name);
                    if (old != previous.end() && old->second->kind == child->kind) {
                        if (child->kind == Kind::Directory) loaded_.directories.erase(child->id);
                        child->id = old->second->id;
                        child->refreshQueued = old->second->refreshQueued;
                        if (child->kind == Kind::Directory) loaded_.directories.emplace(child->id, child.get());
                    }
                    child->parent = &root_;
                }
                root_ = std::move(loaded_.root);
                directories_ = std::move(loaded_.directories);
                directories_[root_.id] = &root_;
                nextId_ = loaded_.nextId;
                ++searchRevision_;
                ++searchEpoch_;
                initializing_ = false;
                if (!deferValidation_) {
                    queue_.push_back({&root_});
                    validationStarted_ = true;
                }
            }
            saveOverview();
            workAvailable_.notify_all();
        } catch (const std::exception& error) {
            std::lock_guard<std::mutex> lock(mutex_);
            failure_ = displayText(error.what());
            root_.errors += 1;
            initializing_ = false;
            cancel_.store(true, std::memory_order_relaxed);
            scanFinished_ = Clock::now();
            done_ = true;
            workAvailable_.notify_all();
        }
    }

    bool restore(Node& directory, const std::string& key, sizetree::CachedDirectories& saved,
                 std::unordered_map<NodeId, Node*>& directories, NodeId& nextId) {
        if (cancel_.load(std::memory_order_relaxed)) return false;
        const auto found = saved.find(key);
        if (found == saved.end()) return true;
        auto& record = found->second;
        directory.cached = true;
        directory.ownErrors = record.ownErrors;
        directory.errors = record.ownErrors;
        directory.children.reserve(record.children.size());
        for (auto& entry : record.children) {
            if (cancel_.load(std::memory_order_relaxed)) return false;
            auto child = std::make_unique<Node>();
            child->name = std::move(entry.name);
            child->kind = entry.kind;
            child->bytes = entry.bytes;
            child->errors = entry.errors;
            child->parent = &directory;
            child->id = nextId++;
            child->cached = true;
            child->complete = child->kind != Kind::Directory;
            if (child->kind == Kind::Directory) {
                child->path = directory.path / child->name;
                directories.emplace(child->id, child.get());
                // Own the node before recursion so cancellation need not free
                // a large partially restored subtree on the loader's stack.
                directory.children.push_back(std::move(child));
                Node& nested = *directory.children.back();
                if (!restore(nested, key.empty() ? nested.name : key + "/" + nested.name,
                             saved, directories, nextId)) return false;
                directory.bytes += nested.bytes;
                directory.entries += 1 + nested.entries;
                directory.errors += nested.errors;
                continue;
            }
            directory.bytes += child->bytes;
            directory.entries += 1 + child->entries;
            directory.errors += child->errors;
            directory.children.push_back(std::move(child));
        }
        saved.erase(found);
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancel_.store(true, std::memory_order_relaxed);
        }
        workAvailable_.notify_all();
        cache_->cancel();
        if (loader_.joinable()) loader_.join();
        for (auto& worker : workers_)
            if (worker.joinable()) worker.join();
        if (cache_) {
            saveOverview();
            cache_->close();
        }
    }

    // Call while holding mutex_. Ancestors retain partial totals during scanning.
    void markError(Node* node, std::uintmax_t count = 1) {
        for (; node; node = node->parent) node->errors += count;
    }

    void forgetChildren(const Node& directory) {
        for (const auto& child : directory.children) {
            if (child->kind == Kind::Directory) {
                forgetChildren(*child);
                directories_.erase(child->id);
            }
        }
    }

    static void markCached(Node& node) {
        node.cached = true;
        if (node.kind != Kind::Directory) return;
        node.complete = false;
        node.pending = 1;
        for (auto& child : node.children) markCached(*child);
    }

    // Only restart completed subtrees: no worker can still be publishing into them.
    // Register the new work with ancestors without counting an active branch twice.
    void restart(Node& directory) {
        // Allocate the job before changing the scan state.
        queue_.emplace_front();
        if (done_) scanStarted_ = Clock::now();
        for (Node* ancestor = directory.parent; ancestor; ancestor = ancestor->parent) {
            ++ancestor->pending;
            ancestor->cached = true;
            if (!ancestor->complete) break;
            ancestor->complete = false;
        }
        markCached(directory);
        directory.refreshQueued = false;
        done_ = false;
        queue_.front().directory = &directory;
    }

    // A directory is complete only after its enumeration AND every child finish.
    void finishDirectory(Node* directory) {
        Node* refresh = nullptr;
        while (directory && --directory->pending == 0) {
            directory->complete = true;
            directory->cached = false;
            if (directory->refreshQueued) refresh = directory;
            directory = directory->parent;
        }
        // An ancestor refresh includes any requests on its completed descendants.
        if (refresh) restart(*refresh);
        done_ = root_.complete;
        if (done_) {
            scanFinished_ = Clock::now();
            if (refreshInterval_.count() > 0)
                nextAutomaticRefresh_ = std::max(nextAutomaticRefresh_, scanFinished_ + refreshInterval_);
        }
    }

    void work() {
        while (true) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                workAvailable_.wait(lock, [this] {
                    return cancel_.load(std::memory_order_relaxed) || !queue_.empty();
                });
                if (cancel_.load(std::memory_order_relaxed)) return;
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                scan(*job.directory, std::move(job.parent));
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!cancel_.load(std::memory_order_relaxed)) finishDirectory(job.directory);
                }
            } catch (const std::exception& error) {
                std::lock_guard<std::mutex> lock(mutex_);
                failure_ = displayText(error.what());
                markError(job.directory);
                cancel_.store(true, std::memory_order_relaxed);
                scanFinished_ = Clock::now();
                done_ = true;
            }
            workAvailable_.notify_all();
        }
    }

    void publish(Node& directory, std::vector<std::unique_ptr<Node>>& batch,
                 const std::shared_ptr<DirectoryHandle>& parent) {
        if (batch.empty()) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++searchRevision_;
            std::uintmax_t bytes = 0;
            std::uintmax_t errors = 0;
            const auto count = batch.size();
            for (auto& child : batch) {
                Node* node = child.get();
                node->id = nextId_++;
                bytes += node->bytes;
                errors += node->errors;
                directory.children.push_back(std::move(child));
                if (node->kind == Kind::Directory) {
                    ++directory.pending;
                    directories_.emplace(node->id, node);
                    queue_.push_back({node, parent});
                }
            }
            for (Node* ancestor = &directory; ancestor; ancestor = ancestor->parent) {
                ancestor->bytes += bytes;
                ancestor->entries += count;
            }
            if (errors) markError(&directory, errors);
        }
        batch.clear();
        workAvailable_.notify_all();
    }

    void replaceListing(Node& directory, std::vector<std::unique_ptr<Node>> children, std::uintmax_t ownErrors,
                        const std::shared_ptr<DirectoryHandle>& parent = {}) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++searchRevision_;
            // Cached descendants have not been scheduled yet. Reuse directories
            // by name, keeping their last sizes and expanded state until checked.
            std::unordered_map<std::string_view, std::size_t> previous;
            for (std::size_t i = 0; i < directory.children.size(); ++i)
                if (directory.children[i]->kind == Kind::Directory) previous.emplace(directory.children[i]->name, i);
            directory.children.swap(children);
            for (auto& child : directory.children) {
                if (child->kind == Kind::Directory) {
                    const auto old = previous.find(child->name);
                    if (old != previous.end()) child = std::move(children[old->second]);
                }
            }
            // Remove old IDs before any allocation can fail. New jobs always
            // point at nodes already owned by the tree, even on an exception.
            for (const auto& old : children) {
                if (old && old->kind == Kind::Directory) {
                    forgetChildren(*old);
                    directories_.erase(old->id);
                }
            }
            std::uintmax_t bytes = 0, entries = directory.children.size(), errors = ownErrors;
            for (auto& child : directory.children) {
                if (child->kind == Kind::Directory) {
                    if (child->id == 0) {
                        child->id = nextId_++;
                        directories_.emplace(child->id, child.get());
                    }
                    ++directory.pending;
                    queue_.push_back({child.get(), parent});
                } else child->id = nextId_++;
                bytes += child->bytes;
                entries += child->entries;
                errors += child->errors;
            }
            const auto oldBytes = directory.bytes, oldEntries = directory.entries, oldErrors = directory.errors;
            for (Node* ancestor = &directory; ancestor; ancestor = ancestor->parent) {
                ancestor->bytes = ancestor->bytes - oldBytes + bytes;
                ancestor->entries = ancestor->entries - oldEntries + entries;
                ancestor->errors = ancestor->errors - oldErrors + errors;
            }
            directory.ownErrors = ownErrors;
        }
        workAvailable_.notify_all();
        // The old file nodes and removed directories are freed outside the lock.
    }

    void scan(Node& directory, std::shared_ptr<DirectoryHandle> parent) {
        bool replacing;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            replacing = directory.cached;
        }
        const bool save = cache_->writable();
        sizetree::CachedDirectory record;
        if (save) {
            record.path = directory.path.lexically_relative(root_.path).string();
            if (record.path == ".") record.path.clear();
        }
        // Reuse the enumerated parent's descriptor when available. O_NOFOLLOW
        // prevents a directory replaced by a symlink from being traversed.
        const auto stream = std::make_shared<DirectoryHandle>(openDirectories_,
            parent ? parent->descriptor() : AT_FDCWD, parent ? directory.name.c_str() : directory.path.c_str());
        parent.reset();
        const int descriptor = stream->descriptor();
        if (descriptor < 0) {
            if (replacing) replaceListing(directory, {}, 1);
            else {
                std::lock_guard<std::mutex> lock(mutex_);
                directory.ownErrors = 1;
                markError(&directory);
            }
            record.ownErrors = 1;
            if (save) cache_->save(std::move(record));
            return;
        }
        const auto reusable = openDirectories_.load(std::memory_order_relaxed) <= directoryHandleBudget_
            ? stream : std::shared_ptr<DirectoryHandle>{};
#ifdef __linux__
        // Cached directory names can outlive their file attributes on NFS.
        // Refreshing the listing lets READDIRPLUS repopulate attributes in bulk
        // instead of issuing one GETATTR for each expired file. This is only
        // an advisory hint on the directory; file data and sync policy are unchanged.
        if (nfsDirectoryCache_) (void)::posix_fadvise(descriptor, 0, 0, POSIX_FADV_DONTNEED);
#endif
        DirectoryReader reader(*stream);
        std::vector<std::unique_ptr<Node>> batch;
        batch.reserve(128);
        auto lastPublish = std::chrono::steady_clock::now();
        while (!cancel_.load(std::memory_order_relaxed)) {
            errno = 0;
            const char* name;
            unsigned char type;
            if (!reader.next(name, type)) {
                if (errno) {
                    record.ownErrors = 1;
                }
                break;
            }
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
            auto child = std::make_unique<Node>();
            child->name = name;
            child->parent = &directory;
            if (type == DT_DIR) child->kind = Kind::Directory;
            else if (type == DT_LNK) child->kind = Kind::Link;
            else if (type != DT_REG && type != DT_UNKNOWN) child->kind = Kind::Other;
            else {
                // A single lookup returns both type and size, including DT_UNKNOWN on NFS.
                mode_t mode{};
                std::uintmax_t bytes{};
                if (!fileMetadata(descriptor, name, mode, bytes))
                    child->errors = 1;
                else if (S_ISREG(mode)) {
                    child->kind = Kind::File;
                    child->bytes = bytes;
                }
                else if (S_ISDIR(mode)) child->kind = Kind::Directory;
                else if (S_ISLNK(mode)) child->kind = Kind::Link;
                else child->kind = Kind::Other;
            }
            if (child->kind == Kind::Directory) child->path = directory.path / child->name;
            child->complete = child->kind != Kind::Directory;
            if (save) record.children.push_back({child->name, child->kind, child->bytes, child->errors});
            batch.push_back(std::move(child));
            const auto now = std::chrono::steady_clock::now();
            if (!replacing && (batch.size() >= 128 || now - lastPublish >= std::chrono::milliseconds(50))) {
                publish(directory, batch, reusable);
                lastPublish = now;
            }
        }
        if (!cancel_.load(std::memory_order_relaxed)) {
            if (replacing) replaceListing(directory, std::move(batch), record.ownErrors, reusable);
            else {
                publish(directory, batch, reusable);
                if (record.ownErrors) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    directory.ownErrors = record.ownErrors;
                    markError(&directory, record.ownErrors);
                }
            }
            if (save) cache_->save(std::move(record));
        }
    }

    static std::vector<VisibleNode> copyRows(const Node& parent, int depth,
                                           const std::unordered_set<NodeId>& expanded) {
        std::vector<VisibleNode> children;
        children.reserve(parent.children.size());
        for (const auto& child : parent.children) {
            const bool open = expanded.count(child->id) != 0;
            children.push_back({{child->id, child->parent->id, child->name, child->kind, child->bytes,
                                 child->complete, child->errors != 0, open, child->refreshQueued, child->cached, depth}, {}});
            if (open) children.back().children = copyRows(*child, depth + 1, expanded);
        }
        return children;
    }

    static void appendRows(std::vector<VisibleNode>& children, std::vector<Row>& rows) {
        std::sort(children.begin(), children.end(), [](const VisibleNode& lhs, const VisibleNode& rhs) {
            if ((lhs.row.kind == Kind::Directory) != (rhs.row.kind == Kind::Directory))
                return lhs.row.kind == Kind::Directory;
            if (lhs.row.bytes != rhs.row.bytes) return lhs.row.bytes > rhs.row.bytes;
            return lhs.row.name < rhs.row.name;
        });
        for (auto& child : children) {
            child.row.name = displayText(child.row.name);
            rows.push_back(std::move(child.row));
            appendRows(child.children, rows);
        }
    }
};

std::string sizeText(const Row& row) {
    if (row.kind == Kind::Link || row.kind == Kind::Other) return "-";
    return std::string(row.complete && !row.cached ? "" : "~") + humanSize(row.bytes) + (row.partial ? "!" : "");
}

std::string marker(const Row& row) {
    if (row.kind == Kind::Directory) return row.expanded ? "[-]" : "[+]";
    if (row.kind == Kind::Link) return "[@]";
    if (row.kind == Kind::Other) return "[*]";
    return "   ";
}

class FuzzySearch {
public:
    bool active = false;
    std::string query;
    std::unordered_set<NodeId> automaticExpansions;

    void setQuery(std::string value) {
        query = std::move(value);
        matcher_.reset(query);
        cursor_ = {};
        matches_.clear();
        selected_ = 0;
        manualSelection_ = false;
        preferred_.clear();
        sorted_ = true;
    }

    bool empty() const { return matcher_.empty(); }
    bool busy() const { return active && !empty() && !cursor_.done; }
    std::size_t count() const { return matches_.size(); }
    std::size_t index() const { return selected_; }
    const SearchEntry* current() const { return matches_.empty() ? nullptr : &matches_[selected_]; }

    void step(DirectoryTree& tree) {
        if (!active || empty()) return;
        const auto now = DirectoryTree::Clock::now();
        if (cursor_.done) {
            if (now < refreshAfter_ || tree.searchRevision() == cursor_.revision) return;
            if (const auto* entry = current()) preferred_ = entry->path;
            cursor_ = {};
        }
        // Yield between small batches so typing, cancellation, and scanner
        // workers remain responsive even with hundreds of thousands of files.
        const auto deadline = now + std::chrono::milliseconds(5);
        do {
            if (tree.searchBatch(cursor_, batch_)) {
                matches_.clear();
                selected_ = 0;
                manualSelection_ = false;
                sorted_ = true;
            }
            for (auto& entry : batch_) {
                auto score = matcher_.score(entry.path);
                if (!score) continue;
                const auto slash = entry.path.find_last_of('/');
                const std::string_view name(entry.path.data() + (slash == std::string::npos ? 0 : slash + 1),
                    entry.path.size() - (slash == std::string::npos ? 0 : slash + 1));
                if (const auto nameScore = matcher_.score(name)) score = std::max(*score, *nameScore + 200);
                entry.score = *score;
                const bool preferred = !preferred_.empty() && entry.path == preferred_;
                if (matches_.empty() || preferred || (!manualSelection_ && better(entry, matches_[selected_])))
                    selected_ = matches_.size();
                if (preferred) manualSelection_ = true;
                matches_.push_back(std::move(entry));
                sorted_ = false;
            }
        } while (!cursor_.done && DirectoryTree::Clock::now() < deadline);
        if (cursor_.done) {
            sort();
            refreshAfter_ = DirectoryTree::Clock::now() + std::chrono::milliseconds(500);
        }
    }

    void cycle(bool previous = false) {
        if (matches_.empty()) return;
        sort();
        selected_ = previous ? (selected_ + matches_.size() - 1) % matches_.size()
                             : (selected_ + 1) % matches_.size();
        manualSelection_ = true;
        preferred_ = matches_[selected_].path;
    }

    void movePage(std::size_t amount, bool previous = false) {
        if (matches_.empty()) return;
        sort();
        if (previous) selected_ -= std::min(selected_, amount);
        else selected_ += std::min(matches_.size() - 1 - selected_, amount);
        manualSelection_ = true;
        preferred_ = matches_[selected_].path;
    }

    void close() {
        active = false;
        setQuery({});
        automaticExpansions.clear(); // Keep the selected match visible in the browser.
        std::vector<SearchEntry>().swap(matches_);
    }

private:
    sizetree::FuzzyMatcher matcher_;
    SearchCursor cursor_;
    std::vector<SearchEntry> matches_, batch_;
    std::size_t selected_ = 0;
    bool manualSelection_ = false, sorted_ = true;
    std::string preferred_;
    DirectoryTree::Clock::time_point refreshAfter_{};

    static bool better(const SearchEntry& left, const SearchEntry& right) {
        if (left.score != right.score) return left.score > right.score;
        if (left.path.size() != right.path.size()) return left.path.size() < right.path.size();
        if (left.path != right.path) return left.path < right.path;
        return left.node < right.node;
    }

    void sort() {
        if (sorted_ || matches_.empty()) return;
        const auto node = matches_[selected_].node;
        std::sort(matches_.begin(), matches_.end(), better);
        selected_ = static_cast<std::size_t>(std::find_if(matches_.begin(), matches_.end(),
            [node](const auto& entry) { return entry.node == node; }) - matches_.begin());
        sorted_ = true;
    }
};

class FileOpener {
public:
    std::string message;

    void open(const std::optional<fs::path>& path) {
        if (!path) { message = "File is no longer available; refresh or search again."; return; }
        auto filename = path->string();
        children_.reserve(children_.size() + 1);
        posix_spawn_file_actions_t actions;
        int error = ::posix_spawn_file_actions_init(&actions);
        if (error) { failed(error); return; }
        posix_spawnattr_t attributes;
        error = ::posix_spawnattr_init(&attributes);
        if (error) { ::posix_spawn_file_actions_destroy(&actions); failed(error); return; }
        const auto check = [&](int result) { if (!error) error = result; };
        check(::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
        check(::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0));
        check(::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0));
        sigset_t defaults, mask;
        ::sigemptyset(&defaults);
        ::sigemptyset(&mask);
        for (const int signal : {SIGINT, SIGTERM, SIGHUP, SIGPIPE, SIGTSTP, SIGCONT}) ::sigaddset(&defaults, signal);
        check(::posix_spawnattr_setsigdefault(&attributes, &defaults));
        check(::posix_spawnattr_setsigmask(&attributes, &mask));
        check(::posix_spawnattr_setpgroup(&attributes, 0));
        check(::posix_spawnattr_setflags(&attributes,
            POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP));
        pid_t child = -1;
        char command[] = "xdg-open";
        char* arguments[] = {command, filename.data(), nullptr};
        if (!error) error = ::posix_spawnp(&child, command, &actions, &attributes, arguments, environ);
        ::posix_spawnattr_destroy(&attributes);
        ::posix_spawn_file_actions_destroy(&actions);
        if (error) { failed(error); return; }
        message = "Opening " + displayText(path->filename().string()) + "...";
        children_.push_back({child, std::move(filename)});
    }

    void poll() {
        for (auto process = children_.begin(); process != children_.end();) {
            int status = 0;
            const auto result = ::waitpid(process->pid, &status, WNOHANG);
            if (result == 0 || (result < 0 && errno == EINTR)) { ++process; continue; }
            if (result > 0) {
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                    message = "Opened " + displayText(fs::path(process->path).filename().string());
                else if (WIFEXITED(status))
                    message = "xdg-open failed (exit " + std::to_string(WEXITSTATUS(status)) + ").";
                else message = "xdg-open was terminated.";
            }
            process = children_.erase(process);
        }
    }

private:
    struct Process { pid_t pid; std::string path; };
    std::vector<Process> children_;

    void failed(int error) {
        message = error == ENOENT ? "Cannot open file: xdg-open was not found (install xdg-utils)."
                                 : "Cannot start xdg-open: " + displayText(std::strerror(error));
    }
};

class Terminal {
public:
    Terminal() {
        if (::tcgetattr(STDIN_FILENO, &saved_) != 0)
            throw std::runtime_error("cannot read terminal settings: " + std::string(std::strerror(errno)));
        activate();
    }

    ~Terminal() { restore(); }

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    bool handleJobControl() {
        if (!suspendRequested && !resumeRequested) return false;
        if (suspendRequested) {
            suspendRequested = 0;
            // Terminal I/O belongs on the UI thread, outside the signal handler.
            restore();
            struct sigaction stop{}, previous{};
            stop.sa_handler = SIG_DFL;
            ::sigemptyset(&stop.sa_mask);
            if (::sigaction(SIGTSTP, &stop, &previous) != 0)
                throw std::runtime_error("cannot suspend terminal");
            ::raise(SIGTSTP);
            // Execution continues here when the shell sends SIGCONT.
            if (::sigaction(SIGTSTP, &previous, nullptr) != 0)
                throw std::runtime_error("cannot restore suspend handler");
        }
        if (!interrupted) activate();
        resumeRequested = 0;
        return true;
    }

    static std::pair<int, int> dimensions() {
        winsize size{};
        if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col && size.ws_row)
            return {size.ws_col, size.ws_row};
        return {80, 24};
    }

private:
    termios saved_{};
    bool active_ = false;

    void activate() {
        // A job continued with bg must wait for fg before touching the terminal.
        while (!interrupted) {
            const auto foreground = ::tcgetpgrp(STDIN_FILENO);
            if (foreground < 0 || foreground == ::getpgrp()) break;
            ::raise(SIGSTOP);
        }
        if (interrupted) return;
        auto raw = saved_;
        raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
        raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
            throw std::runtime_error("cannot configure terminal: " + std::string(std::strerror(errno)));
        active_ = true;
        std::cout << "\x1b[?1049h\x1b[?25l\x1b[2J" << std::flush;
    }

    void restore() {
        if (!active_) return;
        std::cout << "\x1b[0m\x1b[?25h\x1b[?1049l" << std::flush;
        while (::tcsetattr(STDIN_FILENO, TCSANOW, &saved_) != 0 && errno == EINTR) {}
        active_ = false;
    }
};

enum class Key { None, Quit, Up, Down,
                 Toggle, Enter, Search, Text, Backspace, Clear, BackTab,
                 Refresh, Escape, Left, Right, Home, End, PageUp, PageDown };

int readByte(int timeout) {
    pollfd descriptor{STDIN_FILENO, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, timeout);
    if (ready < 0) return errno == EINTR ? -1 : -2;
    if (ready == 0) return -1;
    if (!(descriptor.revents & POLLIN)) return -2;
    unsigned char byte{};
    const auto count = ::read(STDIN_FILENO, &byte, 1);
    if (count == 1) return byte;
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) return -1;
    return -2;
}

Key readKey(bool searching, char& text, int timeout = 100) {
    static int pendingByte = -1;
    int byte = pendingByte;
    pendingByte = -1;
    if (byte < 0) byte = readByte(timeout);
    if (byte == -2 || byte == 4) return Key::Quit;
    if (byte == 6) return Key::PageDown; // Ctrl+F
    if (byte == 2) return Key::PageUp;   // Ctrl+B
    if (byte == '\t') return Key::Toggle;
    if (byte == '\r' || byte == '\n') return Key::Enter;
    if (searching && byte != 27) {
        if (byte == 8 || byte == 127) return Key::Backspace;
        if (byte == 21) return Key::Clear;
        if (byte >= 32) { text = static_cast<char>(byte); return Key::Text; }
        return Key::None;
    }
    switch (byte) {
        case 'q': case 'Q': return Key::Quit;
        case ' ': return Key::Toggle;
        case '/': return Key::Search;
        case 'r': case 'R': return Key::Refresh;
        case 'k': return Key::Up;
        case 'j': return Key::Down;
        case 'h': return Key::Left;
        case 'l': return Key::Right;
        case 27: break;
        default: return Key::None;
    }
    const int prefix = readByte(40);
    if (prefix != '[' && prefix != 'O') {
        // A standalone Escape is an action. Preserve a following ordinary key
        // (or another Escape), while still decoding terminal navigation sequences.
        pendingByte = prefix;
        return Key::Escape;
    }
    std::string sequence;
    for (int i = 0; i < 12; ++i) {
        const int next = readByte(40);
        if (next < 0) return Key::None;
        sequence += static_cast<char>(next);
        if (next >= 0x40 && next <= 0x7e) break;
    }
    if (sequence == "A") return Key::Up;
    if (sequence == "B") return Key::Down;
    if (sequence == "C") return Key::Right;
    if (sequence == "D") return Key::Left;
    if (sequence == "Z") return Key::BackTab;
    if (sequence == "H" || sequence == "1~" || sequence == "7~") return Key::Home;
    if (sequence == "F" || sequence == "4~" || sequence == "8~") return Key::End;
    if (sequence == "5~") return Key::PageUp;
    if (sequence == "6~") return Key::PageDown;
    return Key::None;
}

std::string render(const Snapshot& snapshot, const std::string& path, int width, int height,
                   std::size_t selected, std::size_t& scroll, const FuzzySearch* search = nullptr,
                   const std::string& notice = {}) {
    std::string frame = "\x1b[H";
    int lineNumber = 0;
    const auto line = [&](const std::string& text, bool highlight = false) {
        frame += "\x1b[2K";
        if (highlight) frame += "\x1b[7m";
        // Leave the last column free to avoid automatic wrapping.
        frame += clip(text, width - 1);
        if (highlight) frame += "\x1b[0m";
        if (++lineNumber < height) frame += "\r\n";
    };
    if (width < 40 || height < 8) {
        line("Enlarge terminal (40x8 minimum). q: quit");
        while (lineNumber < height) line("");
        return frame;
    }
    const auto visible = static_cast<std::size_t>(height - 6);
    if (selected < scroll) scroll = selected;
    if (selected >= scroll + visible) scroll = selected - visible + 1;
    if (scroll > (snapshot.rows.size() > visible ? snapshot.rows.size() - visible : 0))
        scroll = snapshot.rows.size() > visible ? snapshot.rows.size() - visible : 0;

    line("sizetree | " + path);
    if (snapshot.loading && !snapshot.cached) line("Loading saved sizes... | q: quit");
    else line("Total: " + std::string(snapshot.done ? "" : "~") + humanSize(snapshot.bytes) +
         (snapshot.errors ? "!" : "") + " | " +
         (snapshot.done ? "Scanned " : snapshot.cached ? "Cached; checking... " : "Scanning... ") +
         std::to_string(snapshot.entries) + " entries" +
         (snapshot.done ? " in " : " | elapsed ") + elapsedText(snapshot.elapsed) +
         (snapshot.errors ? " | " + std::to_string(snapshot.errors) + " errors" : "") +
         (snapshot.loading ? " | loading details" : ""));
    if (search && search->active) {
        const auto status = search->empty() ? "Type a file name or path" :
            search->count() ? std::to_string(search->index() + 1) + "/" + std::to_string(search->count()) + " matches" :
            search->busy() ? "Searching..." : "No matches";
        line("/" + displayText(search->query) + " | " + status +
             (search->count() && search->busy() ? " (searching...)" : ""));
    } else line("");
    line("          SIZE  NAME");
    for (std::size_t offset = 0; offset < visible; ++offset) {
        const std::size_t index = scroll + offset;
        if (index >= snapshot.rows.size()) {
            line(index == 0 ? (!snapshot.done ? "  Scanning..." : snapshot.errors
                              ? "  (contents unavailable)" : "  (empty directory)") : "");
            continue;
        }
        const auto& row = snapshot.rows[index];
        const auto size = sizeText(row);
        const int indent = std::min(row.depth * 2, std::max(0, width - 32));
        const std::string indentation(static_cast<std::size_t>(indent), ' ');
        line(std::string(index == selected ? "> " : "  ") +
             std::string(size.size() < 12 ? 12 - size.size() : 0, ' ') + size + "  " +
             indentation + (indent < row.depth * 2 ? ".." : "") + marker(row) + " " + row.name +
             (row.refreshQueued ? " (refresh queued)" : ""),
             index == selected);
    }
    line(search && search->active ? "Tab/Down: next | Shift-Tab/Up: previous | Enter: open | Esc: exit search"
                                 : "Up/Down: move | Tab: toggle | /: search | r: refresh | q: quit");
    if (!snapshot.failure.empty()) line("Scan failed: " + snapshot.failure);
    else if (!notice.empty()) line(notice);
    else if (!snapshot.cacheWarning.empty()) line("Cache unavailable: " + snapshot.cacheWarning);
    else if (snapshot.errors) line("!: partial size (unreadable entries) | Arrows: navigate");
    else line("PgUp/PgDn: scroll | Ctrl+F/B: page down/up | ~: scanning | [@]: link");
    return frame;
}

int interactive(DirectoryTree& tree, const fs::path& path) {
    Terminal terminal;
    FileOpener opener;
    FuzzySearch search;
    NodeId revealedMatch = 0;
    std::unordered_set<NodeId> expanded;
    std::vector<NodeId> selection;
    std::size_t scroll = 0;
    std::string previousFrame;
    while (!interrupted) {
        if (terminal.handleJobControl()) previousFrame.clear();
        if (interrupted) break;
        opener.poll();
        tree.refreshIfDue();
        search.step(tree);
        if (search.active) {
            if (const auto* match = search.current(); match && match->node != revealedMatch) {
                if (tree.reveal(*match, expanded, search.automaticExpansions, selection)) revealedMatch = match->node;
            }
        }
        const auto snapshot = tree.snapshot(expanded);
        const auto& rows = snapshot.rows;
        std::size_t selected = 0;
        // A queued refresh may remove the selected descendant. Fall back to its
        // nearest surviving ancestor, using IDs that cannot become dangling pointers.
        for (const auto id : selection) {
            const auto found = std::find_if(rows.begin(), rows.end(), [id](const Row& row) { return row.node == id; });
            if (found != rows.end()) {
                selected = static_cast<std::size_t>(found - rows.begin());
                break;
            }
        }
        const auto [width, height] = Terminal::dimensions();
        const auto frame = render(snapshot, displayText(path.string()), width, height, selected, scroll, &search, opener.message);
        if (frame != previousFrame) {
            std::cout << frame << std::flush;
            previousFrame = frame;
        }
        // Always render the loaded cache once before workers start validating it.
        tree.start();
        if (!std::cout) return 1;
        char typed = 0;
        const auto key = readKey(search.active, typed, search.busy() ? 0 : 100);
        if (suspendRequested || resumeRequested || interrupted) continue;
        if (key == Key::Quit) return snapshot.errors ? 1 : 0;
        // A resize can happen while waiting for a key; use the current height.
        std::size_t page = 0;
        if (key == Key::PageUp || key == Key::PageDown)
            page = static_cast<std::size_t>(std::max(1, Terminal::dimensions().second - 6));
        if (search.active) {
            switch (key) {
                case Key::Text:
                    if (search.query.size() < 1024) search.setQuery(search.query + typed);
                    break;
                case Key::Backspace:
                    if (!search.query.empty()) {
                        auto offset = search.query.size() - 1;
                        while (offset > 0 && (static_cast<unsigned char>(search.query[offset]) & 0xc0) == 0x80) --offset;
                        search.setQuery(search.query.substr(0, offset));
                    }
                    break;
                case Key::Clear: search.setQuery({}); break;
                case Key::Toggle: case Key::Down: search.cycle(); break;
                case Key::BackTab: case Key::Up: search.cycle(true); break;
                case Key::PageDown: search.movePage(page); break;
                case Key::PageUp: search.movePage(page, true); break;
                case Key::Enter:
                    if (const auto* match = search.current()) {
                        opener.open(tree.filePath(match->parent, match->node));
                        search.close();
                    }
                    break;
                case Key::Escape: search.close(); break;
                default: break;
            }
            continue;
        }
        if (key == Key::Search) {
            search.active = true;
            search.setQuery({});
            revealedMatch = 0;
            opener.message.clear();
            continue;
        }
        if (rows.empty()) continue;
        const auto& row = rows[selected];
        switch (key) {
            case Key::Down: if (selected + 1 < rows.size()) ++selected; break;
            case Key::Up: if (selected > 0) --selected; break;
            case Key::Home: selected = 0; break;
            case Key::End: selected = rows.size() - 1; break;
            case Key::PageDown: selected = std::min(rows.size() - 1, selected + page); break;
            case Key::PageUp: selected = selected > page ? selected - page : 0; break;
            case Key::Refresh:
                if (row.kind == Kind::Directory) tree.refresh(row.node);
                break;
            case Key::Enter:
                if (row.kind == Kind::File || row.kind == Kind::Link) {
                    opener.open(tree.filePath(row.parent, row.node));
                    break;
                }
                [[fallthrough]];
            case Key::Toggle:
                if (row.kind == Kind::Directory) {
                    if (row.expanded) expanded.erase(row.node);
                    else expanded.insert(row.node);
                }
                break;
            case Key::Right:
                if (row.kind == Kind::Directory) expanded.insert(row.node);
                break;
            case Key::Left:
            case Key::Escape:
                if (row.expanded) expanded.erase(row.node);
                if (key == Key::Escape || !row.expanded) {
                    for (std::size_t i = 0; i < rows.size(); ++i)
                        if (rows[i].node == row.parent) { selected = i; break; }
                }
                break;
            default: break;
        }
        selection = {rows[selected].node};
        int depth = rows[selected].depth;
        for (std::size_t i = selected; i > 0 && depth > 0; --i) {
            if (rows[i - 1].depth < depth) {
                selection.push_back(rows[i - 1].node);
                depth = rows[i - 1].depth;
            }
        }
    }
    return 128 + interrupted;
}

class ListProgress {
public:
    ~ListProgress() { clear(); }

    void update(DirectoryTree& tree, bool force = false) {
        if (!enabled_) return;
        const auto now = Clock::now();
        if (!force && now < nextUpdate_) return;
        nextUpdate_ = now + std::chrono::milliseconds(250);
        const auto progress = tree.progress();
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - started_).count();
        const char* phase = progress.done ? "Finishing" : progress.loading ? "Loading cache" :
                            progress.cached ? "Checking cached sizes" : "Scanning";
        constexpr char spinner[] = "|/-\\";
        std::string text = std::string(phase) + " [" + spinner[tick_++ % 4] + "] | " +
            std::to_string(progress.entries) + " entries | " + (progress.done ? "" : "~") +
            humanSize(progress.bytes) + " | " + std::to_string(elapsed) + "s";
        if (progress.errors) text += " | " + std::to_string(progress.errors) + " errors";
        const auto width = columns();
        text = clip(text, static_cast<int>(width));
        const auto previous = std::min(previousColumns_, width);
        std::cerr << ('\r' + text + std::string(previous > text.size() ? previous - text.size() : 0, ' '))
                  << std::flush;
        previousColumns_ = text.size();
    }

    void clear() {
        if (!previousColumns_) return;
        std::cerr << ('\r' + std::string(std::min(previousColumns_, columns()), ' ') + '\r') << std::flush;
        previousColumns_ = 0;
    }

private:
    using Clock = std::chrono::steady_clock;
    bool enabled_ = ::isatty(STDERR_FILENO);
    Clock::time_point started_ = Clock::now(), nextUpdate_{};
    std::size_t tick_ = 0, previousColumns_ = 0;

    static std::size_t columns() {
        winsize size{};
        if (::ioctl(STDERR_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col)
            return size.ws_col - 1;
        return 79;
    }
};

void printHelp() {
    std::cout <<
        "Usage: sizetree [--list] [--cache | --no-cache] [-j THREADS] [DIRECTORY]\n"
        "Browse recursive file and folder sizes (defaults to the current directory).\n\n"
        "  Up / Down        Move selection up / down (also k / j)\n"
        "  Tab              Expand or collapse the selected directory\n"
        "  Enter            Open the selected file with xdg-open; toggle directories\n"
        "  Space            Also expand or collapse the selected directory\n"
        "  /                Fuzzy search files, including collapsed subdirectories\n"
        "  Search keys      Tab/Shift-Tab: next/previous; Enter: open; Esc: exit search\n"
        "                   Backspace: delete a character; Ctrl+U: clear the query\n"
        "  r                Refresh the selected directory and its contents\n"
        "  Right / Left     Expand / collapse, or select parent (also l / h)\n"
        "  Escape           Collapse the selected folder and select its parent if visible\n"
        "  Home / End       Select first / last visible entry\n"
        "  PgUp / Ctrl+B    Move up one screen (or page of search matches)\n"
        "  PgDn / Ctrl+F    Move down one screen (or page of search matches)\n"
        "  Ctrl+Z           Suspend; use fg in the shell to resume\n"
        "  q or Ctrl+C      Quit\n\n"
        "  --list           Print top-level sizes with scan progress on terminal stderr\n"
        "  --cache          Enable persistent cache for this run\n"
        "  --no-cache       Disable persistent cache reads and writes for this run\n"
        "  -j, --threads N  Scan with N workers (1-64; default: 2-8 based on CPU count)\n"
        "  -h, --help       Show this help\n"
        "  --version        Show the version\n"
        "  --               End options (for directory names beginning with '-')\n\n"
        "Directories are listed first; entries are sorted by size, largest first.\n"
        "Sizes are logical file bytes, in powers of 1024, including hidden files.\n"
        "Symlinks [@] and special files [*] are not followed or counted.\n"
        "~ means scanning; ! means a partial size due to an unreadable entry.\n"
        "Cached sizes appear first and are checked in the background.\n"
        "While browsing, the whole tree refreshes after 24 hours without scan work by default.\n"
        "Active scans and queued refreshes finish before that timer restarts.\n"
        "Caching is enabled by default; missing config settings are saved automatically.\n"
        "Config: $XDG_CONFIG_HOME/sizetree/config or ~/.config/sizetree/config.\n"
        "Set cache_enabled=true or false in that file to change the preference.\n"
        "Set refresh_interval_seconds=86400 to change the period (0 disables it).\n"
        "Cache: $XDG_CACHE_HOME/sizetree or ~/.cache/sizetree.\n"
        "Redirected input or output automatically uses --list.\n"
        "List progress shows entries, estimated size, and elapsed time; stdout stays plain.\n"
        "Progress is hidden when stderr is redirected.\n";
}

int run(int argc, char** argv) {
    bool list = false;
    std::optional<bool> cacheOverride;
    unsigned threads = std::min(8u, std::max(2u, std::thread::hardware_concurrency()));
    bool options = true;
    bool pathProvided = false;
    fs::path path = ".";
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (options && (argument == "--help" || argument == "-h")) { printHelp(); return 0; }
        if (options && argument == "--version") { std::cout << "sizetree " << SIZETREE_VERSION << '\n'; return 0; }
        if (options && argument == "--list") { list = true; continue; }
        if (options && argument == "--cache") { cacheOverride = true; continue; }
        if (options && argument == "--no-cache") { cacheOverride = false; continue; }
        if (options && (argument == "-j" || argument == "--threads")) {
            if (++i >= argc) throw std::runtime_error(argument + " requires a worker count (1-64)");
            const std::string value = argv[i];
            if (value.empty() || value.size() > 2 || value.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("worker count must be between 1 and 64");
            threads = static_cast<unsigned>(std::stoul(value));
            if (threads < 1 || threads > 64) throw std::runtime_error("worker count must be between 1 and 64");
            continue;
        }
        if (options && argument == "--") { options = false; continue; }
        if (options && !argument.empty() && argument.front() == '-')
            throw std::runtime_error("unknown option: " + argument + " (try --help)");
        if (pathProvided) throw std::runtime_error("expected at most one directory (try --help)");
        path = argument;
        pathProvided = true;
    }

    // Preserve '..' until filesystem resolution, including paths through symlinks.
    std::error_code error;
    path = fs::canonical(path, error);
    if (error) throw std::runtime_error("cannot open directory: " + error.message());
    if (!fs::is_directory(path, error) || error)
        throw std::runtime_error("not a directory: " + path.string());

    struct sigaction action{};
    action.sa_handler = handleSignal;
    ::sigemptyset(&action.sa_mask);
    for (const int signal : {SIGINT, SIGTERM, SIGHUP, SIGPIPE})
        if (::sigaction(signal, &action, nullptr) != 0)
            throw std::runtime_error("cannot install signal handler");

    const char* term = std::getenv("TERM");
    const bool browser = !list && ::isatty(STDIN_FILENO) && ::isatty(STDOUT_FILENO) &&
                         (!term || std::string(term) != "dumb");
    if (browser) {
        action.sa_handler = handleJobSignal;
        for (const int signal : {SIGTSTP, SIGCONT})
            if (::sigaction(signal, &action, nullptr) != 0)
                throw std::runtime_error("cannot install job-control signal handler");
    }
    const auto config = sizetree::loadConfig();
    if (!config.warning.empty())
        std::cerr << "sizetree: config: " << displayText(config.warning) << '\n';
    const bool useCache = cacheOverride.value_or(config.cacheEnabled);
    if (interrupted) return 128 + interrupted;
    DirectoryTree tree(path, threads, useCache, browser, config.refreshInterval);
    if (browser) tree.finishAndExit(interactive(tree, path));

    ListProgress progress;
    progress.update(tree);
    tree.start();
    while (!interrupted && !tree.waitForCompletion()) progress.update(tree);
    if (interrupted) {
        progress.clear();
        tree.finishAndExit(128 + interrupted);
    }
    progress.update(tree, true);
    tree.flushCache();
    std::unordered_set<NodeId> collapsed;
    const auto snapshot = tree.snapshot(collapsed);
    progress.clear();
    std::cout << "Directory: " << displayText(path.string()) << '\n';
    std::cout << "Total: " << humanSize(snapshot.bytes) << (snapshot.errors ? "!" : "") << "\n\n";
    for (const auto& row : snapshot.rows)
        std::cout << std::setw(12) << sizeText(row) << "  " << marker(row) << ' ' << row.name << '\n';
    if (!snapshot.failure.empty()) std::cerr << "sizetree: " << snapshot.failure << '\n';
    else if (snapshot.errors)
        std::cerr << "sizetree: " << snapshot.errors << " unreadable entries; sizes marked ! are partial\n";
    if (!snapshot.cacheWarning.empty()) std::cerr << "sizetree: cache unavailable: " << snapshot.cacheWarning << '\n';
    tree.finishAndExit(snapshot.errors || !std::cout ? 1 : 0);
}

} // namespace

int main(int argc, char** argv) {
    std::setlocale(LC_CTYPE, "");
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "sizetree: " << displayText(error.what()) << '\n';
        return 1;
    }
}
