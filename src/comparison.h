#pragma once
#include "document.h"

struct ViewOptions {
    bool diff = false, split = false, changesOnly = false;
    bool operator==(const ViewOptions &) const = default;
};
struct WorkspaceSnapshot {
    std::vector<Document> files, patches, rendered;
    qsizetype added = 0, removed = 0, changedFiles = 0;
    QString status;
    ViewOptions options;
};
WorkspaceSnapshot loadWorkspace(const QString &root, const std::atomic_bool &cancel,
                                const WorkspaceSnapshot &previous, ViewOptions options,
                                std::atomic_int *progress);
WorkspaceSnapshot renderWorkspace(WorkspaceSnapshot snapshot, ViewOptions options, const std::atomic_bool &cancel);
