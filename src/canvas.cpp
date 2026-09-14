#include "canvas.h"
#include "labels.h"
#include <QPainter>
#include <QFontDatabase>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QNativeGestureEvent>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QSignalBlocker>
#include <QTextLayout>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <functional>

namespace {
double smooth(double low, double high, double value) {
    const double t = std::clamp((value - low) / (high - low), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}
}

Canvas::Canvas(const QString &directory) : root(directory) {
    setWindowTitle("CodeReview · Directory boxes — " + root);
    setMinimumSize(640, 360);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    scrollTimer.setInterval(16);
    scrollTimer.setTimerType(Qt::PreciseTimer);
    connect(&scrollTimer, &QTimer::timeout, this, &Canvas::tickAutoscroll);
    setAttribute(Qt::WA_OpaquePaintEvent);
    codeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    codeFont.setPixelSize(14);
    codeFont.setFixedPitch(true);
    codeFont.setStyleHint(QFont::TypeWriter);
    const QFontMetricsF metrics(codeFont);
    cell = metrics.horizontalAdvance('M');
    lineHeight = std::ceil(metrics.height() + 3);
    ascent = metrics.ascent();
    panelWidth = gutter + 100 * cell + 16;
    stride = panelWidth + 40;
    horizontal = new QScrollBar(Qt::Horizontal, this);
    vertical = new QScrollBar(Qt::Vertical, this);
    horizontal->setObjectName("horizontalScrollBar");
    vertical->setObjectName("verticalScrollBar");
    const QString style = "QScrollBar { background: #f5f5f5; border: none; width: 12px; height: 12px; }"
        "QScrollBar::handle { background: #bcbcbc; border-radius: 4px; min-width: 20px; min-height: 20px; }"
        "QScrollBar::handle:hover { background: #929292; }"
        "QScrollBar::handle:disabled { background: #eeeeee; }"
        "QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }"
        "QScrollBar::add-page, QScrollBar::sub-page { background: none; }";
    for (auto *bar : {horizontal, vertical}) bar->setStyleSheet(style);
    for (int axis = 0; axis < 2; ++axis) {
        auto *bar = axis ? vertical : horizontal;
        connect(bar, &QScrollBar::valueChanged, this, [this, axis](int value) {
            const double position = scrollLow[axis] + (scrollHigh[axis] - scrollLow[axis]) * value / 1000000.0;
            (axis ? offset.ry() : offset.rx()) = -position * scale;
            changed();
        });
    }
    auto button = [&](const QString &name, const QString &text, int option) {
        auto *b = new QToolButton(this);
        b->setObjectName(name); b->setText(text); b->setCheckable(true); b->setFocusPolicy(Qt::NoFocus);
        b->setStyleSheet("QToolButton { background: #f4f6f8; color: #304655; border: 1px solid #bbc8d3; border-radius: 4px; padding: 3px 7px; } QToolButton:checked { background: #8dc8f0; border-color: #548db4; }");
        connect(b, &QToolButton::clicked, this, [this, option] { toggleOption(option); });
        return b;
    };
    viewToggle = button("diffToggle", "Diff [D]", 0);
    splitToggle = button("splitToggle", "Split [S]", 1);
    changesToggle = button("changesToggle", "Changes only [C]", 2);
    treeToggle = button("treeToggle", "Tree [T]", 3);
    globalInfo = new QLabel(this);
    globalInfo->setObjectName("globalInfo"); globalInfo->setTextFormat(Qt::RichText);
    globalInfo->setStyleSheet("QLabel { color: #304655; background: white; }");
    updateControls();
    loadingBar = new QProgressBar(this);
    loadingBar->setObjectName("loadingProgress");
    loadingBar->setRange(0, 0);
    loadingBar->setStyleSheet("QProgressBar { border: none; background: #e9eef3; color: #304d68; text-align: center; } QProgressBar::chunk { background: #9bbad5; }");
    progressTimer.setInterval(50);
    connect(&progressTimer, &QTimer::timeout, this, [this] {
        if (!liveEnabled || !refreshInFlight) { loadingBar->hide(); progressTimer.stop(); return; }
        if (!loading && loadClock.elapsed() < 400) return;
        const int value = loadProgress.load();
        loadingBar->setRange(0, value < 0 ? 0 : 100);
        loadingBar->setValue(std::max(0, value));
        loadingBar->setFormat(diffView ? "Reading diff… %p%" : loading ? "Loading files… %p%" : "Updating files… %p%");
        loadingBar->show(); loadingBar->raise();
    });
    resize(1440, 900);
    connect(&watcher, &QFutureWatcher<WorkspaceSnapshot>::finished, this, [this] {
        auto result = watcher.future().takeResult();
        refreshInFlight = false; progressTimer.stop(); loadingBar->hide();
        if (liveEnabled && !cancelled) {
            if (result.options != ViewOptions{diffView, splitView, changesOnly}) {
                // Still retain the new disk/Git caches while another toggle is pending.
                workspace.files = std::move(result.files); workspace.patches = std::move(result.patches);
                startRefresh(); return;
            }
            workspace = std::move(result);
            applyDocuments(workspace.rendered, !loading);
            updateControls();
        }
    });
    refreshTimer.setInterval(1000);
    connect(&refreshTimer, &QTimer::timeout, this, &Canvas::startRefresh);
    refreshTimer.start();
    startRefresh();
}
Canvas::~Canvas() { refreshTimer.stop(); cancelled = true; watcher.waitForFinished(); }
void Canvas::startRefresh() {
    if (!liveEnabled || refreshInFlight) return;
    refreshInFlight = true; loadProgress = -1; loadClock.start(); progressTimer.start();
    requestedOptions = {diffView, splitView, changesOnly};
    auto snapshot = workspace;
    watcher.setFuture(QtConcurrent::run([this, options = requestedOptions, snapshot = std::move(snapshot)] {
        return loadWorkspace(root, cancelled, snapshot, options, &loadProgress);
    }));
}
void Canvas::toggleDiff() { toggleOption(0); }
void Canvas::toggleOption(int option) {
    stopAutoscroll();
    if (option == 0) diffView = !diffView;
    if (option == 1) splitView = !splitView;
    if (option == 2) changesOnly = !changesOnly;
    if (option == 3) {
        treeView = !treeView;
        layoutTree(); updateBounds(); changed();
    } else if (liveEnabled) startRefresh();
    else {
        auto snapshot = renderWorkspace(workspace, {diffView, splitView, changesOnly}, cancelled);
        workspace = std::move(snapshot); applyDocuments(workspace.rendered, true);
    }
    updateControls();
}
void Canvas::updateControls() {
    if (!viewToggle) return;
    viewToggle->setChecked(diffView); splitToggle->setChecked(splitView);
    changesToggle->setChecked(changesOnly); treeToggle->setChecked(treeView);
    const QString status = workspace.status.isEmpty() ? QString("%1 files · HEAD").arg(workspace.files.size()) : QString("No Git diff");
    globalInfo->setText(status + QString("  <span style='color:#167039'>+%1</span> <span style='color:#b42e38'>−%2</span>")
        .arg(workspace.added).arg(workspace.removed));
    setWindowTitle(QString("CodeReview · %1%2 — ").arg(treeView ? "Tree" : "Directories", diffView ? " · Diff against HEAD" : "") + root);
    positionControls();
}
void Canvas::positionControls() {
    int right = width() - 18;
    for (auto *button : {treeToggle, changesToggle, splitToggle, viewToggle}) if (button) {
        const int w = button->sizeHint().width();
        button->setGeometry(right - w, 3, w, 26); right -= w + 5;
    }
    if (globalInfo) globalInfo->setGeometry(8, 3, std::max(0, right - 12), 26);
}
QRect Canvas::viewport() const { return QRect(0, 0, width() - 12, height() - 12); }
void Canvas::resizeEvent(QResizeEvent *) {
    horizontal->setGeometry(0, height() - 12, width() - 12, 12);
    vertical->setGeometry(width() - 12, 0, 12, height() - 12);
    if (loadingBar) loadingBar->setGeometry(24, height() - 48, std::max(100, width() - 60), 22);
    positionControls();
    changed();
}
void Canvas::setDocuments(std::vector<Document> docs) {
    liveEnabled = false; refreshTimer.stop(); progressTimer.stop(); loadingBar->hide();
    workspace = {}; workspace.files = docs; workspace.rendered = docs;
    applyDocuments(std::move(docs), false); updateControls();
}
void Canvas::applyDocuments(std::vector<Document> docs, bool preserve) {
    bool same = preserve && docs.size() == documents.size();
    if (same) for (size_t i = 0; i < docs.size(); ++i)
        if (docs[i].path != documents[i].path || docs[i].contentHash != documents[i].contentHash || docs[i].renderKey != documents[i].renderKey) { same = false; break; }
    if (same) { documents = std::move(docs); return; }
    QHash<QString, bool> states;
    QString focusPath;
    int focusSide = 0;
    QPointF anchor;
    if (preserve && !leaves.empty()) {
        for (size_t i = 0; i < documents.size(); ++i) states.insert(documents[i].path, collapsed[i]);
        const auto slot = slotNear(viewport().center());
        focusPath = documents[nodes[leaves[slot]].document].path;
        focusSide = documents[nodes[leaves[slot]].document].comparisonSide;
        const auto r = leafRect(slot);
        // Preserve the screen position of the existing file, even while panning.
        anchor = offset + r.topLeft() * scale;
    }
    if (!preserve) textTiles.clear();
    documents = std::move(docs);
    loading = false;
    collapsed.clear();
    qsizetype maxRows = 1;
    for (const auto &d : documents) {
        maxRows = std::max(maxRows, d.rowCount());
        collapsed.push_back(d.binary && states.value(d.path, true));
    }
    qsizetype numberWidth = QString::number(maxRows).size();
    for (const auto &d : documents) if (d.diff) numberWidth = std::max(numberWidth, qsizetype(2 * d.lineDigits + 2));
    if (liveEnabled) {
        // Reserve the comparison gutter even with highlighting off, so toggling
        // colors does not shift every file horizontally.
        qsizetype digits = 1;
        for (const auto &d : workspace.files) digits = std::max(digits, QString::number(d.rowCount()).size());
        for (const auto &d : workspace.patches) digits = std::max(digits, qsizetype(d.lineDigits));
        numberWidth = std::max(numberWidth, 2 * digits + 2);
    }
    gutter = std::max(48.0, (numberWidth + 2) * cell);
    panelWidth = gutter + 100 * cell + 16;
    stride = panelWidth + 40;
    layoutTree();
    updateBounds();
    if (!focusPath.isEmpty()) {
        auto r = fileRect(focusPath, focusSide);
        if (r.isEmpty()) r = fileRect(focusPath);
        if (!r.isEmpty()) offset = anchor - r.topLeft() * scale;
    }
    changed();
}
void Canvas::layoutTree() {
    nodes.clear(); leaves.clear(); treeDepth = 0; layoutScale = -1;
    nodes.push_back({QFileInfo(root).fileName().isEmpty() ? root : QFileInfo(root).fileName(), -1, 0, 0, 0, {}});
    QHash<QString, int> directories;
    directories.insert("", 0);
    for (qsizetype i = 0; i < qsizetype(documents.size()); ++i) {
        const auto parts = documents[i].path.split('/');
        QString path;
        int parent = 0;
        for (int j = 0; j + 1 < parts.size(); ++j) {
            path += parts[j] + '/';
            if (!directories.contains(path)) {
                int next = int(nodes.size());
                nodes.push_back({parts[j], -1, 0, 0, j + 1, {}});
                nodes[parent].children.push_back(next);
                directories.insert(path, next);
            }
            parent = directories.value(path);
        }
        const int next = int(nodes.size());
        nodes.push_back({parts.last(), i, 0, 0, int(parts.size()), {}});
        nodes[parent].children.push_back(next);
    }
    QHash<QString, int> beforeNodes;
    for (int id = 0; id < int(nodes.size()); ++id) if (nodes[id].document >= 0) {
        const auto &d = documents[nodes[id].document];
        if (d.comparisonSide < 0) beforeNodes.insert(d.path, id);
        else if (d.comparisonSide > 0 && beforeNodes.contains(d.path)) {
            const int before = beforeNodes.value(d.path);
            nodes[id].partner = before; nodes[before].partner = id;
        }
    }
    std::vector<int> previousColor;
    std::function<void(int)> visit = [&](int id) {
        auto &n = nodes[id];
        n.first = qsizetype(leaves.size());
        if (n.document >= 0) leaves.push_back(id);
        else {
            std::stable_sort(n.children.begin(), n.children.end(), [&](int a, int b) { return nodes[a].name < nodes[b].name; });
            for (int child : n.children) {
                auto &c = nodes[child];
                if (c.document < 0) {
                    if (previousColor.size() <= size_t(c.depth)) previousColor.resize(c.depth + 1, -1);
                    c.color = (previousColor[c.depth] + 1) % 8;
                    if (c.color == n.color) c.color = (c.color + 1) % 8;
                    previousColor[c.depth] = c.color;
                }
                visit(child);
            }
        }
        n.last = qsizetype(leaves.size());
    };
    visit(0);
    QFont label = codeFont; label.setPixelSize(12); label.setBold(true);
    const QFontMetricsF metrics(label);
    for (auto &n : nodes) if (n.document < 0) {
        n.labelWidth = metrics.horizontalAdvance(n.name + '/') + 32;
        treeDepth = std::max(treeDepth, n.depth + 1);
    }
    layoutScene();
}
void Canvas::layoutScene() {
    if (layoutScale == scale || nodes.empty()) return;
    if (treeView) { layoutBranches(); return; }
    if (layoutScale < 0) {
        // Horizontal spans are pure world units. A directory is the union of its
        // immediate children, including all descendant file widths.
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            auto &n = *it;
            if (n.document >= 0)
                n.worldWidth = gutter + std::clamp(std::max(documents[n.document].columns, documents[n.document].layoutColumns), 12, 100) * cell + 40;
            else {
                n.worldWidth = 0;
                for (int child : n.children) n.worldWidth += nodes[child].worldWidth;
            }
        }
        nodes[0].worldLeft = 0;
        for (auto &n : nodes) if (n.document < 0) {
            double x = n.worldLeft;
            for (int child : n.children) { nodes[child].worldLeft = x; x += nodes[child].worldWidth; }
        }
    }
    const double rowHeight = directoryHeight();
    for (auto &n : nodes) {
        n.left = n.worldLeft * scale;
        n.width = n.worldWidth * scale;
        n.top = n.depth * rowHeight;
        n.height = n.document < 0 ? rowHeight : headerHeight(scale)
            + (collapsed[n.document] ? 0 : documents[n.document].rowCount() * lineHeight * scale);
    }
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
        for (int child : it->children)
            it->height = std::max(it->height, nodes[child].top + nodes[child].height - it->top);
    sceneWidth = nodes[0].width;
    layoutScale = scale;
}
void Canvas::layoutBranches() {
    const bool repack = layoutScale < 0;
    if (repack) {
        // Painting uses a spatial order; packing always uses name/pair order.
        for (auto &n : nodes) std::stable_sort(n.children.begin(), n.children.end(), [&](int a, int b) {
            if (nodes[a].name != nodes[b].name) return nodes[a].name < nodes[b].name;
            if (nodes[a].document < 0 || nodes[b].document < 0) return a < b;
            return documents[nodes[a].document].comparisonSide < documents[nodes[b].document].comparisonSide;
        });
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            auto &n = *it;
            n.above = n.partner = -1;
            if (n.document >= 0) {
                const auto &d = documents[n.document];
                n.worldWidth = gutter + std::clamp(std::max(d.columns, d.layoutColumns), 12, 100) * cell + 40;
                n.worldHeight = 42 + (collapsed[n.document] ? 0 : std::max(d.rowCount(), d.layoutRows) * lineHeight);
                continue;
            }
            double x = 20, columnWidth = 0, stackHeight = 0, bottom = 64;
            int previous = -1;
            for (size_t i = 0; i < n.children.size();) {
                const int first = n.children[i++];
                auto &a = nodes[first];
                int second = -1;
                if (a.document >= 0 && documents[a.document].comparisonSide == -1 && i < n.children.size()) {
                    const auto &candidate = nodes[n.children[i]];
                    if (candidate.document >= 0 && documents[candidate.document].path == documents[a.document].path
                        && documents[candidate.document].comparisonSide == 1) second = n.children[i++];
                }
                const double w = a.worldWidth + (second >= 0 ? nodes[second].worldWidth : 0);
                const double h = std::max(a.worldHeight, second >= 0 ? nodes[second].worldHeight : 0);
                const bool shortFile = a.document >= 0 && h <= 340;
                if (!shortFile || previous < 0 || stackHeight + h > 340) {
                    x += columnWidth; columnWidth = 0; stackHeight = 0; previous = -1;
                }
                a.relativeX = x; a.above = previous; a.partner = second;
                if (second >= 0) {
                    nodes[second].relativeX = x + a.worldWidth;
                    nodes[second].above = previous; nodes[second].partner = first;
                }
                bottom = std::max(bottom, 64 + stackHeight + h);
                stackHeight += h + 16; columnWidth = std::max(columnWidth, w);
                previous = shortFile ? first : -1;
            }
            n.worldWidth = std::max(120.0, x + columnWidth + 20);
            n.worldHeight = bottom;
        }
        nodes[0].worldLeft = 0;
        for (auto &n : nodes) for (int child : n.children)
            nodes[child].worldLeft = n.worldLeft + nodes[child].relativeX;
    }
    const double lane = std::max(48.0, 72 * scale), gap = std::max(8.0, 16 * scale);
    // The fan ends eight pixels above the directory row. File stems stay in
    // their own gutters, so they need only a short, zoom-scaled drop from it.
    const double fileDrop = std::clamp(32 * scale, 4.0, 48.0);
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
        auto &n = *it;
        n.left = n.worldLeft * scale; n.width = n.worldWidth * scale;
        if (n.document >= 0) {
            const auto &d = documents[n.document];
            n.height = headerHeight(scale) + (collapsed[n.document] ? 0 : std::max(d.rowCount(), d.layoutRows) * lineHeight * scale);
        } else {
            n.height = lane;
            for (int child : n.children) {
                auto &c = nodes[child];
                c.relativeY = c.document >= 0 ? lane - 8 + fileDrop : lane;
                if (c.above >= 0) {
                    const auto &above = nodes[c.above];
                    c.relativeY = above.relativeY + std::max(above.height,
                        above.partner >= 0 ? nodes[above.partner].height : 0) + gap;
                }
                n.height = std::max(n.height, c.relativeY + c.height);
            }
        }
    }
    nodes[0].top = 0;
    for (auto &n : nodes) for (int child : n.children) nodes[child].top = n.top + nodes[child].relativeY;
    if (repack) {
        std::stable_sort(leaves.begin(), leaves.end(), [&](int a, int b) {
            if (nodes[a].left != nodes[b].left) return nodes[a].left < nodes[b].left;
            return nodes[a].top < nodes[b].top;
        });
        for (qsizetype slot = 0; slot < qsizetype(leaves.size()); ++slot) {
            nodes[leaves[slot]].first = slot; nodes[leaves[slot]].last = slot + 1;
        }
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) if (it->document < 0) {
            it->first = leaves.size(); it->last = 0;
            for (int child : it->children) {
                it->first = std::min(it->first, nodes[child].first);
                it->last = std::max(it->last, nodes[child].last);
            }
            std::sort(it->children.begin(), it->children.end(), [&](int a, int b) { return nodes[a].first < nodes[b].first; });
        }
    }
    sceneWidth = nodes[0].width; layoutScale = scale;
}
QRectF Canvas::treeLabelRect(int id) const {
    const auto &n = nodes[id];
    QFont font = codeFont; font.setPixelSize(12); font.setBold(true);
    const double textWidth = QFontMetricsF(font).horizontalAdvance(n.name + '/');
    const QRectF subtree(offset.x() + n.left, offset.y() + n.top, n.width, n.height);
    const QRectF visible(0, 34, viewport().width(), viewport().height() - 34);
    const double center = subtree.center().x();
    auto box = pinnedTreeLabel(subtree, center, subtree.top(), n.depth, textWidth + 8, visible);
    if (!box.isEmpty() && (box.top() != subtree.top() || std::abs(box.center().x() - center) > 0.01))
        box = pinnedTreeLabel(subtree, center, subtree.top(), n.depth, textWidth + 20, visible);
    return box;
}
void Canvas::drawBranches(QPainter &p, int id, qsizetype first, qsizetype end, bool labels) {
    const auto &n = nodes[id];
    if (n.document >= 0 || n.last <= first || n.first >= end) return;
    const QPointF anchor(offset.x() + n.left + n.width / 2, offset.y() + n.top);
    p.save(); p.setRenderHint(QPainter::Antialiasing);
    auto begin = std::lower_bound(n.children.begin(), n.children.end(), first,
        [&](int child, qsizetype slot) { return nodes[child].last <= slot; });
    const auto edgeBegin = labels ? begin : n.children.begin();
    const QRectF lineClip(0, 34, viewport().width(), viewport().height() - 34);
    for (auto it = edgeBegin; it != n.children.end() && (!labels || nodes[*it].first < end); ++it) {
        const auto &c = nodes[*it];
        if (!labels && !(c.document >= 0 && c.partner >= 0 && documents[c.document].comparisonSide > 0)) {
            const double y = offset.y() + c.top;
            // All diagonals end in the same clear lane. Vertical stems then
            // stay inside their own subtree or file gutter, without sideways hooks.
            const double x = offset.x() + c.left + (c.document < 0 ? c.width / 2 : std::min(4.0, gutter * scale * 0.25));
            const double junctionY = offset.y() + n.top + std::max(48.0, 72 * scale) - 8;
            p.setPen(QPen(QColor(c.document < 0 ? "#6b8da8" : "#a5b6c4"), 1));
            if (c.above < 0) drawClippedLine(p, anchor + QPointF(0, 21), QPointF(x, junctionY), lineClip);
            drawClippedLine(p, QPointF(x, junctionY), QPointF(x, y), lineClip);
        }
        drawBranches(p, *it, first, end, labels);
    }
    if (labels) {
        const auto rect = treeLabelRect(id);
        if (!rect.isEmpty()) {
            QFont font = codeFont; font.setPixelSize(12); font.setBold(true); p.setFont(font);
            const QFontMetricsF metrics(font);
            p.setClipRect(rect, Qt::IntersectClip);
            p.setPen(Qt::NoPen); p.setBrush(QColor("#d7e8f4")); p.drawRoundedRect(rect, 3, 3);
            double left = rect.left() + std::min(4.0, rect.width() * 0.08);
            const bool movedY = rect.top() > anchor.y() + 0.01;
            const bool movedX = std::abs(rect.center().x() - anchor.x()) > 0.01;
            if ((movedX || movedY) && rect.width() > 24) {
                p.setPen(QColor("#536b7b"));
                p.drawText(QPointF(left, rect.top() + metrics.ascent() + 3),
                    movedY ? "↑" : anchor.x() < rect.center().x() ? "‹" : "›");
                left += 12;
            }
            const double available = rect.right() - left;
            const QString caption = metrics.horizontalAdvance(n.name + '/') <= available ? n.name + '/' : n.name;
            const QString title = labelForPainting(caption, metrics, available, false);
            p.setPen(QColor("#304655")); p.drawText(QPointF(left, rect.top() + metrics.ascent() + 3), title);
            drawLabelFade(p, rect, labelFadeAmount(caption, metrics, available));
        }
    }
    p.restore();
}
qsizetype Canvas::slotAt(double screenX) const {
    const double x = screenX - offset.x();
    auto it = std::upper_bound(leaves.begin(), leaves.end(), x,
        [&](double position, int id) { return position < nodes[id].left; });
    if (it == leaves.begin()) return 0;
    const double left = nodes[*(it - 1)].left;
    auto first = std::lower_bound(leaves.begin(), it, left,
        [&](int id, double position) { return nodes[id].left < position; });
    return qsizetype(first - leaves.begin());
}
qsizetype Canvas::slotNear(QPointF point) const {
    if (leaves.empty()) return 0;
    const auto first = slotAt(point.x() - stride * scale);
    qsizetype nearest = first;
    double distance = 1e300;
    for (qsizetype slot = first; slot < qsizetype(leaves.size()); ++slot) {
        const auto r = leafRect(slot);
        if (nodes[leaves[slot]].left > point.x() - offset.x() + stride * scale && slot > first) break;
        const QPointF p = (point - offset) / scale;
        const double dx = std::max({r.left() - p.x(), 0.0, p.x() - r.right()});
        const double dy = std::max({r.top() - p.y(), 0.0, p.y() - r.bottom()});
        if (dx * dx + dy * dy < distance) { distance = dx * dx + dy * dy; nearest = slot; }
    }
    return nearest;
}
int Canvas::directoryCount() const {
    return int(nodes.size() - leaves.size());
}
QRectF Canvas::leafRect(qsizetype slot) const {
    const auto &n = nodes[leaves[slot]];
    return QRectF(n.worldLeft, n.top / scale, n.worldWidth - 24,
                  headerHeight(scale) / scale + (collapsed[n.document] ? 0 : double(documents[n.document].rowCount()) * lineHeight));
}
QRectF Canvas::fileRect(const QString &path, int side) const {
    for (qsizetype i = 0; i < qsizetype(leaves.size()); ++i)
        if (documents[nodes[leaves[i]].document].path == path
            && (!side || documents[nodes[leaves[i]].document].comparisonSide == side)) return leafRect(i);
    return {};
}
QRectF Canvas::contentRect(const QString &path) const {
    auto result = fileRect(path);
    if (!result.isEmpty()) result.setTop(result.top() + headerHeight(scale) / scale);
    return result;
}
bool Canvas::isCollapsed(const QString &path) const {
    for (qsizetype i = 0; i < qsizetype(documents.size()); ++i)
        if (documents[i].path == path) return collapsed[i];
    return false;
}
double Canvas::headerHeight(double zoom) const { return std::clamp(42 + 4 * std::log2(zoom), 36.0, 58.0); }
double Canvas::sceneHeight(double) const {
    return nodes.empty() ? 1 : nodes[0].height;
}
void Canvas::updateBounds() {
    layoutScale = -1; layoutScene();
    depthRows.clear(); expandedSlots.clear();
    for (int id : leaves) {
        const auto &n = nodes[id];
        if (depthRows.size() <= size_t(n.depth)) depthRows.resize(n.depth + 1, -1);
        depthRows[n.depth] = std::max(depthRows[n.depth], collapsed[n.document] ? 0 : double(documents[n.document].rowCount()));
    }
    for (qsizetype slot = 0; slot < qsizetype(leaves.size()); ++slot)
        if (!collapsed[nodes[leaves[slot]].document]) expandedSlots.push_back(slot);
    tallest = sceneHeight(scale) / scale;
}
void Canvas::syncScrollbars() {
    const double extents[] = {sceneWidth / scale, scrollBottom / scale};
    for (int axis = 0; axis < 2; ++axis) {
        auto *bar = axis ? vertical : horizontal;
        const QSignalBlocker blocker(bar);
        const double visible = (axis ? viewport().height() : viewport().width()) / scale;
        const double position = -(axis ? offset.y() : offset.x()) / scale;
        const double lo = std::min(-24 / scale, position);
        const double hi = std::max({lo, extents[axis] + 24 / scale - visible, position});
        scrollLow[axis] = lo; scrollHigh[axis] = hi;
        const double span = hi - lo;
        bar->setRange(0, span > 1e-9 ? 1000000 : 0);
        bar->setPageStep(span > 1e-9 ? int(std::clamp(1000000 * visible / span, 1.0, 1000000.0)) : 1000000);
        bar->setSingleStep(std::max(1, bar->pageStep() / 10));
        bar->setValue(span > 1e-9 ? int(std::round((position - lo) / span * 1000000)) : 0);
    }
}
void Canvas::constrainCamera() {
    if (leaves.empty()) { scrollBottom = 1; return; }
    constexpr double margin = 80;
    offset.setX(std::clamp(offset.x(), std::min(-margin, margin - sceneWidth), double(viewport().width()) - margin));
    // Use the visible columns' bottom so moving from a tall file to a short one
    // cannot leave the camera thousands of lines below every visible file.
    double bottom = 0;
    const auto first = slotAt(-stride * scale);
    auto end = std::min(qsizetype(leaves.size()), slotAt(viewport().width() + stride * scale) + 1);
    while (end < qsizetype(leaves.size()) && nodes[leaves[end]].left == nodes[leaves[end - 1]].left) ++end;
    for (qsizetype i = first; i < end; ++i) bottom = std::max(bottom, leafRect(i).bottom() * scale);
    scrollBottom = bottom;
    offset.setY(std::clamp(offset.y(), std::min(-margin, margin - bottom), double(viewport().height()) - margin));
}
void Canvas::changed() {
    layoutScene(); tallest = sceneHeight(scale) / scale;
    constrainCamera(); syncScrollbars(); update();
}
void Canvas::zoomAt(double factor, QPointF anchor) {
    if (!std::isfinite(factor) || factor <= 0) return;
    const double next = std::clamp(scale * factor, 1e-6, 8.0);
    if (leaves.empty()) { scale = next; changed(); return; }
    // Bar heights scale logarithmically. Keep the same file/source position
    // under the cursor when resolving its new vertical position.
    qsizetype slot = slotNear(anchor);
    if (collapsed[nodes[leaves[slot]].document] && !expandedSlots.empty()) {
        auto it = std::lower_bound(expandedSlots.begin(), expandedSlots.end(), slot);
        if (it == expandedSlots.end()) slot = expandedSlots.back();
        else if (it == expandedSlots.begin() || *it - slot < slot - *(it - 1)) slot = *it;
        else slot = *(it - 1);
    }
    const auto oldRect = leafRect(slot);
    const double oldBody = offset.y() + nodes[leaves[slot]].top + headerHeight(scale);
    const double column = std::clamp((anchor.x() - offset.x() - oldRect.x() * scale) / scale, 0.0, panelWidth);
    const double row = collapsed[nodes[leaves[slot]].document] ? 0 : std::clamp((anchor.y() - oldBody) / (lineHeight * scale), 0.0,
        double(documents[nodes[leaves[slot]].document].rowCount() - 1));
    scale = next; layoutScene();
    offset.setX(anchor.x() - nodes[leaves[slot]].left - column * scale);
    offset.setY(anchor.y() - nodes[leaves[slot]].top - headerHeight(scale) - row * lineHeight * scale);
    changed();
}
void Canvas::fit() {
    if (documents.empty()) return;
    double low = 1e-6, high = 1;
    for (int i = 0; i < 28; ++i) {
        scale = (low + high) / 2; layoutScene();
        if (sceneWidth <= viewport().width() - 48 && sceneHeight(scale) <= viewport().height() - 80) low = scale;
        else high = scale;
    }
    scale = low > 1.1e-6 ? low : 0.03;
    offset = {24, 56}; changed();
}
double Canvas::directoryHeight() const {
    // Uniform screen-space height across every depth. Logarithmic scaling keeps
    // the hierarchy legible while widths remain exactly proportional to zoom.
    return std::clamp(28 + 6 * std::log2(scale), 20.0, 44.0);
}
QRectF Canvas::directoryBox(const QString &path) const {
    for (const auto &n : nodes) {
        if (n.document >= 0 || n.first >= qsizetype(leaves.size())) continue;
        const auto parts = documents[nodes[leaves[n.first]].document].path.split('/');
        if (parts.mid(0, n.depth).join('/') == path)
            return QRectF(offset.x() + n.left, offset.y() + n.top, n.width, directoryHeight());
    }
    return {};
}
QPointF Canvas::directoryAnchor(const QString &path) const {
    const auto box = directoryBox(path);
    const double left = std::max(0.0, box.left());
    const double width = std::max(0.0, std::min(double(viewport().width()), box.right()) - left);
    return {left + std::min(8.0, width * 0.08) + (box.left() < 0 && width > 24 ? 12 : 0), box.top()};
}
void Canvas::drawDirectories(QPainter &p, int id, qsizetype first, qsizetype end) {
    const auto &n = nodes[id];
    if (n.last <= first || n.first >= end || n.document >= 0) return;
    const QRectF box(offset.x() + n.left, offset.y() + n.top, n.width, directoryHeight());
    const QRectF visible = box.intersected(QRectF(viewport()));
    if (!visible.isEmpty()) {
        static const QColor colors[] = {
            QColor("#8dc8f0"), QColor("#f3bb78"), QColor("#9bd7ac"), QColor("#c6a7e9"),
            QColor("#f29ea9"), QColor("#7fd4cd"), QColor("#e6d47c"), QColor("#a7b6ed")};
        p.save();
        p.setClipRect(visible, Qt::IntersectClip);
        p.fillRect(visible, colors[n.color]);
        p.setPen(QPen(QColor("#ffffff"), 1));
        p.drawRect(box);
        QFont label = codeFont;
        label.setPixelSize(12); label.setBold(true); p.setFont(label);
        const QFontMetricsF metrics(label);
        const double padding = std::min(8.0, visible.width() * 0.08);
        double left = visible.left() + padding;
        const double baseline = box.top() + (box.height() - metrics.height()) / 2 + metrics.ascent();
        if (box.left() < 0 && visible.width() > 24) {
            p.setPen(QColor("#536b7b"));
            p.drawText(QPointF(left, baseline), "‹"); left += 12;
        }
        const double available = visible.right() - left;
        if (available > 0) {
            p.setClipRect(QRectF(left, box.top(), available, box.height()), Qt::IntersectClip);
            p.setPen(QColor("#304655"));
            const QString caption = metrics.horizontalAdvance(n.name + '/') <= available ? n.name + '/' : n.name;
            const QString title = labelForPainting(caption, metrics, available, false);
            p.drawText(QPointF(left, baseline), title);
            drawLabelFade(p, QRectF(left, box.top(), available, box.height()),
                          labelFadeAmount(caption, metrics, available));
        }
        p.restore();
    }
    // Child boxes sit directly below their parent and retain the same x spans.
    auto begin = std::lower_bound(n.children.begin(), n.children.end(), first,
        [&](int child, qsizetype slot) { return nodes[child].last <= slot; });
    for (auto it = begin; it != n.children.end() && nodes[*it].first < end; ++it)
        drawDirectories(p, *it, first, end);
}
QRectF Canvas::headerRect(qsizetype slot) const {
    const auto r = leafRect(slot);
    const double top = offset.y() + r.y() * scale;
    const double bottom = top + r.height() * scale;
    const double h = headerHeight(scale);
    const double y = std::min(std::max(34.0, top), bottom - h);
    return QRectF(offset.x() + r.x() * scale, y, r.width() * scale, h);
}
QRectF Canvas::sharedHeaderRect(int id) const {
    const auto &n = nodes[id];
    double left = n.left, right = n.left + n.width - 24 * scale;
    double bottom = n.top + headerHeight(scale) + (collapsed[n.document] ? 0 : documents[n.document].rowCount() * lineHeight * scale);
    if (n.partner >= 0) {
        const auto &other = nodes[n.partner];
        left = std::min(left, other.left); right = std::max(right, other.left + other.width - 24 * scale);
        bottom = std::max(bottom, other.top + headerHeight(scale) + (collapsed[other.document] ? 0 : documents[other.document].rowCount() * lineHeight * scale));
    }
    const double y = std::min(std::max(34.0, offset.y() + n.top), offset.y() + bottom - headerHeight(scale));
    return QRectF(offset.x() + left, y, right - left, headerHeight(scale));
}
QRectF Canvas::fileHeaderRect(const QString &path) const {
    for (int id : leaves) if (documents[nodes[id].document].path == path) return sharedHeaderRect(id);
    return {};
}
bool Canvas::binaryControlVisible(int id) const {
    const auto &n = nodes[id];
    if (!documents[n.document].binary) return false;
    const auto header = sharedHeaderRect(id);
    const auto visible = header.intersected(QRectF(0, 34, viewport().width(), viewport().height() - 34));
    QFont label = codeFont; label.setPixelSize(std::clamp(int(14 * scale), 12, 22));
    const QFontMetricsF metrics(label);
    const double available = visible.width() - std::min(gutter * scale, visible.width() * 0.1);
    return visible.height() >= metrics.height() && header.top() >= 34
        && available >= metrics.horizontalAdvance(QString("▸ ") + n.name.left(2));
}
void Canvas::drawFileHeader(QPainter &p, int id) {
    const auto &n = nodes[id];
    const auto &d = documents[n.document];
    const auto header = sharedHeaderRect(id);
    if (header.bottom() < 34 || header.top() > viewport().height()) return;
    const auto visible = header.intersected(QRectF(0, 34, viewport().width(), viewport().height() - 34));
    if (visible.isEmpty()) return;
    p.save(); p.setClipRect(visible, Qt::IntersectClip);
    p.fillRect(header, Qt::white);
    if (n.partner >= 0) {
        const double divider = offset.x() + nodes[n.partner].left;
        p.fillRect(QRectF(header.left(), header.bottom() - 2, divider - header.left(), 2), QColor("#c9747d"));
        p.fillRect(QRectF(divider, header.bottom() - 2, header.right() - divider, 2), QColor("#66a87b"));
    }
    QFont label = codeFont; label.setPixelSize(std::clamp(int(14 * scale), 12, 22)); p.setFont(label);
    const QFontMetricsF metrics(label);
    const double labelX = visible.left() + std::min(gutter * scale, visible.width() * 0.1);
    const double available = visible.right() - labelX;
    const QString marker = d.binary ? (collapsed[n.document] ? "▸ " : "▾ ") : "";
    const bool showMarker = binaryControlVisible(id);
    const QString shortened = labelForPainting(n.name, metrics, available - (showMarker ? metrics.horizontalAdvance(marker) : 0), true);
    const QString title = (showMarker ? marker : QString()) + shortened;
    p.setPen(QColor("#444b52"));
    p.drawText(QPointF(labelX, header.top() + metrics.ascent() + 2), title);
    QFont stats = codeFont; stats.setPixelSize(11); p.setFont(stats);
    const QString added = QString("+%1").arg(d.added);
    p.setPen(QColor("#167039")); p.drawText(QPointF(labelX, header.top() + metrics.ascent() + 18), added);
    p.setPen(QColor("#b42e38"));
    p.drawText(QPointF(labelX + QFontMetricsF(stats).horizontalAdvance(added + " "), header.top() + metrics.ascent() + 18), QString("−%1").arg(d.removed));
    drawLabelFade(p, QRectF(labelX, header.top(), available, metrics.height() + 4),
                  labelFadeAmount(n.name, metrics, available - (showMarker ? metrics.horizontalAdvance(marker) : 0)));
    drawLabelFade(p, QRectF(labelX, header.top() + metrics.ascent() + 7, available, 13),
                  labelFadeAmount(added + QString(" −%1").arg(d.removed), QFontMetricsF(stats), available));
    p.restore();
}
void Canvas::drawTextRow(QPainter &p, const Document &d, qsizetype row, double y) {
    if (d.diff && (d.rows[row].change == '+' || d.rows[row].change == '-'))
        p.fillRect(QRectF(0, y - ascent, 100 * cell, lineHeight),
                   d.rows[row].change == '+' ? QColor("#e4f3e8") : QColor("#fbe6e7"));
    const QString text = d.rowText(row);
    const double textWidth = QFontMetricsF(codeFont).horizontalAdvance(text);
    if (d.syntax.empty()) {
        p.setPen(QColor::fromRgba(inkColor(Ink::Plain)));
        p.save(); p.translate(0, y);
        if (textWidth > 100 * cell) p.scale(100 * cell / textWidth, 1);
        p.drawText(QPointF(0, 0), text);
        p.restore();
        return;
    }
    const auto &span = d.rows[row];
    auto token = std::lower_bound(d.syntax.begin(), d.syntax.end(), span.start,
        [](const SyntaxSpan &s, qsizetype start) { return s.start + s.length <= start; });
    QList<QTextLayout::FormatRange> formats;
    for (; token != d.syntax.end() && token->start < span.start + span.length; ++token) {
        QTextLayout::FormatRange format;
        format.start = int(std::max(token->start, span.start) - span.start);
        format.length = int(std::min(token->start + token->length, span.start + span.length) - span.start - format.start);
        format.format.setForeground(QColor::fromRgba(inkColor(token->ink)));
        formats.push_back(format);
    }
    QTextLayout layout(text, codeFont);
    layout.setFormats(formats);
    layout.beginLayout();
    auto line = layout.createLine();
    if (line.isValid()) line.setLineWidth(std::max(textWidth, 100 * cell) + 1);
    layout.endLayout();
    p.save(); p.translate(0, y - ascent);
    if (textWidth > 100 * cell) p.scale(100 * cell / textWidth, 1);
    p.setPen(QColor::fromRgba(inkColor(Ink::Plain)));
    layout.draw(&p, QPointF(0, 0));
    p.restore();
}
const QImage &Canvas::textTile(qsizetype document, qsizetype tile) {
    const QString key = documents[document].path + QChar(0) + (documents[document].diff ? "diff:" : "file:")
        + QString::fromLatin1(documents[document].contentHash.toHex()) + ':' + QString::number(tile);
    if (auto *cached = textTiles.object(key)) return *cached;
    // Fixed-resolution glyph tiles are reusable across zoom levels; LRU bounds RAM.
    const auto &d = documents[document];
    const qsizetype first = tile * 64, count = std::min<qsizetype>(64, d.rowCount() - first);
    auto *image = new QImage(int(std::ceil(100 * cell * 0.5)), int(std::ceil(count * lineHeight * 0.5)), QImage::Format_RGB32);
    image->fill(Qt::white);
    QPainter painter(image);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.scale(0.5, 0.5); painter.setFont(codeFont);
    for (qsizetype r = 0; r < count; ++r) drawTextRow(painter, d, first + r, r * lineHeight + ascent);
    painter.end();
    textTiles.insert(key, image, int(image->sizeInBytes()));
    return *image;
}
void Canvas::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), Qt::white);
    p.setClipRect(QRect(0, 34, viewport().width(), viewport().height() - 34));
    p.setFont(codeFont);
    if (!documents.empty()) {
        const auto first = slotAt(-stride * scale);
        auto end = std::min(qsizetype(leaves.size()), slotAt(viewport().width() + stride * scale) + 1);
        while (end < qsizetype(leaves.size()) && nodes[leaves[end]].left == nodes[leaves[end - 1]].left) ++end;
        if (treeView) drawBranches(p, 0, first, end, false);
        else drawDirectories(p, 0, first, end);
        for (qsizetype slot = first; slot < end; ++slot) {
            const auto &n = nodes[leaves[slot]];
            const auto &d = documents[n.document];
            const QRectF world = leafRect(slot);
            const double x = offset.x() + world.x() * scale;
            const double top = offset.y() + world.y() * scale;
            const double bottom = top + world.height() * scale;
            if (bottom < 0 || top > viewport().height()) continue;
            const QRectF header = headerRect(slot);
            const double originY = top + headerHeight(scale);
            const double clipTop = std::max(0.0, header.bottom());
            if (!collapsed[n.document] && clipTop < viewport().height()) {
                const auto rowFirst = qsizetype(std::clamp(std::floor((clipTop - originY) / (lineHeight * scale)), 0.0, double(d.rowCount())));
                const auto rowEnd = qsizetype(std::clamp(std::ceil((viewport().height() - originY) / (lineHeight * scale)), 0.0, double(d.rowCount())));
                p.save();
                p.setClipRect(QRectF(x, clipTop, world.width() * scale, viewport().height() - clipTop), Qt::IntersectClip);
                const double overviewWeight = 1 - smooth(0.12, 0.24, scale);
                const double liveWeight = smooth(0.40, 0.60, scale);
                const double tileWeight = 1 - overviewWeight - liveWeight;
                p.setRenderHint(QPainter::SmoothPixmapTransform);
                if (overviewWeight > 0 && !d.overview.isEmpty()) {
                    const double fullHeight = d.rowCount() * lineHeight * scale;
                    const double y1 = std::max(clipTop, originY);
                    const double y2 = std::min(double(viewport().height()), bottom);
                    // Continuous mip blending; choose by horizontal footprint so tall
                    // files do not lose all their horizontal text detail.
                    const double lod = std::clamp(std::log2(1 / std::max(1e-9, cell * scale)), 0.0, double(d.overviewLevels.size()));
                    const int level = int(std::floor(lod));
                    const double fraction = lod - level;
                    auto drawLevel = [&](int index, double opacity) {
                        if (opacity <= 0 || y2 <= y1) return;
                        int w = 100, h = int(d.overview.size() / 100);
                        for (int i = 0; i < index; ++i) { w = (w + 1) / 2; h = (h + 1) / 2; }
                        const bool colored = !d.overviewColor.isEmpty();
                        const auto &pixels = colored ? (index ? d.overviewColorLevels[index - 1] : d.overviewColor)
                                                     : (index ? d.overviewLevels[index - 1] : d.overview);
                        const QImage ink(reinterpret_cast<const uchar *>(pixels.constData()), w, h, w * (colored ? 4 : 1),
                                         colored ? QImage::Format_RGB32 : QImage::Format_Grayscale8);
                        p.setOpacity(opacity);
                        p.drawImage(QRectF(x + gutter * scale, y1, std::max(1.0, 100 * cell * scale), y2 - y1), ink,
                            QRectF(0, (y1 - originY) / fullHeight * h, w, (y2 - y1) / fullHeight * h));
                    };
                    drawLevel(level, overviewWeight);
                    if (fraction > 0) drawLevel(level + 1, overviewWeight * fraction);
                }
                if (tileWeight > 0) {
                    p.setOpacity(tileWeight);
                    for (qsizetype tile = rowFirst / 64; tile * 64 < rowEnd; ++tile) {
                        const auto &image = textTile(n.document, tile);
                        const qsizetype count = std::min<qsizetype>(64, d.rowCount() - tile * 64);
                        p.drawImage(QRectF(x + gutter * scale, originY + tile * 64 * lineHeight * scale,
                                         100 * cell * scale, count * lineHeight * scale), image);
                    }
                }
                if (liveWeight > 0) {
                    p.setOpacity(liveWeight);
                    p.translate(x, originY + double(rowFirst) * lineHeight * scale);
                    p.scale(scale, scale);
                    for (qsizetype row = rowFirst; row < rowEnd; ++row) {
                        const double y = double(row - rowFirst) * lineHeight + ascent;
                        p.setPen(QColor("#999999"));
                        const QString number = d.rowNumber(row);
                        p.drawText(QPointF(gutter - 12 - QFontMetricsF(codeFont).horizontalAdvance(number), y), number);
                        p.save(); p.translate(gutter, 0);
                        drawTextRow(p, d, row, y);
                        p.restore();
                    }
                }
                p.restore();
            }
        }
        // Draw pair-wide headers after both bodies. Either visible half can
        // request its shared header, including when the HEAD half is offscreen.
        QSet<int> headers;
        for (qsizetype slot = first; slot < end; ++slot) {
            int id = leaves[slot];
            if (nodes[id].partner >= 0 && documents[nodes[id].document].comparisonSide > 0) id = nodes[id].partner;
            if (!headers.contains(id)) { headers.insert(id); drawFileHeader(p, id); }
        }
        if (treeView) drawBranches(p, 0, first, end, true);
    }
    if (autoScroll) {
        p.save(); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor("#667b8d"), 1.5)); p.setBrush(QColor(255, 255, 255, 230));
        p.drawEllipse(scrollAnchor, 12, 12);
        p.drawLine(scrollAnchor + QPointF(-7, 0), scrollAnchor + QPointF(7, 0));
        p.drawLine(scrollAnchor + QPointF(0, -7), scrollAnchor + QPointF(0, 7));
        p.restore();
    }
    if (loading || documents.empty()) {
        p.setPen(QColor("#888888"));
        p.drawText(viewport(), Qt::AlignCenter, loading ? (diffView ? "Reading diff…" : "Reading files…")
            : changesOnly ? "No changes against HEAD" : "No files in this directory");
    }
}
void Canvas::wheelEvent(QWheelEvent *e) {
    if (autoScroll) stopAutoscroll();
    QPointF delta = e->pixelDelta().isNull() ? QPointF(e->angleDelta()) * 0.5 : QPointF(e->pixelDelta());
    if (e->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))
        zoomAt(std::exp(delta.y() * 0.008), e->position());
    else {
        if (e->modifiers() & Qt::ShiftModifier) delta = {delta.y(), delta.x()};
        offset += delta; changed();
    }
    e->accept();
}
void Canvas::stopAutoscroll() {
    autoScroll = false; middleHeld = false; middleMoved = false;
    scrollTimer.stop(); unsetCursor(); update();
}
void Canvas::tickAutoscroll() {
    if (!autoScroll) return;
    const double dt = std::clamp(scrollClock.restart() / 1000.0, 0.0, 0.05);
    auto velocity = [](double displacement) {
        const double distance = std::max(0.0, std::abs(displacement) - 12);
        return std::copysign(std::min(12000.0, 2.4 * std::pow(distance, 1.5)), displacement);
    };
    const QPointF displacement = scrollPointer - scrollAnchor;
    const QPointF movement(velocity(displacement.x()) * dt, velocity(displacement.y()) * dt);
    if (!movement.isNull()) { offset -= movement; changed(); }
}
void Canvas::mousePressEvent(QMouseEvent *e) {
    setFocus();
    if (autoScroll) { stopAutoscroll(); e->accept(); return; }
    if (e->button() == Qt::MiddleButton) {
        dragging = false; autoScroll = true; middleHeld = true; middleMoved = false;
        scrollAnchor = scrollPointer = e->position();
        scrollClock.start(); scrollTimer.start();
        setCursor(Qt::SizeAllCursor); update(); e->accept(); return;
    }
    if (e->button() == Qt::LeftButton && !leaves.empty()) {
        const qsizetype slot = slotNear(e->position());
        const auto index = nodes[leaves[slot]].document;
        if (binaryControlVisible(leaves[slot]) && sharedHeaderRect(leaves[slot]).contains(e->position())) {
            collapsed[index] = !collapsed[index];
            const int partner = nodes[leaves[slot]].partner;
            if (partner >= 0 && documents[nodes[partner].document].binary) collapsed[nodes[partner].document] = collapsed[index];
            updateBounds(); changed(); return;
        }
    }
    if (e->button() == Qt::LeftButton) {
        dragging = true; lastMouse = e->position(); setCursor(Qt::ClosedHandCursor);
    }
}
void Canvas::mouseMoveEvent(QMouseEvent *e) {
    if (autoScroll) {
        scrollPointer = e->position();
        if (middleHeld && QLineF(scrollAnchor, scrollPointer).length() > 12) middleMoved = true;
    } else if (dragging) {
        offset += e->position() - lastMouse; lastMouse = e->position(); changed();
    }
}
void Canvas::mouseReleaseEvent(QMouseEvent *e) {
    if (autoScroll && e->button() == Qt::MiddleButton) {
        if (middleMoved) stopAutoscroll();
        else middleHeld = false;
        return;
    }
    dragging = false;
    if (!autoScroll) unsetCursor();
}
void Canvas::mouseDoubleClickEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) zoomAt(1 / scale, e->position());
}
void Canvas::keyPressEvent(QKeyEvent *e) {
    if (autoScroll) stopAutoscroll();
    if (e->modifiers() & Qt::ControlModifier) {
        if (e->key() == Qt::Key_U || e->key() == Qt::Key_D) {
            offset.ry() += (e->key() == Qt::Key_U ? 1 : -1) * viewport().height() * 0.5;
            changed(); e->accept(); return;
        }
    }
    if (!(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
        switch (e->key()) {
        case Qt::Key_D: toggleDiff(); return;
        case Qt::Key_S: toggleOption(1); return;
        case Qt::Key_C: toggleOption(2); return;
        case Qt::Key_T: toggleOption(3); return;
        default: break;
        }
    }
    switch (e->key()) {
    case Qt::Key_Escape: break;
    case Qt::Key_F11:
        if (isFullScreen()) setWindowState(beforeFullscreen);
        else { beforeFullscreen = windowState(); showFullScreen(); }
        break;
    case Qt::Key_F: fit(); break;
    case Qt::Key_0:
        scale = 1; offset = {24, 56};
        changed(); break;
    case Qt::Key_Home:
        if (!leaves.empty()) {
            const auto &node = nodes[leaves[slotNear(viewport().center())]];
            const double left = offset.x() + node.left;
            if (left > viewport().width() - 40 || left + panelWidth * scale < 40)
                offset.setX(viewport().width() / 2.0 - node.left - gutter * scale);
            offset.setY(24 - node.top);
        }
        changed(); break;
    case Qt::Key_PageUp: offset.ry() += viewport().height() * 0.9; changed(); break;
    case Qt::Key_PageDown: offset.ry() -= viewport().height() * 0.9; changed(); break;
    case Qt::Key_Plus: case Qt::Key_Equal: zoomAt(1.2, viewport().center()); break;
    case Qt::Key_Minus: zoomAt(1 / 1.2, viewport().center()); break;
    case Qt::Key_Left: offset.rx() += 100; changed(); break;
    case Qt::Key_Right: offset.rx() -= 100; changed(); break;
    case Qt::Key_Up: offset.ry() += 100; changed(); break;
    case Qt::Key_Down: offset.ry() -= 100; changed(); break;
    default: QWidget::keyPressEvent(e);
    }
}
bool Canvas::event(QEvent *e) {
    if (autoScroll && (e->type() == QEvent::WindowDeactivate || e->type() == QEvent::FocusOut
        || e->type() == QEvent::Hide || e->type() == QEvent::Leave)) stopAutoscroll();
    if (e->type() == QEvent::ToolTip) { e->accept(); return true; }
    if (e->type() == QEvent::NativeGesture) {
        if (autoScroll) stopAutoscroll();
        auto *gesture = static_cast<QNativeGestureEvent *>(e);
        if (gesture->gestureType() == Qt::ZoomNativeGesture) {
            zoomAt(1 + gesture->value(), gesture->position()); e->accept(); return true;
        }
        if (gesture->gestureType() == Qt::PanNativeGesture) {
            offset += gesture->delta(); changed(); e->accept(); return true;
        }
    }
    return QWidget::event(e);
}
