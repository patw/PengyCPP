#include <QCoreApplication>
#include <QTextStream>
#include "webserver.h"
#include "version.h"
#include "../config.h"

/// Report a command-line usage error and exit 2, matching the other frontends.
[[noreturn]] static void argError(const QString& msg) {
    QTextStream(stderr) << "error: " << msg << "\n"
                        << "Try 'pengy_web --help' for more information.\n";
    std::exit(2);
}

/// Consume the value following a flag, or fail if it is missing.
static QString requireValue(const QStringList& args, int& i, const QString& flag) {
    if (i + 1 >= args.size())
        argError(QString("option '%1' requires a value").arg(flag));
    return args[++i];
}

static quint16 parsePort(const QString& value) {
    bool ok = false;
    const int p = value.toInt(&ok);
    if (!ok || p <= 0 || p > 65535)
        argError(QString("invalid port '%1' (expected 1-65535)").arg(value));
    return static_cast<quint16>(p);
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    quint16 port = 5000;
    QString host = "127.0.0.1";
    QStringList trustedHosts;
    const QStringList args = app.arguments().mid(1);
    for (int i = 0; i < args.size(); i++) {
        if (args[i] == "-v" || args[i] == "--version") {
            QTextStream(stdout) << "Pengy v" << PENGY_VERSION << "\n";
            return 0;
        } else if (args[i] == "-h" || args[i] == "--help") {
            QTextStream(stdout)
                << "Pengy web UI — chat with LLMs from your browser\n\n"
                << "Usage: pengy_web [OPTIONS]\n\n"
                << "Options:\n"
                << "  --port PORT     Bind port (default: 5000).\n"
                << "  --host HOST     Bind host (default: 127.0.0.1). Pass\n"
                << "                 --host 0.0.0.0 to expose beyond localhost —\n"
                << "                 this app has no authentication and exposes\n"
                << "                 run_bash/run_python tools, so only do this\n"
                << "                 on a trusted network.\n"
                << "  --trusted-host HOST  Public hostname this server is\n"
                << "                 reached as when behind a reverse proxy\n"
                << "                 (e.g. pengy.example). Repeatable. Needed\n"
                << "                 only for a proxy in front of a loopback bind.\n"
                << "  --config-dir PATH  Use a custom config directory.\n"
                << "  -v, --version   Show version information and exit.\n"
                << "  -h, --help      Show this help message and exit\n";
            return 0;
        } else if (args[i] == "--host") {
            host = requireValue(args, i, "--host");
        } else if (args[i] == "--port") {
            port = parsePort(requireValue(args, i, "--port"));
        } else if (args[i] == "--trusted-host") {
            trustedHosts << requireValue(args, i, "--trusted-host");
        } else if (args[i] == "--config-dir") {
            setConfigDir(requireValue(args, i, "--config-dir"));
        } else if (args[i].startsWith('-')) {
            // Unrecognised flags used to be discarded silently, so a typo like
            // --prot 8080 started the server on defaults.
            argError(QString("unknown option '%1'").arg(args[i]));
        } else {
            // Bare number: the legacy positional PORT, still accepted.
            port = parsePort(args[i]);
        }
    }

    WebServer server(host, port);
    server.setTrustedHosts(trustedHosts);
    if (!server.start()) {
        QTextStream(stderr) << "Failed to bind on " << host << ":" << port << "\n";
        return 1;
    }

    QTextStream(stdout) << "Pengy web UI running at http://" << host << ":" << port << "\n";
    if (host != "127.0.0.1" && host != "localhost" && host != "::1")
        QTextStream(stdout) << "  note: bound beyond loopback — Pengy Web has no auth of its "
                               "own, so put it behind a proxy or a VM boundary.\n";
    return app.exec();
}
