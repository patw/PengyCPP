#include "tools.h"
#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QRegularExpression>
#include <QProcess>
#include <QStandardPaths>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>
#include <QTemporaryFile>
#include <QMutex>
#include <QSet>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QUuid>
#include <QCoreApplication>
#include <QThread>
#include <QUrlQuery>

#ifdef Q_OS_UNIX
#include <unistd.h>
#include <signal.h>
#endif

namespace Tools {

static QString   g_userAgent = "PengyAgent/1.0";
static int       g_timeout   = 60;
static QMutex    g_mutex;

// ── Sudo password provider (installed by the GUI before each LLM run) ─
static Tools::SudoPasswordFn g_sudoProvider;
static QString               g_cachedSudoPassword;
static QMutex                g_sudoMutex;

// ── Active process group tracking ─────────────────────────────────────
static QSet<qint64>  g_activeProcessGroups;
static QMutex        g_processMutex;

// ── Rate limiter for web searches ─────────────────────────────────────
static QElapsedTimer g_lastSearchTimer;
static bool          g_lastSearchTimerStarted = false;
static QMutex        g_searchTimerMutex;

void setSudoPasswordProvider(SudoPasswordFn fn) {
    QMutexLocker lock(&g_sudoMutex);
    g_sudoProvider = std::move(fn);
    g_cachedSudoPassword.clear();
}
void clearSudoPasswordProvider() {
    QMutexLocker lock(&g_sudoMutex);
    g_sudoProvider = nullptr;
    g_cachedSudoPassword.clear();
}

void setUserAgent(const QString& ua) {
    QMutexLocker lock(&g_mutex);
    g_userAgent = ua;
}
void setTimeout(int secs) {
    QMutexLocker lock(&g_mutex);
    g_timeout = secs;
}
static QString userAgent() {
    QMutexLocker lock(&g_mutex);
    return g_userAgent;
}
static int toolTimeout() {
    QMutexLocker lock(&g_mutex);
    return g_timeout;
}

// ── Process group management ──────────────────────────────────────────

static void registerProcess(qint64 pid) {
    QMutexLocker lock(&g_processMutex);
    g_activeProcessGroups.insert(pid);
}

static void unregisterProcess(qint64 pid) {
    QMutexLocker lock(&g_processMutex);
    g_activeProcessGroups.remove(pid);
}

static void terminateProcessGroup(qint64 pid) {
#ifdef Q_OS_UNIX
    QProcess::execute("kill", {"-9", QString("-%1").arg(pid)});
#else
    QProcess::execute("taskkill", {"/PID", QString::number(pid), "/T", "/F"});
#endif
}

void killActiveProcesses() {
    QMutexLocker lock(&g_processMutex);
    for (qint64 pid : g_activeProcessGroups) {
        terminateProcessGroup(pid);
    }
    g_activeProcessGroups.clear();
}

// ── Tool schema helpers ───────────────────────────────────────────────

static QJsonObject prop(const QString& type, const QString& desc) {
    return QJsonObject{{"type", type}, {"description", desc}};
}

static QJsonObject td(const QString& name, const QString& desc,
                       const QJsonObject& props, const QJsonArray& required) {
    return QJsonObject{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", name},
            {"description", desc},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", props},
                {"required", required}
            }}
        }}
    };
}

QJsonArray toolDefinitions() {
    return QJsonArray{
        td("read_file", "Read the contents of a file",
            QJsonObject{{"path", prop("string", "The file path to read")}},
            QJsonArray{"path"}),

        td("write_file", "Write content to a file",
            QJsonObject{
                {"path",    prop("string", "The file path to write to")},
                {"content", prop("string", "The content to write to the file")}},
            QJsonArray{"path", "content"}),

        td("replace_in_file",
            "Perform an exact string replacement in an existing file. "
            "The old_str must match exactly one occurrence.",
            QJsonObject{
                {"path",    prop("string", "The file path to edit")},
                {"old_str", prop("string", "The exact text to find and replace. Must match exactly one location.")},
                {"new_str", prop("string", "The text to replace it with. Use empty string to delete.")}},
            QJsonArray{"path", "old_str", "new_str"}),

        td("run_bash", "Run a bash command in the terminal",
            QJsonObject{{"command", prop("string", "The bash command to execute")}},
            QJsonArray{"command"}),

        td("web_search",
            "Search the web using metasearch across multiple backends "
            "(Brave, DuckDuckGo, Mojeek, Yahoo, Google, Startpage, Yandex)",
            QJsonObject{
                {"query",       prop("string",  "The search query")},
                {"max_results", prop("integer", "Maximum number of results to return (default: 5)")}},
            QJsonArray{"query"}),

        td("download_file", "Download a file from a URL to the user's Downloads directory",
            QJsonObject{
                {"url",      prop("string", "The URL of the file to download")},
                {"filename", prop("string", "Optional filename to save as")}},
            QJsonArray{"url"}),

        td("fetch_url", "Fetch the text content of a URL into the context window",
            QJsonObject{{"url", prop("string", "The URL to fetch")}},
            QJsonArray{"url"}),

        td("run_python", "Execute Python code",
            QJsonObject{{"code", prop("string", "The Python code to execute")}},
            QJsonArray{"code"}),

        td("directory_tree",
            "Show a visual tree of the directory structure. "
            "Skips common noise directories like .git, node_modules, __pycache__ by default.",
            QJsonObject{
                {"path",        prop("string",  "The directory path to show the tree for")},
                {"max_depth",   prop("integer", "Maximum depth to recurse (default: 3)")},
                {"show_hidden", prop("boolean", "Whether to show hidden files/directories (default: false)")}},
            QJsonArray{"path"}),

        td("read_multiple_files", "Read multiple files at once, returning each with a clear header.",
            QJsonObject{{"paths", prop("array", "List of file paths to read")}},
            QJsonArray{"paths"}),

        td("search_content",
            "Search for a regex pattern in files under a directory. "
            "Returns matching lines with file path, line number, and optional surrounding context.",
            QJsonObject{
                {"pattern",       prop("string",  "The regex pattern to search for")},
                {"path",          prop("string",  "The directory or file to search in")},
                {"file_glob",     prop("string",  "Optional glob to filter files")},
                {"context_lines", prop("integer", "Number of lines of context (default: 0)")},
                {"max_results",   prop("integer", "Maximum number of matches to return (default: 50)")}},
            QJsonArray{"pattern", "path"}),
    };
}

bool isReadOnly(const QString& name) {
    static const QSet<QString> ro{
        "read_file", "read_multiple_files", "directory_tree",
        "search_content", "web_search", "fetch_url"
    };
    return ro.contains(name);
}

// ── Argument helpers ─────────────────────────────────────────────────

static QString aStr(const QJsonObject& a, const QString& k, const QString& def = {}) {
    return a.value(k).toString(def);
}
static int aInt(const QJsonObject& a, const QString& k, int def = 0) {
    auto v = a.value(k);
    if (v.isDouble()) return (int)v.toDouble();
    return def;
}
static bool aBool(const QJsonObject& a, const QString& k, bool def = false) {
    auto v = a.value(k);
    if (v.isBool()) return v.toBool();
    return def;
}

// ── Path helper ───────────────────────────────────────────────────────

static QString expandHome(const QString& path) {
    if (path.startsWith("~/")) {
        return QDir::homePath() + path.mid(1);
    }
    if (path == "~") return QDir::homePath();
    return path;
}

// ── Temp file output helpers ──────────────────────────────────────────

struct TempOutputFiles {
    QString stdoutPath;
    QString stderrPath;
    bool valid = false;
};

static TempOutputFiles createOutputFiles(const QString& prefix) {
    TempOutputFiles f;
    qint64 nanos = QDateTime::currentMSecsSinceEpoch();
    qint64 pid   = QCoreApplication::applicationPid();
    f.stdoutPath = QDir::tempPath() + QString("/pengy_%1_%2_%3.out").arg(prefix).arg(pid).arg(nanos);
    f.stderrPath = QDir::tempPath() + QString("/pengy_%1_%2_%3.err").arg(prefix).arg(pid).arg(nanos);
    QFile outFile(f.stdoutPath);
    QFile errFile(f.stderrPath);
    f.valid = outFile.open(QIODevice::WriteOnly) && errFile.open(QIODevice::WriteOnly);
    return f;
}

static QString readAndRemove(const QString& path) {
    QFile f(path);
    QString text;
    if (f.open(QIODevice::ReadOnly)) {
        text = QString::fromUtf8(f.readAll());
        f.close();
    }
    QFile::remove(path);
    return text;
}

static void removeOutputFiles(const TempOutputFiles& f) {
    QFile::remove(f.stdoutPath);
    QFile::remove(f.stderrPath);
}

// ── Synchronous HTTP helpers ─────────────────────────────────────────

static QByteArray httpGet(const QUrl& url, const QString& ua, int timeoutMs = 30000) {
    QNetworkAccessManager mgr;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, ua);
    req.setTransferTimeout(timeoutMs);

    QNetworkReply* reply = mgr.get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    QByteArray data;
    if (reply->error() == QNetworkReply::NoError)
        data = reply->readAll();
    reply->deleteLater();
    return data;
}

static QByteArray httpGetWithRedirect(const QUrl& startUrl, const QString& ua, int timeoutMs = 30000) {
    QNetworkAccessManager mgr;
    mgr.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkRequest req(startUrl);
    req.setHeader(QNetworkRequest::UserAgentHeader, ua);
    req.setTransferTimeout(timeoutMs);

    QNetworkReply* reply = mgr.get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    QByteArray data;
    if (reply->error() == QNetworkReply::NoError)
        data = reply->readAll();
    reply->deleteLater();
    return data;
}

// ── HTML utilities ───────────────────────────────────────────────────

static QString decodeEntities(QString s) {
    s.replace("&amp;",  "&");
    s.replace("&lt;",   "<");
    s.replace("&gt;",   ">");
    s.replace("&quot;", "\"");
    s.replace("&apos;", "'");
    s.replace("&nbsp;", " ");
    s.replace("&#39;",  "'");
    s.replace("&#x27;", "'");
    s.remove(QRegularExpression("&#\\d+;"));
    return s;
}

static QString stripTags(const QString& html) {
    QString s = html;
    s.remove(QRegularExpression("<script[^>]*>[\\s\\S]*?</script>",
                                QRegularExpression::CaseInsensitiveOption));
    s.remove(QRegularExpression("<style[^>]*>[\\s\\S]*?</style>",
                                QRegularExpression::CaseInsensitiveOption));
    s.remove(QRegularExpression("<[^>]+>"));
    return decodeEntities(s).trimmed();
}

static QString extractByClass(const QString& html, const QString& cls) {
    static QRegularExpression tagRx(
        "<[a-zA-Z][^>]*class=\"[^\"]*\\b%1\\b[^\"]*\"[^>]*>([\\s\\S]*?)</[a-zA-Z]+>",
        QRegularExpression::MultilineOption);

    QRegularExpression re(tagRx.pattern().replace("%1", QRegularExpression::escape(cls)));
    auto m = re.match(html);
    if (!m.hasMatch()) return {};
    return stripTags(m.captured(1)).trimmed();
}

static QString extractFirstHref(const QString& html) {
    static QRegularExpression rx(R"RE(<a[^>]+href="([^"]+)")RE");
    auto m = rx.match(html);
    return m.hasMatch() ? decodeEntities(m.captured(1)) : QString();
}

static QString extractFirstHrefByClass(const QString& html, const QString& cls) {
    QRegularExpression rx(
        "<a[^>]*class=\"[^\"]*\\b" + QRegularExpression::escape(cls) +
        "\\b[^\"]*\"[^>]*href=\"([^\"]+)\"");
    auto m = rx.match(html);
    if (m.hasMatch()) return decodeEntities(m.captured(1));
    QRegularExpression rx2(
        "<a[^>]*href=\"([^\"]+)\"[^>]*class=\"[^\"]*\\b" +
        QRegularExpression::escape(cls) + "\\b[^\"]*\"");
    auto m2 = rx2.match(html);
    return m2.hasMatch() ? decodeEntities(m2.captured(1)) : QString();
}

static QString extractTextByTag(const QString& html, const QString& tag) {
    QRegularExpression rx("<" + tag + "[^>]*>([\\s\\S]*?)</" + tag + ">",
                          QRegularExpression::CaseInsensitiveOption);
    auto m = rx.match(html);
    return m.hasMatch() ? stripTags(m.captured(1)).trimmed() : QString();
}

static QString normalizeSearchText(const QString& s) {
    QString result;
    for (const QChar& c : s) {
        if (!c.isNonCharacter() && c.category() != QChar::Other_Control)
            result += c;
    }
    return result.simplified();
}

static QString urldecode(const QString& s) {
    QString result;
    int i = 0;
    while (i < s.size()) {
        if (s[i] == '%' && i + 2 < s.size()) {
            bool ok;
            int byte = s.mid(i + 1, 2).toInt(&ok, 16);
            if (ok) {
                result += QChar(byte);
                i += 3;
                continue;
            }
        }
        if (s[i] == '+') {
            result += ' ';
        } else {
            result += s[i];
        }
        i++;
    }
    return result;
}

// ── Tool implementations ─────────────────────────────────────────────

static QString toolReadFile(const QJsonObject& args) {
    QString path = expandHome(aStr(args, "path"));
    if (path.isEmpty()) return "Error: path is required.";

    QFileInfo fi(path);
    if (!fi.exists())       return "Error: File not found: " + path;
    if (!fi.isFile())       return "Error: Not a file: " + path;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return "Error reading file: " + f.errorString();
    return QString::fromUtf8(f.readAll());
}

static QString toolWriteFile(const QJsonObject& args) {
    QString path    = expandHome(aStr(args, "path"));
    QString content = aStr(args, "content");
    if (path.isEmpty()) return "Error: path is required.";

    QFileInfo fi(path);
    QDir().mkpath(fi.dir().absolutePath());

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return "Error writing file: " + f.errorString();
    f.write(content.toUtf8());
    return "Successfully wrote to " + path;
}

static QString toolReplaceInFile(const QJsonObject& args) {
    QString path   = expandHome(aStr(args, "path"));
    QString oldStr = aStr(args, "old_str");
    QString newStr = aStr(args, "new_str");

    if (path.isEmpty())   return "Error: path is required.";
    if (oldStr.isEmpty()) return "Error: old_str is empty. You must provide the exact text to replace.";

    QFileInfo ffi(path);
    if (!ffi.exists())  return "Error: File not found: " + path;
    if (!ffi.isFile())  return "Error: Not a file: " + path;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return "Error reading file: " + f.errorString();
    QString content = QString::fromUtf8(f.readAll());
    f.close();

    int count = content.count(oldStr);
    if (count == 0) {
        return "Error: old_str not found in " + path +
               ".\n\nTip: read the file first to get the exact text.";
    }
    if (count > 1) {
        QList<int> lines;
        int pos = 0;
        for (int i = 0; i < count; ++i) {
            int idx = content.indexOf(oldStr, pos);
            lines.append(content.left(idx).count('\n') + 1);
            pos = idx + 1;
        }
        QStringList lineStrs;
        for (int l : lines) lineStrs.append(QString::number(l));
        return QString("Error: old_str matches %1 locations in %2.\n\n"
                       "Matches found on lines: [%3]\n\nMake old_str longer or more specific.")
               .arg(count).arg(path).arg(lineStrs.join(", "));
    }

    int oldLine  = content.left(content.indexOf(oldStr)).count('\n') + 1;
    int oldLines = oldStr.count('\n') + 1;
    int newLines = newStr.count('\n') + 1;

    content.replace(oldStr, newStr);

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return "Error writing file: " + out.errorString();
    out.write(content.toUtf8());

    return QString("✅ Successfully replaced in %1:\n   Lines %2–%3 → "
                   "%4 line(s) replaced with %5 line(s)")
           .arg(path).arg(oldLine).arg(oldLine + oldLines - 1)
           .arg(oldLines).arg(newLines);
}

// ── Bash (with temp file output & process groups) ────────────────────

static QString toolRunBash(const QJsonObject& args, std::atomic<bool>* cancel) {
    QString command = aStr(args, "command");
    if (command.isEmpty()) return "Error: command is required.";

    int timeoutSecs = toolTimeout();

    // ── sudo detection ──────────────────────────────────────────────
    static QRegularExpression sudoRx("\\bsudo\\b");
    static QRegularExpression sudoRewriteRx("\\bsudo\\b(?!\\s+-S)");
    bool needsSudo = sudoRx.match(command).hasMatch();

    if (needsSudo) {
        QMutexLocker lock(&g_sudoMutex);
        if (g_cachedSudoPassword.isEmpty()) {
            if (!g_sudoProvider) {
                return "Error: sudo detected but no password provider is configured.";
            }
            auto provider = g_sudoProvider;
            lock.unlock();
            QString pw = provider();
            lock.relock();
            if (pw.isEmpty()) {
                return "Cancelled: sudo password not provided.";
            }
            g_cachedSudoPassword = pw;
        }
        // Only rewrite the first eligible "sudo" (not already followed by -S);
        // the piped password is consumed once, so rewriting every occurrence
        // would leave later `sudo` invocations blocking on an interactive prompt.
        QRegularExpressionMatch m = sudoRewriteRx.match(command);
        if (m.hasMatch()) {
            command.replace(m.capturedStart(), m.capturedLength(), "sudo -S");
        }
    }

    // Create temp files for stdout/stderr to avoid pipe buffer deadlock
    auto tmpFiles = createOutputFiles("bash");
    if (!tmpFiles.valid) {
        return "Error: Could not create temp output files.";
    }

    QProcess proc;
    proc.setProgram("bash");
    proc.setArguments({"-c", command});
    proc.setStandardOutputFile(tmpFiles.stdoutPath);
    proc.setStandardErrorFile(tmpFiles.stderrPath);

#ifdef Q_OS_UNIX
    proc.setChildProcessModifier([]() {
        setsid();
    });
#endif

    proc.start();
    if (!proc.waitForStarted(5000)) {
        removeOutputFiles(tmpFiles);
        return "Error running command: " + proc.errorString();
    }

    qint64 pid = proc.processId();
    registerProcess(pid);

    if (needsSudo) {
        QMutexLocker lock(&g_sudoMutex);
        if (!g_cachedSudoPassword.isEmpty()) {
            proc.write((g_cachedSudoPassword + "\n").toUtf8());
        }
    }
    proc.closeWriteChannel();

    int waitMs = timeoutSecs > 0 ? timeoutSecs * 1000 : -1;

    if (cancel) {
        int elapsed = 0;
        int step    = 100;
        while (!proc.waitForFinished(step)) {
            if (cancel->load()) {
                terminateProcessGroup(pid);
                proc.kill();
                proc.waitForFinished(2000);
                unregisterProcess(pid);
                removeOutputFiles(tmpFiles);
                return "Error: Command was cancelled.";
            }
            elapsed += step;
            if (waitMs > 0 && elapsed >= waitMs) {
                terminateProcessGroup(pid);
                proc.kill();
                proc.waitForFinished(2000);
                unregisterProcess(pid);
                QString out = readAndRemove(tmpFiles.stdoutPath);
                QString err = readAndRemove(tmpFiles.stderrPath);
                QString result = out;
                if (!err.isEmpty()) {
                    result += "\n" + err;
                }
                result += QString("\n\nError: Command timed out after %1 seconds.").arg(timeoutSecs);
                return result.trimmed();
            }
        }
    } else {
        if (!proc.waitForFinished(waitMs)) {
            terminateProcessGroup(pid);
            proc.kill();
            proc.waitForFinished(2000);
            unregisterProcess(pid);
            removeOutputFiles(tmpFiles);
            return QString("Error: Command timed out after %1 seconds.").arg(timeoutSecs);
        }
    }

    unregisterProcess(pid);

    QString out = readAndRemove(tmpFiles.stdoutPath);
    QString err = readAndRemove(tmpFiles.stderrPath);

    // Strip sudo password prompt lines from stderr only
    err.remove(QRegularExpression("^\\[sudo[^\\]]*\\].*\\n?", QRegularExpression::MultilineOption));
    err = err.trimmed();

    if (!err.isEmpty()) {
        out += "\n" + err;
    }

    if (proc.exitCode() != 0)
        out += QString("\n[Exit code: %1]").arg(proc.exitCode());

    return out.trimmed().isEmpty() ? "(No output)" : out;
}

// ── Web search metasearch ────────────────────────────────────────────

struct WebSearchHit {
    QString title;
    QString href;
    QString body;
    QString engine;
};

static QString searchBrowserUa() {
    return "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
           "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/146.0.0.0 Safari/537.36";
}

static QString googleMobileUa() {
    return "Mozilla/5.0 (Linux; Android 8.0; Pixel 2 Build/OPD3.170816.012) "
           "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/56.0.2924.1880 "
           "Mobile Safari/537.36NST^WV";
}

// Find positions of all blocks matching an attribute pattern
static QList<int> findBlockPositions(const QString& html, const QString& attrPattern) {
    QRegularExpression rx(attrPattern);
    QList<int> positions;
    auto it = rx.globalMatch(html);
    while (it.hasNext()) {
        auto m = it.next();
        positions.append(m.capturedStart());
    }
    return positions;
}

static QString blockBetween(const QString& html, int start, int end) {
    return html.mid(start, qMin(end - start, 8000));
}

// ── Individual search backends ───────────────────────────────────────

static QList<WebSearchHit> parseBrave(const QString& html, int maxResults) {
    QList<WebSearchHit> hits;
    auto positions = findBlockPositions(html, R"(data-type=["']web["'])");

    for (int i = 0; i < positions.size() && hits.size() < maxResults; ++i) {
        int end = (i + 1 < positions.size()) ? positions[i + 1] : html.size();
        QString block = blockBetween(html, positions[i], end);

        QString title = extractByClass(block, "title");
        if (title.isEmpty()) title = extractByClass(block, "sitename-container");
        if (title.isEmpty()) continue;

        QString href = extractFirstHref(block);
        if (!href.startsWith("http")) continue;

        QString body = extractByClass(block, "content");
        if (body.isEmpty()) body = extractByClass(block, "snippet");

        hits.append({normalizeSearchText(title), href, normalizeSearchText(body), "brave"});
    }
    return hits;
}

static QList<WebSearchHit> parseDDG(const QString& html, int maxResults) {
    QList<WebSearchHit> hits;
    if (html.size() < 5000) return hits;

    int pos = 0;
    while (hits.size() < maxResults) {
        int rStart = html.indexOf("class=\"result", pos);
        if (rStart == -1) break;

        int divStart = html.lastIndexOf('<', rStart);
        if (divStart == -1) { pos = rStart + 1; continue; }

        int nextResult = html.indexOf("class=\"result", rStart + 13);
        QString block = (nextResult > 0)
            ? html.mid(divStart, nextResult - divStart)
            : html.mid(divStart, 5000);

        if (block.contains("result--ad")) {
            pos = (nextResult > 0) ? nextResult : html.size();
            continue;
        }

        QString title = extractByClass(block, "result__a");
        if (title.isEmpty()) title = extractByClass(block, "result__title");
        if (title.isEmpty()) {
            pos = (nextResult > 0) ? nextResult : html.size();
            continue;
        }

        QString href = extractFirstHrefByClass(block, "result__a");
        if (href.contains("uddg=")) {
            int uddgPos = href.indexOf("uddg=");
            href = urldecode(href.mid(uddgPos + 5));
        }
        if (href.contains("duckduckgo.com/y.js")) {
            pos = (nextResult > 0) ? nextResult : html.size();
            continue;
        }

        QString snippet = extractByClass(block, "result__snippet");

        hits.append({normalizeSearchText(title), href, normalizeSearchText(snippet), "duckduckgo"});
        pos = (nextResult > 0) ? nextResult : html.size();
    }
    return hits;
}

static QList<WebSearchHit> parseMojeek(const QString& html, int maxResults) {
    QList<WebSearchHit> hits;
    QRegularExpression liRx(R"(<li\b[^>]*>([\s\S]*?)</li>)",
                            QRegularExpression::CaseInsensitiveOption);
    auto it = liRx.globalMatch(html);
    while (it.hasNext() && hits.size() < maxResults) {
        auto m = it.next();
        QString block = m.captured(1);

        QString title = extractByClass(block, "title");
        if (title.isEmpty()) title = extractTextByTag(block, "h2");
        if (title.isEmpty()) continue;

        QString href = extractFirstHrefByClass(block, "title");
        if (href.isEmpty()) {
            QRegularExpression rx(R"RE(<h2[^>]*>[\s\S]*?<a[^>]+href="([^"]+)")RE");
            auto hm = rx.match(block);
            if (hm.hasMatch()) href = decodeEntities(hm.captured(1));
        }

        QString body = extractByClass(block, "s");

        if (!title.isEmpty()) {
            hits.append({normalizeSearchText(title), href, normalizeSearchText(body), "mojeek"});
        }
    }
    return hits;
}

static QString extractYahooUrl(const QString& raw) {
    int ruPos = raw.indexOf("/RU=");
    if (ruPos >= 0) {
        QString rest = raw.mid(ruPos + 4);
        int rkPos = rest.indexOf("/RK=");
        int rsPos = rest.indexOf("/RS=");
        int endPos = rest.size();
        if (rkPos >= 0) endPos = qMin(endPos, rkPos);
        if (rsPos >= 0) endPos = qMin(endPos, rsPos);
        return urldecode(rest.left(endPos));
    }
    return raw;
}

static QList<WebSearchHit> parseYahoo(const QString& html, int maxResults) {
    QList<WebSearchHit> hits;
    auto positions = findBlockPositions(html, R"(class="[^"]*relsrch[^"]*")");

    for (int i = 0; i < positions.size() && hits.size() < maxResults; ++i) {
        int end = (i + 1 < positions.size()) ? positions[i + 1] : html.size();
        QString block = blockBetween(html, positions[i], end);

        QString title = extractTextByTag(block, "h3");
        if (title.isEmpty()) continue;

        QString href = extractFirstHref(block);
        href = extractYahooUrl(href);
        if (!href.startsWith("http")) continue;

        QString body;
        QRegularExpression pRx("<p[^>]*>([\\s\\S]*?)</p>", QRegularExpression::CaseInsensitiveOption);
        auto pm = pRx.match(block);
        if (pm.hasMatch()) body = stripTags(pm.captured(1));

        hits.append({normalizeSearchText(title), href, normalizeSearchText(body), "yahoo"});
    }
    return hits;
}

static QList<WebSearchHit> parseGoogle(const QString& html, int maxResults) {
    QList<WebSearchHit> hits;
    auto positions = findBlockPositions(html, R"(data-hveid=)");

    for (int i = 0; i < positions.size() && hits.size() < maxResults; ++i) {
        int end = (i + 1 < positions.size()) ? positions[i + 1] : html.size();
        QString block = blockBetween(html, positions[i], end);

        QString title = extractTextByTag(block, "h3");
        if (title.isEmpty()) continue;

        // Extract href - look for /url?q= or direct http links
        QString href;
        QRegularExpression hrefRx(R"RE(<a[^>]+href="([^"]+)")RE");
        auto hIt = hrefRx.globalMatch(block);
        while (hIt.hasNext()) {
            auto hm = hIt.next();
            QString h = decodeEntities(hm.captured(1));
            if (h.startsWith("/url?q=") || h.startsWith("http")) {
                href = h;
                break;
            }
        }
        if (href.startsWith("/url?q=")) {
            href = href.mid(7); // skip "/url?q="
            int ampPos = href.indexOf('&');
            if (ampPos >= 0) href = href.left(ampPos);
            href = urldecode(href);
        }
        if (!href.startsWith("http")) continue;

        // Body: all text in the block minus the title
        QString allText = normalizeSearchText(stripTags(block));
        QString body = allText;
        int titlePos = body.indexOf(title);
        if (titlePos >= 0) {
            body = body.mid(titlePos + title.size()).trimmed();
        }

        hits.append({normalizeSearchText(title), href, body, "google"});
    }
    return hits;
}

static QList<WebSearchHit> parseStartpage(const QString& html, int maxResults) {
    QList<WebSearchHit> hits;
    auto positions = findBlockPositions(html, R"(class="[^"]*\bresult\b[^"]*")");

    for (int i = 0; i < positions.size() && hits.size() < maxResults; ++i) {
        int end = (i + 1 < positions.size()) ? positions[i + 1] : html.size();
        QString block = blockBetween(html, positions[i], end);

        QString title = extractTextByTag(block, "h2");
        if (title.isEmpty()) title = extractTextByTag(block, "h3");
        if (title.isEmpty()) continue;

        QString href = extractFirstHref(block);
        if (!href.startsWith("http")) continue;

        QString body;
        QRegularExpression pRx("<p[^>]*>([\\s\\S]*?)</p>", QRegularExpression::CaseInsensitiveOption);
        auto pm = pRx.match(block);
        if (pm.hasMatch()) body = stripTags(pm.captured(1));

        hits.append({normalizeSearchText(title), href, normalizeSearchText(body), "startpage"});
    }
    return hits;
}

static QList<WebSearchHit> parseYandex(const QString& html, int maxResults) {
    QList<WebSearchHit> hits;
    auto positions = findBlockPositions(html, R"(class="[^"]*serp-item[^"]*")");

    for (int i = 0; i < positions.size() && hits.size() < maxResults; ++i) {
        int end = (i + 1 < positions.size()) ? positions[i + 1] : html.size();
        QString block = blockBetween(html, positions[i], end);

        QString title = extractTextByTag(block, "h3");
        if (title.isEmpty()) continue;

        QString href;
        QRegularExpression rx(R"RE(<h3[^>]*>[\s\S]*?<a[^>]+href="([^"]+)")RE");
        auto hm = rx.match(block);
        if (hm.hasMatch()) {
            href = decodeEntities(hm.captured(1));
        } else {
            href = extractFirstHref(block);
        }
        if (!href.startsWith("http")) continue;

        QString body = extractByClass(block, "text");

        hits.append({normalizeSearchText(title), href, normalizeSearchText(body), "yandex"});
    }
    return hits;
}

// ── Dedup, ranking, and formatting ───────────────────────────────────

static QStringList queryTokens(const QString& query) {
    QStringList tokens;
    for (const QString& word : query.split(QRegularExpression("[^a-zA-Z0-9]+"))) {
        QString lower = word.toLower();
        if (lower.size() >= 3) tokens.append(lower);
    }
    return tokens;
}

static QString canonicalUrlKey(const QString& url) {
    QString u = url.trimmed().toLower();
    while (u.endsWith('/')) u.chop(1);
    for (const QString& marker : {"?utm_", "&utm_", "?fbclid=", "&fbclid="}) {
        int idx = u.indexOf(marker);
        if (idx >= 0) u = u.left(idx);
    }
    return u;
}

static QString normalizeSearchUrl(const QString& s) {
    return urldecode(s.trimmed()).replace(' ', '+');
}

static QList<WebSearchHit> rankAndDedupeHits(QList<WebSearchHit> hits, const QString& query) {
    QSet<QString> seen;
    QList<WebSearchHit> deduped;

    for (auto& hit : hits) {
        hit.title = normalizeSearchText(hit.title);
        hit.body  = normalizeSearchText(hit.body);
        hit.href  = normalizeSearchUrl(hit.href);
        if (hit.title.isEmpty() || hit.href.isEmpty() || !hit.href.startsWith("http"))
            continue;
        QString key = canonicalUrlKey(hit.href);
        if (!seen.contains(key)) {
            seen.insert(key);
            deduped.append(hit);
        }
    }

    QStringList tokens = queryTokens(query);

    auto score = [&](const WebSearchHit& hit) -> int {
        QString hrefL  = hit.href.toLower();
        QString titleL = hit.title.toLower();
        QString bodyL  = hit.body.toLower();
        int s = 0;
        if (hrefL.contains("wikipedia.org")) s += 100;
        if (hit.engine == "brave" || hit.engine == "google" ||
            hit.engine == "yahoo" || hit.engine == "startpage")
            s += 5;
        int titleHits = 0, bodyHits = 0;
        for (const QString& t : tokens) {
            if (titleL.contains(t)) titleHits++;
            if (bodyL.contains(t))  bodyHits++;
        }
        if (titleHits > 0 && bodyHits > 0)      s += 40;
        else if (titleHits > 0)                  s += 25;
        else if (bodyHits > 0)                   s += 10;
        s += titleHits * 3 + bodyHits;
        return s;
    };

    std::sort(deduped.begin(), deduped.end(), [&](const WebSearchHit& a, const WebSearchHit& b) {
        return score(a) > score(b);
    });

    return deduped;
}

static QString formatHits(const QList<WebSearchHit>& hits, int maxResults) {
    QStringList lines;
    int count = 0;
    for (const auto& hit : hits) {
        if (count >= maxResults) break;
        count++;
        lines.append(QString("%1. %2").arg(count).arg(hit.title));
        if (!hit.href.isEmpty()) lines.append("   URL: " + hit.href);
        if (!hit.body.isEmpty()) lines.append("   " + hit.body);
        lines.append(QString());
    }
    return lines.join("\n").trimmed();
}

// ── Web search main function ─────────────────────────────────────────

static QString toolWebSearch(const QJsonObject& args) {
    QString query      = aStr(args, "query");
    int     maxResults = aInt(args, "max_results", 5);
    if (query.isEmpty()) return "Error: query is required.";
    if (maxResults <= 0) maxResults = 5;
    if (maxResults > 25) maxResults = 25;

    // Rate-limit between searches
    {
        QMutexLocker lock(&g_searchTimerMutex);
        if (g_lastSearchTimerStarted) {
            qint64 elapsed = g_lastSearchTimer.elapsed();
            if (elapsed < 800) {
                QThread::msleep(800 - elapsed);
            }
        }
        g_lastSearchTimer.start();
        g_lastSearchTimerStarted = true;
    }

    QString browserUa = searchBrowserUa();
    QString mobileUa  = googleMobileUa();
    QString encoded   = QString(QUrl::toPercentEncoding(query));

    // Fire all search backends in parallel
    QNetworkAccessManager mgr;
    mgr.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

    struct PendingSearch {
        QString engine;
        QNetworkReply* reply;
    };
    QList<PendingSearch> pending;

    auto makeReq = [](const QUrl& url, const QString& ua, int timeout = 8000) {
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::UserAgentHeader, ua);
        req.setTransferTimeout(timeout);
        return req;
    };

    // Brave
    {
        auto req = makeReq(QUrl("https://search.brave.com/search?q=" + encoded + "&source=web"), browserUa);
        req.setRawHeader("Cookie", "useLocation=0; safesearch=off; us=us");
        pending.append({"brave", mgr.get(req)});
    }

    // DuckDuckGo (POST)
    {
        QNetworkRequest req(QUrl("https://html.duckduckgo.com/html/"));
        req.setHeader(QNetworkRequest::UserAgentHeader, browserUa);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
        req.setTransferTimeout(8000);
        QByteArray postData = "q=" + QUrl::toPercentEncoding(query) + "&b=&l=us-en";
        pending.append({"ddg", mgr.post(req, postData)});
    }

    // Mojeek
    {
        auto req = makeReq(QUrl("https://www.mojeek.com/search?q=" + encoded), browserUa);
        req.setRawHeader("Cookie", "arc=us; lb=en");
        pending.append({"mojeek", mgr.get(req)});
    }

    // Yahoo
    {
        QString tokenA = QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
        QString tokenB = QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
        QUrl yahooUrl(QString("https://search.yahoo.com/search;_ylt=%1;_ylu=%2?p=%3")
                      .arg(tokenA, tokenB, encoded));
        pending.append({"yahoo", mgr.get(makeReq(yahooUrl, browserUa))});
    }

    // Google (mobile UA)
    {
        QUrl googleUrl("https://www.google.com/search");
        QUrlQuery gq;
        gq.addQueryItem("q", query);
        gq.addQueryItem("filter", "1");
        gq.addQueryItem("start", "0");
        gq.addQueryItem("hl", "en-US");
        gq.addQueryItem("lr", "lang_en");
        gq.addQueryItem("cr", "countryUS");
        googleUrl.setQuery(gq);
        auto req = makeReq(googleUrl, mobileUa);
        req.setRawHeader("Cookie", "CONSENT=YES+");
        pending.append({"google", mgr.get(req)});
    }

    // Startpage (two-step: GET homepage for sc token, then POST search)
    // We'll do a simpler single GET approach that often works
    {
        pending.append({"startpage_home", mgr.get(makeReq(QUrl("https://www.startpage.com/"), browserUa))});
    }

    // Yandex
    {
        QString searchId = QString::number(QDateTime::currentMSecsSinceEpoch() % 9000000 + 1000000);
        QUrl yandexUrl("https://yandex.com/search/site/");
        QUrlQuery yq;
        yq.addQueryItem("text", query);
        yq.addQueryItem("web", "1");
        yq.addQueryItem("searchid", searchId);
        yandexUrl.setQuery(yq);
        pending.append({"yandex", mgr.get(makeReq(yandexUrl, browserUa))});
    }

    // Wait for all with 12s global timeout
    int remaining = pending.size();
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.start(12000);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    for (const auto& p : pending) {
        QObject::connect(p.reply, &QNetworkReply::finished, [&]() {
            --remaining;
            if (remaining <= 0) loop.quit();
        });
    }

    if (remaining > 0) loop.exec();

    // Collect HTML from each backend
    QMap<QString, QString> htmlMap;
    for (const auto& p : pending) {
        if (p.reply->isFinished() && p.reply->error() == QNetworkReply::NoError) {
            htmlMap[p.engine] = QString::fromUtf8(p.reply->readAll());
        }
        p.reply->deleteLater();
    }

    // Handle Startpage two-step: if we got the homepage, extract sc token and do a POST
    QString startpageHtml;
    if (htmlMap.contains("startpage_home")) {
        QString homeHtml = htmlMap["startpage_home"];
        QRegularExpression scRx(R"RE(<input[^>]*name="sc"[^>]*value="([^"]*)")RE");
        auto scm = scRx.match(homeHtml);
        QString sc = scm.hasMatch() ? scm.captured(1) : "";

        QNetworkRequest spReq(QUrl("https://www.startpage.com/sp/search"));
        spReq.setHeader(QNetworkRequest::UserAgentHeader, browserUa);
        spReq.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
        spReq.setRawHeader("Referer", "https://www.startpage.com/");
        spReq.setTransferTimeout(8000);

        QByteArray spData;
        spData += "query=" + QUrl::toPercentEncoding(query);
        spData += "&cat=web&t=device&sc=" + QUrl::toPercentEncoding(sc);
        spData += "&lui=english&language=english&abp=1&abd=0&abe=0";
        spData += "&qsr=en_US&qadf=none&segment=organic";

        QNetworkReply* spReply = mgr.post(spReq, spData);
        QEventLoop spLoop;
        QTimer spTimer;
        spTimer.setSingleShot(true);
        spTimer.start(5000);
        QObject::connect(spReply, &QNetworkReply::finished, &spLoop, &QEventLoop::quit);
        QObject::connect(&spTimer, &QTimer::timeout, &spLoop, &QEventLoop::quit);
        spLoop.exec();

        if (spReply->isFinished() && spReply->error() == QNetworkReply::NoError) {
            startpageHtml = QString::fromUtf8(spReply->readAll());
        }
        spReply->deleteLater();
    }

    // Parse results from each backend
    QList<WebSearchHit> allHits;
    QStringList failures;

    auto tryParse = [&](const QString& engine, const QString& html,
                        std::function<QList<WebSearchHit>(const QString&, int)> parser) {
        if (html.isEmpty()) {
            failures.append(engine + ": no response");
            return;
        }
        auto hits = parser(html, maxResults);
        if (hits.isEmpty()) {
            failures.append(engine + ": no results found");
        } else {
            allHits.append(hits);
        }
    };

    if (htmlMap.contains("brave"))
        tryParse("Brave", htmlMap["brave"], parseBrave);
    else
        failures.append("Brave: request failed");

    if (htmlMap.contains("ddg"))
        tryParse("DuckDuckGo", htmlMap["ddg"], parseDDG);
    else
        failures.append("DuckDuckGo: request failed");

    if (htmlMap.contains("mojeek"))
        tryParse("Mojeek", htmlMap["mojeek"], parseMojeek);
    else
        failures.append("Mojeek: request failed");

    if (htmlMap.contains("yahoo"))
        tryParse("Yahoo", htmlMap["yahoo"], parseYahoo);
    else
        failures.append("Yahoo: request failed");

    if (htmlMap.contains("google"))
        tryParse("Google", htmlMap["google"], parseGoogle);
    else
        failures.append("Google: request failed");

    if (!startpageHtml.isEmpty())
        tryParse("Startpage", startpageHtml, parseStartpage);
    else
        failures.append("Startpage: request failed");

    if (htmlMap.contains("yandex"))
        tryParse("Yandex", htmlMap["yandex"], parseYandex);
    else
        failures.append("Yandex: request failed");

    auto ranked = rankAndDedupeHits(allHits, query);
    if (!ranked.isEmpty()) {
        return formatHits(ranked, maxResults);
    }

    if (failures.isEmpty()) {
        return QString("No results found for query: %1").arg(query);
    }
    return QString("Web search failed for query: %1\n\nBackends tried:\n- %2")
           .arg(query, failures.join("\n- "));
}

// ── Download file ────────────────────────────────────────────────────

static QString toolDownloadFile(const QJsonObject& args) {
    QString urlStr   = aStr(args, "url");
    QString filename = aStr(args, "filename");
    if (urlStr.isEmpty()) return "Error: url is required.";

    QUrl url(urlStr);
    if (!url.isValid())   return "Error: Invalid URL: " + urlStr;
    QString scheme = url.scheme();
    if (scheme != "http" && scheme != "https")
        return QString("Error: Only http/https URLs are supported (got '%1').").arg(scheme);

    QString downloads = QDir::homePath() + "/Downloads";
    QDir().mkpath(downloads);

    if (filename.isEmpty()) {
        filename = urlStr.split('?').first().split('/').last();
        if (filename.isEmpty()) filename = "download";
    }

    QString dest = downloads + "/" + filename;
    QByteArray data = httpGetWithRedirect(url, userAgent(), 60000);
    if (data.isEmpty()) return "Error: Failed to download file or file is empty.";

    const qsizetype maxSize = 100LL * 1024 * 1024;
    if (data.size() > maxSize)
        return QString("Error: Download exceeds maximum size of %1 bytes.").arg(maxSize);

    QFile f(dest);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return "Error writing file: " + f.errorString();
    f.write(data);
    return QString("Downloaded to %1 (%2 bytes)").arg(dest).arg(data.size());
}

// ── Fetch URL (with improved HTML body extraction) ───────────────────

static QString toolFetchUrl(const QJsonObject& args) {
    QString urlStr = aStr(args, "url");
    if (urlStr.isEmpty()) return "Error: url is required.";

    QUrl url(urlStr);
    if (!url.isValid()) return "Error: Invalid URL: " + urlStr;
    QString scheme = url.scheme();
    if (scheme != "http" && scheme != "https")
        return QString("Error: Only http/https URLs are supported (got '%1').").arg(scheme);

    QNetworkAccessManager mgr;
    mgr.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    req.setTransferTimeout(30000);

    QNetworkReply* reply = mgr.get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        QString err = reply->errorString();
        reply->deleteLater();
        return "Error fetching URL: " + err;
    }

    QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
    QByteArray raw = reply->readAll();
    reply->deleteLater();

    const qsizetype maxRaw = 2 * 1024 * 1024;
    if (raw.size() > maxRaw) raw = raw.left(maxRaw);

    QString text = QString::fromUtf8(raw);
    bool isHtml = contentType.contains("html") ||
                  text.toLower().contains("<html") ||
                  text.toLower().contains("<!doctype");

    if (isHtml) {
        // Extract <body> content for cleaner text
        QRegularExpression bodyRx("<body[^>]*>([\\s\\S]*)</body>",
                                  QRegularExpression::CaseInsensitiveOption);
        auto bm = bodyRx.match(text);
        QString bodyHtml = bm.hasMatch() ? bm.captured(1) : text;
        text = stripTags(bodyHtml);
        text.replace(QRegularExpression("\\n{3,}"), "\n\n");
        text = text.trimmed();
    }

    const int maxChars = 50000;
    if (text.size() > maxChars) {
        text = text.left(maxChars) + "\n\n[... truncated at 50,000 characters ...]";
    }
    return text;
}

// ── Run Python (with PENGY_PYTHON support & temp file output) ────────

static QString pythonInterpreter() {
    QString pengyPy = qEnvironmentVariable("PENGY_PYTHON");
    if (!pengyPy.trimmed().isEmpty()) return pengyPy;

    QString venv = qEnvironmentVariable("VIRTUAL_ENV");
    if (!venv.trimmed().isEmpty()) {
#ifdef Q_OS_WIN
        return venv + "/Scripts/python.exe";
#else
        return venv + "/bin/python";
#endif
    }

    return "python3";
}

static QString toolRunPython(const QJsonObject& args) {
    QString code = aStr(args, "code");
    if (code.isEmpty()) return "Error: code is required.";

    QTemporaryFile tmp;
    tmp.setFileTemplate(QDir::tempPath() + "/pengy_py_XXXXXX.py");
    tmp.setAutoRemove(true);
    if (!tmp.open()) return "Error: Could not create temp file.";
    tmp.write(code.toUtf8());
    tmp.flush();
    QString tmpPath = tmp.fileName();

    auto tmpFiles = createOutputFiles("python");
    if (!tmpFiles.valid) {
        return "Error: Could not create temp output files.";
    }

    QProcess proc;
    proc.setProgram(pythonInterpreter());
    proc.setArguments({tmpPath});
    proc.setStandardOutputFile(tmpFiles.stdoutPath);
    proc.setStandardErrorFile(tmpFiles.stderrPath);

#ifdef Q_OS_UNIX
    proc.setChildProcessModifier([]() {
        setsid();
    });
#endif

    proc.start();

    if (!proc.waitForStarted(5000)) {
        removeOutputFiles(tmpFiles);
        return "Error: Could not start " + pythonInterpreter();
    }

    qint64 pid = proc.processId();
    registerProcess(pid);

    int timeoutMs = toolTimeout() > 0 ? toolTimeout() * 1000 : -1;
    if (!proc.waitForFinished(timeoutMs)) {
        terminateProcessGroup(pid);
        proc.kill();
        proc.waitForFinished(2000);
        unregisterProcess(pid);
        removeOutputFiles(tmpFiles);
        return "Error: Python execution timed out.";
    }

    unregisterProcess(pid);

    QString out = readAndRemove(tmpFiles.stdoutPath);
    QString err = readAndRemove(tmpFiles.stderrPath);

    if (!err.trimmed().isEmpty()) {
        out += "\n" + err;
    }

    if (proc.exitCode() != 0)
        out += QString("\n[Exit code: %1]").arg(proc.exitCode());
    return out.trimmed().isEmpty() ? "(No output)" : out;
}

// ── Directory tree ───────────────────────────────────────────────────

static const QSet<QString> ALWAYS_SKIP{
    "node_modules", ".git", ".svn", ".hg", "__pycache__",
    ".mypy_cache", ".pytest_cache", ".ruff_cache", ".tox",
    ".eggs", ".DS_Store"
};

static QString formatSize(qint64 sz) {
    if (sz < 1024)             return QString("%1 B").arg(sz);
    if (sz < 1024*1024)        return QString("%1 KB").arg(sz / 1024.0, 0, 'f', 1);
    if (sz < 1024*1024*1024LL) return QString("%1 MB").arg(sz / (1024.0*1024.0), 0, 'f', 1);
    return QString("%1 GB").arg(sz / (1024.0*1024.0*1024.0), 0, 'f', 1);
}

static void buildTree(const QString& dir, const QString& prefix,
                      int depth, int maxDepth, bool showHidden,
                      QStringList& lines, int& count, int maxEntries) {
    if (depth > maxDepth || count >= maxEntries) return;

    QDir d(dir);
    QDir::Filters filters = QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot;
    if (showHidden) filters |= QDir::Hidden;

    QFileInfoList entries = d.entryInfoList(filters, QDir::DirsFirst | QDir::Name);
    entries.erase(
        std::remove_if(entries.begin(), entries.end(), [&](const QFileInfo& fi) {
            QString name = fi.fileName();
            if (!showHidden && name.startsWith('.')) return true;
            return ALWAYS_SKIP.contains(name) || name.endsWith(".egg-info");
        }),
        entries.end()
    );

    for (int i = 0; i < entries.size(); ++i) {
        if (count >= maxEntries) {
            lines.append(prefix + QString("... (truncated, %1 entries reached)").arg(maxEntries));
            return;
        }
        bool isLast  = (i == entries.size() - 1);
        QString conn = isLast ? "└── " : "├── ";
        const QFileInfo& fi = entries[i];

        if (fi.isDir()) {
            lines.append(prefix + conn + fi.fileName() + "/");
            ++count;
            if (depth < maxDepth) {
                QString ext = isLast ? "    " : "│   ";
                buildTree(fi.filePath(), prefix + ext, depth + 1, maxDepth,
                          showHidden, lines, count, maxEntries);
            }
        } else {
            lines.append(prefix + conn + fi.fileName() +
                         "  (" + formatSize(fi.size()) + ")");
            ++count;
        }
    }
}

static QString toolDirectoryTree(const QJsonObject& args) {
    QString path      = expandHome(aStr(args, "path"));
    int     maxDepth  = aInt(args, "max_depth", 3);
    bool    showHidden = aBool(args, "show_hidden", false);

    QFileInfo fi(path);
    if (!fi.exists())  return "Error: Directory not found: " + path;
    if (!fi.isDir())   return "Error: Not a directory: " + path;

    QStringList lines{fi.absoluteFilePath() + "/"};
    int count = 0;
    buildTree(path, "", 1, maxDepth, showHidden, lines, count, 500);
    if (lines.size() == 1) lines.append("(empty directory)");

    QString result = lines.join("\n");
    if (result.size() > 40000)
        result = result.left(40000) + "\n\n[... truncated at 40,000 characters ...]";
    return result;
}

// ── Read multiple files ──────────────────────────────────────────────

static QString toolReadMultipleFiles(const QJsonObject& args) {
    QJsonArray pathsArr = args["paths"].toArray();
    if (pathsArr.isEmpty()) return "Error: no paths provided.";

    const int MAX_FILES    = 20;
    const int MAX_PER_FILE = 50000;
    const int MAX_TOTAL    = 120000;

    if (pathsArr.size() > MAX_FILES)
        return QString("Error: too many files (%1). Maximum is %2.").arg(pathsArr.size()).arg(MAX_FILES);

    QStringList parts;
    int total = 0;

    for (const QJsonValue& pv : pathsArr) {
        QString rawPath = pv.toString();
        QString absPath = expandHome(rawPath);
        QString sep     = QString(60, '=');
        QString header  = sep + "\n\U0001F4C4 " + rawPath;

        QFileInfo fi(absPath);
        if (!fi.exists()) {
            parts.append(header + "\n  ❌ File not found.");
            continue;
        }
        if (!fi.isFile()) {
            parts.append(header + "\n  ❌ Not a file.");
            continue;
        }
        QFile f(absPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            parts.append(header + "\n  ❌ Error reading file: " + f.errorString());
            continue;
        }
        QString content = QString::fromUtf8(f.readAll());
        f.close();

        if (content.size() > MAX_PER_FILE) {
            content = content.left(MAX_PER_FILE) +
                      QString("\n\n[... truncated at %1 characters ...]").arg(MAX_PER_FILE);
        }

        QString block = header + "\n" + content;
        if (total + block.size() > MAX_TOTAL) {
            int remaining = MAX_TOTAL - total;
            if (remaining > 200) {
                int take = qMax(0, remaining - header.size() - 4);
                parts.append(header + "\n" + content.left(take) + "...");
            } else {
                parts.append(QString("\n[... output limit reached; %1 files skipped ...]")
                             .arg(pathsArr.size() - parts.size()));
                break;
            }
        } else {
            parts.append(block);
        }
        total += parts.last().size();
    }

    return parts.join("\n\n");
}

// ── Search content ───────────────────────────────────────────────────

static bool isLikelyText(const QFileInfo& fi) {
    static const QSet<QString> TEXT_EXTS{
        "py","pyi","pyx","c","cpp","cc","cxx","h","hpp","hxx","rs",
        "go","java","kt","scala","swift","js","jsx","ts","tsx","mjs",
        "cjs","rb","rake","php","pl","pm","sh","bash","zsh","fish",
        "html","htm","css","scss","sass","less","json","yaml","yml",
        "toml","ini","cfg","conf","xml","svg","rss","md","markdown",
        "rst","txt","tex","sql","r","jl","lua","zig","nim","ex","exs",
        "cmake","make","mk","dockerfile","env","gitignore","editorconfig"
    };
    static const QSet<QString> TEXT_NAMES{
        "makefile","dockerfile","license","changelog","authors","todo"
    };
    QString ext  = fi.suffix().toLower();
    QString name = fi.fileName().toLower();
    return TEXT_EXTS.contains(ext) || TEXT_NAMES.contains(name);
}

static bool matchesGlob(const QString& name, const QString& glob) {
    QRegularExpression braceRx(R"(^(.*)\{([^}]+)\}(.*)$)");
    auto m = braceRx.match(glob);
    if (m.hasMatch()) {
        QString pre  = m.captured(1);
        QString suf  = m.captured(3);
        for (const QString& choice : m.captured(2).split(',')) {
            QString pat = (pre + choice + suf)
                .replace(QChar('.'), QStringLiteral("\\."))
                .replace(QChar('*'), QStringLiteral(".*"))
                .replace(QChar('?'), QStringLiteral("."));
            if (QRegularExpression("^" + pat + "$").match(name).hasMatch())
                return true;
        }
        return false;
    }
    QString pat = QString(glob).replace(QChar('.'), QStringLiteral("\\."))
                              .replace(QChar('*'), QStringLiteral(".*"))
                              .replace(QChar('?'), QStringLiteral("."));
    return QRegularExpression("^" + pat + "$").match(name).hasMatch();
}

static bool searchOneFile(const QString& filepath, const QRegularExpression& rx,
                           int contextLines, const QString& displayPath,
                           QStringList& results, int maxResults) {
    QFile f(filepath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
    f.close();

    QSet<int> matched;
    for (int i = 0; i < lines.size(); ++i) {
        if (rx.match(lines[i]).hasMatch())
            matched.insert(i);
    }
    if (matched.isEmpty()) return false;

    QList<int> sorted = matched.values();
    std::sort(sorted.begin(), sorted.end());

    struct Region { int start, end; };
    QList<Region> regions;
    for (int ln : sorted) {
        int s = qMax(0, ln - contextLines);
        int e = qMin(lines.size(), ln + contextLines + 1);
        if (!regions.isEmpty() && s <= regions.last().end) {
            regions.last().end = qMax(regions.last().end, e);
        } else {
            regions.append({s, e});
        }
    }

    for (const Region& reg : regions) {
        if (results.size() >= maxResults) return true;
        QStringList block{QString("\U0001F4C4 %1:").arg(displayPath)};
        for (int ln = reg.start; ln < reg.end; ++ln) {
            QString marker = matched.contains(ln) ? " ▸" : "  ";
            block.append(QString("%1%2 │ %3").arg(marker).arg(ln + 1, 5).arg(lines[ln]));
        }
        results.append(block.join("\n"));
    }
    return results.size() >= maxResults;
}

static QString toolSearchContent(const QJsonObject& args) {
    QString pattern      = aStr(args, "pattern");
    QString path         = expandHome(aStr(args, "path"));
    QString fileGlob     = aStr(args, "file_glob");
    int     contextLines = qMin(aInt(args, "context_lines", 0), 10);
    int     maxResults   = qBound(1, aInt(args, "max_results", 50), 200);

    if (pattern.isEmpty()) return "Error: pattern is required.";

    QFileInfo pathInfo(path);
    if (!pathInfo.exists()) return "Error: Path not found: " + path;

    QRegularExpression rx(pattern);
    if (!rx.isValid()) {
        rx = QRegularExpression(QRegularExpression::escape(pattern));
        if (!rx.isValid())
            return "Error: Invalid regex pattern.";
    }

    QStringList results;
    int filesSearched = 0, filesSkipped = 0;
    bool truncated = false;

    if (pathInfo.isFile()) {
        searchOneFile(path, rx, contextLines, path, results, maxResults);
        if (results.isEmpty()) return QString("No matches found for '%1' in %2").arg(pattern, path);
        return results.join("\n\n");
    }

    QDirIterator it(path, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (truncated) break;
        QString fp = it.next();
        QFileInfo fi(fp);

        if (!fi.isFile()) continue;
        QString name = fi.fileName();
        if (name == ".DS_Store" || name == "Thumbs.db") continue;

        bool skip = false;
        QString rel = QDir(path).relativeFilePath(fp);
        for (const QString& part : rel.split('/')) {
            if (ALWAYS_SKIP.contains(part) || part.endsWith(".egg-info")) {
                skip = true; break;
            }
        }
        if (skip) continue;

        if (!fileGlob.isEmpty() && !matchesGlob(name, fileGlob)) continue;

        if (!isLikelyText(fi)) { ++filesSkipped; continue; }
        ++filesSearched;

        if (searchOneFile(fp, rx, contextLines,
                          QDir(path).relativeFilePath(fp),
                          results, maxResults)) {
            truncated = true;
        }
    }

    if (results.isEmpty()) {
        QString summary = QString("No matches found for '%1' in %2").arg(pattern, path);
        if (filesSearched > 0) {
            summary += QString(" (searched %1 files").arg(filesSearched);
            if (filesSkipped > 0)
                summary += QString(", skipped %1 binary/non-matching files").arg(filesSkipped);
            summary += ')';
        }
        return summary;
    }

    QString summary = QString("Found %1 match(es) for '%2' across %3 file(s)")
                      .arg(results.size()).arg(pattern).arg(filesSearched);
    if (truncated) summary += " (results truncated)";
    return summary + "\n" + QString(60, QChar(0x2500)) + "\n" + results.join("\n\n");
}

// ── Dispatcher ────────────────────────────────────────────────────────

QString execute(const QString& name, const QJsonObject& args, std::atomic<bool>* cancel) {
    if (name == "read_file")          return toolReadFile(args);
    if (name == "write_file")         return toolWriteFile(args);
    if (name == "replace_in_file")    return toolReplaceInFile(args);
    if (name == "run_bash")           return toolRunBash(args, cancel);
    if (name == "web_search")         return toolWebSearch(args);
    if (name == "download_file")      return toolDownloadFile(args);
    if (name == "fetch_url")          return toolFetchUrl(args);
    if (name == "run_python")         return toolRunPython(args);
    if (name == "directory_tree")     return toolDirectoryTree(args);
    if (name == "read_multiple_files") return toolReadMultipleFiles(args);
    if (name == "search_content")     return toolSearchContent(args);
    return "Unknown tool: " + name;
}

} // namespace Tools
