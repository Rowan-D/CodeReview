#include "canvas.h"
#include "labels.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QTest>
#include <QElapsedTimer>
#include <QWheelEvent>
#include <QNativeGestureEvent>
#include <QProcess>
#include <QHelpEvent>
#include <cstdio>
#include <cstdlib>

void check(bool ok, const char *message) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void write(const QString &path, const QByteArray &bytes) {
    QFile f(path); check(f.open(QIODevice::WriteOnly), "create fixture");
    check(f.write(bytes) == bytes.size(), "write fixture");
}
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QFont labelFont("monospace"); labelFont.setPixelSize(12);
    const QFontMetricsF labelMetrics(labelFont);
    check(compactLabel("component.cpp", labelMetrics, labelMetrics.horizontalAdvance("co"), true) == "co",
          "tiny file labels use name characters instead of ellipsis");
    check(compactLabel("component.cpp", labelMetrics, 1, true) == "c",
          "sub-character label widths retain first glyph for clipping");
    const auto abbreviated = compactLabel("component.cpp", labelMetrics,
        labelMetrics.horizontalAdvance(QString::fromUtf8("com….cpp")), true);
    check(abbreviated.startsWith("co") && abbreviated.endsWith(QString::fromUtf8("….cpp")),
          "moderate file label widths preserve extension and prefix");
    check(compactLabel(".gitignore", labelMetrics, labelMetrics.horizontalAdvance(".git"), true) == ".git",
          "dotfiles are treated as names rather than extensions");
    QImage fadePreview(100, 20, QImage::Format_RGB32); fadePreview.fill(Qt::white);
    {
        QPainter painter(&fadePreview);
        drawLabelFade(painter, QRectF(0, 0, 100, 20), false);
    }
    check(fadePreview.pixelColor(99, 10) == Qt::white, "complete labels have no truncation fade");
    {
        QPainter painter(&fadePreview);
        drawLabelFade(painter, QRectF(0, 0, 100, 20), true);
    }
    check(fadePreview.pixelColor(99, 10).lightness() < 110 && fadePreview.pixelColor(80, 10) == Qt::white,
          "shortened labels darken only at the clipped edge");
    check(labelFadeAmount("test", labelMetrics, labelMetrics.horizontalAdvance("tes")) < 0.2,
          "one missing character has only a gentle fade");
    check(labelFadeAmount("test", labelMetrics, labelMetrics.horizontalAdvance("test")) == 0,
          "omitting an optional directory slash leaves a complete name unfaded");
    QImage longLine(100, 20, QImage::Format_RGB32); longLine.fill(Qt::white);
    {
        QPainter painter(&longLine); painter.setPen(Qt::black);
        drawClippedLine(painter, QPointF(-1000000, 10), QPointF(1000000, 10), QRectF(0, 0, 100, 20));
    }
    check(longLine.pixelColor(50, 10) == Qt::black, "tree edges with distant offscreen endpoints survive raster clipping");
    const QRectF labelViewport(0, 34, 800, 500);
    const auto distantParent = pinnedTreeLabel(QRectF(-500, -100, 5000, 3000), 2000, -100, 0, 120, labelViewport);
    const auto distantChild = pinnedTreeLabel(QRectF(-200, -50, 2000, 2000), 800, -50, 1, 100, labelViewport);
    check(labelViewport.contains(distantParent) && labelViewport.contains(distantChild),
          "offscreen ancestor labels remain inside the viewport");
    check(distantParent.bottom() < distantChild.top(), "pinned ancestors keep distinct depth rows");
    const auto natural = pinnedTreeLabel(QRectF(100, 100, 200, 400), 200, 100, 1, 80, labelViewport);
    check(natural.center().x() == 200 && natural.top() == 100, "visible tree labels retain their original position");
    check(pinnedTreeLabel(QRectF(-400, 100, 200, 400), -300, 100, 1, 80, labelViewport).isEmpty(),
          "unrelated offscreen subtrees do not add pinned labels");
    check(labelForPainting("controller", labelMetrics, labelMetrics.horizontalAdvance("contr") + 2, false) == "controller",
          "hard clipping uses the full box width including the partial final character");
    QImage edgePreview(100, 22, QImage::Format_RGB32); edgePreview.fill(Qt::white);
    {
        QPainter painter(&edgePreview); painter.setClipRect(QRect(0, 0, 100, 22));
        painter.setFont(labelFont); painter.setPen(Qt::black);
        painter.drawText(QPointF(4, 15), labelForPainting("long_controller_name", labelMetrics, 96, false));
        drawLabelFade(painter, QRectF(0, 0, 100, 22), 1);
    }
    check(edgePreview.pixelColor(99, 20).lightness() < 110 && edgePreview.pixelColor(89, 20) == Qt::white,
          "truncation fade is anchored to the box edge, not the last whole glyph");
    const QRectF narrowLabel(100, 80, 20, 22);
    const auto widerLabel = expandedTreeLabel(narrowLabel, 100, 90, 200);
    check(widerLabel.contains(narrowLabel) && widerLabel.width() == 100
          && widerLabel.top() == 80 && widerLabel.height() == 22,
          "directory labels use free space without moving their original anchor or row");
    const auto leftLabel = expandedTreeLabel(narrowLabel, 200, 0, 158);
    const auto rightLabel = expandedTreeLabel(QRectF(200, 80, 20, 22), 200, 162, 300);
    check(leftLabel.right() + 4 <= rightLabel.left(),
          "neighboring expanded labels preserve a gap even when both names are long");
    check(expandedTreeLabel(narrowLabel, 100, 100, 120) == narrowLabel,
          "a neighboring file column prevents label expansion");
    check(expandedTreeLabel(narrowLabel, 10, 0, 200) == narrowLabel,
          "expansion never shrinks an existing directory label");
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == "--labels-only") return 0;
    if (argc == 2) {
        std::atomic_bool cancelled{false};
        Canvas preview(argv[1]); preview.resize(1100, 820); preview.show();
        QTest::qWait(100);
        preview.setDocuments(loadDirectory(argv[1], cancelled));
        QTest::keyClick(&preview, Qt::Key_F);
        preview.grab().save("/tmp/code-review-directory.png");
        return 0;
    }
    auto d = decode("wrap", QByteArray(201, 'x') + "\nlast\r\n\tend\rempty\n");
    check(d.rowText(0).size() == 100 && d.rowText(1).size() == 100 && d.rowText(2) == "x", "wrap without truncation");
    check(d.rowNumber(0) == "1" && d.rowNumber(1).isEmpty() && d.rowNumber(3) == "2", "source line numbering");
    check(d.rowText(4) == "    end" && d.rowText(5) == "empty" && d.rowText(6).isEmpty(), "tabs and all newline forms");
    check(decode("empty", {}).rowCount() == 1, "empty file");
    const auto ink = decode("indent", "  ab\n    c");
    check(uchar(ink.overview[0]) == 255 && uchar(ink.overview[2]) == 55 && uchar(ink.overview[204]) == 55,
          "minimap preserves indentation and ink");
    check(ink.overviewLevels.back().size() == 1 && uchar(ink.overviewLevels.back()[0]) <= 210,
          "smallest overview retains sparse ink");
    check(decode("exact", QByteArray(100, 'a') + '\n').rowCount() == 2, "exact width newline");
    auto unicode = decode("unicode", QByteArray(99, 'a') + QString::fromUcs4(U"😀z").toUtf8());
    check(unicode.rowText(0).endsWith(QString::fromUcs4(U"😀")) && unicode.rowText(1) == "z", "surrogate wrap");
    auto binary = decode("binary", QByteArray::fromHex("0001ff4142"));
    check(binary.binary && binary.rowText(0).contains("00 01 ff 41 42") && binary.rowText(0).endsWith("...AB"), "binary bytes preserved");
    check(decode("legacy", QByteArray::fromHex("fffe")).binary, "invalid UTF-8 hex fallback");
    auto colored = decode("main.cpp", "// comment\nconst char *text = \"// string\";\n/* multi\nline */ return 42;\n");
    auto styleAt = [](const Document &doc, const QString &token) {
        const auto index = doc.text.indexOf(token);
        for (const auto &span : doc.syntax)
            if (index >= span.start && index < span.start + span.length) return span.ink;
        return Ink::Plain;
    };
    check(styleAt(colored, "comment") == Ink::Comment && styleAt(colored, "const") == Ink::Keyword
          && styleAt(colored, "// string") == Ink::String && styleAt(colored, "line */") == Ink::Comment
          && styleAt(colored, "42") == Ink::Number, "syntax states and token colors");
    check(!colored.overviewColor.isEmpty() && colored.overviewColorLevels.size() == colored.overviewLevels.size(),
          "syntax colors retained in minimap levels");
    auto py = decode("sample.py", "def sample():\n    text = \"\"\"multi\n# still string\"\"\"\n    return True # comment\n");
    check(styleAt(py, "def") == Ink::Keyword && styleAt(py, "# still") == Ink::String
          && styleAt(py, "# comment") == Ink::Comment, "Python multiline strings and comments");
    check(decode("notes.txt", "class is plain text").syntax.empty(), "unknown file types stay plain");
    QTemporaryDir tmp;
    check(tmp.isValid(), "temporary directory");
    QDir(tmp.path()).mkpath("nested/.hidden");
    write(tmp.filePath("nested/.hidden/file.txt"), "hello");
    write(tmp.filePath(".dotfile"), "hidden");
    write(tmp.filePath("empty"), {});
    check(QFile::link(tmp.path(), tmp.filePath("loop")), "symlink fixture");
    std::atomic_bool cancel{false};
    auto files = loadDirectory(tmp.path(), cancel);
    check(files.size() == 4 && files.front().path == ".dotfile", "recursive hidden files, sorted, no symlink cycle");
    cancel = true;
    check(loadDirectory(tmp.path(), cancel).empty(), "cancel traversal");
    Canvas canvas(tmp.path());
    canvas.show();
    QTest::qWait(100);
    std::vector<Document> fixture;
    fixture.push_back(decode("src/main.cpp", "#include <iostream>\n\nint main() {\n\tstd::cout << \"Hello, CodeReview!\";\n}\n"));
    fixture.push_back(decode("docs/long-lines.txt", QByteArray(240, 'x') + "\nEverything is visible.\n"));
    fixture.push_back(binary);
    canvas.setDocuments(fixture);
    check(canvas.isCollapsed("binary"), "binary collapsed initially");
    const auto binaryBefore = canvas.fileRect("binary");
    const QPoint binaryClick = (canvas.camera() + binaryBefore.topLeft() + QPointF(100, 10)).toPoint();
    QTest::mouseClick(&canvas, Qt::LeftButton, Qt::NoModifier, binaryClick);
    check(!canvas.isCollapsed("binary") && canvas.fileRect("binary").height() > binaryBefore.height(), "expand binary");
    QTest::mouseClick(&canvas, Qt::LeftButton, Qt::NoModifier, binaryClick);
    check(canvas.isCollapsed("binary"), "collapse binary again");
    check(canvas.fileRect("src/main.cpp").y() > binaryBefore.y() && canvas.directoryCount() == 3,
          "shallow files sit higher in the tree");
    Canvas tinyBinary(tmp.path()); tinyBinary.show(); QTest::qWait(60);
    tinyBinary.setDocuments({binary});
    QNativeGestureEvent tinyZoom(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2,
        QPointF(24, 56), QPointF(24, 56), QPointF(24, 56), 0.005 / tinyBinary.zoom() - 1, {}, 0);
    QApplication::sendEvent(&tinyBinary, &tinyZoom);
    QTest::mouseClick(&tinyBinary, Qt::LeftButton, Qt::NoModifier, tinyBinary.fileHeaderRect("binary").center().toPoint());
    check(tinyBinary.isCollapsed("binary"), "invisible binary dropdown cannot be activated at tiny zoom");
    auto *hbar = canvas.findChild<QScrollBar *>("horizontalScrollBar");
    auto *vbar = canvas.findChild<QScrollBar *>("verticalScrollBar");
    check(hbar && vbar && hbar->isVisible() && vbar->isVisible(), "edge scrollbars visible");
    const auto scrollBefore = canvas.camera();
    hbar->setValue(hbar->maximum());
    check(canvas.camera().x() < scrollBefore.x(), "horizontal scrollbar moves camera");
    QTest::keyClick(&canvas, Qt::Key_0);
    QTest::keyClick(&canvas, Qt::Key_F);
    canvas.grab().save("/tmp/code-review-viewer.png");
    QTest::keyClick(&canvas, Qt::Key_0);
    const QPointF anchor(350, 250);
    QWheelEvent zoom(anchor, anchor, {}, {0, 120}, Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&canvas, &zoom);
    check(canvas.zoom() > 1, "mouse zoom");
    const auto focused = canvas.fileRect("docs/long-lines.txt");
    check(canvas.camera().x() + focused.left() * canvas.zoom() < canvas.width()
          && canvas.camera().x() + focused.right() * canvas.zoom() > 0,
          "zoom from collapsed binary focuses nearby readable file");
    auto before = canvas.camera();
    QWheelEvent pan(anchor, anchor, {31, -47}, {}, Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QApplication::sendEvent(&canvas, &pan);
    check(std::abs(canvas.camera().x() - before.x() - 31) < 0.01
          && canvas.camera().y() >= before.y() - 47, "trackpad pixel pan respects content bounds");
    before = canvas.camera();
    QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
    QTest::mouseMove(&canvas, QPoint(150, 120));
    QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(150, 120));
    check(canvas.camera().x() <= before.x() + 50 && canvas.camera().y() <= before.y() + 20
          && canvas.camera() != before, "mouse drag pan respects scene bounds");
    QTest::keyClick(&canvas, Qt::Key_F);
    check(canvas.zoom() < 1, "fit all");
    QTest::keyClick(&canvas, Qt::Key_0);
    check(canvas.zoom() == 1 && canvas.camera() == QPointF(24, 56), "reset");
    // At 10% zoom the text area must still contain ink (no blank silhouettes).
    canvas.setDocuments({decode("sample.cpp", QByteArray("    int value = 42;\n").repeated(100))});
    QWheelEvent overviewZoom(anchor, anchor, {0, -288}, {}, Qt::NoButton, Qt::ControlModifier, Qt::ScrollUpdate, false);
    QApplication::sendEvent(&canvas, &overviewZoom);
    QTest::keyClick(&canvas, Qt::Key_F);
    // Fit yields roughly 40%; reduce once more around the upper-left origin.
    QWheelEvent smaller({24, 24}, {24, 24}, {0, -160}, {}, Qt::NoButton, Qt::ControlModifier, Qt::ScrollUpdate, false);
    QApplication::sendEvent(&canvas, &smaller);
    check(canvas.zoom() < 0.35, "minimap zoom active");
    const auto shot = canvas.grab().toImage();
    const auto sample = canvas.fileRect("sample.cpp");
    const int sy = int(canvas.camera().y() + (sample.y() + 34) * canvas.zoom());
    int darkPixels = 0;
    for (int y = sy + 2; y < std::min(shot.height() - 12, sy + 70); ++y)
        for (int x = 28; x < std::min(shot.width() - 12, 110); ++x)
            if (shot.pixelColor(x, y).lightness() < 150) ++darkPixels;
    check(darkPixels > 10, "text ink remains visible at overview zoom");
    canvas.grab().save("/tmp/code-review-minimap.png");
    // Exercise continuous zoom through the former hard LOD cutoff, with real glyph caches.
    auto zoomTo = [&](double target) {
        QNativeGestureEvent gesture(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2,
            QPointF(24, 24), QPointF(24, 24), QPointF(24, 24), target / canvas.zoom() - 1, {}, 0);
        QApplication::sendEvent(&canvas, &gesture);
    };
    zoomTo(0.349);
    const auto below = canvas.grab().toImage();
    const int cached = canvas.textCacheBytes();
    check(cached > 0 && cached <= 48 * 1024 * 1024, "bounded glyph tile cache populated");
    canvas.grab();
    check(canvas.textCacheBytes() == cached, "glyph tiles reused on repaint");
    zoomTo(0.351);
    const auto above = canvas.grab().toImage();
    qint64 difference = 0;
    for (int y = 0; y < above.height(); ++y)
        for (int x = 0; x < above.width(); ++x)
            difference += std::abs(above.pixelColor(x, y).lightness() - below.pixelColor(x, y).lightness());
    check(double(difference) / (above.width() * above.height()) < 2, "no image-wide pop at old LOD cutoff");
    std::vector<Document> tree;
    const QByteArray code = "struct Point {\n    double x, y;\n};\n\nPoint translate(Point p) {\n    return {p.x + 4, p.y + 8};\n}\n\n";
    for (int i = 0; i < 12; ++i)
        tree.push_back(decode(QString("src/%1/component_%2.cpp").arg(i < 6 ? "canvas" : "model").arg(i), code.repeated(8)));
    canvas.setDocuments(std::move(tree));
    check(canvas.textCacheBytes() == 0, "document replacement invalidates glyph cache");
    QTest::keyClick(&canvas, Qt::Key_F);
    canvas.grab().save("/tmp/code-review-tree-lod.png");
    const auto treeFileBeforeZoom = canvas.fileRect("src/canvas/component_0.cpp");
    const auto paletteShot = canvas.grab().toImage();
    const auto canvasBox = canvas.directoryBox("src/canvas"), modelBox = canvas.directoryBox("src/model");
    check(paletteShot.pixelColor(int(canvasBox.right() - 3), int(canvasBox.bottom() - 3))
          != paletteShot.pixelColor(int(modelBox.right() - 3), int(modelBox.bottom() - 3)),
          "adjacent directory boxes use distinct colors");
    zoomTo(0.376);
    const double captionBefore = canvas.directoryAnchor("").y() - canvas.camera().y();
    zoomTo(0.374);
    const double captionAfter = canvas.directoryAnchor("").y() - canvas.camera().y();
    check(std::abs(captionAfter - captionBefore) < 1, "directory captions do not jump at the old overview threshold");
    for (double zoom : {0.344, 0.343, 0.2, 0.05, 0.01}) {
        zoomTo(zoom);
        const auto rootAnchor = canvas.directoryAnchor("");
        const auto srcAnchor = canvas.directoryAnchor("src");
        const auto canvasAnchor = canvas.directoryAnchor("src/canvas");
        check(srcAnchor.y() - rootAnchor.y() >= 19.9 && canvasAnchor.y() - srcAnchor.y() >= 19.9,
              "directory rows remain distinct at overview zoom");
        const auto file = canvas.fileRect("src/canvas/component_0.cpp");
        check(file.left() == treeFileBeforeZoom.left() && file.width() == treeFileBeforeZoom.width(),
              "zoom preserves horizontal world positions and widths");
        const auto rootBox = canvas.directoryBox(""), srcBox = canvas.directoryBox("src");
        const auto childBox = canvas.directoryBox("src/canvas");
        check(rootBox.height() == srcBox.height() && srcBox.height() == childBox.height(),
              "all directory bars have equal height");
        check(std::abs(rootBox.bottom() - srcBox.top()) < 0.001
              && std::abs(srcBox.bottom() - childBox.top()) < 0.001,
              "child directory bars directly abut their parent");
        check(childBox.left() >= srcBox.left() && childBox.right() <= srcBox.right() + 0.001,
              "parent spans its descendant directories");
        check(std::abs(canvas.camera().y() + file.top() * canvas.zoom() - childBox.bottom()) < 0.001,
              "file starts directly beneath its containing directory");
        canvas.grab();
    }
    QTest::keyClick(&canvas, Qt::Key_F);
    canvas.grab().save("/tmp/code-review-tree-lod.png");
    zoomTo(1.0);
    const auto fullBox = canvas.directoryBox("");
    zoomTo(0.5);
    const auto halfBox = canvas.directoryBox("");
    check(std::abs(fullBox.width() * 0.5 - halfBox.width()) < 0.001
          && std::abs(fullBox.height() - halfBox.height() - 6) < 0.001,
          "bar widths scale linearly and heights logarithmically");
    auto *treeScrollbar = canvas.findChild<QScrollBar *>("horizontalScrollBar");
    treeScrollbar->setValue(treeScrollbar->maximum() / 2);
    check(canvas.directoryBox("").left() < 0 && canvas.directoryAnchor("").x() == 20,
          "directory label pins to viewport left when bar extends offscreen");
    // Many narrow sibling directories must get separate readable space, not disappear.
    std::vector<Document> dense;
    for (int i = 0; i < 50; ++i)
        dense.push_back(decode(QString("directory_with_name_%1/source.cpp").arg(i), "int value = 42;\n"));
    Canvas crowded(tmp.path()); crowded.show(); QTest::qWait(100);
    crowded.setDocuments(std::move(dense));
    QTest::keyClick(&crowded, Qt::Key_F);
    check(crowded.directoryCount() == 51, "dense layout retains every directory");
    const auto a = crowded.fileRect("directory_with_name_0/source.cpp");
    const auto b = crowded.fileRect("directory_with_name_1/source.cpp");
    check((b.x() - a.x()) * crowded.zoom() > 5 && (b.x() - a.x()) * crowded.zoom() < 150,
          "directory widths compact as zoom decreases");
    check(crowded.findChild<QScrollBar *>("horizontalScrollBar")->isVisible(), "horizontal navigation remains available");
    const auto deepLeaf = canvas.fileRect("src/canvas/component_0.cpp");
    check(deepLeaf.y() * canvas.zoom() > 0 && deepLeaf.y() * canvas.zoom() < 3 * 64,
          "directory levels scale with code");
    const QString deepPath = QString("nested/").repeated(40) + "leaf.txt";
    canvas.setDocuments({decode(deepPath, "still visible\n")});
    QTest::keyClick(&canvas, Qt::Key_F);
    check(canvas.zoom() > 1e-6 && canvas.directoryCount() == 41,
          "deep trees retain the hierarchy at compact zoom");
    QTest::keyClick(&canvas, Qt::Key_0);
    QElapsedTimer timer; timer.start();
    auto tall = decode("000-tall.txt", QByteArray("line contents\n").repeated(100000));
    check(tall.rowCount() == 100001, "full large file");
    const auto indexMs = timer.elapsed();
    std::vector<Document> large;
    large.push_back(std::move(tall));
    for (int i = 1; i < 10000; ++i) large.push_back(decode(QString::number(i), "small file\n"));
    canvas.setDocuments(std::move(large));
    const double topBefore = canvas.camera().y();
    vbar->setValue(vbar->maximum());
    check(canvas.camera().y() < topBefore, "vertical scrollbar moves camera");
    QTest::keyClick(&canvas, Qt::Key_0);
    timer.restart();
    for (int i = 0; i < 30; ++i) canvas.grab();
    fprintf(stdout, "Tests passed; 100,000-line indexing: %lld ms; 10,000-file visible paint average: %.2f ms\n", indexMs, timer.elapsed() / 30.0);
    QTest::keyClick(&canvas, Qt::Key_F);
    canvas.grab();
    zoomTo(0.25);
    canvas.grab(); // warm cache
    timer.restart();
    for (int i = 0; i < 30; ++i) canvas.grab();
    fprintf(stdout, "Cached 25%% LOD paint average: %.2f ms; glyph cache: %.2f MiB\n", timer.elapsed() / 30.0,
            canvas.textCacheBytes() / (1024.0 * 1024));
    canvas.setDocuments({colored});
    QTest::keyClick(&canvas, Qt::Key_0);
    const auto syntaxShot = canvas.grab().toImage();
    syntaxShot.save("/tmp/code-review-syntax.png");
    int coloredPixels = 0;
    const auto codeRect = canvas.fileRect("main.cpp");
    const int codeY = int(canvas.camera().y() + codeRect.y() + 34);
    for (int y = codeY; y < std::min(syntaxShot.height(), codeY + 100); ++y)
        for (int x = 60; x < 500; ++x) {
            const auto c = syntaxShot.pixelColor(x, y);
            if (c.lightness() < 200 && std::abs(c.red() - c.blue()) > 35) ++coloredPixels;
        }
    check(coloredPixels > 30, "syntax highlighting paints colored glyphs");
    canvas.setDocuments({decode("a.cpp", QByteArray("int value = 42;\n").repeated(10000)),
                         decode("nested/b.cpp", QByteArray("// other file\n").repeated(10000))});
    QTest::keyClick(&canvas, Qt::Key_0);
    zoomTo(0.12);
    const QPointF codeAnchor(canvas.camera().x() + canvas.contentRect("a.cpp").center().x() * canvas.zoom(), 320);
    auto sourcePosition = [&] {
        return (codeAnchor - canvas.camera()) / canvas.zoom() - canvas.contentRect("a.cpp").topLeft();
    };
    const auto sourceBefore = sourcePosition();
    QNativeGestureEvent anchored(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2,
        codeAnchor, codeAnchor, codeAnchor, 0.28 / canvas.zoom() - 1, {}, 0);
    QApplication::sendEvent(&canvas, &anchored);
    check(QLineF(sourceBefore, sourcePosition()).length() < 0.01,
          "zoom preserves file and source position across semantic layout thresholds");
    QNativeGestureEvent reverse(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2,
        codeAnchor, codeAnchor, codeAnchor, 0.12 / canvas.zoom() - 1, {}, 0);
    QApplication::sendEvent(&canvas, &reverse);
    check(QLineF(sourceBefore, sourcePosition()).length() < 0.01, "zoom round trip does not drift");
    QTest::keyClick(&canvas, Qt::Key_0);
    const auto navStart = canvas.camera();
    QTest::keyClick(&canvas, Qt::Key_PageDown);
    check(std::abs(canvas.camera().y() - navStart.y() + (canvas.height() - 12) * 0.9) < 0.01, "page down");
    QTest::keyClick(&canvas, Qt::Key_PageUp);
    check(std::abs(canvas.camera().y() - navStart.y()) < 0.01, "page up");
    QTest::keyClick(&canvas, Qt::Key_D, Qt::ControlModifier);
    check(std::abs(canvas.camera().y() - navStart.y() + (canvas.height() - 12) * 0.5) < 0.01, "Ctrl+D half page");
    QTest::keyClick(&canvas, Qt::Key_U, Qt::ControlModifier);
    check(std::abs(canvas.camera().y() - navStart.y()) < 0.01, "Ctrl+U half page");
    QTest::mouseClick(&canvas, Qt::MiddleButton, Qt::NoModifier, QPoint(250, 250));
    check(canvas.autoscrolling(), "middle click latches autoscroll");
    const auto autoStart = canvas.camera();
    QTest::qWait(40);
    check(canvas.camera() == autoStart, "autoscroll dead zone");
    QTest::mouseMove(&canvas, QPoint(340, 450));
    QTest::qWait(100);
    check(canvas.camera().y() < autoStart.y() - 100 && canvas.camera().x() < autoStart.x(), "fast two-axis autoscroll");
    QTest::keyClick(&canvas, Qt::Key_Escape);
    check(!canvas.autoscrolling(), "Escape cancels autoscroll");
    const auto stopped = canvas.camera(); QTest::qWait(40);
    check(canvas.camera() == stopped, "autoscroll timer stops");
    QTest::mouseClick(&canvas, Qt::MiddleButton, Qt::NoModifier, QPoint(250, 250));
    QTest::mouseClick(&canvas, Qt::LeftButton, Qt::NoModifier, QPoint(250, 250));
    check(!canvas.autoscrolling(), "click cancels autoscroll");
    QTest::mousePress(&canvas, Qt::MiddleButton, Qt::NoModifier, QPoint(250, 250));
    QTest::mouseMove(&canvas, QPoint(250, 350));
    QTest::mouseRelease(&canvas, Qt::MiddleButton, Qt::NoModifier, QPoint(250, 350));
    check(!canvas.autoscrolling(), "middle hold and release cancels autoscroll");
    QTest::mouseClick(&canvas, Qt::MiddleButton, Qt::NoModifier, QPoint(250, 250));
    QEvent deactivate(QEvent::WindowDeactivate);
    QApplication::sendEvent(&canvas, &deactivate);
    check(!canvas.autoscrolling(), "window deactivation cancels autoscroll");
    QTest::keyClick(&canvas, Qt::Key_Home);
    const double aTop = canvas.camera().y() + canvas.fileRect("a.cpp").top() * canvas.zoom();
    const double bTop = canvas.camera().y() + canvas.fileRect("nested/b.cpp").top() * canvas.zoom();
    check((aTop >= 0 && aTop < canvas.height()) || (bTop >= 0 && bTop < canvas.height()), "Home recovers nearby file headers");
    canvas.setDocuments({decode("a.txt", "one\n"), decode("b.txt", "two\n"), decode("deep/c.txt", "three\n")});
    QTest::keyClick(&canvas, Qt::Key_0);
    check(canvas.fileRect("a.txt").right() <= canvas.fileRect("b.txt").left()
          && canvas.fileRect("b.txt").top() == canvas.fileRect("a.txt").top(),
          "sibling files sit side by side beneath their directory");
    const auto fixedA = canvas.fileRect("a.txt"), fixedB = canvas.fileRect("b.txt");
    zoomTo(0.18);
    check(canvas.fileRect("a.txt").left() == fixedA.left() && canvas.fileRect("b.txt").left() == fixedB.left()
          && canvas.fileRect("a.txt").width() == fixedA.width() && canvas.fileRect("b.txt").width() == fixedB.width(),
          "zoom does not repack file columns");
    QTest::keyClick(&canvas, Qt::Key_T);
    check(canvas.isTreeView(), "T selects branch layout");
    QTest::keyClick(&canvas, Qt::Key_0);
    check(canvas.fileRect("a.txt").left() == canvas.fileRect("b.txt").left()
          && canvas.fileRect("b.txt").top() >= canvas.fileRect("a.txt").bottom(),
          "branch layout stacks short sibling files vertically");
    for (double z : {0.3, 0.05, 0.005}) {
        zoomTo(z);
        check(canvas.fileRect("b.txt").top() >= canvas.fileRect("a.txt").bottom(),
              "stacked titles and code never overlap at overview zoom");
    }
    for (double z : {0.05, 0.5, 1.0, 4.0}) {
        zoomTo(z);
        // World geometry avoids sticky-header positioning when the camera pans.
        const double fileTop = canvas.camera().y() + canvas.fileRect("a.txt").top() * canvas.zoom();
        const double junction = canvas.directoryBox("deep").top() - 8;
        check(std::abs(fileTop - junction - std::clamp(32 * z, 4.0, 48.0)) < 0.001,
              "file stems scale with zoom and remain below the clear connector fan");
    }
    QTest::keyClick(&canvas, Qt::Key_F); canvas.grab().save("/tmp/code-review-branch-layout.png");
    QTest::keyClick(&canvas, Qt::Key_T);
    check(!canvas.isTreeView() && canvas.fileRect("a.txt").right() <= canvas.fileRect("b.txt").left(),
          "T restores flame layout");
    Canvas stackedPairs(tmp.path()); stackedPairs.show(); QTest::qWait(60);
    auto narrowBefore = decode("a.txt", "small\n"), narrowAfter = narrowBefore;
    auto wideBefore = decode("b.txt", QByteArray(100, 'x') + "\n"), wideAfter = wideBefore;
    narrowBefore.comparisonSide = wideBefore.comparisonSide = -1;
    narrowAfter.comparisonSide = wideAfter.comparisonSide = 1;
    wideBefore.diff = true; wideBefore.rows[0].change = '-';
    stackedPairs.setDocuments({narrowBefore, narrowAfter, wideBefore, wideAfter});
    QTest::keyClick(&stackedPairs, Qt::Key_T); QTest::keyClick(&stackedPairs, Qt::Key_0);
    QWheelEvent panPair({600, 300}, {600, 300}, {-400, 0}, {}, Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QApplication::sendEvent(&stackedPairs, &panPair);
    const auto pairShot = stackedPairs.grab().toImage();
    const int bodyTop = int(stackedPairs.camera().y() + stackedPairs.contentRect("b.txt").top());
    int redCoverage = 0;
    for (int y = bodyTop; y < bodyTop + 20; ++y) for (int x = 10; x < 150; ++x)
        redCoverage += pairShot.pixelColor(x, y) == QColor("#fbe6e7");
    check(redCoverage > 100, "panning stacked unequal-width pairs retains the intersecting wide file");
    canvas.setDocuments({decode("narrow.txt", "short line\n"), decode("wide.txt", QByteArray(100, 'x'))});
    check(canvas.fileRect("narrow.txt").width() < canvas.fileRect("wide.txt").width() * 0.5,
          "columns compact based on actual content width");
    QTest::keyClick(&canvas, Qt::Key_F11);
    check(canvas.isFullScreen(), "F11 enters fullscreen");
    QTest::keyClick(&canvas, Qt::Key_F11);
    check(!canvas.isFullScreen(), "F11 exits fullscreen");

    QTemporaryDir repo;
    auto git = [&](QStringList args, const QString &directory = QString()) {
        QProcess process;
        process.setWorkingDirectory(directory.isEmpty() ? repo.path() : directory);
        process.start("git", args);
        check(process.waitForFinished(10000) && process.exitCode() == 0, "Git fixture command");
    };
    git({"init", "--quiet"});
    write(repo.filePath(".gitignore"), "build/\n*.log\n");
    write(repo.filePath("tracked.cpp"), "int tracked = 1;\n");
    write(repo.filePath("ignored.log"), "ignored even if tracked\n");
    git({"add", "tracked.cpp"});
    git({"add", "--force", "ignored.log"});
    QDir(repo.path()).mkpath("build");
    write(repo.filePath("build/output.cpp"), "generated\n");
    QDir(repo.path()).mkpath("vendor");
    write(repo.filePath("vendor/secret.cpp"), "submodule contents\n");
    git({"update-index", "--add", "--cacheinfo", "160000,1111111111111111111111111111111111111111,vendor"});
    QDir(repo.path()).mkpath("nested-repo");
    git({"init", "--quiet"}, repo.filePath("nested-repo"));
    write(repo.filePath("nested-repo/inside.cpp"), "nested repository\n");
    write(repo.filePath("new.cpp"), "int fresh = 2;\n");
    cancel = false;
    std::atomic_int progress{-2};
    const auto snapshot = loadDirectory(repo.path(), cancel, {}, &progress);
    check(progress == 100, "file loading reports completed progress");
    check(snapshot.size() == 3, "tracked and eligible untracked only; ignore rules and submodules excluded");
    const auto unchanged = loadDirectory(repo.path(), cancel, snapshot);
    check(unchanged.size() == snapshot.size() && unchanged[0].rows.constData() == snapshot[0].rows.constData(),
          "unchanged refresh shares indexed document storage");
    QDir(repo.path()).mkpath("subdir");
    write(repo.filePath("subdir/visible.cpp"), "int sub = 3;\n");
    write(repo.filePath("subdir/hidden.log"), "ignored by parent rules\n");
    check(loadDirectory(repo.filePath("subdir"), cancel).size() == 1, "subdirectory scan respects repository ignore rules");
    write(repo.filePath("data.bin"), QByteArray::fromHex("00010203"));

    Canvas live(repo.path()); live.show();
    check(live.findChild<QProgressBar *>("loadingProgress") != nullptr, "loading progress bar exists");
    auto waitFor = [&](auto condition) {
        QElapsedTimer deadline; deadline.start();
        while (!condition() && deadline.elapsed() < 5000) QTest::qWait(30);
        check(condition(), "automatic refresh condition");
    };
    waitFor([&] { return !live.fileRect("tracked.cpp").isEmpty(); });
    check(!live.findChild<QProgressBar *>("loadingProgress")->isVisible(), "loading progress hides after completion");
    const auto binaryRect = live.fileRect("data.bin");
    QTest::mouseClick(&live, Qt::LeftButton, Qt::NoModifier,
                     (live.camera() + binaryRect.topLeft() * live.zoom() + QPointF(10, 8)).toPoint());
    check(!live.isCollapsed("data.bin"), "expand live binary fixture");
    const double originalHeight = live.fileRect("tracked.cpp").height();
    const double originalZoom = live.zoom();
    write(repo.filePath("tracked.cpp"), QByteArray("int updated = 4;\n").repeated(40));
    waitFor([&] { return live.fileRect("tracked.cpp").height() > originalHeight; });
    check(live.zoom() == originalZoom, "automatic refresh preserves zoom");
    check(!live.isCollapsed("data.bin"), "automatic refresh preserves binary expansion");
    write(repo.filePath("arrived.cpp"), "int added = 5;\n");
    waitFor([&] { return !live.fileRect("arrived.cpp").isEmpty(); });
    check(QFile::rename(repo.filePath("arrived.cpp"), repo.filePath("renamed.cpp")), "atomic rename fixture");
    waitFor([&] { return live.fileRect("arrived.cpp").isEmpty() && !live.fileRect("renamed.cpp").isEmpty(); });
    write(repo.filePath(".gitignore"), "build/\n*.log\nrenamed.cpp\n");
    waitFor([&] { return live.fileRect("renamed.cpp").isEmpty(); });
    check(QFile::remove(repo.filePath("new.cpp")), "delete fixture");
    waitFor([&] { return live.fileRect("new.cpp").isEmpty(); });
    QTest::mouseClick(&live, Qt::MiddleButton, Qt::NoModifier, QPoint(250, 250));
    QHelpEvent tooltip(QEvent::ToolTip, QPoint(250, 250), live.mapToGlobal(QPoint(250, 250)));
    QApplication::sendEvent(&live, &tooltip);
    check(live.autoscrolling(), "tooltip events cannot interrupt autoscroll");
    const double beforeEdit = live.fileRect("tracked.cpp").height();
    write(repo.filePath("tracked.cpp"), QByteArray("int updated = 5;\n").repeated(45));
    waitFor([&] { return live.fileRect("tracked.cpp").height() > beforeEdit; });
    check(live.autoscrolling(), "live refresh does not interrupt autoscroll");
    QTest::keyClick(&live, Qt::Key_Escape);
    QTemporaryDir diffRepo;
    auto diffGit = [&](QStringList args) { git(args, diffRepo.path()); };
    diffGit({"init", "--quiet"});
    QDir(diffRepo.path()).mkpath("sub");
    write(diffRepo.filePath(".gitignore"), "*.log\n");
    write(diffRepo.filePath("changed.cpp"), "int value = 1;\n");
    write(diffRepo.filePath("deleted.txt"), "removed line\n");
    write(diffRepo.filePath("unchanged.txt"), "stable\n");
    write(diffRepo.filePath("type.txt"), "regular file\n");
    write(diffRepo.filePath("sub/odd name\n.cpp"), "old\n");
    write(diffRepo.filePath("binary.bin"), QByteArray::fromHex("000102"));
    diffGit({"add", "."});
    diffGit({"-c", "user.name=Viewer Test", "-c", "user.email=viewer@example.invalid", "commit", "-qm", "base"});
    write(diffRepo.filePath("changed.cpp"), "int staged = 2;\n");
    diffGit({"add", "changed.cpp"});
    write(diffRepo.filePath("changed.cpp"), "int unstaged = 3;\n");
    check(QFile::remove(diffRepo.filePath("deleted.txt")), "diff delete fixture");
    write(diffRepo.filePath("sub/odd name\n.cpp"), "new\n");
    write(diffRepo.filePath("binary.bin"), QByteArray::fromHex("000104"));
    write(diffRepo.filePath("new.txt"), "new file without newline");
    write(diffRepo.filePath("hidden.log"), "ignored\n");
    check(QFile::remove(diffRepo.filePath("type.txt")) && QFile::link("unchanged.txt", diffRepo.filePath("type.txt")),
          "file to symbolic link fixture");
    auto differences = loadGitDiff(diffRepo.path(), cancel, {}, &progress);
    check(differences.size() == 6 && progress == 100, "diff contains only changed and new eligible files");
    auto findDiff = [&](const QString &path) -> const Document & {
        for (const auto &d : differences) if (d.path == path) return d;
        check(false, "expected diff document exists"); return differences.front();
    };
    check(findDiff("type.txt").text.contains("-regular file") && findDiff("type.txt").text.contains("+unchanged.txt"),
          "type changes retain both deletion and addition patch sections");
    const auto &changedDiff = findDiff("changed.cpp");
    check(changedDiff.text.contains("-int value = 1;") && changedDiff.text.contains("+int unstaged = 3;")
          && !changedDiff.text.contains("int staged = 2;"), "diff compares full working tree against HEAD");
    bool removedNumber = false, addedNumber = false;
    for (const auto &row : changedDiff.rows) {
        if (row.change == '-') removedNumber |= row.oldLine == 1 && row.newLine == -1;
        if (row.change == '+') addedNumber |= row.oldLine == -1 && row.newLine == 1;
    }
    check(removedNumber && addedNumber, "diff rows carry old and new source line numbers");
    check(!changedDiff.overviewColor.isEmpty(), "diff colors survive minimap LOD");
    check(findDiff("deleted.txt").text.contains("-removed line"), "deleted file remains in diff view");
    check(findDiff("sub/odd name\n.cpp").text.contains("+new"), "NUL-delimited diff metadata preserves unusual filenames");
    check(findDiff("binary.bin").text.contains("Binary files"), "binary changes have a readable summary");
    check(findDiff("new.txt").text.contains("+new file without newline"), "untracked content appears as additions");
    const auto diffAgain = loadGitDiff(diffRepo.path(), cancel, differences);
    check(diffAgain.front().rows.constData() == differences.front().rows.constData(), "unchanged diff documents reuse cached indices");
    const auto subDiff = loadGitDiff(diffRepo.filePath("sub"), cancel);
    check(subDiff.size() == 1 && subDiff.front().path == "odd name\n.cpp", "diff respects opened subdirectory scope");
    auto comparison = loadWorkspace(diffRepo.path(), cancel, {}, {true, true, false}, &progress);
    check(comparison.added == 4 && comparison.removed == 4 && comparison.changedFiles == 6,
          "global line counts include each changed source line once");
    const Document *oldCopy = nullptr, *newCopy = nullptr;
    for (const auto &d : comparison.rendered) if (d.path == "changed.cpp") {
        if (d.comparisonSide < 0) oldCopy = &d; else newCopy = &d;
    }
    check(oldCopy && newCopy && oldCopy->text.contains("int value = 1;") && newCopy->text.contains("int unstaged = 3;"),
          "split mode reconstructs actual before and after code");
    check(oldCopy->rowCount() == newCopy->rowCount() && oldCopy->added == 1 && oldCopy->removed == 1,
          "split rows align and per-file counts stay exact");
    const auto stableComparison = loadWorkspace(diffRepo.path(), cancel, comparison, {true, true, false}, &progress);
    check(stableComparison.rendered.front().rows.constData() == comparison.rendered.front().rows.constData(),
          "unchanged comparison renderings reuse indexed storage");
    WorkspaceSnapshot wrapped;
    const QByteArray longOld(230, 'x');
    wrapped.files = {decode("wrap.cpp", "start\nnew\nextra\nend\n")};
    wrapped.patches = {decode("wrap.cpp", "@@ -1,3 +1,4 @@\n start\n-" + longOld + "\n+new\n+extra\n end\n", {}, true)};
    wrapped = renderWorkspace(std::move(wrapped), {true, true, false}, cancel);
    check(wrapped.rendered.size() == 2 && wrapped.rendered[0].rowCount() == wrapped.rendered[1].rowCount(),
          "side-by-side alignment accounts for unequal visual wrapping");
    qsizetype oldEnd = -1, newEnd = -1;
    for (qsizetype r = 0; r < wrapped.rendered[0].rowCount(); ++r) {
        if (wrapped.rendered[0].rowText(r) == "end") oldEnd = r;
        if (wrapped.rendered[1].rowText(r) == "end") newEnd = r;
    }
    check(oldEnd >= 0 && oldEnd == newEnd, "matching code stays on the same row after insertions and wrapping");
    for (bool highlight : {false, true}) for (bool split : {false, true}) for (bool filtered : {false, true}) {
        const auto combination = renderWorkspace(comparison, {highlight, split, filtered}, cancel);
        check(combination.rendered.size() == size_t((filtered ? 6 : 8) * (split ? 2 : 1)),
              "all comparison toggle combinations retain the correct files and copies");
        check(combination.added == 4 && combination.removed == 4,
              "global totals do not double in split mode or change with filtering");
        for (const auto &d : combination.rendered) if (d.path == "type.txt" && highlight)
            check(!d.text.contains("diff --git"), "symbolic link type changes reconstruct real source instead of raw patches");
    }
    WorkspaceSnapshot contextFixture;
    QByteArray contextText;
    for (int i = 0; i < 30; ++i) contextText += (i == 15 ? QByteArray("CHANGED") : "line" + QByteArray::number(i)) + '\n';
    contextFixture.files = {decode("context.txt", contextText)};
    contextFixture.patches = {decode("context.txt", "@@ -15,3 +15,3 @@\n line14\n-line15\n+CHANGED\n line16\n", {}, true)};
    auto fullContext = renderWorkspace(contextFixture, {true, false, false}, cancel);
    auto foldedContext = renderWorkspace(contextFixture, {true, false, true}, cancel);
    check(fullContext.rendered.front().text.contains("line29") && fullContext.rendered.front().rowCount() >= 30,
          "unfiltered diff retains full unchanged source context");
    check(foldedContext.rendered.front().text.contains("unchanged lines") && foldedContext.rendered.front().rowCount() < 20,
          "changes-only mode collapses distant unchanged context");
    Canvas diffLive(diffRepo.path()); diffLive.show();
    waitFor([&] { return !diffLive.fileRect("unchanged.txt").isEmpty(); });
    QTest::keyClick(&diffLive, Qt::Key_Plus);
    const double normalZoom = diffLive.zoom();
    const auto normalFileRect = diffLive.fileRect("changed.cpp");
    QTest::keyClick(&diffLive, Qt::Key_D);
    waitFor([&] { return diffLive.isDiffView() && !diffLive.fileRect("deleted.txt").isEmpty(); });
    check(!diffLive.fileRect("unchanged.txt").isEmpty(), "diff highlighting retains unchanged files by default");
    check(diffLive.zoom() == normalZoom, "diff toggle preserves zoom");
    check(diffLive.fileRect("changed.cpp").left() == normalFileRect.left()
          && diffLive.fileRect("changed.cpp").width() == normalFileRect.width(),
          "diff toggle preserves file column positions and widths");
    for (const auto &name : {"diffToggle", "splitToggle", "changesToggle", "treeToggle"})
        check(diffLive.findChild<QToolButton *>(name)->text().contains('['), "each toggle displays its keyboard shortcut");
    check(diffLive.findChild<QLabel *>("globalInfo")->text().contains("+4"), "global additions appear at the top left");
    QTest::keyClick(&diffLive, Qt::Key_C);
    waitFor([&] { return diffLive.fileRect("unchanged.txt").isEmpty(); });
    check(diffLive.isChangesOnly(), "C explicitly hides unchanged files");
    QTest::keyClick(&diffLive, Qt::Key_C);
    waitFor([&] { return !diffLive.fileRect("unchanged.txt").isEmpty(); });
    QTest::keyClick(&diffLive, Qt::Key_S);
    waitFor([&] { return !diffLive.fileRect("changed.cpp", -1).isEmpty() && !diffLive.fileRect("changed.cpp", 1).isEmpty(); });
    check(diffLive.fileRect("changed.cpp", -1).right() <= diffLive.fileRect("changed.cpp", 1).left()
          && diffLive.fileRect("changed.cpp", -1).top() == diffLive.fileRect("changed.cpp", 1).top(),
          "S shows aligned HEAD and working file copies side by side");
    const auto sharedHeader = diffLive.fileHeaderRect("changed.cpp");
    check(std::abs(sharedHeader.left() - (diffLive.camera().x() + diffLive.fileRect("changed.cpp", -1).left() * diffLive.zoom())) < 0.001
          && std::abs(sharedHeader.right() - (diffLive.camera().x() + diffLive.fileRect("changed.cpp", 1).right() * diffLive.zoom())) < 0.001,
          "split copies share one name and counts header spanning the pair");
    QTest::keyClick(&diffLive, Qt::Key_F); diffLive.grab().save("/tmp/code-review-split-diff.png");
    QTest::keyClick(&diffLive, Qt::Key_T); QTest::keyClick(&diffLive, Qt::Key_F);
    diffLive.grab().save("/tmp/code-review-split-tree.png");
    check(diffLive.fileRect("changed.cpp", -1).top() == diffLive.fileRect("changed.cpp", 1).top(),
          "tree packing keeps comparison pairs together");
    QTest::keyClick(&diffLive, Qt::Key_T);
    QTest::keyClick(&diffLive, Qt::Key_S);
    waitFor([&] { return diffLive.fileRect("changed.cpp", -1).isEmpty(); });
    QTest::keyClick(&diffLive, Qt::Key_0);
    diffLive.grab().save("/tmp/code-review-git-diff.png");
    const double diffHeight = diffLive.fileRect("changed.cpp").height();
    write(diffRepo.filePath("changed.cpp"), QByteArray("int live = 4;\n").repeated(20));
    waitFor([&] { return diffLive.fileRect("changed.cpp").height() > diffHeight; });
    QTest::mouseClick(diffLive.findChild<QToolButton *>("diffToggle"), Qt::LeftButton);
    check(!diffLive.isDiffView() && diffLive.zoom() == 1,
          "diff button toggles independently without resetting the camera zoom");
    check(!diffLive.fileRect("unchanged.txt").isEmpty(), "normal view restores unchanged files");
    // HEAD advances while the application is open; staged/unstaged distinctions
    // must not leave a stale diff after all current contents are committed.
    diffGit({"add", "--all"});
    diffGit({"-c", "user.name=Viewer Test", "-c", "user.email=viewer@example.invalid", "commit", "-qm", "current"});
    QTest::keyClick(&diffLive, Qt::Key_D);
    QTest::keyClick(&diffLive, Qt::Key_C);
    waitFor([&] { return diffLive.fileRect("changed.cpp").isEmpty(); });
    check(diffLive.addedLines() == 0 && diffLive.removedLines() == 0, "global counts update when HEAD advances");
    check(loadGitDiff(diffRepo.path(), cancel).empty(), "clean HEAD produces an empty diff");
    diffGit({"rm", "--cached", "unchanged.txt"});
    const auto replacementDiff = loadGitDiff(diffRepo.path(), cancel);
    check(replacementDiff.size() == 1 && replacementDiff.front().path == "unchanged.txt"
          && replacementDiff.front().text.contains("-stable") && replacementDiff.front().text.contains("+stable"),
          "staged deletion with an untracked replacement shares one file column");
    check(loadGitDiff(diffRepo.path(), cancel, replacementDiff).size() == 1,
          "replacement diff cache does not duplicate columns");
    QTemporaryDir unborn;
    git({"init", "--quiet"}, unborn.path());
    write(unborn.filePath("first.txt"), "first\n");
    git({"add", "first.txt"}, unborn.path());
    check(loadGitDiff(unborn.path(), cancel).front().text.contains("+first"), "unborn repository shows initial files as additions");
    QTemporaryDir ordinary;
    check(loadGitDiff(ordinary.path(), cancel).front().text.contains("requires a Git working tree"),
          "non-repository diff has an explicit status");

}
