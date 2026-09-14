# CodeReview

A native C++ / Qt 6 application for exploring source code and Git diffs on a read-only, zoomable canvas. Review tracking and editing are future work.

The application is 100% AI-generated. Instructions for AI are in [AGENTS.md](AGENTS.md).

## Build and Run

Requires a C++20 compiler, CMake, Qt 6 Widgets / Concurrent development packages, and Git on `PATH`. The executable also needs the Qt runtime libraries.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build -j
./build/code-review /path/to/directory
```

Omit the directory to open the current one. Paths containing spaces are supported.

Optional installation:

```sh
cmake --install build --prefix /your/install/path
```

## Controls

### View Toggles

The independent, checkable toggles are at the top right. Each button shows its shortcut.

| Toggle | Key | Behavior |
| --- | --- | --- |
| Diff | D | Highlight additions/deletions against HEAD without hiding unchanged files. |
| Split | S | Show aligned HEAD and working copies side by side, with highlighting on or off. |
| Changes only | C | Hide unchanged files and collapse distant unchanged lines, keeping three context lines around changes. |
| Tree | T | Switch between directory bars and branches with vertically stacked short files. |

Toggles preserve zoom and keep a nearby file anchored during layout changes. Press **F** to fit after switching layouts if needed.

### Navigation

| Input | Action |
| --- | --- |
| Left mouse drag | Pan |
| Middle click, then move | Accelerating two-axis autoscroll |
| Hold middle button and move | Autoscroll until release |
| Click / Escape / wheel / leave window | Cancel autoscroll |
| Wheel / two-finger trackpad scroll | Pan |
| Shift + wheel | Pan horizontally |
| Ctrl (or Command) + wheel / trackpad pinch | Zoom around a file/text position |
| Double-click | Return to readable 100% zoom |
| F | Fit the layout |
| F11 | Toggle fullscreen; restore the previous window state on exit |
| 0 | Reset to 100% |
| Home | Return to nearby file headers |
| Page Up / Page Down | Scroll 90% of a page |
| Ctrl+U / Ctrl+D | Scroll half a page up / down |
| + / - | Zoom around the viewport center |
| Arrow keys / edge scrollbars | Pan |
| Click binary file name | Expand / collapse when the dropdown is visible |

Zoom keeps the same file/text position through layout changes. Zooming over empty space brings nearby readable content into view. Panning is bounded, and vertical navigation follows visible file heights to avoid getting stranded below shorter files.

## Layouts

### Directory Bars

Directories form a flame chart. Each bar spans all descendant file columns; child directories sit immediately below their parent, and files begin beneath their containing directory. Files appear side by side in alphabetical tree order, with widths based on their content, up to 100 characters.

Horizontal positions and widths stay fixed in world coordinates and scale exactly with zoom. All bars have the same screen height at a given zoom: `clamp(28 + 6 × log2(zoom), 20, 44)` pixels.

### Tree

Directories and files form branches and leaves on the same 2D canvas. Short sibling files stack vertically; split comparison pairs stay side by side and stack as a unit.

Ancestor labels stay within the visible part of their subtree. When scrolling below them, they pin in depth order beneath the top controls so the directory path remains readable. Arrows indicate labels displaced from their original positions.

Directory labels expand into spare space beside neighboring directory labels, without moving files or branches. File columns keep their space, and neighboring labels share gaps to avoid overlap.

Horizontal packing stays fixed during zoom. Minimum header and branch spacing prevents overlapping stacks. Angled connectors meet above child labels, then run straight down within their own gutter or subtree. File headers sit just below this fan; their final connector scales from 4 to 48 screen pixels and continues inside the header toward the filename when space permits.

### Labels

- Directory labels use 12-pixel text, stay at the bar's visible left edge, and use less padding in narrow bars. A left chevron marks labels pinned to the viewport. Trailing directory slashes are optional when space is tight.
- File labels preserve the extension with a middle ellipsis when a useful prefix also fits. Tiny labels show the first characters without an ellipsis.
- Shortened labels fade at the box's visible right edge, darkening as more characters are hidden. Hard clipping uses the full available width, including partial final characters; useful file extensions still use middle elision.
- Saturated pastel colors distinguish neighboring directories and parents from children. There are no path tooltips.

## File Display

Text wraps at 100 monospaced characters without truncation. Unused column width is trimmed. A gutter shows source line numbers; wrapped continuations leave it blank. Tabs use four-column stops.

Binary and non-UTF-8 files start collapsed. Expanding them shows every byte as hex plus ASCII. The dropdown is disabled when there is too little room to show its control.

Symlinks display their targets without following them. Special files are not read, and read errors appear alongside their files.

### Syntax Highlighting

A lightweight lexical scanner supports C/C++, Python, JavaScript/TypeScript, Rust, Go, Java, shell, JSON, YAML, TOML, and CMake. Keywords, strings, comments, numbers, types, and function-like names retain their colors in live text, cached glyphs, and minimaps.

Multiline block comments and Python triple-quoted strings retain state across lines and visual wraps. Unknown file types stay plain. Highlighting is approximate: embedded languages, regex literals, and C++ raw strings are not fully parsed.

## Git Diffs

Diffs compare current files against `HEAD`, combining staged and unstaged changes. Eligible untracked files appear as additions. Before the first commit, all current files appear as additions. Renames appear as a deletion and an addition.

### Comparison Display

- Full source context stays visible unless **Changes only** is enabled.
- Deleted files retain their previous text, so toggling highlighting does not remove their columns.
- Inline comparisons show old/new line numbers. Split comparisons align source and wrapped lines, adding blank padding where needed.
- Syntax highlighting remains available. Red/green backgrounds mark changed code, and red/green markers identify the HEAD/working columns.
- Binary comparisons show summaries rather than treating bytes as source lines.

### Change Counts

Each file's additions and removals appear beneath its name. Split copies share one header and count; the header stays visible when panning to either copy.

Global totals stay at the top left and cover the opened directory and its eligible descendants. Each source line is counted once, regardless of split mode or filtering. Unchanged files show zero changes; binary and mode-only changes have no textual line count.

### File Selection

Git supplies tracked files and eligible untracked files. The following are excluded:

- Git-ignored files, including tracked files matching ignore rules.
- Repository metadata, submodules, and nested repositories.
- Empty directories.

Repository, global, and local exclude rules apply in every mode. Opening a subdirectory retains its parent repository's ignore rules. For ordinary directories, a temporary Git index outside the viewed directory applies ignore rules without modifying it. Git must be installed.

Git runs read-only, with optional index writes, external diff programs, and text converters disabled.

## Loading and Live Updates

The loading bar shows activity during Git enumeration, then file-count progress during reading and indexing. It disappears when loading finishes. Background refreshes show it only after 400 ms to avoid flashing during quick scans.

The view polls about once a second for edits, atomic replacements, additions, removals, renames, and ignore-rule changes. Git scans and comparison construction run in the background, including when HEAD advances.

Changed files are reread and indexed off the UI thread. Files modified during a read keep their previous snapshot until the next scan. Linux checks include nanosecond modification/change timestamps and inode identity. Camera position, zoom, binary expansion, and autoscroll survive updates.

Updates use polling, not instant filesystem notifications; long reads can delay a refresh.

## Performance and Limits

Only the selected comparison representation is built. Unchanged source, row indices, syntax spans, patches, derived comparisons, previews, and minimaps reuse cached storage.

### Text Rendering

| Zoom level | Rendering |
| --- | --- |
| Above 60% | Live glyphs |
| Intermediate | Cached glyph tiles |
| Farther out | Filtered colored minimaps |

Transitions blend across 40–60% and 12–24% zoom. Glyph tiles cover 64 rows, are generated only when visible, and use a 48 MiB LRU cache. Keys include file path and content hash, so editing one file does not invalidate other files' tiles.

Cached positions allow binary search of visible columns; offscreen subtrees and rows are skipped. Rendering is event-driven, and the autoscroll timer runs only while active.

### Memory

File contents stay in RAM without a file-size or line-count cutoff. Large directories can take substantial memory and loading time. Disk streaming is not implemented.

## Tests

Qt Test is required in addition to the build dependencies.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Coverage includes:

| Area | Checks |
| --- | --- |
| Text | Decoding, wrapping, syntax colors, and LOD/cache behavior |
| Layout | Directory containment, equal bar heights, sticky labels, proportional zoom widths, content-based widths, and short-file tree packing |
| Interaction | Loading progress, fullscreen, zoom anchoring, page navigation, and autoscroll |
| Git | Ignores/submodules, staged and unstaged changes, additions/deletions, binary summaries, unusual names, file type changes, and subdirectory scope |
| Comparisons | Independent toggles, exact per-file/global counts, full/collapsed context, and split-view wrap alignment |
| Live updates | Edits, additions, renames, deletions, and advancing HEAD |

Tests also exercise a 10,000-file scene containing a 100,000-line document. They use Qt's offscreen platform; physical trackpad behavior depends on desktop input support.

To run only the isolated label, clipping, and pinning checks without invoking Git:

```sh
QT_QPA_PLATFORM=offscreen ./build/viewer-tests --labels-only
```
