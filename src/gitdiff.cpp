#include "document.h"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QStringDecoder>
#include <algorithm>
#include <filesystem>

std::vector<Document> loadGitDiff(const QString &root, const std::atomic_bool &cancel,
                                 const std::vector<Document> &previous, std::atomic_int *progress) {
    if (progress) *progress = -1;
    auto git = [&](QStringList args, QByteArray &output) {
        if (cancel) return false;
        QProcess process;
        process.setWorkingDirectory(root);
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("GIT_OPTIONAL_LOCKS", "0");
        environment.insert("GIT_LITERAL_PATHSPECS", "1");
        process.setProcessEnvironment(environment);
        process.start("git", args, QIODevice::ReadOnly);
        if (!process.waitForStarted(3000)) return false;
        QElapsedTimer elapsed; elapsed.start();
        while (!process.waitForFinished(50)) {
            if (cancel || elapsed.elapsed() > 30000) { process.kill(); process.waitForFinished(); return false; }
        }
        output = process.readAllStandardOutput();
        return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    };
    auto error = [&](const char *message) {
        if (progress) *progress = 100;
        return std::vector<Document>{decode("[Git status]", message)};
    };
    QByteArray output;
    if (!git({"rev-parse", "--is-inside-work-tree"}, output) || output.trimmed() != "true")
        return error("Diff view requires a Git working tree.");
    if (!git({"ls-files", "-z", "--cached", "--others", "--ignored", "--exclude-standard", "--", "."}, output))
        return error("Unable to read Git ignore rules. Diff will retry automatically.");
    QSet<QByteArray> ignored;
    for (const auto &name : output.split('\0')) ignored.insert(name);
    const bool head = git({"rev-parse", "--verify", "HEAD"}, output);
    QByteArray patch;
    if (head && !git({"-c", "diff.suppressBlankEmpty=false", "diff", "--raw", "-z", "--patch",
                      "--no-color", "--no-ext-diff", "--no-textconv", "--no-renames", "--ignore-submodules=all",
                      "--relative", "--src-prefix=a/", "--dst-prefix=b/", "--unified=3",
                      "--output-indicator-new=+", "--output-indicator-old=-", "--output-indicator-context= ", "HEAD", "--", "."}, patch))
        return error("Unable to read the Git diff. Resolve Git errors and the view will retry automatically.");
    QStringList untracked = {"ls-files", "-z", "--others", "--exclude-standard"};
    if (!head) untracked << "--cached";
    if (!git(untracked + QStringList{"--", "."}, output)) return error("Unable to enumerate new files.");
    const auto names = output.split('\0');
    const QSet<QByteArray> newNames(names.begin(), names.end());
    QHash<QByteArray, QByteArray> replacementPatches;
    QHash<QString, const Document *> cache;
    for (const auto &doc : previous) cache.insert(doc.path, &doc);
    std::vector<Document> docs;
    auto append = [&](const QByteArray &name, const QByteArray &contents) {
        if (cancel || ignored.contains(name)) return;
        const QString path = QFile::decodeName(name);
        if (path.split('/').contains(".git")) return;
        const auto hash = QCryptographicHash::hash(contents, QCryptographicHash::Sha256);
        const auto old = cache.value(path, nullptr);
        if (old && old->diff && old->contentHash == hash) docs.push_back(*old);
        else docs.push_back(decode(path, contents, hash, true));
    };
    // --raw -z gives unambiguous names (including tabs/newlines); patch sections
    // follow in the same order. No parsing of quoted display filenames is needed.
    struct Entry { QByteArray name; bool gitlink; bool typeChange; };
    std::vector<Entry> entries;
    qsizetype position = 0;
    while (position < patch.size() && patch[position] == ':') {
        const auto metadataEnd = patch.indexOf('\0', position);
        if (metadataEnd < 0) return error("Cannot parse Git diff metadata.");
        const auto metadata = patch.mid(position, metadataEnd - position).split(' ');
        position = metadataEnd + 1;
        const auto nameEnd = patch.indexOf('\0', position);
        if (nameEnd < 0 || metadata.size() < 5) return error("Cannot parse Git diff filenames.");
        entries.push_back({patch.mid(position, nameEnd - position), metadata[0] == ":160000" || metadata[1] == "160000", metadata[4] == "T"});
        position = nameEnd + 1;
    }
    if (position < patch.size() && patch[position] == '\0') ++position;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (cancel) return {};
        if (!patch.mid(position, 11).startsWith("diff --git ")) return error("Cannot parse Git patch sections.");
        auto next = patch.indexOf("\ndiff --git ", position);
        // Git emits a deletion and an addition for file/symlink type changes.
        if (entries[i].typeChange && !entries[i].gitlink && next >= 0)
            next = patch.indexOf("\ndiff --git ", next + 1);
        const auto end = next < 0 ? patch.size() : next + 1;
        if (!entries[i].gitlink) {
            const auto section = patch.mid(position, end - position);
            // A staged deletion can have an untracked replacement at the same
            // path. Keep both changes in one column rather than duplicate leaves.
            if (newNames.contains(entries[i].name)) replacementPatches.insert(entries[i].name, section);
            else append(entries[i].name, section);
        }
        position = end;
        if (progress) *progress = int(70 * (i + 1) / std::max<size_t>(1, entries.size()));
    }
    // With no initial commit, every eligible current file is an addition.
    QSet<QByteArray> seen;
    int visited = 0;
    for (const auto &name : names) {
        if (cancel) return {};
        if (progress) *progress = 70 + int(30.0 * visited++ / std::max<qsizetype>(1, names.size()));
        if (name.isEmpty() || ignored.contains(name) || seen.contains(name)) continue;
        seen.insert(name);
        const QString absolute = QDir(root).filePath(QFile::decodeName(name));
        const QFileInfo info(absolute);
        if ((info.isDir() && !info.isSymLink()) || (!info.isFile() && !info.isSymLink())) continue;
        const auto sourceStamp = documentStamp(absolute);
        const auto replacement = replacementPatches.value(name);
        const auto stamp = sourceStamp.isEmpty() ? QByteArray() : sourceStamp + ':'
            + QCryptographicHash::hash(replacement, QCryptographicHash::Sha256);
        const auto old = cache.value(QFile::decodeName(name), nullptr);
        if (old && !stamp.isEmpty() && old->stamp == stamp) { docs.push_back(*old); continue; }
        QByteArray contents;
        if (info.isSymLink()) {
            std::error_code error;
            const auto target = std::filesystem::read_symlink(QFile::encodeName(absolute).constData(), error);
            if (error) { append(name, "Cannot read symbolic link target.\n"); continue; }
            contents = QByteArray::fromStdString(target.string());
        }
        else {
            QFile file(absolute);
            if (!file.open(QIODevice::ReadOnly)) { append(name, "Cannot read added file: " + file.errorString().toUtf8()); continue; }
            contents = file.readAll();
            if (file.error() != QFileDevice::NoError) { append(name, "Read failed: " + file.errorString().toUtf8()); continue; }
            if (documentStamp(absolute) != sourceStamp) {
                if (old) docs.push_back(*old);
                continue;
            }
        }
        QByteArray addition = replacement + "New file\n";
        QStringDecoder decoder(QStringDecoder::Utf8);
        const QString decoded = decoder(contents);
        if (decoded.contains(QChar(0)) || decoder.hasError()) addition += "Binary / non-UTF-8 file added (" + QByteArray::number(contents.size()) + " bytes)\n";
        else if (!contents.isEmpty()) {
            const auto count = contents.count('\n') + (contents.endsWith('\n') ? 0 : 1);
            addition += "@@ -0,0 +1," + QByteArray::number(count) + " @@\n";
            qsizetype start = 0;
            while (start < contents.size()) {
                const auto newline = contents.indexOf('\n', start);
                const auto end = newline < 0 ? contents.size() : newline;
                addition += '+'; addition += contents.mid(start, end - start); addition += '\n';
                if (newline < 0) addition += "\\ No newline at end of file\n";
                start = end + 1;
            }
        } else addition += "Empty file added\n";
        append(name, addition);
        if (!docs.empty() && docs.back().path == QFile::decodeName(name)) docs.back().stamp = stamp;
    }
    std::sort(docs.begin(), docs.end(), [](const auto &a, const auto &b) { return a.path < b.path; });
    if (progress) *progress = 100;
    return docs;
}
