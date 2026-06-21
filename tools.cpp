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

namespace Tools {

static QString   g_userAgent = "PengyAgent/1.0";
static int       g_timeout   = 60;
static QMutex    g_mutex;

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

        td("web_search", "Search the web using DuckDuckGo",
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

// ── Synchronous HTTP (safe to call from any thread via local QEventLoop) ──

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

// ── Simple HTML utilities ─────────────────────────────────────────────

static QString decodeEntities(QString s) {
    s.replace("&amp;",  "&");
    s.replace("&lt;",   "<");
    s.replace("&gt;",   ">");
    s.replace("&quot;", "\"");
    s.replace("&apos;", "'");
    s.replace("&nbsp;", " ");
    s.replace("&#39;",  "'");
    s.replace("&#x27;", "'");
    // Numeric HTML entities &#NNN;  (simplified — skip the lambda, just remove them)
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

// Extract first text from element whose class contains `cls`
static QString extractByClass(const QString& html, const QString& cls) {
    static QRegularExpression tagRx(
        "<[a-zA-Z][^>]*class=\"[^\"]*\\b%1\\b[^\"]*\"[^>]*>([\\s\\S]*?)</[a-zA-Z]+>",
        QRegularExpression::MultilineOption);

    QRegularExpression re(tagRx.pattern().replace("%1", QRegularExpression::escape(cls)));
    auto m = re.match(html);
    if (!m.hasMatch()) return {};
    return stripTags(m.captured(1)).trimmed();
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

static QString toolRunBash(const QJsonObject& args, std::atomic<bool>* cancel) {
    QString command = aStr(args, "command");
    if (command.isEmpty()) return "Error: command is required.";

    int timeoutSecs = toolTimeout();

    QProcess proc;
    proc.setProgram("bash");
    proc.setArguments({"-c", command});
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start();
    if (!proc.waitForStarted(5000))
        return "Error running command: " + proc.errorString();

    int waitMs = timeoutSecs > 0 ? timeoutSecs * 1000 : -1;

    // Poll with cancel check
    if (cancel) {
        int elapsed = 0;
        int step    = 100;
        while (!proc.waitForFinished(step)) {
            if (cancel->load()) {
                proc.kill();
                proc.waitForFinished(2000);
                return "Error: Command was cancelled.";
            }
            elapsed += step;
            if (waitMs > 0 && elapsed >= waitMs) {
                proc.kill();
                proc.waitForFinished(2000);
                return QString("Error: Command timed out after %1 seconds.").arg(timeoutSecs);
            }
        }
    } else {
        if (!proc.waitForFinished(waitMs)) {
            proc.kill();
            proc.waitForFinished(2000);
            return QString("Error: Command timed out after %1 seconds.").arg(timeoutSecs);
        }
    }

    QString out = QString::fromUtf8(proc.readAllStandardOutput());
    // Strip sudo password prompt lines
    out.remove(QRegularExpression("^\\[sudo[^\\]]*\\].*\\n?", QRegularExpression::MultilineOption));

    if (proc.exitCode() != 0)
        out += QString("\n[Exit code: %1]").arg(proc.exitCode());

    return out.trimmed().isEmpty() ? "(No output)" : out;
}

static QString toolWebSearch(const QJsonObject& args) {
    QString query      = aStr(args, "query");
    int     maxResults = aInt(args, "max_results", 5);
    if (query.isEmpty()) return "Error: query is required.";
    if (maxResults <= 0) maxResults = 5;

    QString ua = userAgent();

    // URL-encode: spaces become +
    QString encoded;
    for (const QChar& c : query) {
        uchar u = c.toLatin1();
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ||
            (u >= '0' && u <= '9') || u == '-' || u == '_' || u == '.' || u == '~') {
            encoded += c;
        } else if (u == ' ') {
            encoded += '+';
        } else {
            encoded += QString("%%1").arg(u, 2, 16, QChar('0')).toUpper();
        }
    }

    QUrl url("https://html.duckduckgo.com/html/?q=" + encoded);
    QByteArray html = httpGet(url, ua, 15000);
    if (html.isEmpty()) return "Error: Failed to fetch search results.";

    QString page = QString::fromUtf8(html);

    // DuckDuckGo HTML: each result is a <div class="result ..."> block
    QStringList lines;
    int count = 0;
    int pos   = 0;

    while (count < maxResults) {
        // Find next result block
        int rStart = page.indexOf("class=\"result", pos);
        if (rStart == -1) break;

        // Find the start of the div tag containing this class
        int divStart = page.lastIndexOf('<', rStart);
        if (divStart == -1) { pos = rStart + 1; continue; }

        // Find next result to delimit this block
        int nextResult = page.indexOf("class=\"result", rStart + 13);
        QString block = (nextResult > 0)
            ? page.mid(divStart, nextResult - divStart)
            : page.mid(divStart);

        // Skip ads / sponsored (DDG marks them with result--ad)
        if (block.contains("result--ad")) {
            pos = (nextResult > 0) ? nextResult : page.length();
            continue;
        }

        QString title   = extractByClass(block, "result__a");
        if (title.isEmpty()) title = extractByClass(block, "result__title");
        if (title.isEmpty()) {
            pos = (nextResult > 0) ? nextResult : page.length();
            continue;
        }
        QString url2    = extractByClass(block, "result__url");
        QString snippet = extractByClass(block, "result__snippet");

        count++;
        lines.append(QString("%1. %2").arg(count).arg(title));
        if (!url2.isEmpty())    lines.append("   URL: " + url2);
        if (!snippet.isEmpty()) lines.append("   " + snippet);
        lines.append(QString());

        pos = (nextResult > 0) ? nextResult : page.length();
    }

    if (lines.isEmpty()) return "No results found.";
    return lines.join("\n").trimmed();
}

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

static QString toolFetchUrl(const QJsonObject& args) {
    QString urlStr = aStr(args, "url");
    if (urlStr.isEmpty()) return "Error: url is required.";

    QUrl url(urlStr);
    if (!url.isValid()) return "Error: Invalid URL: " + urlStr;
    QString scheme = url.scheme();
    if (scheme != "http" && scheme != "https")
        return QString("Error: Only http/https URLs are supported (got '%1').").arg(scheme);

    QByteArray raw = httpGetWithRedirect(url, userAgent(), 30000);
    if (raw.isEmpty()) return "Error: Failed to fetch URL or empty response.";

    const qsizetype maxRaw = 2 * 1024 * 1024;
    if (raw.size() > maxRaw) raw = raw.left(maxRaw);

    QString text = QString::fromUtf8(raw);
    bool isHtml = text.toLower().contains("<html") || text.toLower().contains("<!doctype");

    if (isHtml) {
        // Strip script/style, then all tags
        text = stripTags(text);
        text.replace(QRegularExpression("\\n{3,}"), "\n\n");
    }

    const int maxChars = 50000;
    if (text.size() > maxChars) {
        text = text.left(maxChars) + "\n\n[... truncated at 50,000 characters ...]";
    }
    return text;
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

    QProcess proc;
    proc.setProgram("python3");
    proc.setArguments({tmpPath});
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start();

    int timeoutMs = toolTimeout() > 0 ? toolTimeout() * 1000 : -1;
    if (!proc.waitForStarted(5000)) return "Error: Could not start python3.";
    if (!proc.waitForFinished(timeoutMs))
        return "Error: Python execution timed out.";

    QString out = QString::fromUtf8(proc.readAllStandardOutput());
    if (proc.exitCode() != 0)
        out += QString("\n[Exit code: %1]").arg(proc.exitCode());
    return out.trimmed().isEmpty() ? "(No output)" : out;
}

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
        QString header  = sep + "\n📄 " + rawPath;

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
                parts.append(header + "\n" + content.left(remaining - header.size() - 4) + "...");
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
    // Handle brace expansion like *.{js,ts}
    QRegularExpression braceRx(R"(^(.*)\{([^}]+)\}(.*)$)");
    auto m = braceRx.match(glob);
    if (m.hasMatch()) {
        QString pre  = m.captured(1);
        QString suf  = m.captured(3);
        for (const QString& choice : m.captured(2).split(',')) {
            QString pat = (pre + choice + suf)
                .replace('.', "\\.")
                .replace('*', ".*")
                .replace('?', ".");
            if (QRegularExpression("^" + pat + "$").match(name).hasMatch())
                return true;
        }
        return false;
    }
    QString pat = QString(glob).replace('.', "\\.")
                              .replace('*', ".*")
                              .replace('?', ".");
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

    // Build regions
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
        QStringList block{QString("📄 %1:").arg(displayPath)};
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

        // Skip noise dirs
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
