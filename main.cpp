#include "config.h"
#include "mainwindow.h"
#include <QApplication>
#include <QFont>
#include <QFontDatabase>

int main(int argc, char* argv[]) {
    // Read ui_scale before creating QApplication so we can set the env var first
    Config cfg = configLoad();
    if (cfg.uiScale != 100) {
        qputenv("QT_SCALE_FACTOR",
                QByteArray::number(cfg.uiScale / 100.0, 'f', 2));
    }

    QApplication app(argc, argv);
    app.setApplicationName("Pengy");
    app.setOrganizationName("Pengy");

    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    font.setPointSize(10);
    app.setFont(font);

    MainWindow window;
    window.show();
    return app.exec();
}
