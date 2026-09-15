#include "comparison.h"
#include <QCryptographicHash>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace {
struct Line {
    QString text;
    qsizetype oldNumber = -1, newNumber = -1;
    char change = 0;
};
QString normalized(QString s) {
    if (s.endsWith('\r')) s.chop(1);
    QString result;
    for (auto c : s) {
        if (c == '\t') result += QString(4 - result.size() % 4, ' ');
        else result += c;
    }
    return result;
}
QList<QString> sourceLines(const Document *d) {
    if (!d || d->binary || d->text.isEmpty()) return {};
    if (!d->linkTarget.isNull()) return d->linkTarget.split('\n');
    auto lines = d->text.split('\n');
    if (d->text.endsWith('\n')) lines.removeLast();
    return lines;
}
// Merge Git's hunks with the untouched working-tree lines to recover full
// before/after source. Validate context so a concurrent save cannot misalign it.
bool fullLines(const Document *current, const Document *patch, std::vector<Line> &out) {
    const auto currentLines = sourceLines(current);
    qsizetype cursor = 0, oldNumber = 1, newNumber = 1;
    auto unchangedUntil = [&](qsizetype end) {
        while (cursor < end && cursor < currentLines.size())
            out.push_back({currentLines[cursor++], oldNumber++, newNumber++, ' '});
    };
    if (patch) {
        static const QRegularExpression hunk("^@@ -(\\d+)(?:,(\\d+))? \\+(\\d+)(?:,(\\d+))? @@");
        bool inHunk = false;
        const auto lines = QString::fromUtf8(patch->patchBytes).split('\n');
        for (const auto &raw : lines) {
            if (raw.startsWith("diff --git ")) inHunk = false;
            const auto match = hunk.match(raw);
            if (match.hasMatch()) {
                const auto target = match.captured(3).toLongLong();
                const bool zero = match.captured(4) == "0";
                const qsizetype start = std::max<qsizetype>(0, target - (zero ? 0 : 1));
                if (start < cursor || start > currentLines.size()) return false;
                unchangedUntil(start);
                oldNumber = match.captured(1).toLongLong() + (match.captured(2) == "0" ? 1 : 0);
                newNumber = target + (zero ? 1 : 0);
                inHunk = true;
            } else if (inHunk && !raw.isEmpty()) {
                const char kind = raw[0].toLatin1();
                if (kind != '+' && kind != '-' && kind != ' ') continue;
                const auto text = normalized(raw.mid(1));
                if (kind != '-') {
                    if (cursor >= currentLines.size() || currentLines[cursor] != text) return false;
                    ++cursor;
                }
                out.push_back({text, kind == '+' ? -1 : oldNumber++, kind == '-' ? -1 : newNumber++, kind});
            }
        }
    }
    unchangedUntil(currentLines.size());
    return true;
}
std::vector<Line> compact(const std::vector<Line> &lines) {
    std::vector<Line> result;
    std::vector<bool> keep(lines.size(), false);
    for (size_t i = 0; i < lines.size(); ++i) if (lines[i].change == '+' || lines[i].change == '-') {
        const auto first = i > 3 ? i - 3 : 0;
        for (size_t j = first; j < std::min(lines.size(), i + 4); ++j) keep[j] = true;
    }
    // Mode-only changes have no changed text; leave that small file readable.
    if (std::none_of(keep.begin(), keep.end(), [](bool v) { return v; })) return lines;
    for (size_t i = 0; i < lines.size();) {
        if (keep[i]) result.push_back(lines[i++]);
        else {
            const auto first = i;
            while (i < lines.size() && !keep[i]) ++i;
            result.push_back({QString("⋯ %1 unchanged lines").arg(i - first), -1, -1, '@'});
        }
    }
    return result;
}
Document makeDocument(const QString &path, const std::vector<Line> &lines, int side,
                      bool color, const QByteArray &key) {
    QString text;
    for (const auto &line : lines) { text += line.text; text += '\n'; }
    auto d = decode(path, text.toUtf8(), QCryptographicHash::hash(key, QCryptographicHash::Sha256));
    d.diff = true; d.comparisonSide = side; d.renderKey = key;
    if (!d.rows.empty()) d.rows.removeLast(); // synthetic final newline, not a source row
    qsizetype logical = 0;
    for (auto &row : d.rows) {
        if (row.line) logical = row.line - 1;
        if (logical >= qsizetype(lines.size())) continue;
        const auto &line = lines[logical];
        row.oldLine = line.oldNumber < 0 ? -1 : row.line ? line.oldNumber : 0;
        row.newLine = line.newNumber < 0 ? -1 : row.line ? line.newNumber : 0;
        row.change = color ? line.change : 0;
        d.lineDigits = std::max(d.lineDigits, int(QString::number(std::max(line.oldNumber, line.newNumber)).size()));
    }
    if (d.rows.empty()) d.rows.push_back({0, 0, 0});
    rebuildTextOverview(d);
    return d;
}
void alignWrappedRows(Document &before, Document &after, qsizetype logicalCount) {
    QList<Row> left, right;
    qsizetype a = 0, b = 0;
    for (qsizetype line = 1; line <= logicalCount; ++line) {
        const auto aStart = a, bStart = b;
        if (a < before.rows.size()) { ++a; while (a < before.rows.size() && !before.rows[a].line) ++a; }
        if (b < after.rows.size()) { ++b; while (b < after.rows.size() && !after.rows[b].line) ++b; }
        const auto count = std::max(a - aStart, b - bStart);
        for (qsizetype i = 0; i < count; ++i) {
            left.push_back(aStart + i < a ? before.rows[aStart + i] : Row{0, 0, 0});
            right.push_back(bStart + i < b ? after.rows[bStart + i] : Row{0, 0, 0});
        }
    }
    if (!left.empty()) { before.rows = std::move(left); after.rows = std::move(right); }
    rebuildTextOverview(before); rebuildTextOverview(after);
}
}
WorkspaceSnapshot renderWorkspace(WorkspaceSnapshot snapshot, ViewOptions options, const std::atomic_bool &cancel) {
    const auto previous = std::move(snapshot.rendered);
    snapshot.rendered.clear(); snapshot.options = options;
    snapshot.added = snapshot.removed = snapshot.changedFiles = 0; snapshot.status.clear();
    QHash<QString, const Document *> files, patches;
    QHash<QByteArray, const Document *> cached;
    for (const auto &d : previous) cached.insert(d.renderKey, &d);
    QSet<QString> paths;
    for (const auto &d : snapshot.files) { files.insert(d.path, &d); paths.insert(d.path); }
    for (const auto &d : snapshot.patches) {
        if (!d.diff) { snapshot.status = d.text.trimmed(); continue; }
        patches.insert(d.path, &d); paths.insert(d.path);
    }
    auto sorted = paths.values(); std::sort(sorted.begin(), sorted.end());
    for (const auto &path : sorted) {
        if (cancel) return {};
        const auto current = files.value(path, nullptr), patch = patches.value(path, nullptr);
        qsizetype added = 0, removed = 0;
        if (patch) for (const auto &r : patch->rows) if (r.line) {
            added += r.change == '+'; removed += r.change == '-';
        }
        snapshot.added += added; snapshot.removed += removed; snapshot.changedFiles += patch != nullptr;
        if (options.changesOnly && !patch) continue;
        const QByteArray baseKey = path.toUtf8() + '\0' + (current ? current->contentHash : QByteArray()) + '\0'
            + (patch ? patch->contentHash : QByteArray()) + QByteArray::number(options.diff) + QByteArray::number(options.split)
            + QByteArray::number(options.changesOnly);
        auto finish = [&](Document d, int side) {
            d.added = added; d.removed = removed; d.changed = patch != nullptr;
            d.comparisonSide = side;
            d.layoutColumns = options.changesOnly ? d.columns : std::max(current ? current->columns : 0, patch ? patch->columns : 0);
            d.layoutRows = options.changesOnly ? d.rowCount() : std::max(current ? current->rowCount() + removed : removed + 1, d.rowCount());
            d.renderKey = baseKey + QByteArray::number(side);
            snapshot.rendered.push_back(std::move(d));
        };
        const int firstSide = options.split ? -1 : 0;
        if (cached.contains(baseKey + QByteArray::number(firstSide))
            && (!options.split || cached.contains(baseKey + '1'))) {
            snapshot.rendered.push_back(*cached.value(baseKey + QByteArray::number(firstSide)));
            if (options.split) snapshot.rendered.push_back(*cached.value(baseKey + '1'));
            continue;
        }
        // Unchanged text shares the original buffers in both modes.
        if (!patch && current) {
            finish(*current, firstSide);
            if (options.split) finish(*current, 1);
            continue;
        }
        if (current && !options.diff && !options.split && !options.changesOnly) {
            finish(*current, 0); continue;
        }
        std::vector<Line> lines;
        const bool binary = (current && current->binary) || (patch &&
            (patch->text.contains("\nBinary files ") || patch->text.contains("\nBinary / non-UTF-8 file added")));
        if (binary && options.split) {
            // Binary bytes have no meaningful line alignment. Label summaries
            // explicitly instead of presenting current bytes as the HEAD copy.
            const QByteArray before = patch && patch->text.startsWith("New file\n")
                ? "Not present in HEAD.\n" : "Binary change against HEAD.\nBefore contents are not rendered as text.\n";
            finish(decode(path, before), -1);
            if (current && !current->binary) finish(*current, 1);
            else finish(decode(path, current ? "Binary working copy.\nContents are available in normal view.\n" : "File deleted.\n"), 1);
            continue;
        }
        if (binary || !fullLines(current, patch, lines)) {
            auto summary = patch ? *patch : *current;
            summary.note = binary ? "Binary change" : "File changed during comparison; retrying";
            if (!options.diff && current) summary = *current;
            finish(summary, firstSide);
            if (options.split) finish(summary, 1);
            continue;
        }
        if (options.changesOnly) lines = compact(lines);
        if (!options.split) {
            if (!options.diff) {
                if (current && !options.changesOnly) { finish(*current, 0); continue; }
                lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const auto &line) {
                    return current ? line.change == '-' : line.change == '+';
                }), lines.end());
            }
            finish(makeDocument(path, lines, options.diff ? 0 : current ? 1 : -1, options.diff, baseKey + '0'), 0);
        } else {
            std::vector<Line> left, right;
            for (size_t i = 0; i < lines.size();) {
                if (lines[i].change != '-' && lines[i].change != '+') {
                    left.push_back(lines[i]); right.push_back(lines[i]); ++i; continue;
                }
                std::vector<Line> removedLines, addedLines;
                while (i < lines.size() && (lines[i].change == '-' || lines[i].change == '+')) {
                    (lines[i].change == '-' ? removedLines : addedLines).push_back(lines[i]); ++i;
                }
                for (size_t n = 0; n < std::max(removedLines.size(), addedLines.size()); ++n) {
                    left.push_back(n < removedLines.size() ? removedLines[n] : Line{});
                    right.push_back(n < addedLines.size() ? addedLines[n] : Line{});
                }
            }
            auto before = makeDocument(path, left, -1, options.diff, baseKey + "-1");
            auto after = makeDocument(path, right, 1, options.diff, baseKey + '1');
            alignWrappedRows(before, after, left.size());
            finish(std::move(before), -1); finish(std::move(after), 1);
        }
    }
    return snapshot;
}
WorkspaceSnapshot loadWorkspace(const QString &root, const std::atomic_bool &cancel,
                                const WorkspaceSnapshot &previous, ViewOptions options, std::atomic_int *progress) {
    WorkspaceSnapshot next;
    next.files = loadDirectory(root, cancel, previous.files, progress);
    next.patches = loadGitDiff(root, cancel, previous.patches, progress);
    next.rendered = previous.rendered;
    return renderWorkspace(std::move(next), options, cancel);
}
