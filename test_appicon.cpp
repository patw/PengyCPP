#include "appicon.h"
#include <QApplication>
#include <QDebug>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const QIcon icon = pengyApplicationIcon();
    if (icon.isNull() || icon.pixmap(256, 256).isNull()) {
        qCritical() << "Bundled application icon failed to load";
        return 1;
    }
    // Must exercise the native Cocoa conversion, not just decode the PNG:
    // --version exits before QApplication and cannot catch this regression.
    app.setWindowIcon(icon);
    app.processEvents();
    return 0;
}
