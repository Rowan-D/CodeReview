#pragma once
#include "comparison.h"
#include <QWidget>
#include <QFont>
#include <QFutureWatcher>
#include <QScrollBar>
#include <QCache>
#include <QImage>
#include <QTimer>
#include <QElapsedTimer>
#include <QProgressBar>
#include <QToolButton>
#include <QLabel>

class Canvas : public QWidget {
public:
    explicit Canvas(const QString &root);
    ~Canvas() override;
    void setDocuments(std::vector<Document> docs);
    double zoom() const { return scale; }
    QPointF camera() const { return offset; }
    QRectF fileRect(const QString &path, int side = 0) const;
    QRectF contentRect(const QString &path) const;
    QRectF fileHeaderRect(const QString &path) const;
    bool isCollapsed(const QString &path) const;
    int textCacheBytes() const { return textTiles.totalCost(); }
    int directoryCount() const;
    QRectF directoryBox(const QString &path) const;
    QPointF directoryAnchor(const QString &path) const;
    bool autoscrolling() const { return autoScroll; }
    bool isDiffView() const { return diffView; }
    bool isSplitView() const { return splitView; }
    bool isChangesOnly() const { return changesOnly; }
    bool isTreeView() const { return treeView; }
    qsizetype addedLines() const { return workspace.added; }
    qsizetype removedLines() const { return workspace.removed; }
protected:
    void paintEvent(QPaintEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool event(QEvent *) override;
    void resizeEvent(QResizeEvent *) override;
private:
    struct Node {
        QString name;
        qsizetype document = -1, first = 0, last = 0;
        int depth = 0;
        std::vector<int> children;
        double left = 0, width = 0, labelWidth = 0;
        double top = 0, height = 0, relativeX = 0, relativeY = 0;
        double worldLeft = 0, worldTop = 0, worldWidth = 0, worldHeight = 0;
        int color = 0, above = -1, partner = -1;
    };
    void layoutTree();
    void startRefresh();
    void toggleDiff();
    void toggleOption(int option);
    void updateControls();
    void positionControls();
    void layoutBranches();
    void drawBranches(QPainter &p, int node, qsizetype first, qsizetype end, bool labels);
    void applyDocuments(std::vector<Document> docs, bool preserve);
    void layoutScene();
    qsizetype slotAt(double screenX) const;
    qsizetype slotNear(QPointF point) const;
    void updateBounds();
    void changed();
    void constrainCamera();
    void stopAutoscroll();
    void tickAutoscroll();
    void syncScrollbars();
    QRect viewport() const;
    QRectF leafRect(qsizetype slot) const;
    QRectF headerRect(qsizetype slot) const;
    QRectF sharedHeaderRect(int node) const;
    void drawFileHeader(QPainter &p, int node);
    bool binaryControlVisible(int node) const;
    void drawDirectories(QPainter &p, int node, qsizetype first, qsizetype end);
    double directoryHeight() const;
    double headerHeight(double zoom) const;
    double sceneHeight(double zoom) const;
    const QImage &textTile(qsizetype document, qsizetype tile);
    void drawTextRow(QPainter &p, const Document &d, qsizetype row, double y);
    void zoomAt(double factor, QPointF anchor);
    void fit();
    std::vector<Document> documents;
    std::vector<Node> nodes;
    std::vector<int> leaves;
    std::vector<qsizetype> expandedSlots;
    std::vector<bool> collapsed;
    std::vector<double> depthRows;
    double layoutScale = -1, sceneWidth = 1, scrollBottom = 1;
    int treeDepth = 0;
    QCache<QString, QImage> textTiles{48 * 1024 * 1024};
    QScrollBar *horizontal, *vertical;
    double scrollLow[2]{}, scrollHigh[2]{};
    QFont codeFont;
    double cell, lineHeight, ascent, panelWidth, gutter = 80, stride, tallest = 1;
    double scale = 1;
    QPointF offset{24, 56}, lastMouse;
    bool dragging = false, loading = true;
    QTimer scrollTimer;
    QTimer refreshTimer;
    QTimer progressTimer;
    QProgressBar *loadingBar = nullptr;
    QToolButton *viewToggle = nullptr, *splitToggle = nullptr, *changesToggle = nullptr, *treeToggle = nullptr;
    QLabel *globalInfo = nullptr;
    bool diffView = false, splitView = false, changesOnly = false, treeView = false;
    WorkspaceSnapshot workspace;
    ViewOptions requestedOptions;
    std::atomic_int loadProgress{-1};
    QElapsedTimer loadClock;
    bool liveEnabled = true, refreshInFlight = false;
    QElapsedTimer scrollClock;
    QPointF scrollAnchor, scrollPointer;
    bool autoScroll = false, middleHeld = false, middleMoved = false;
    Qt::WindowStates beforeFullscreen;
    QString root;
    std::atomic_bool cancelled{false};
    QFutureWatcher<WorkspaceSnapshot> watcher;
};
