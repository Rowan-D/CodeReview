#include "document.h"
#include <QDir>
#include <QFile>
#include <QStringDecoder>
#include <algorithm>
#include <QProcess>
#include <QTemporaryDir>
#include <QSet>
#include <QHash>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <sys/stat.h>
#include <memory>
#include <filesystem>

namespace {
void buildOverviewLevels(Document &d) {
    int width = 100, height = int(d.overview.size() / 100);
    QByteArray pixels = d.overview;
    QByteArray colors = d.overviewColor;
    while (width > 1 || height > 1) {
        const int nextWidth = (width + 1) / 2, nextHeight = (height + 1) / 2;
        QByteArray next(nextWidth * nextHeight, char(255));
        for (int y = 0; y < nextHeight; ++y)
            for (int x = 0; x < nextWidth; ++x) {
                int total = 0, count = 0, darkest = 255;
                for (int dy = 0; dy < 2 && y * 2 + dy < height; ++dy)
                    for (int dx = 0; dx < 2 && x * 2 + dx < width; ++dx) {
                        const int value = uchar(pixels[(y * 2 + dy) * width + x * 2 + dx]);
                        total += value; ++count; darkest = std::min(darkest, value);
                    }
                // Area coverage avoids the solid blocks produced by min pooling.
                // A modest contrast floor keeps sparse text present at extreme LOD.
                next[y * nextWidth + x] = char(darkest < 255 ? std::min(210, (3 * total / count + darkest) / 4) : 255);
            }
        d.overviewLevels.push_back(next);
        if (!colors.isEmpty()) {
            QByteArray reduced(nextWidth * nextHeight * 4, char(255));
            const auto *source = reinterpret_cast<const uint32_t *>(colors.constData());
            auto *target = reinterpret_cast<uint32_t *>(reduced.data());
            for (int y = 0; y < nextHeight; ++y) for (int x = 0; x < nextWidth; ++x) {
                int r = 0, g = 0, b = 0, count = 0;
                for (int dy = 0; dy < 2 && y * 2 + dy < height; ++dy)
                    for (int dx = 0; dx < 2 && x * 2 + dx < width; ++dx) {
                        const auto color = source[(y * 2 + dy) * width + x * 2 + dx];
                        if (color == 0xffffffff) continue;
                        r += (color >> 16) & 255; g += (color >> 8) & 255; b += color & 255; ++count;
                    }
                if (count) target[y * nextWidth + x] = 0xff000000 | (uint32_t(r / count) << 16) | (uint32_t(g / count) << 8) | uint32_t(b / count);
            }
            d.overviewColorLevels.push_back(reduced);
            colors = std::move(reduced);
        }
        pixels = std::move(next); width = nextWidth; height = nextHeight;
    }
}
}

qsizetype Document::rowCount() const {
    return binary ? std::max<qsizetype>(1, (bytes.size() + 15) / 16) : qsizetype(rows.size());
}
QString Document::rowText(qsizetype row) const {
    if (!binary) return text.mid(rows[row].start, rows[row].length);
    const auto chunk = bytes.mid(row * 16, 16);
    QString ascii;
    for (unsigned char c : chunk) ascii += c >= 32 && c < 127 ? QChar(c) : QChar('.');
    return QString::fromLatin1(chunk.toHex(' ')).leftJustified(48) + "  " + ascii;
}
QString Document::rowNumber(qsizetype row) const {
    if (binary) return QString::number(row + 1);
    if (diff) {
        const auto &r = rows[row];
        auto number = [](qsizetype value) { return value <= 0 ? QString() : QString::number(value); };
        if (comparisonSide) return number(comparisonSide < 0 ? r.oldLine : r.newLine);
        return number(r.oldLine).rightJustified(lineDigits) + "  " + number(r.newLine).rightJustified(lineDigits);
    }
    return rows[row].line ? QString::number(rows[row].line) : QString();
}
Document decode(QString path, const QByteArray &bytes, const QByteArray &hash, bool diff) {
    Document d;
    d.path = std::move(path);
    d.diff = diff;
    if (diff) d.patchBytes = bytes;
    d.contentHash = hash.isEmpty() ? QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) : hash;
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString input = decoder(bytes);
    if (decoder.hasError() || input.contains(QChar(0))) {
        d.binary = true;
        d.columns = 66;
        d.bytes = bytes;
        d.note = "Binary / non-UTF-8 · hex + ASCII · 16 bytes per row";
        const qsizetype height = std::min<qsizetype>(2048, d.rowCount() * 2);
        d.overview = QByteArray(height * 100, char(255));
        for (qsizetype y = 0; y < height; y += 2) {
            const auto text = d.rowText(y * d.rowCount() / height);
            for (int x = 0; x < text.size(); ++x)
                if (!text[x].isSpace()) d.overview[y * 100 + x] = 55;
        }
        buildOverviewLevels(d);
        return d;
    }
    d.text.reserve(input.size());
    qsizetype start = 0, line = 1, label = 1;
    int column = 0;
    auto flush = [&] {
        d.rows.push_back({start, d.text.size() - start, label});
        start = d.text.size(); column = 0; label = 0;
    };
    for (qsizetype i = 0; i < input.size(); ++i) {
        QChar ch = input[i];
        if (ch == '\r' || ch == '\n') {
            if (ch == '\r' && i + 1 < input.size() && input[i+1] == '\n') ++i;
            flush();
            d.text += '\n'; start = d.text.size();
            label = ++line;
        } else {
            int count = ch == '\t' ? 4 - column % 4 : 1;
            while (count--) {
                if (column == 100) flush();
                d.text += ch == '\t' ? QChar(' ') : (ch.isPrint() ? ch : QChar(0xfffd));
                // Keep UTF-16 surrogate pairs together at wrap boundaries.
                if (ch.isHighSurrogate() && i+1 < input.size() && input[i+1].isLowSurrogate()) {
                    d.text.back() = ch;
                    d.text += input[++i];
                }
                ++column;
                if (!ch.isSpace()) d.columns = std::max(d.columns, ch.unicode() > 127 ? 100 : column);
            }
        }
    }
    flush();
    if (diff) {
        static const QRegularExpression hunk("^@@ -(\\d+)(?:,\\d+)? \\+(\\d+)(?:,\\d+)? @@");
        qsizetype oldLine = 0, newLine = 0;
        bool inHunk = false;
        char change = 0;
        for (auto &row : d.rows) {
            if (row.line) {
                const QString text = d.text.mid(row.start, row.length);
                const auto match = hunk.match(text);
                change = 0;
                if (text.startsWith("diff --git ")) inHunk = false;
                if (match.hasMatch()) {
                    oldLine = match.captured(1).toLongLong(); newLine = match.captured(2).toLongLong();
                    inHunk = true; change = '@';
                } else if (inHunk && !text.isEmpty()) {
                    change = text[0].toLatin1();
                    if (change == ' ' || change == '-') row.oldLine = oldLine++;
                    if (change == ' ' || change == '+') row.newLine = newLine++;
                }
            } else {
                if (change == ' ' || change == '-') row.oldLine = 0;
                if (change == ' ' || change == '+') row.newLine = 0;
            }
            row.change = change;
            const auto ink = change == '+' ? Ink::Added : change == '-' ? Ink::Removed : change == '@' ? Ink::Hunk : Ink::Plain;
            if (row.length) d.syntax.push_back({row.start, row.length, ink});
        }
        d.lineDigits = QString::number(std::max(oldLine, newLine)).size();
    } else {
        const auto tokens = highlight(d.path, d.text);
        d.syntax = QList<SyntaxSpan>(tokens.begin(), tokens.end());
    }
    rebuildTextOverview(d);
    return d;
}
void rebuildTextOverview(Document &d) {
    d.overviewLevels.clear(); d.overviewColorLevels.clear(); d.overviewColor.clear();
    // Bounded, character-occupancy minimap, built on the loading thread.
    // Aggregate ink when a file has more rows than the cache can represent.
    const qsizetype height = std::min<qsizetype>(2048, d.rowCount() * 2);
    d.overview = QByteArray(height * 100, char(255));
    if (d.diff || !d.syntax.empty()) d.overviewColor = QByteArray(height * 100 * 4, char(255));
    auto *colorPixels = d.overviewColor.isEmpty() ? nullptr : reinterpret_cast<uint32_t *>(d.overviewColor.data());
    size_t token = 0;
    for (qsizetype r = 0; r < d.rowCount(); ++r) {
        const auto &span = d.rows[r];
        const qsizetype y = r * height / d.rowCount();
        int col = 0;
        for (qsizetype i = span.start; i < span.start + span.length && col < 100; ++i, ++col) {
            if (!d.text[i].isSpace()) {
                d.overview[y * 100 + col] = 55;
                if (colorPixels) {
                    while (token < d.syntax.size() && d.syntax[token].start + d.syntax[token].length <= i) ++token;
                    const auto ink = span.change == '+' ? Ink::Added : span.change == '-' ? Ink::Removed : token < d.syntax.size() && d.syntax[token].start <= i ? d.syntax[token].ink : Ink::Plain;
                    colorPixels[y * 100 + col] = inkColor(ink);
                }
            }
            if (d.text[i].isHighSurrogate() && i + 1 < span.start + span.length) ++i;
        }
    }
    buildOverviewLevels(d);
}
QByteArray documentStamp(const QString &path) {
    struct stat st{};
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0) return {};
    QByteArray stamp = QByteArray::number(st.st_size) + ':' + QByteArray::number(st.st_ino);
#if defined(__linux__)
    stamp += ':' + QByteArray::number(st.st_mtim.tv_sec) + ':' + QByteArray::number(st.st_mtim.tv_nsec)
           + ':' + QByteArray::number(st.st_ctim.tv_sec) + ':' + QByteArray::number(st.st_ctim.tv_nsec);
#else
    stamp += ':' + QByteArray::number(st.st_mtime) + ':' + QByteArray::number(st.st_ctime);
#endif
    return stamp;
}
std::vector<Document> loadDirectory(const QString &root, const std::atomic_bool &cancel,
                                  const std::vector<Document> &previous, std::atomic_int *progress) {
    if (progress) *progress = -1; // Git enumeration has no known total yet.
    if (cancel) return {};
    auto git = [&](const QStringList &args, QByteArray &output) {
        QProcess process;
        process.setWorkingDirectory(root);
        process.start("git", args, QIODevice::ReadOnly);
        if (!process.waitForStarted(3000)) return false;
        QElapsedTimer elapsed; elapsed.start();
        while (!process.waitForFinished(50)) {
            if (cancel || elapsed.elapsed() > 30000) { process.kill(); process.waitForFinished(); return false; }
        }
        output = process.readAllStandardOutput();
        return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    };
    QByteArray output;
    QStringList prefix;
    std::unique_ptr<QTemporaryDir> temporaryIndex;
    if (!git({"rev-parse", "--show-toplevel"}, output)) {
        // Use Git's own ignore engine for ordinary directories too. The temporary
        // index lives outside the viewed directory and never changes its files.
        temporaryIndex = std::make_unique<QTemporaryDir>();
        if (!temporaryIndex->isValid() || !git({"init", "--bare", "--quiet", temporaryIndex->path()}, output))
            return {decode("[directory status]", "Git is required to enumerate files and apply ignore rules.")};
        prefix = {"--git-dir=" + temporaryIndex->path(), "--work-tree=" + root};
    }
    const QStringList base = {"ls-files", "-z", "--cached", "--others", "--exclude-standard"};
    if (!git(prefix + base + QStringList{"--", "."}, output)) return previous;
    const auto candidates = output.split('\0');
    if (!git(prefix + base + QStringList{"--ignored", "--", "."}, output)) return previous;
    QSet<QByteArray> ignored;
    for (const auto &path : output.split('\0')) ignored.insert(path);
    QHash<QString, const Document *> cache;
    for (const auto &doc : previous) cache.insert(doc.path, &doc);
    QSet<QString> seen;
    std::vector<Document> docs;
    QDir baseDir(root);
    int visited = 0;
    for (const auto &name : candidates) {
        if (progress) *progress = int(100.0 * visited++ / std::max<qsizetype>(1, candidates.size()));
        if (cancel) return {};
        if (name.isEmpty() || ignored.contains(name)) continue;
        const QString path = QFile::decodeName(name);
        if (path.split('/').contains(".git") || seen.contains(path)) continue;
        seen.insert(path);
        const QString absolute = baseDir.filePath(path);
        const QFileInfo info(absolute);
        // Gitlinks/submodules and nested repositories are directory entries;
        // ls-files never recurses through them. Deleted tracked files are absent.
        if (info.isDir() && !info.isSymLink()) continue;
        if (!info.exists() && !info.isSymLink()) continue;
        const auto stamp = documentStamp(absolute);
        const auto old = cache.value(path, nullptr);
        if (old && !stamp.isEmpty() && old->stamp == stamp) { docs.push_back(*old); continue; }
        Document d;
        if (info.isSymLink()) {
            d = decode(path, ("Symbolic link → " + info.symLinkTarget()).toUtf8());
            d.note = "Symbolic link (not followed)";
            std::error_code error;
            const auto target = std::filesystem::read_symlink(QFile::encodeName(absolute).constData(), error);
            if (!error) d.linkTarget = QFile::decodeName(QByteArray::fromStdString(target.string()));
        } else if (info.isFile()) {
            QFile file(absolute);
            if (!file.open(QIODevice::ReadOnly)) {
                d = decode(path, ("Cannot read: " + file.errorString()).toUtf8());
            } else {
                const auto bytes = file.readAll();
                if (file.error() != QFileDevice::NoError) {
                    d = decode(path, ("Read failed: " + file.errorString()).toUtf8());
                } else if (documentStamp(absolute) != stamp) {
                    // A writer was active during the read. Keep the last coherent
                    // snapshot and try again next scan, avoiding partial saves.
                    if (old) docs.push_back(*old);
                    continue;
                } else {
                    const auto hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
                    d = old && old->contentHash == hash ? *old : decode(path, bytes, hash);
                }
            }
        } else continue;
        d.stamp = stamp;
        docs.push_back(std::move(d));
    }
    std::sort(docs.begin(), docs.end(), [](const auto &a, const auto &b) { return a.path < b.path; });
    if (progress) *progress = 100;
    return docs;
}
