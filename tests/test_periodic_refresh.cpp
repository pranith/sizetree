// Exercise the real tree and scanner with explicit monotonic deadlines rather
// than making the tests wait for each configured refresh period.
#include <fstream>
#define main sizetree_cli_main
#include "../src/main.cpp"
#undef main

namespace {

using namespace std::chrono_literals;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        auto pattern = (fs::temp_directory_path() / "sizetree-refresh-tests.XXXXXX").string();
        const char* created = ::mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("cannot create test directory");
        path = created;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void writeFile(const fs::path& path, const std::string& contents) {
    std::ofstream output(path);
    output << contents;
    check(static_cast<bool>(output), "cannot write test file");
}

void waitForScan(DirectoryTree& tree) {
    const auto deadline = DirectoryTree::Clock::now() + 5s;
    while (!tree.waitForCompletion())
        check(DirectoryTree::Clock::now() < deadline, "scan did not complete");
}

Row findRow(const Snapshot& snapshot, const std::string& name) {
    for (const auto& row : snapshot.rows)
        if (row.name == name) return row;
    throw std::runtime_error("missing row: " + name);
}

void testMetadataSizeAndType() {
    TemporaryDirectory temporary;
    const auto payload = temporary.path / "payload";
    writeFile(payload, "abc");
    fs::create_directory(temporary.path / "directory");
    fs::create_symlink("payload", temporary.path / "link");
    check(::mkfifo((temporary.path / "pipe").c_str(), 0600) == 0, "cannot create test FIFO");
    std::atomic<unsigned> handles{0};
    DirectoryHandle directory(handles, AT_FDCWD, temporary.path.c_str());
    check(directory.descriptor() >= 0, "cannot open metadata test directory");
    mode_t mode{};
    std::uintmax_t bytes{};
    check(fileMetadata(directory.descriptor(), "payload", mode, bytes) && S_ISREG(mode) && bytes == 3,
          "metadata did not return the file size");
    const auto parentTime = fs::last_write_time(temporary.path);
    constexpr std::uintmax_t largeSize = 5ULL * 1024 * 1024 * 1024;
    fs::resize_file(payload, largeSize);
    check(fs::last_write_time(temporary.path) == parentTime, "test changed the parent timestamp");
    check(fileMetadata(directory.descriptor(), "payload", mode, bytes) && bytes == largeSize,
          "metadata missed file growth or truncated a 64-bit size");
    check(fileMetadata(directory.descriptor(), "directory", mode, bytes) && S_ISDIR(mode),
          "metadata did not identify a directory");
    check(fileMetadata(directory.descriptor(), "link", mode, bytes) && S_ISLNK(mode),
          "metadata followed a symbolic link");
    check(fileMetadata(directory.descriptor(), "pipe", mode, bytes) && S_ISFIFO(mode),
          "metadata did not identify a special file");
    check(!fileMetadata(directory.descriptor(), "missing", mode, bytes), "metadata accepted a missing file");
}

void testScanElapsedTime() {
    TemporaryDirectory temporary;
    fs::create_directory(temporary.path / "alpha");
    writeFile(temporary.path / "alpha" / "payload", "abc");
    DirectoryTree tree(temporary.path, 1, false, true, 1s);
    std::unordered_set<NodeId> expanded;
    check(tree.snapshot(expanded).elapsed == DirectoryTree::Clock::duration::zero(),
          "timer ran before the scan started");
    tree.start();
    const auto running = tree.snapshot(expanded).elapsed;
    std::this_thread::sleep_for(10ms);
    check(tree.snapshot(expanded).elapsed > running, "active scan timer did not advance");
    const auto deadline = DirectoryTree::Clock::now() + 5s;
    while (!tree.waitForCompletion()) {
        check(DirectoryTree::Clock::now() < deadline, "deferred scan did not complete");
        tree.start(); // Start validation once asynchronous initialization is ready.
    }
    const auto completed = tree.snapshot(expanded);
    std::this_thread::sleep_for(10ms);
    check(tree.snapshot(expanded).elapsed == completed.elapsed, "completed scan timer kept advancing");

    const auto manualStart = DirectoryTree::Clock::now();
    tree.refresh(findRow(completed, "alpha").node);
    waitForScan(tree);
    check(tree.snapshot(expanded).elapsed <= DirectoryTree::Clock::now() - manualStart,
          "manual refresh included previous scan or idle time");

    const auto periodicStart = DirectoryTree::Clock::now();
    check(tree.refreshIfDue(periodicStart + 2s), "periodic refresh did not start");
    waitForScan(tree);
    check(tree.snapshot(expanded).elapsed <= DirectoryTree::Clock::now() - periodicStart,
          "periodic refresh did not reset the scan timer");
}

void testPeriodicRefresh() {
    TemporaryDirectory temporary;
    const auto root = temporary.path / "scan";
    fs::create_directories(root / "alpha");
    writeFile(root / "alpha" / "payload", "abc");
    writeFile(root / "loose", "xy");
    check(::setenv("XDG_CACHE_HOME", (temporary.path / "cache").c_str(), 1) == 0,
          "cannot isolate cache");
    {
        DirectoryTree warm(root, 1, true);
        warm.start();
        waitForScan(warm);
        warm.flushCache();
    }

    const auto configHome = temporary.path / "config";
    fs::create_directories(configHome / "sizetree");
    check(::setenv("XDG_CONFIG_HOME", configHome.c_str(), 1) == 0, "cannot isolate config");
    writeFile(configHome / "sizetree" / "config", "cache_enabled=true\nrefresh_interval_seconds=300\n");
    const auto config = sizetree::loadConfig();
    check(config.warning.empty() && config.refreshInterval == 5min, "custom refresh period was not loaded");
    DirectoryTree tree(root, 1, config.cacheEnabled, true, config.refreshInterval);
    std::unordered_set<NodeId> expanded;
    check(!tree.refreshIfDue(DirectoryTree::Clock::now() + 1h), "refreshed before initialization");
    tree.start();
    const auto loadDeadline = DirectoryTree::Clock::now() + 5s;
    Snapshot snapshot;
    do {
        snapshot = tree.snapshot(expanded);
        check(DirectoryTree::Clock::now() < loadDeadline, "cache did not load");
        if (snapshot.loading) std::this_thread::sleep_for(1ms);
    } while (snapshot.loading);

    const auto alpha = findRow(snapshot, "alpha").node;
    expanded.insert(alpha);
    tree.refresh(alpha);
    check(findRow(tree.snapshot(expanded), "alpha").refreshQueued, "manual refresh was not queued");
    check(!tree.refreshIfDue(DirectoryTree::Clock::now() + 1h), "periodic refresh overtook queued work");
    tree.start(); // The browser has displayed the cache; allow validation now.
    waitForScan(tree);
    check(tree.snapshot(expanded).bytes == 5, "incorrect initial size");

    const auto idle = DirectoryTree::Clock::now();
    writeFile(root / "alpha" / "payload", "12345678");
    fs::remove(root / "loose");
    writeFile(root / "added", "1234");
    fs::create_directory(root / "beta");
    writeFile(root / "beta" / "new", "12");
    check(!tree.refreshIfDue(idle + 4min), "periodic refresh ran too early");
    check(tree.snapshot(expanded).bytes == 5, "filesystem changed before periodic refresh");
    check(tree.refreshIfDue(idle + 6min), "periodic refresh did not start when due");
    waitForScan(tree);
    snapshot = tree.snapshot(expanded);
    check(snapshot.bytes == 14 && snapshot.entries == 5, "whole-tree changes were not discovered");
    check(findRow(snapshot, "alpha").node == alpha && findRow(snapshot, "alpha").expanded,
          "periodic refresh lost selection or expansion");
    check(findRow(snapshot, "payload").bytes == 8, "nested size was not refreshed");
    check(findRow(snapshot, "beta").bytes == 2, "new top-level folder was not refreshed");
    check(!tree.refreshIfDue(idle + 6min), "automatic refresh was duplicated");
    check(!tree.refreshIfDue(idle + 11min - 1ms), "timer was not rearmed for five minutes");
    writeFile(root / "added", "12345");
    check(tree.refreshIfDue(idle + 11min), "next periodic refresh missed its deadline");
    waitForScan(tree);
    check(tree.snapshot(expanded).bytes == 15, "later periodic refresh did not update sizes");
    tree.flushCache();
}

void testEmptyDirectoryWithoutCache() {
    TemporaryDirectory temporary;
    DirectoryTree tree(temporary.path, 1, false);
    std::unordered_set<NodeId> expanded;
    tree.start();
    waitForScan(tree);
    check(tree.snapshot(expanded).rows.empty(), "test directory is not empty");
    writeFile(temporary.path / "discovered", "1234567");
    const auto idle = DirectoryTree::Clock::now();
    check(!tree.refreshIfDue(idle + 23h), "default refresh ran before one day");
    check(tree.refreshIfDue(idle + 25h), "empty directory did not refresh after one day");
    waitForScan(tree);
    const auto snapshot = tree.snapshot(expanded);
    check(snapshot.bytes == 7 && snapshot.entries == 1, "new entry in empty directory was missed");
}

void testDisabledAutomaticRefreshStillAllowsManualRefresh() {
    TemporaryDirectory temporary;
    const auto configHome = temporary.path / "config";
    fs::create_directories(configHome / "sizetree");
    check(::setenv("XDG_CONFIG_HOME", configHome.c_str(), 1) == 0, "cannot isolate config");
    writeFile(configHome / "sizetree" / "config", "cache_enabled=false\nrefresh_interval_seconds=0\n");
    const auto config = sizetree::loadConfig();
    check(config.warning.empty() && !config.cacheEnabled && config.refreshInterval == 0s,
          "disabled refresh period was not loaded");
    const auto root = temporary.path / "scan";
    fs::create_directories(root / "alpha");
    writeFile(root / "alpha" / "payload", "a");
    DirectoryTree tree(root, 1, config.cacheEnabled, false, config.refreshInterval);
    std::unordered_set<NodeId> expanded;
    tree.start();
    waitForScan(tree);
    const auto alpha = findRow(tree.snapshot(expanded), "alpha").node;
    writeFile(root / "alpha" / "payload", "ab");
    check(!tree.refreshIfDue(DirectoryTree::Clock::now() + 24h * 365), "disabled automatic refresh started");
    check(tree.snapshot(expanded).bytes == 1, "disabled automatic refresh changed sizes");
    tree.refresh(alpha);
    waitForScan(tree);
    check(tree.snapshot(expanded).bytes == 2, "disabling automatic refresh also disabled manual refresh");
}

void testFuzzyMatching() {
    sizetree::FuzzyMatcher matcher("MAIN");
    check(matcher.score("main.cpp").has_value(), "fuzzy matching was case sensitive");
    check(*matcher.score("main.cpp") > *matcher.score("m_a_i_n.cpp"), "adjacent matches were not preferred");
    check(!matcher.score("minimal.cc"), "fuzzy matching accepted letters in the wrong order");
    matcher.reset({});
    check(!matcher.score("anything"), "empty search matched every file");
    matcher.reset(" \t\r\n\v\f ");
    check(matcher.empty() && !matcher.score("anything"), "whitespace-only search matched every file");

    for (const auto* path : {"EBM/LoadStore.cpp", "LoadStore/ebm_notes.txt", "ebm_loadstore.txt"}) {
        matcher.reset("ebm loadstore");
        const auto expected = matcher.score(path);
        check(expected.has_value(), "search did not require terms independently across a path");
        for (const auto* query : {"loadstore ebm", "EBM LOADSTORE", "LoAdStOrE EbM", "  ebm   loadstore  ",
                                  "\tloadstore\n\r ebm\f"}) {
            matcher.reset(query);
            check(matcher.score(path) == expected, "term order, case, or whitespace changed the ranking");
        }
        matcher.reset("Bm LDst");
        check(matcher.score(path).has_value(), "individual search terms were not fuzzy");
    }
    matcher.reset("ebm loadstore");
    check(!matcher.score("EBM/compute.cpp"), "search accepted a file missing the second term");
    check(!matcher.score("misc/LoadStore.cpp"), "search accepted a file missing the first term");
    check(!matcher.score("EBM/storeload.cpp"), "search ignored letter order within a term");
    check(!matcher.score(""), "search accepted an empty filename");
    matcher.reset("main MAIN");
    check(matcher.score("main.cpp").has_value(), "repeated terms required separate occurrences");
}

void testFuzzySearchAndRefresh() {
    TemporaryDirectory temporary;
    fs::create_directories(temporary.path / "alpha" / "nested");
    fs::create_directory(temporary.path / "NeedleFolder");
    check(::mkfifo((temporary.path / "Needle.pipe").c_str(), 0600) == 0, "cannot create search FIFO");
    const auto original = temporary.path / "alpha" / "nested" / "Needle.cpp";
    writeFile(original, "abc");
    for (int i = 0; i < 1200; ++i) writeFile(temporary.path / ("noise-" + std::to_string(i)), "x");
    DirectoryTree tree(temporary.path, 2, false);
    tree.start();
    waitForScan(tree);
    SearchCursor cursor;
    std::vector<SearchEntry> batch;
    tree.searchBatch(cursor, batch);
    check(batch.size() <= 512 && !cursor.done, "search did not split a large tree into batches");

    FuzzySearch search;
    search.active = true;
    search.setQuery("   ");
    search.step(tree);
    check(search.empty() && !search.busy() && !search.current(), "blank query started a search");
    search.setQuery("NEEDLE");
    auto deadline = DirectoryTree::Clock::now() + 5s;
    do {
        search.step(tree);
        check(DirectoryTree::Clock::now() < deadline, "fuzzy search did not finish");
    } while (search.busy());
    check(search.count() == 1, "search missed a hidden descendant or included directories/special files");
    const auto match = *search.current();
    check(match.path == "alpha/nested/Needle.cpp", "search did not preserve the relative path");
    check(tree.filePath(match.parent, match.node) == original, "search resolved the wrong file");
    std::unordered_set<NodeId> expanded, automatic;
    std::vector<NodeId> selection;
    check(tree.reveal(match, expanded, automatic, selection), "could not reveal search result");
    auto snapshot = tree.snapshot(expanded);
    check(findRow(snapshot, "Needle.cpp").node == selection.front(), "search did not select the nested file");
    check(snapshot.done, "search started a filesystem scan");

    fs::rename(original, original.parent_path() / "NewNeedle.cpp");
    tree.refresh(findRow(snapshot, "alpha").node);
    waitForScan(tree);
    check(!tree.filePath(match.parent, match.node), "a stale search result resolved to a replacement file");
    check(!tree.reveal(match, expanded, automatic, selection), "a stale search result was revealed");
    deadline = DirectoryTree::Clock::now() + 5s;
    do {
        search.step(tree);
        check(DirectoryTree::Clock::now() < deadline, "search did not follow a refreshed tree");
        std::this_thread::sleep_for(1ms);
    } while (search.busy() || !search.current() || search.current()->path != "alpha/nested/NewNeedle.cpp");
    check(search.count() == 1, "search retained a deleted result after refresh");
}

} // namespace

int main() {
    try {
        testMetadataSizeAndType();
        testScanElapsedTime();
        testPeriodicRefresh();
        testEmptyDirectoryWithoutCache();
        testDisabledAutomaticRefreshStillAllowsManualRefresh();
        testFuzzyMatching();
        testFuzzySearchAndRefresh();
        std::cout << "Periodic refresh checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
