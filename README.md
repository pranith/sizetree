# sizetree

A C++17 terminal browser for the sizes of files and folders in the current directory. Directories start collapsed as `[+]`; expand them to see their contents and recursive sizes. With caching enabled, previous results are saved on disk and shown first, while parallel background scans check for changes.

Requires Linux or macOS, a C++17 compiler with `std::filesystem` support, and CMake 3.20 or newer. Python 3.6 or newer is needed for the CLI tests. No external terminal library is needed.

## Build and run

```sh
git clone https://github.com/pranith/sizetree.git
cd sizetree
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/sizetree
```

Alternatively, run `make` for the regular build, or `make static` for a fully statically linked Linux executable:

```sh
make static
./build-static/sizetree
ctest --test-dir build-static --output-on-failure
```

Static builds require static C and C++ runtime libraries for the selected compiler. Use `make static STATIC_CXX=/path/to/g++` to select another compiler; additional CMake configuration options can be passed through `CMAKE_ARGS`. CMake retains these settings in `build-static` for subsequent builds. The equivalent CMake option is `-DSIZETREE_STATIC=ON`.

Run `make clean` to remove both `build/` and `build-static/`. Custom directories set with `BUILD_DIR` and `STATIC_BUILD_DIR` are also honored.

On NFS, cleaning a binary that is still running can leave a busy `.nfs*` file. Close the instance first (`fg`, then `q` if suspended), then rerun `make clean`. Use `lsof -- FILE` or `fuser -v FILE` on the host running the process to find which program holds the file. Cleanup reports failure and a diagnostic while these files remain.

Run from any directory to inspect that directory, or supply a path:

```sh
./build/sizetree /path/to/folder
```

Optional installation: `cmake --install build --prefix "$HOME/.local"` installs the program to `~/.local/bin/sizetree`, along with its license and documentation. Ensure `~/.local/bin` is on your `PATH`, then run `sizetree` from any directory. Use `sizetree --version` to report the installed version.

The scanner defaults to 2–8 workers based on CPU count. It scans separate directories concurrently to overlap metadata requests, which helps on network filesystems such as NFS. Each regular file needs one metadata lookup; known symlinks and special files need none. Initial results are published in batches. Cached folder listings are replaced once their immediate entries have been checked; their subfolders keep their cached sizes until checked in turn. Only directory entries retain full paths to reduce memory use.

On Linux, directory entries are read in 64 KiB batches with `getdents64`, avoiding the extra directory metadata check performed by `fdopendir`. File metadata uses `statx` requesting only type and size, so NFS need not validate unused timestamps or flush writes to obtain them. Normal metadata synchronization still applies; unsupported `statx` calls fall back to `fstatat`. Other platforms use `readdir` and `fstatat`.

For Linux NFS roots, the scanner advises the kernel to discard each directory's cached listing before reading it. This encourages bulk retrieval of names and attributes (`READDIRPLUS` on NFSv3), preventing an old listing from causing a separate server request for every file whose attributes have expired. The hint applies to directory listings only and is ignored if unsupported. Its benefit depends on the NFS client, server, and cache state; an already-warm metadata cache can be faster without a new server listing.

Queued subdirectories reuse open parent handles when available, avoiding repeated resolution of deep paths. Retained handles are bounded according to the process's file descriptor limit, with a maximum budget of 64; when that budget is occupied, the scanner uses full paths. These optimizations keep the existing worker count and size-validation behavior.

Use `-j N` or `--threads N` to choose between 1 and 64 workers. For example:

```sh
./build/sizetree -j 8 /path/to/folder
./build/sizetree -j 1 /path/to/folder
```

The best worker count depends on storage latency and directory structure. A single flat directory is scanned by one worker; separate subdirectories can use all workers. Exact totals still require visiting every directory and checking every regular file's metadata. File contents are never read.

## Controls

| Key | Action |
| --- | --- |
| Up / Down (or k / j) | Move selection up / down through files and directories |
| Tab (or Space) | Expand or collapse the selected directory |
| Enter | Open the selected file with `xdg-open`, or expand/collapse a directory |
| / | Start fuzzy file search throughout the scanned tree |
| r | Refresh the selected directory, including all descendants |
| Right (or l) | Expand the selected directory |
| Left (or h) | Collapse the directory, or select its parent |
| Escape | Collapse the selected directory and select its parent if visible; on a file, select its parent |
| Home / End | Select the first / last visible entry |
| Page Up / Ctrl+B | Move up one screen |
| Page Down / Ctrl+F | Move down one screen |
| Ctrl+Z | Suspend; run `fg` in the shell to resume |
| q / Ctrl+C | Quit |

Example after expanding `src`:

```text
sizetree | /path/to/project
Total: 14.0 KiB | Scanned 6 entries in 0 min 1 sec

          SIZE  NAME
>      9.0 KiB  [-] src
       8.0 KiB        main.cpp
       1.0 KiB        helper.cpp
       4.0 KiB  [+] docs
       1.0 KiB      README.md
```

Directories come first at each level, then files; each group is sorted largest first, with names breaking ties. Selection stays on the same entry as sizes update. The screen scrolls with selection and adapts to terminal resizing.

Press `/` and type part of a filename or relative path to jump to a fuzzy match, including files inside collapsed folders. Separate terms with spaces to require **all terms**, in any order, anywhere in the relative path. For example, `ebm loadstore` and `LOADSTORE EBM` both match `EBM/LoadStore.cpp`. Each term is fuzzy: `scb` can match `secret.bin`. Matching always ignores letter case (non-ASCII letters follow the current locale) and favors filenames, adjacent letters, and word boundaries. Extra spaces are ignored. Files and symbolic links are searched; directories and special files are excluded. No `fzf` installation is needed.

In search mode, **Tab / Down** selects the next match, **Shift-Tab / Up** selects the previous match, **Backspace** deletes a character, and **Ctrl+U** clears the query. The match counter shows your position, and cycling wraps around. **Ctrl+F / Page Down** and **Ctrl+B / Page Up** move down or up one page of matches, stopping at the last or first match. **Enter** opens the selected file and returns to browsing; **Escape** leaves search with the selected file visible. Letters such as `q` and `r`, and `/`, are ordinary query text while searching; spaces separate terms.

Search uses entries already loaded in memory, works in small batches, and updates as scanning or refreshes discover changes. The selected match's parent folders expand automatically. Opening files requires `xdg-open` (usually provided by `xdg-utils`) and a working desktop session. The opener runs asynchronously, receives the filename as a single argument, and reports launch failures in the status line.

The status line shows elapsed time while scanning, for example `Scanning... 2841 entries | elapsed 0 min 12 sec`. Once all queued scans finish, it shows `Scanned 2841 entries in 0 min 15 sec` and keeps that duration fixed. A manual or automatic refresh started while idle resets the timer.

Ctrl+Z restores the shell's screen, cursor, and terminal settings before suspending the process. Run `fg` to restore the browser, keeping your selection and expanded folders. A job resumed with `bg` stays suspended until brought to the foreground.

Sizes sum logical regular-file bytes recursively, including hidden files. They are **not disk usage**: sparse files report their logical length, directory metadata is excluded, and each hard-link entry is counted. Units use powers of 1024 (KiB, MiB, GiB, etc.). Symlinks (`[@]`) are displayed but never traversed or counted; other special files (`[*]`) are also excluded. A directory explicitly supplied as the starting path may itself be a symlink.

`~` marks a size still being scanned or a cached size awaiting verification. `!` marks an incomplete result caused by an unreadable or vanished entry; the scan continues elsewhere. Once loaded, entries are kept in memory to support immediate expansion.

Select a folder and press `r` to rescan its contents in the background. This picks up added, modified, and deleted files, corrects ancestor totals, and clears errors that have been resolved. Previous sizes remain visible during refresh. The selected folder stays selected, and surviving nested folders retain their expanded state. Other folders retain their cached results. If the selected folder is already scanning, a refresh is queued after its current pass; repeated requests during that pass are combined.

While the browser is open, the entire starting directory tree refreshes automatically after the configured period with no scan work, which defaults to 24 hours. The timer starts when the initial scan finishes and restarts after manual or automatic refreshes finish. Automatic refreshes wait for all active scans and queued refreshes to complete, preserve folder selection and expansion, and update the disk cache when enabled. This also discovers new top-level entries and works with caching disabled. Plain `--list` runs still scan once and exit.

## Configuration

Caching is enabled by default, and automatic refresh defaults to once a day (24 hours). There is no first-run prompt. On startup, missing settings are saved automatically in `$XDG_CONFIG_HOME/sizetree/config`, or `~/.config/sizetree/config` when `XDG_CONFIG_HOME` is empty, unset, or relative. Existing values and comments are preserved. The config directory and new file are created with permissions `0700` and `0600` respectively.

Edit the file to change either setting, then restart the utility:

```ini
cache_enabled=true
refresh_interval_seconds=86400
```

Set `cache_enabled=false` to disable cache reads and writes. Set `refresh_interval_seconds` to a whole number of seconds: `300` for five minutes, `3600` for one hour, or `86400` for one day. Use `0` to disable automatic refresh; the `r` key still works. The supported range is 0 through 4294967295 seconds. Existing config files containing only `cache_enabled` automatically receive the default refresh interval.

`--cache` and `--no-cache` override the cache preference for a single run; if both are given, the last one wins. These overrides are not saved. Missing config settings are still added, and the configured refresh interval is used for interactive runs with either flag. Plain runs also create missing config settings and cache by default. Help and invalid arguments do not create a config file. Invalid settings or config access failures print a warning and use defaults for the affected settings; valid settings still apply, and malformed files are left intact.

## Persistent cache

The default cache location is `~/.cache/sizetree/`. An absolute `XDG_CACHE_HOME` overrides this with `$XDG_CACHE_HOME/sizetree/`. Each canonical starting directory has a separate `.cache` file, so opening the same directory through a symlink reuses its cache. Cache files are private to your user.

The browser opens immediately. When caching is enabled, a small `.cache.overview` file supplies the previous top-level sizes while the full cached hierarchy loads in the background. `loading details` indicates that this is still happening. You can navigate, toggle folders, queue a refresh, or quit during loading; selected and expanded folders are preserved when their contents become available. Existing caches need one load with the updated utility to create this overview. If it is missing or damaged, a loading screen remains responsive while the main cache is read.

Saved results are marked `Cached; checking...`. After loading, the scanner checks the filesystem in the background and updates the displayed totals and cache. Cached values may be stale until the checks finish. Plain `--list` output always waits for fresh results.

Completed directory listings are written incrementally by a background writer, with buffered writes flushed approximately every second and on normal exit. Identical listings are not rewritten, and superseded records are compacted automatically. An interrupted final record is discarded on the next run. Corrupt or unavailable caches do not prevent a fresh scan. Simultaneous instances can read the same cache; one instance owns its writer lock.

Quitting cancels cache loading and scanning, stops optional compaction, and flushes queued cache updates. The process then lets the OS reclaim the in-memory tree, avoiding millions of individual deallocations. An outstanding filesystem request or cache write can still delay exit on an unresponsive filesystem.

```sh
./build/sizetree --no-cache /path/to/folder  # Bypass cache reads and writes
./build/sizetree --cache /path/to/folder     # Enable cache for this run
```

Caching avoids waiting for a new full scan before seeing previous results. Accurate NFS updates still require checking file metadata: an existing file can grow without changing its parent directory's timestamp, and remote changes do not reliably generate local notifications. Cache updates happen during startup scans, `r` refreshes, and periodic refreshes; changes between scans are picked up on the next pass. Cache files can be removed while the utility is closed; the next scan recreates them.

## Plain output and checks

```sh
./build/sizetree --list         # Print top-level sizes and exit
./build/sizetree --help
./build/sizetree --version
ctest --test-dir build --output-on-failure
```

Piped input/output and `TERM=dumb` automatically use plain output. Filenames containing terminal control characters are escaped. Exit status is 0 on success, 1 for an error or partial scan, and 128 plus the signal number when interrupted.

While `--list` scans, a progress line on terminal stderr shows the current phase, entry count, estimated size (`~`), and elapsed time. It updates four times per second and clears before the final listing or on interruption. Cached totals remain estimates until validation finishes. Progress is also shown when plain output is selected automatically, including when stdout is piped or saved to a file. Redirecting stderr disables progress; stdout contains only the final listing.

Tests use Python 3.6 or newer's standard library, including a pseudo-terminal to verify keyboard navigation and terminal restoration. CMake registers them when Python 3 is available.

## Development

The version is set in `CMakeLists.txt`. See [CHANGELOG.md](CHANGELOG.md) for release notes. To build and test locally:

```sh
make test
```

Use version tags such as `v0.1.0` for releases, keeping each published tag on its original commit.

The GitHub Actions workflow builds and tests regular Linux, static Linux, and macOS configurations. To report a problem, include `sizetree --version`, your operating system, filesystem type, and steps to reproduce it.

## License

Licensed under the [MIT License](LICENSE).
