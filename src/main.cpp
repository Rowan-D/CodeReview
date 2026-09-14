#include "canvas.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <cstdio>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("code-review");
    QCommandLineParser parser;
    parser.setApplicationDescription("Read-only spatial directory viewer");
    parser.addHelpOption();
    parser.addPositionalArgument("directory", "Directory to open (defaults to current directory)", "[directory]");
    parser.process(app);
    const auto args = parser.positionalArguments();
    if (args.size() > 1) parser.showHelp(2);
    const QString root = QFileInfo(args.isEmpty() ? QDir::currentPath() : args.first()).absoluteFilePath();
    const QFileInfo info(root);
    if (!info.isDir() || !info.isReadable()) {
        fprintf(stderr, "Cannot open directory: %s\n", qPrintable(root));
        return 1;
    }
    Canvas canvas(root);
    canvas.show();
    return app.exec();
}
