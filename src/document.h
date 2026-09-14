#pragma once
#include <QString>
#include <QByteArray>
#include <vector>
#include <atomic>
#include "syntax.h"
#include <QList>

struct Row { qsizetype start, length, line; qsizetype oldLine = -1, newLine = -1; char change = 0; };
struct Document {
    QString path, text, note, linkTarget;
    QByteArray bytes, overview;
    QByteArray stamp, contentHash, patchBytes, renderKey;
    QByteArray overviewColor;
    std::vector<QByteArray> overviewLevels;
    std::vector<QByteArray> overviewColorLevels;
    QList<Row> rows;
    QList<SyntaxSpan> syntax;
    bool binary = false;
    bool diff = false;
    int columns = 0;
    int lineDigits = 1;
    int comparisonSide = 0, layoutColumns = 0;
    qsizetype added = 0, removed = 0, layoutRows = 0;
    bool changed = false;
    qsizetype rowCount() const;
    QString rowText(qsizetype row) const;
    QString rowNumber(qsizetype row) const;
};
Document decode(QString path, const QByteArray &bytes, const QByteArray &hash = {}, bool diff = false);
QByteArray documentStamp(const QString &path);
std::vector<Document> loadGitDiff(const QString &root, const std::atomic_bool &cancel,
                               const std::vector<Document> &previous = {}, std::atomic_int *progress = nullptr);
std::vector<Document> loadDirectory(const QString &root, const std::atomic_bool &cancel,
                                  const std::vector<Document> &previous = {},
                                  std::atomic_int *progress = nullptr);

void rebuildTextOverview(Document &document);
