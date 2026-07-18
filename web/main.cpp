#include <QCoreApplication>
#include <QTextStream>
#include "webserver.h"
#include "version.h"
#include "../config.h"

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    quint16 port = 5000;
    QString host = "127.0.0.1";
    const QStringList args = app.arguments().mid(1);
    for (int i = 0; i < args.size(); i++) {
        if (args[i] == "-v" || args[i] == "--version") {
            QTextStream(stdout) << "Pengy v" << PENGY_VERSION << "\n";
            return 0;
        } else if (args[i] == "-h" || args[i] == "--help") {
            QTextStream(stdout)
                << "Pengy web UI — chat with LLMs from your browser\n\n"
                << "Usage: pengy_web [PORT] [OPTIONS]\n\n"
                << "Arguments:\n"
                << "  PORT          Bind port (default: 5000)\n\n"
                << "Options:\n"
                << "  --host HOST     Bind host (default: 127.0.0.1). Pass\n"
                << "                 --host 0.0.0.0 to expose beyond localhost —\n"
                << "                 this app has no authentication and exposes\n"
                << "                 run_bash/run_python tools, so only do this\n"
                << "                 on a trusted network.\n"
                << "  --config-dir PATH  Use a custom config directory.\n"
                << "  -v, --version   Show version information and exit.\n"
                << "  -h, --help      Show this help message and exit\n";
            return 0;
        } else if (args[i] == "--host") {
            if (i + 1 < args.size()) {
                host = args[++i];
            }
        } else if (args[i] == "--config-dir" && i + 1 < args.size()) {
            setConfigDir(args[++i]);
        } else {
            bool ok;
            int p = args[i].toInt(&ok);
            if (ok && p > 0 && p < 65536) port = static_cast<quint16>(p);
        }
    }

    WebServer server(host, port);
    if (!server.start()) {
        QTextStream(stderr) << "Failed to bind on " << host << ":" << port << "\n";
        return 1;
    }

    QTextStream(stdout) << "Pengy web UI running at http://" << host << ":" << port << "\n";
    return app.exec();
}
