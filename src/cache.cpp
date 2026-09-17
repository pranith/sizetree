#include "cache.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>

namespace sizetree {
namespace {

constexpr char magic[] = "FSZCACHE1";
constexpr std::size_t maxRecord = 256 * 1024 * 1024;
constexpr std::size_t maxQueued = 64 * 1024 * 1024;
constexpr std::size_t maxOverview = 16 * 1024 * 1024;

std::uint64_t checksum(const std::string& data) {
    std::uint64_t value = 14695981039346656037ULL;
    for (const unsigned char byte : data) { value ^= byte; value *= 1099511628211ULL; }
    return value;
}

void number(std::string& output, std::uint64_t value, unsigned width = 8) {
    for (unsigned i = 0; i < width; ++i) output += static_cast<char>((value >> (8 * i)) & 255);
}

void string(std::string& output, const std::string& value) {
    if (value.size() > 1024 * 1024) throw std::runtime_error("cache pathname is too long");
    number(output, value.size(), 4);
    output += value;
}

struct Reader {
    const std::string& data;
    std::size_t position = 0;

    std::uint64_t number(unsigned width = 8) {
        if (width > data.size() - position) throw std::runtime_error("truncated cache record");
        std::uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i)
            value |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[position++])) << (8 * i);
        return value;
    }

    std::string string() {
        const auto length = number(4);
        if (length > 1024 * 1024 || length > data.size() - position)
            throw std::runtime_error("invalid cache string");
        std::string value = data.substr(position, static_cast<std::size_t>(length));
        position += static_cast<std::size_t>(length);
        return value;
    }
};

bool validName(const std::string& name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == std::string::npos && name.find('\0') == std::string::npos;
}

bool validPath(const std::string& path) {
    if (path.empty()) return true;
    std::size_t position = 0;
    while (position < path.size()) {
        const auto slash = path.find('/', position);
        if (!validName(path.substr(position, slash - position))) return false;
        if (slash == std::string::npos) return true;
        position = slash + 1;
    }
    return false;
}

std::string encode(const CachedDirectory& directory) {
    std::string payload;
    string(payload, directory.path);
    number(payload, directory.ownErrors);
    if (directory.children.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("too many entries for a cache record");
    number(payload, directory.children.size(), 4);
    for (const auto& child : directory.children) {
        number(payload, static_cast<unsigned>(child.kind), 1);
        string(payload, child.name);
        number(payload, child.bytes);
        number(payload, child.errors);
        if (payload.size() > maxRecord) throw std::runtime_error("directory exceeds the cache record limit");
    }
    std::string frame;
    number(frame, payload.size());
    number(frame, checksum(payload));
    frame += payload;
    return frame;
}

CachedDirectory decode(const std::string& payload, const std::atomic<bool>* cancelled = nullptr) {
    Reader input{payload};
    CachedDirectory directory;
    directory.path = input.string();
    if (!validPath(directory.path)) throw std::runtime_error("invalid cached directory path");
    directory.ownErrors = input.number();
    const auto count = input.number(4);
    if (count > (payload.size() - input.position) / 22)
        throw std::runtime_error("invalid cached entry count");
    directory.children.reserve(static_cast<std::size_t>(count));
    std::unordered_set<std::string> names;
    for (std::uint64_t i = 0; i < count; ++i) {
        if (cancelled && cancelled->load(std::memory_order_relaxed)) return directory;
        const auto kind = input.number(1);
        auto name = input.string();
        if (kind > static_cast<unsigned>(EntryKind::Unknown) || !validName(name) || !names.insert(name).second)
            throw std::runtime_error("invalid cached entry");
        const auto bytes = input.number();
        const auto errors = input.number();
        directory.children.push_back({std::move(name), static_cast<EntryKind>(kind), bytes, errors});
    }
    if (input.position != payload.size()) throw std::runtime_error("invalid cache record length");
    return directory;
}

bool read(std::FILE* file, std::string& output, std::size_t size) {
    output.resize(size);
    return std::fread(output.data(), 1, size, file) == size;
}

void write(std::FILE* file, const std::string& data) {
    if (std::fwrite(data.data(), 1, data.size(), file) != data.size())
        throw std::runtime_error("cannot write cache: " + std::string(std::strerror(errno)));
}

void flushFile(std::FILE* file) {
    if (std::fflush(file) != 0)
        throw std::runtime_error("cannot flush cache: " + std::string(std::strerror(errno)));
}

} // namespace

DiskCache::DiskCache(std::filesystem::path root, bool enabled)
    : root_(std::move(root)), enabled_(enabled), buffer_(1024 * 1024) {}

DiskCache::~DiskCache() { close(); }

void DiskCache::cancel() {
    cancelled_.store(true, std::memory_order_relaxed);
    changed_.notify_all();
}

void DiskCache::close() {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        stopping_ = true;
    }
    changed_.notify_all();
    if (writer_.joinable()) writer_.join();
    if (file_) { std::fclose(file_); file_ = nullptr; }
    if (lock_ >= 0) { ::close(lock_); lock_ = -1; }
    writable_.store(false, std::memory_order_relaxed);
}

void DiskCache::resolvePath() {
    if (!path_.empty()) return;
    std::filesystem::path base;
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    if (xdg && std::filesystem::path(xdg).is_absolute()) base = xdg;
    else if (home && *home) base = std::filesystem::path(home) / ".cache";
    else throw std::runtime_error("HOME and XDG_CACHE_HOME are unavailable");
    std::ostringstream name;
    name << std::hex << std::setw(16) << std::setfill('0') << checksum(root_.string()) << ".cache";
    path_ = base / "sizetree" / name.str();
}

std::optional<CacheOverview> DiskCache::loadOverview() {
    if (!enabled_ || cancelled_.load(std::memory_order_relaxed)) return {};
    try {
        resolvePath();
        const int descriptor = ::open((path_.string() + ".overview").c_str(),
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (descriptor < 0) return {};
        struct stat status{};
        if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
            status.st_size < 16 || status.st_size > static_cast<off_t>(maxOverview)) {
            ::close(descriptor);
            return {};
        }
        std::unique_ptr<std::FILE, decltype(&std::fclose)> input(::fdopen(descriptor, "rb"), std::fclose);
        if (!input) { ::close(descriptor); return {}; }
        std::string frame;
        if (!read(input.get(), frame, static_cast<std::size_t>(status.st_size))) return {};
        Reader prefix{frame};
        const auto length = prefix.number(), digest = prefix.number();
        const auto payload = frame.substr(16);
        if (length != payload.size() || digest != checksum(payload)) return {};
        Reader fields{payload};
        if (fields.string() != "SIZETREE-OVERVIEW-1" || fields.string() != root_.string()) return {};
        CacheOverview overview;
        overview.bytes = fields.number();
        overview.entries = fields.number();
        overview.errors = fields.number();
        const auto listingSize = fields.number();
        if (listingSize > payload.size() - fields.position) return {};
        overview.listing = decode(payload.substr(fields.position, static_cast<std::size_t>(listingSize)), &cancelled_);
        fields.position += static_cast<std::size_t>(listingSize);
        if (!overview.listing.path.empty() || cancelled_.load(std::memory_order_relaxed)) return {};
        for (auto& child : overview.listing.children) child.entries = fields.number();
        if (fields.position != payload.size()) return {};
        return overview;
    } catch (const std::exception&) {
        return {}; // A missing or damaged overview falls back to the full journal.
    }
}

void DiskCache::saveOverview(CacheOverview overview) {
    if (!writable()) return;
    try {
        // Avoid a second enormous cache for an unusually wide scan root.
        std::size_t estimate = 128;
        for (const auto& child : overview.listing.children) {
            estimate += child.name.size() + 40;
            if (estimate > maxOverview) return;
        }
        const auto listing = encode(overview.listing);
        std::string payload;
        string(payload, "SIZETREE-OVERVIEW-1");
        string(payload, root_.string());
        number(payload, overview.bytes);
        number(payload, overview.entries);
        number(payload, overview.errors);
        number(payload, listing.size() - 16);
        payload.append(listing, 16, std::string::npos);
        for (const auto& child : overview.listing.children) number(payload, child.entries);
        std::string frame;
        number(frame, payload.size());
        number(frame, checksum(payload));
        frame += payload;
        if (frame.size() > maxOverview) return;
        std::lock_guard<std::mutex> guard(mutex_);
        if (!stopping_ && frame != overview_) {
            overview_ = frame;
            pendingOverview_ = std::move(frame);
        }
    } catch (const std::exception&) {
        // The overview is optional; keep writing the authoritative journal.
    }
}

void DiskCache::writeOverview(const std::string& contents) {
    std::string temporary = path_.string() + ".overview.tmp.XXXXXX";
    const int descriptor = ::mkstemp(temporary.data());
    if (descriptor < 0) return;
    std::FILE* output = ::fdopen(descriptor, "wb");
    if (!output) { ::close(descriptor); ::unlink(temporary.c_str()); return; }
    const bool written = std::fwrite(contents.data(), 1, contents.size(), output) == contents.size();
    const bool closed = std::fclose(output) == 0;
    if (!written || !closed || ::rename(temporary.c_str(), (path_.string() + ".overview").c_str()) != 0)
        ::unlink(temporary.c_str());
}

CachedDirectories DiskCache::load() {
    CachedDirectories directories;
    if (!enabled_ || cancelled_.load(std::memory_order_relaxed)) return directories;
    try {
        resolvePath();
        std::filesystem::create_directories(path_.parent_path());
        if (::chmod(path_.parent_path().c_str(), 0700) != 0) throw std::runtime_error("cannot protect cache directory");
        lock_ = ::open((path_.string() + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lock_ < 0) throw std::runtime_error("cannot open cache lock: " + std::string(std::strerror(errno)));
        const bool owner = ::flock(lock_, LOCK_EX | LOCK_NB) == 0;
        if (!owner && errno != EWOULDBLOCK && errno != EAGAIN)
            throw std::runtime_error("cannot lock cache: " + std::string(std::strerror(errno)));
        const int descriptor = ::open(path_.c_str(), (owner ? O_RDWR | O_CREAT : O_RDONLY) |
            O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        if (descriptor < 0) {
            if (!owner && errno == ENOENT) return directories;
            throw std::runtime_error("cannot open cache: " + std::string(std::strerror(errno)));
        }
        struct stat status{};
        if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
            ::close(descriptor);
            throw std::runtime_error("cache is not a regular file");
        }
        file_ = ::fdopen(descriptor, owner ? "r+b" : "rb");
        if (!file_) { ::close(descriptor); throw std::runtime_error("cannot open cache stream"); }
        std::setvbuf(file_, buffer_.data(), _IOFBF, buffer_.size());
        header_ = magic;
        string(header_, root_.string()); // Verify the path as well as its filename hash.
        std::string existing;
        const auto size = static_cast<std::uint64_t>(status.st_size);
        const bool headerValid = size >= header_.size() && read(file_, existing, header_.size()) && existing == header_;
        end_ = headerValid ? header_.size() : 0;
        if (headerValid) {
            while (end_ + 16 <= size) {
                if (cancelled_.load(std::memory_order_relaxed)) return directories;
                std::string prefix, payload;
                if (!read(file_, prefix, 16)) break;
                Reader fields{prefix};
                const auto length = fields.number();
                const auto expected = fields.number();
                if (length > maxRecord || length > size - end_ - 16 ||
                    !read(file_, payload, static_cast<std::size_t>(length)) || checksum(payload) != expected) break;
                CachedDirectory directory;
                try { directory = decode(payload, &cancelled_); }
                catch (const std::exception&) { break; }
                if (cancelled_.load(std::memory_order_relaxed)) return directories;
                const std::string key = directory.path;
                auto previous = latest_.find(key);
                if (previous != latest_.end()) liveBytes_ -= previous->second.size;
                latest_[key] = {end_, length + 16, expected};
                liveBytes_ += length + 16;
                directories[key] = std::move(directory);
                end_ += length + 16;
            }
        }
        if (owner && !cancelled_.load(std::memory_order_relaxed)) {
            if ((end_ != size && ::ftruncate(descriptor, static_cast<off_t>(end_)) != 0) || ::fseeko(file_, 0, SEEK_END) != 0)
                throw std::runtime_error("cannot repair cache tail");
            if (!headerValid) { write(file_, header_); end_ = header_.size(); flushFile(file_); }
            // Entries removed from a parent listing need not survive compaction.
            std::unordered_set<std::string> reachable;
            std::vector<std::string> todo{std::string{}};
            while (!todo.empty()) {
                if (cancelled_.load(std::memory_order_relaxed)) return directories;
                auto key = std::move(todo.back());
                todo.pop_back();
                const auto found = directories.find(key);
                if (found == directories.end() || !reachable.insert(key).second) continue;
                for (const auto& child : found->second.children)
                    if (child.kind == EntryKind::Directory)
                        todo.push_back(key.empty() ? child.name : key + "/" + child.name);
            }
            for (auto record = latest_.begin(); record != latest_.end();) {
                if (!reachable.count(record->first)) {
                    liveBytes_ -= record->second.size;
                    record = latest_.erase(record);
                } else ++record;
            }
            writable_.store(true, std::memory_order_relaxed);
            writer_ = std::thread([this] { writeLoop(); });
        }
    } catch (const std::exception& error) {
        fail(error.what());
    }
    return directories;
}

void DiskCache::save(CachedDirectory directory) {
    if (!writable() || cancelled_.load(std::memory_order_relaxed)) return;
    try {
        Pending item{directory.path, encode(directory)};
        std::unique_lock<std::mutex> guard(mutex_);
        changed_.wait(guard, [&] { return !writable() || cancelled_.load(std::memory_order_relaxed) ||
            pending_.empty() || queuedBytes_ + item.frame.size() <= maxQueued; });
        if (!writable() || cancelled_.load(std::memory_order_relaxed)) return;
        queuedBytes_ += item.frame.size();
        pending_.push_back(std::move(item));
        guard.unlock();
        changed_.notify_all();
    } catch (const std::exception& error) {
        fail(error.what());
    }
}

void DiskCache::flush() {
    std::unique_lock<std::mutex> guard(mutex_);
    if (!writable()) return;
    flushRequested_ = true;
    changed_.notify_all();
    changed_.wait(guard, [this] { return !flushRequested_ || !writable(); });
}

std::string DiskCache::warning() {
    std::lock_guard<std::mutex> guard(mutex_);
    return warning_;
}

void DiskCache::fail(const std::string& message) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        warning_ = message;
        writable_.store(false, std::memory_order_relaxed);
        pending_.clear();
        queuedBytes_ = 0;
        flushRequested_ = false;
    }
    changed_.notify_all();
}

void DiskCache::writeLoop() {
    try {
        auto lastFlush = std::chrono::steady_clock::now();
        while (writable()) {
            Pending item;
            bool stop, flush;
            {
                std::unique_lock<std::mutex> guard(mutex_);
                changed_.wait_for(guard, std::chrono::milliseconds(250), [this] {
                    return stopping_ || !writable() || flushRequested_ || !pending_.empty();
                });
                if (!writable()) break;
                if (!pending_.empty()) {
                    item = std::move(pending_.front());
                    pending_.pop_front();
                    queuedBytes_ -= item.frame.size();
                }
                stop = stopping_ && pending_.empty() && item.frame.empty();
                flush = (flushRequested_ || stop) && pending_.empty();
            }
            changed_.notify_all();
            if (!item.frame.empty()) {
                Reader prefix{item.frame};
                prefix.number();
                const auto digest = prefix.number();
                const auto previous = latest_.find(item.key);
                if (previous == latest_.end() || previous->second.size != item.frame.size() || previous->second.digest != digest) {
                    write(file_, item.frame);
                    if (previous != latest_.end()) liveBytes_ -= previous->second.size;
                    latest_[item.key] = {end_, item.frame.size(), digest};
                    liveBytes_ += item.frame.size();
                    end_ += item.frame.size();
                }
            }
            if (!cancelled_.load(std::memory_order_relaxed) &&
                end_ > header_.size() + 2 * liveBytes_ + 1024 * 1024) compact();
            const auto now = std::chrono::steady_clock::now();
            if (flush || now - lastFlush >= std::chrono::seconds(1)) {
                flushFile(file_);
                std::string overview;
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    overview.swap(pendingOverview_);
                }
                if (!overview.empty()) writeOverview(overview);
                lastFlush = now;
            }
            if (flush) {
                std::lock_guard<std::mutex> guard(mutex_);
                if (pending_.empty()) flushRequested_ = false;
                changed_.notify_all();
            }
            if (stop) break;
        }
        flushFile(file_);
    } catch (const std::exception& error) {
        fail(error.what());
    }
}

void DiskCache::compact() {
    flushFile(file_);
    std::vector<char> inputBuffer(1024 * 1024), outputBuffer(1024 * 1024);
    std::unique_ptr<std::FILE, decltype(&std::fclose)> input(std::fopen(path_.c_str(), "rb"), std::fclose);
    if (!input) throw std::runtime_error("cannot read cache for compaction");
    std::setvbuf(input.get(), inputBuffer.data(), _IOFBF, inputBuffer.size());
    if (::fseeko(input.get(), static_cast<off_t>(header_.size()), SEEK_SET) != 0)
        throw std::runtime_error("cannot read cache for compaction");
    std::string temporary = path_.string() + ".tmp.XXXXXX";
    const int descriptor = ::mkstemp(temporary.data());
    if (descriptor < 0) throw std::runtime_error("cannot create cache checkpoint");
    std::FILE* output = ::fdopen(descriptor, "w+b");
    if (!output) { ::close(descriptor); ::unlink(temporary.c_str()); throw std::runtime_error("cannot open cache checkpoint"); }
    std::setvbuf(output, outputBuffer.data(), _IOFBF, outputBuffer.size());
    try {
        write(output, header_);
        std::vector<std::pair<Location*, std::uint64_t>> relocated;
        std::uint64_t offset = header_.size(), newEnd = header_.size();
        while (offset < end_) {
            if (cancelled_.load(std::memory_order_relaxed)) {
                std::fclose(output);
                ::unlink(temporary.c_str());
                return;
            }
            std::string prefix, payload;
            if (!read(input.get(), prefix, 16)) throw std::runtime_error("truncated cache during compaction");
            Reader fields{prefix};
            const auto length = fields.number();
            if (length > maxRecord || length > end_ - offset - 16 ||
                !read(input.get(), payload, static_cast<std::size_t>(length)))
                throw std::runtime_error("invalid cache during compaction");
            Reader record{payload};
            const auto key = record.string();
            const auto current = latest_.find(key);
            if (current != latest_.end() && current->second.offset == offset) {
                write(output, prefix);
                write(output, payload);
                relocated.emplace_back(&current->second, newEnd);
                newEnd += length + 16;
            }
            offset += length + 16;
        }
        flushFile(output);
        if (::rename(temporary.c_str(), path_.c_str()) != 0) throw std::runtime_error("cannot replace cache checkpoint");
        for (const auto& item : relocated) item.first->offset = item.second;
        std::fclose(file_);
        file_ = output;
        output = nullptr;
        buffer_ = std::move(outputBuffer);
        end_ = newEnd;
    } catch (...) {
        if (output) std::fclose(output);
        ::unlink(temporary.c_str());
        throw;
    }
}

} // namespace sizetree
