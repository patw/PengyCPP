#include <QCoreApplication>
#include <QTextStream>
#include "webserver.h"

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    quint16 port = 5000;
    const QStringList args = app.arguments().mid(1);
    if (!args.isEmpty()) {
        bool ok;
        int p = args.first().toInt(&ok);
        if (ok && p > 0 && p < 65536) port = static_cast<quint16>(p);
    }

    WebServer server(port);
    if (!server.start()) {
        QTextStream(stderr) << "Failed to bind on port " << port << "\n";
        return 1;
    }

    QTextStream(stdout) << "Pengy web UI running at http://localhost:" << port << "\n";
    return app.exec();
}
