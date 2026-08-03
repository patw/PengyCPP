#include "config.h"
#include "mainwindow.h"
#include "version.h"
#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QTextStream>
#include "themehelper.h"

static void showHelp(const char* argv0) {
    QTextStream out(stdout);
    out << "Pengy v" << PENGY_VERSION << " — Local-first AI agent with tools (GUI)\n\n";
    out << "Usage: " << (argv0 ? argv0 : "pengy") << " [OPTIONS]\n\n";
    out << "Options:\n";
    out << "  -h, --help         Show this help message and exit.\n";
    out << "  -v, --version      Show version information and exit.\n";
    out << "  --config-dir PATH  Use a custom config directory.\n";
    out << "\n"
           "The desktop GUI launches a Qt6 window. No additional\n"
           "command-line options are supported.\n";
    out.flush();
}

int main(int argc, char* argv[]) {
    // Handle flags before creating QApplication
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromUtf8(argv[i]);
        if (arg == "-v" || arg == "--version") {
            QTextStream(stdout) << "Pengy v" << PENGY_VERSION << "\n";
            return 0;
        }
        if (arg == "-h" || arg == "--help") {
            showHelp(argv[0]);
            return 0;
        }
        if (arg == "--config-dir" && i + 1 < argc) {
            setConfigDir(QString::fromUtf8(argv[++i]));
        }
    }

    QApplication app(argc, argv);
    app.setApplicationName("Pengy");
    app.setOrganizationName("Pengy");
    app.setWindowIcon(QIcon(":/pengy.png"));

    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    font = scaledSystemFont(configLoad().uiScale);
    app.setFont(font);

    MainWindow window;
    window.show();
    return app.exec();
}
