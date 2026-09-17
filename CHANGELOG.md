# Changelog

## 0.1.0

Initial public release.

- Interactive file and recursive folder sizes, with arrow navigation, Tab expansion, and Escape to collapse and select the parent.
- Case-insensitive fuzzy file search with `/`, space-separated terms that all match in any order, Tab to cycle matches, Enter to open files through `xdg-open`, and `o` (or Ctrl+O while searching) to open a selected file in the terminal editor.
- Background scanning with elapsed time, manual subtree refresh, and configurable periodic refresh.
- Persistent caching and configuration in XDG directories, enabled by default.
- Linux scanning optimizations for large trees and NFS, with portable fallbacks.
- Plain `--list` output with progress on terminal stderr.
- Terminal restoration on exit and Ctrl+Z / `fg` support.
- Normal and static Linux builds and version reporting.
- MIT license.
