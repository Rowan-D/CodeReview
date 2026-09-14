#pragma once
#include <QFontMetricsF>
#include <QTextBoundaryFinder>
#include <algorithm>
#include <QPainter>
#include <QLinearGradient>

inline double labelFadeAmount(const QString &full, const QFontMetricsF &metrics, double available) {
    const double missing = metrics.horizontalAdvance(full) - available;
    return std::clamp(missing / std::max(1.0, metrics.horizontalAdvance('M') * 6), 0.0, 1.0);
}

inline void drawLabelFade(QPainter &p, const QRectF &bounds, double amount) {
    if (amount <= 0 || bounds.width() <= 0) return;
    const double width = std::min(9.0, bounds.width() * 0.3);
    const QRectF edge(bounds.right() - width, bounds.top(), width, bounds.height());
    QLinearGradient fade(edge.topLeft(), edge.topRight());
    fade.setColorAt(0, QColor(0, 0, 0, 0));
    fade.setColorAt(1, QColor(0, 0, 0, int(180 * std::clamp(amount, 0.0, 1.0))));
    p.fillRect(edge, fade);
}

// Clip in double precision before submitting distant world coordinates to the
// raster painter, whose large-coordinate clipping can drop visible segments.
inline void drawClippedLine(QPainter &p, QPointF a, QPointF b, const QRectF &clip) {
    const QPointF delta = b - a;
    double first = 0, last = 1;
    auto edge = [&](double direction, double distance) {
        if (direction == 0) return distance >= 0;
        const double t = distance / direction;
        if (direction < 0) first = std::max(first, t);
        else last = std::min(last, t);
        return first <= last;
    };
    if (edge(-delta.x(), a.x() - clip.left()) && edge(delta.x(), clip.right() - a.x())
        && edge(-delta.y(), a.y() - clip.top()) && edge(delta.y(), clip.bottom() - a.y()))
        p.drawLine(a + delta * first, a + delta * last);
}

// Preserve a useful prefix in tiny columns; reserve the extension only when
// there is enough room for a prefix and a clear truncation marker as well.
inline QString compactLabel(const QString &name, const QFontMetricsF &metrics,
                            double width, bool file) {
    if (width <= 0) return {};
    if (metrics.horizontalAdvance(name) <= width) return name;
    auto prefix = [&](double space) {
        qsizetype low = 0, high = name.size();
        while (low < high) {
            const auto mid = (low + high + 1) / 2;
            if (metrics.horizontalAdvance(name.left(mid)) <= space) low = mid;
            else high = mid - 1;
        }
        QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, name);
        boundary.setPosition(low);
        if (!boundary.isAtBoundary()) low = boundary.toPreviousBoundary();
        return name.left(std::max(qsizetype(0), low));
    };
    const auto dot = name.lastIndexOf('.');
    if (file && dot > 0 && dot + 1 < name.size()) {
        const QString suffix = QChar(0x2026) + name.mid(dot);
        const auto beginning = prefix(width - metrics.horizontalAdvance(suffix));
        if (beginning.size() >= 2) return beginning + suffix;
    }
    const auto beginning = prefix(width);
    // The painter clips a partial first glyph when even one character cannot fit.
    if (beginning.isEmpty()) {
        QTextBoundaryFinder boundary(QTextBoundaryFinder::Grapheme, name);
        return name.left(boundary.toNextBoundary());
    }
    return beginning;
}
