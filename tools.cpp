#include "tools.h"
#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QRegularExpression>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <memory>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>
#include <QTemporaryFile>
#include <QMutex>
#include <QSet>
#include <QDirIterator>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QUuid>
#include <QCoreApplication>
#include <QThread>
#include <QUrlQuery>
#include <QImage>
#include "image_utils.h"

#ifdef Q_OS_UNIX
#include <unistd.h>
#include <signal.h>
#endif

namespace Tools {

static QString   g_userAgent = "PengyAgent/1.0";
static int       g_timeout   = 300;
static int       g_imageMaxDimension = 4096;
static double    g_imageMaxMb        = 4.5;
static int       g_imageQuality      = 85;
static int       g_toolOutputMaxChars = 250000;
static QMutex    g_mutex;

// ── Rate limiter for web searches ─────────────────────────────────────
static QElapsedTimer g_lastSearchTimer;
static bool          g_lastSearchTimerStarted = false;
static QMutex        g_searchTimerMutex;

static void terminateProcessGroup(qint64 pid);   // fwd decl

// ── ToolContext (per-run sudo + subprocess state) ─────────────────────

void ToolContext::setSudoProvider(SudoPasswordFn fn) {
    QMutexLocker lock(&m_mutex);
    m_sudoProvider = std::move(fn);
    m_cachedSudoPassword.clear();
}
SudoPasswordFn ToolContext::sudoProvider() {
    QMutexLocker lock(&m_mutex);
    return m_sudoProvider;
}
QString ToolContext::cachedSudoPassword() {
    QMutexLocker lock(&m_mutex);
    return m_cachedSudoPassword;
}
void ToolContext::setCachedSudoPassword(const QString& pw) {
    QMutexLocker lock(&m_mutex);
    m_cachedSudoPassword = pw;
}
void ToolContext::clearSudo() {
    QMutexLocker lock(&m_mutex);
    m_cachedSudoPassword.clear();
}
void ToolContext::registerProcess(qint64 pid) {
    QMutexLocker lock(&m_mutex);
    m_procs.insert(pid);
}
void ToolContext::unregisterProcess(qint64 pid) {
    QMutexLocker lock(&m_mutex);
    m_procs.remove(pid);
}
void ToolContext::killAll() {
    QSet<qint64> procs;
    {
        QMutexLocker lock(&m_mutex);
        procs = m_procs;
        m_procs.clear();
    }
    for (qint64 pid : procs) {
        terminateProcessGroup(pid);
    }
}

// Context used by callers that don't pass their own (CLI, Web, direct calls).
static ToolContext g_defaultContext;

void setSudoPasswordProvider(SudoPasswordFn fn) {
    g_defaultContext.setSudoProvider(std::move(fn));
}
void clearSudoPasswordProvider() {
    g_defaultContext.setSudoProvider(nullptr);
}

void setUserAgent(const QString& ua) {
    QMutexLocker lock(&g_mutex);
    g_userAgent = ua;
}
void setTimeout(int secs) {
    QMutexLocker lock(&g_mutex);
    g_timeout = secs;
}
void setImageLimits(int maxDimension, double maxMb, int quality) {
    QMutexLocker lock(&g_mutex);
    g_imageMaxDimension = maxDimension;
    g_imageMaxMb        = maxMb;
    g_imageQuality      = quality;
}

void setToolOutputMaxChars(int chars) {
    QMutexLocker lock(&g_mutex);
    g_toolOutputMaxChars = chars;
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

static void terminateProcessGroup(qint64 pid) {
#ifdef Q_OS_UNIX
    QProcess::execute("kill", {"-9", QString("-%1").arg(pid)});
#else
    QProcess::execute("taskkill", {"/PID", QString::number(pid), "/T", "/F"});
#endif
}

void killActiveProcesses() {
    g_defaultContext.killAll();
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

const QJsonArray& toolDefinitions() {
    // Built once; QJsonArray is implicitly shared so callers copy cheaply.
    static const QJsonArray defs = QJsonArray{
        td("read_file", "Read the contents of a text file. Returns the whole file by default; very large files are truncated to the output limit, with a header telling you how to continue with offset/limit. Pass offset and limit to read one line range instead, which is how to page through a file too large to return at once. Use read_image for images — this tool cannot decode binary data.",
            QJsonObject{
                {"path",   prop("string", "The file path to read")},
                {"offset", prop("integer", "1-based line number to start reading from. Omit to start at the beginning.")},
                {"limit",  prop("integer", "Maximum number of lines to return, counting from offset. Omit to read to the end of the file.")}},
            QJsonArray{"path"}),

        td("read_image", "Look at an image file — a screenshot, photo, diagram, or a chart/render produced by an earlier command. The image is added to the conversation so you can see it directly and describe or judge what it shows; use this instead of read_file, which cannot decode image data. Supports PNG, JPEG, GIF, WebP, BMP and TIFF; large images are downscaled automatically.",
            QJsonObject{{"path", prop("string", "The path of the image file to look at")}},
            QJsonArray{"path"}),

        td("write_file", "Write content to a file, replacing it entirely if it already exists. Parent directories are created automatically, so there is no need to mkdir first. To change part of an existing file use replace_in_file instead of rewriting the whole thing.",
            QJsonObject{
                {"path",    prop("string", "The file path to write to")},
                {"content", prop("string", "The content to write to the file")}},
            QJsonArray{"path", "content"}),

        td("replace_in_file",
            "Perform an exact string replacement in an existing file. "
            "The old_str must match exactly one occurrence in the file — if zero or multiple "
            "matches are found, the edit is rejected with a clear error. This is the preferred "
            "way to make targeted edits instead of rewriting an entire file.",
            QJsonObject{
                {"path",    prop("string", "The file path to edit")},
                {"old_str", prop("string", "The exact text to find and replace. Must match exactly one location in the file, including whitespace and indentation.")},
                {"new_str", prop("string", "The text to replace it with. Use empty string to delete.")}},
            QJsonArray{"path", "old_str", "new_str"}),

        td("apply_changes",
            "Apply a bounded, transactional set of exact-text edits across files. Every "
            "operation is validated in memory before anything is written — if validation "
            "fails, nothing is changed. Use dry_run=true to preview the unified diff before "
            "writing. Limits: at most 20 files, 100 operations total, and ~1 MB of content.",
            QJsonObject{
                {"changes", QJsonObject{
                    {"type", "array"},
                    {"description", "Files and exact-text operations to apply"},
                    {"items", QJsonObject{
                        {"type", "object"},
                        {"properties", QJsonObject{
                            {"path", QJsonObject{
                                {"type", "string"},
                                {"description", "File path to edit"}
                            }},
                            {"operations", QJsonObject{
                                {"type", "array"},
                                {"items", QJsonObject{
                                    {"type", "object"},
                                    {"properties", QJsonObject{
                                        {"kind", QJsonObject{
                                            {"type", "string"},
                                            {"enum", QJsonArray{"replace", "insert_after", "delete"}}
                                        }},
                                        {"old", QJsonObject{
                                            {"type", "string"},
                                            {"description", "Exact text to match for replace/delete"}
                                        }},
                                        {"anchor", QJsonObject{
                                            {"type", "string"},
                                            {"description", "Exact text after which to insert"}
                                        }},
                                        {"new", QJsonObject{
                                            {"type", "string"},
                                            {"description", "Replacement text"}
                                        }},
                                        {"text", QJsonObject{
                                            {"type", "string"},
                                            {"description", "Text to insert"}
                                        }},
                                        {"expected_matches", QJsonObject{
                                            {"type", "integer"},
                                            {"description", "Expected exact match count; defaults to 1"}
                                        }}
                                    }},
                                    {"required", QJsonArray{"kind"}}
                                }}
                            }}
                        }},
                        {"required", QJsonArray{"path", "operations"}}
                    }}
                }},
                {"dry_run", QJsonObject{
                    {"type", "boolean"},
                    {"description", "Validate and return a diff without writing files (default: false)"}
                }},
                {"postconditions", QJsonObject{
                    {"type", "array"},
                    {"description", "Optional content checks evaluated before writing"},
                    {"items", QJsonObject{
                        {"type", "object"},
                        {"properties", QJsonObject{
                            {"path", QJsonObject{{"type", "string"}}},
                            {"contains", QJsonObject{{"type", "string"}}},
                            {"does_not_contain", QJsonObject{{"type", "string"}}}
                        }},
                        {"required", QJsonArray{"path"}}
                    }}
                }}
            },
            QJsonArray{"changes"}),

        td("run_bash", "Run a command with bash. The command is non-interactive: stdin is closed, so anything that prompts or waits for input (a password prompt, an editor, `read`) will fail rather than wait — pass non-interactive flags instead. Set cwd to run the command in a specific working directory (defaults to the current directory). sudo is supported and prompts the user for their password separately. Commands are killed once the configured tool timeout elapses.",
            QJsonObject{
                {"command", prop("string", "The bash command to execute")},
                {"cwd",     prop("string", "Optional working directory to run the command in")}},
            QJsonArray{"command"}),

        td("web_search",
            "Search the web using metasearch across multiple backends "
            "(Brave, DuckDuckGo, Mojeek, Yahoo, Google, Startpage, Yandex)",
            QJsonObject{
                {"query",       prop("string",  "The search query")},
                {"max_results", prop("integer", "Maximum number of results to return (default: 5)")}},
            QJsonArray{"query"}),

        td("download_file", "Download a file from a URL to the user's Downloads directory. Existing files of the same name are overwritten; downloads larger than 100 MB are rejected.",
            QJsonObject{
                {"url",      prop("string", "The URL of the file to download")},
                {"filename", prop("string", "Optional filename to save as; defaults to the name from the URL")}},
            QJsonArray{"url"}),

        td("fetch_url", "Fetch a URL and return its text content. Works for documentation and web pages (HTML is stripped to plain text) and for JSON or plain-text endpoints, including local ones such as http://127.0.0.1:8080/api/status. Returns the body only — use run_bash with curl if you need status codes or response headers. Large responses are truncated to the configured tool output limit; pass max_chars to return more (0 = no limit).",
            QJsonObject{
                {"url",       prop("string",  "The URL to fetch")},
                {"max_chars", prop("integer", "Maximum characters to return. Defaults to the configured tool output limit; 0 returns everything (up to the 2 MB response cap).")}},
            QJsonArray{"url"}),

        td("run_python", "Execute Python code in a fresh subprocess. Nothing persists between calls — variables, imports and state from an earlier call are gone, so each call must stand on its own. Only what you print() comes back; a bare expression returns nothing. Set cwd to run in a specific working directory. The process is killed once the configured tool timeout elapses.",
            QJsonObject{
                {"code", prop("string", "The Python code to execute")},
                {"cwd",  prop("string", "Optional working directory to run the code in")}},
            QJsonArray{"code"}),

        td("directory_tree",
            "Show a visual tree of the directory structure, useful for understanding project layout quickly. "
            "Skips common noise directories like .git, node_modules, __pycache__ by default.",
            QJsonObject{
                {"path",        prop("string",  "The directory path to show the tree for")},
                {"max_depth",   prop("integer", "Maximum depth to recurse (default: 3)")},
                {"show_hidden", prop("boolean", "Whether to show hidden files/directories (default: false)")}},
            QJsonArray{"path"}),

        td("read_multiple_files", "Read multiple files at once, returning each with a clear header. Use this when you know you need to inspect several files to reduce round-trips.",
            QJsonObject{{"paths", QJsonObject{
                {"type", "array"},
                {"description", "List of file paths to read"},
                {"items", QJsonObject{{"type", "string"}}}}}},
            QJsonArray{"paths"}),

        td("search_content",
            "Search for text in files under a directory. "
            "Returns matching lines with file path, line number, and optional surrounding context. "
            "The pattern is matched literally by default — regex metacharacters are escaped automatically; set regex=true to interpret it as a regular expression. Skips binary files and common noise directories.",
            QJsonObject{
                {"pattern",       prop("string",  "The text to search for. Matched literally by default — metacharacters like '.', '*', '(', '[' are escaped automatically. Set regex=true to interpret it as a regular expression instead.")},
                {"regex",         prop("boolean", "Treat pattern as a regular expression instead of a literal string (default: false)")},
                {"path",          prop("string",  "The directory or file to search in")},
                {"file_glob",     prop("string",  "Optional glob to filter files, e.g. '*.py' or '*.{js,ts}'. Defaults to all text files.")},
                {"context_lines", prop("integer", "Number of lines of context (default: 0)")},
                {"max_results",   prop("integer", "Maximum number of matches to return (default: 50)")}},
            QJsonArray{"pattern", "path"}),

        td("glob", "Find files matching a glob pattern. Returns sorted file paths with sizes. Use ** for recursive search (e.g. 'src/**/*.py'). Noise directories are always skipped: .git, node_modules, __pycache__, .venv/venv, build, dist and target. Prefer this over run_bash('find ...') or run_bash('ls ...'). Results are capped at 200 paths.",
            QJsonObject{
                {"pattern", prop("string", "The glob pattern to match against file paths. Supports ** for recursive matching, * for any characters, ? for single character.")},
                {"path",    prop("string", "The directory to search in (default: current working directory)")}},
            QJsonArray{"pattern"}),

        td("todowrite",
            "Create and update a structured task list for tracking progress during complex "
            "multi-step operations. Send the COMPLETE list every time — do not send "
            "incremental updates. At most one task must be in_progress at any time — it is "
            "fine to have none. Mark tasks completed immediately after finishing them. Use "
            "imperative forms for content (e.g. 'Run tests', 'Add JWT middleware').",
            QJsonObject{
                {"todos", QJsonObject{
                    {"type", "array"},
                    {"description", "The complete list of tasks with their current statuses"},
                    {"items", QJsonObject{
                        {"type", "object"},
                        {"properties", QJsonObject{
                            {"content", QJsonObject{
                                {"type", "string"},
                                {"description", "Imperative task description, e.g. 'Run the tests'"}
                            }},
                            {"status", QJsonObject{
                                {"type", "string"},
                                {"enum", QJsonArray{"pending", "in_progress", "completed"}},
                                {"description", "Current task status — at most one task should be in_progress"}
                            }}
                        }},
                        {"required", QJsonArray{"content", "status"}}
                    }}
                }}
            },
            QJsonArray{"todos"}),

        td("ask_user_question", "Ask the user one or more multiple-choice questions to clarify requirements, gather preferences, or resolve ambiguity. Use this when instructions are vague, multiple valid approaches exist, or you need a decision before proceeding. Each question includes a header, the question text, and a list of options with descriptions.",
            QJsonObject{{"questions", QJsonObject{
                {"type", "array"},
                {"description", "One or more questions, each with a header, question text, and list of options"},
                {"items", QJsonObject{
                    {"type", "object"},
                    {"properties", QJsonObject{
                        {"header", QJsonObject{
                            {"type", "string"},
                            {"description", "Short label for the question group (e.g. 'Theme', 'Output Format')"}
                        }},
                        {"question", QJsonObject{
                            {"type", "string"},
                            {"description", "The question text to display to the user"}
                        }},
                        {"options", QJsonObject{
                            {"type", "array"},
                            {"description", "List of answer choices for this question"},
                            {"items", QJsonObject{
                                {"type", "object"},
                                {"properties", QJsonObject{
                                    {"label", QJsonObject{
                                        {"type", "string"},
                                        {"description", "Short answer label (e.g. 'Dark', 'JSON')"}
                                    }},
                                    {"description", QJsonObject{
                                        {"type", "string"},
                                        {"description", "Brief explanation of what this option means"}
                                    }}
                                }},
                                {"required", QJsonArray{"label", "description"}}
                            }}
                        }}
                    }},
                    {"required", QJsonArray{"header", "question", "options"}}
                }}
            }}},
            QJsonArray{"questions"}),
    };
    return defs;
}

void ToolContext::addPendingImage(const QString& path, const QString& mime,
                                  const QByteArray& b64) {
    QMutexLocker lock(&m_mutex);
    m_pendingImages.append(QJsonObject{
        {"path", path},
        {"mime", mime},
        {"b64",  QString::fromLatin1(b64)},
    });
}

QJsonArray ToolContext::takePendingImages() {
    QMutexLocker lock(&m_mutex);
    QJsonArray images = m_pendingImages;
    m_pendingImages = QJsonArray{};
    return images;
}

QJsonArray takePendingImages(ToolContext* ctx) {
    if (!ctx) ctx = &g_defaultContext;
    return ctx->takePendingImages();
}

bool isReadOnly(const QString& name) {
    static const QSet<QString> ro{
        "read_file", "read_multiple_files", "directory_tree",
        "search_content", "web_search", "fetch_url",
        "glob", "todowrite", "read_image"
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
    static QRegularExpression numericEntityRx("&#\\d+;");
    s.remove(numericEntityRx);
    return s;
}

static QString stripTags(const QString& html) {
    QString s = html;
    static QRegularExpression scriptRx("<script[^>]*>[\\s\\S]*?</script>",
                                QRegularExpression::CaseInsensitiveOption);
    static QRegularExpression styleRx("<style[^>]*>[\\s\\S]*?</style>",
                                QRegularExpression::CaseInsensitiveOption);
    static QRegularExpression tagRx("<[^>]+>");
    s.remove(scriptRx);
    s.remove(styleRx);
    s.remove(tagRx);
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

/// First *chars* of *text*, backed up to the last line break.
///
/// Cutting on a raw index leaves a broken half-line at the seam, which on
/// source code is a fragment the model may try to reason about or "fix".
/// Falls back to a hard cut when one line is longer than the budget.
static QString cutAtLineEnd(const QString& text, int chars) {
    if (chars >= text.size()) return text;
    QString head = text.left(chars);
    int cut = head.lastIndexOf('\n');
    return cut > 0 ? head.left(cut) : head;
}

/// Last *chars* of *text*, advanced to the start of the next whole line.
static QString cutAtLineStart(const QString& text, int chars) {
    if (chars >= text.size()) return text;
    QString tail = text.right(chars);
    int nl = tail.indexOf('\n');
    return nl != -1 ? tail.mid(nl + 1) : tail;
}

/// Head truncation for *file* content, cut on a line boundary.
///
/// Files truncate from the head rather than being snipped in the middle: the
/// head is where imports and declarations live, and the caller can report which
/// lines survived so the model can page through the rest with offset/limit.
/// *linesKept* and *truncated* are out-parameters.
static QString truncateHeadLines(const QString& text, int limit,
                                 int* linesKept, bool* truncated) {
    const int budget = limit > 0 ? limit : g_toolOutputMaxChars;
    if (budget <= 0 || text.size() <= budget) {
        if (linesKept)  *linesKept = text.count('\n') + 1;
        if (truncated)  *truncated = false;
        return text;
    }
    QString kept = cutAtLineEnd(text, budget);
    if (linesKept) *linesKept = kept.count('\n') + 1;
    if (truncated) *truncated = true;
    return kept;
}

/// Tail-biased truncation for *command* output (run_bash, run_python).
///
/// Keeps the head (~20%) and tail (~80%) and snips the middle: a command echo
/// sits at the start and the error that matters usually sits at the end, so the
/// middle of a build log is the disposable part.  File reads use
/// truncateHeadLines instead — a gap in the middle of a source file is not
/// disposable, and unlike a log it can be paged around.
static QString snipMiddle(const QString& text) {
    int limit = g_toolOutputMaxChars;
    if (limit <= 0 || text.size() <= limit) return text;

    int headChars = qMax(limit / 5, 500);
    int tailChars = limit - headChars;

    // Cut on line boundaries so neither seam leaves a broken half-line.
    QString head = cutAtLineEnd(text, headChars);
    QString tail = cutAtLineStart(text, tailChars);

    int snipped = text.size() - head.size() - tail.size();
    return head
        + QString("\n\n[... snipped %1 chars from middle — set tool_output_max_chars "
                   "to change this limit (current: %2) ...]\n\n")
              .arg(snipped).arg(limit)
        + tail;
}

/// Read file contents, optionally just the line range offset..offset+limit.
///
/// A ranged read is reported as "[Lines A-B of N]" so the model knows where it
/// is in the file and can ask for the next page; a whole-file read that had to
/// be snipped says how many lines exist so paging is discoverable.
static QString toolReadFile(const QJsonObject& args) {
    QString path = expandHome(aStr(args, "path"));
    if (path.isEmpty()) return "Error: path is required.";

    QFileInfo fi(path);
    if (!fi.exists())       return "Error: File not found: " + path;
    if (!fi.isFile())       return "Error: Not a file: " + path;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return "Error reading file: " + f.errorString();
    const QString text = QString::fromUtf8(f.readAll());

    // 0 means "not supplied" — aInt's default; neither is a valid 1-based line.
    const int offset = aInt(args, "offset", 0);
    const int limit  = aInt(args, "limit", 0);

    const QStringList lines = text.split('\n');
    const int total = lines.size();
    const int start = offset > 0 ? offset : 1;
    if (start > total)
        return QString("Error: offset %1 is past the end of %2, which has %3 lines.")
                   .arg(start).arg(path).arg(total);
    const int count = limit > 0 ? limit : (total - start + 1);
    const int end   = qMin(total, start + count - 1);

    QString body = lines.mid(start - 1, end - start + 1).join('\n');
    int kept = 0;
    bool truncated = false;
    body = truncateHeadLines(body, 0, &kept, &truncated);
    const int shownEnd = start + kept - 1;

    // A plain whole-file read that fit stays bare — no header to parse.
    if (!truncated && offset <= 0 && limit <= 0) return body;

    QString header = QString("[Lines %1-%2 of %3 in %4")
                         .arg(start).arg(shownEnd).arg(total).arg(path);
    if (truncated)
        header += QString(" — output limit reached, pass offset=%1 to continue")
                      .arg(shownEnd + 1);
    return header + "]\n" + body;
}

static QString formatSize(qint64 sz);

/// Extensions read_image will attempt, so a text file gets a useful error
/// rather than a decoder failure.
static const QSet<QString>& imageSuffixes() {
    static const QSet<QString> s{
        "png", "jpg", "jpeg", "gif", "webp", "bmp", "tif", "tiff"
    };
    return s;
}

/// Queue an image for attachment to the conversation.  Returns a text summary
/// for the tool result; the picture reaches the caller via
/// ToolContext::takePendingImages — see addPendingImage for why.
static QString toolReadImage(const QJsonObject& args, ToolContext* ctx) {
    QString path = expandHome(aStr(args, "path"));
    if (path.isEmpty()) return "Error: path is required.";

    QFileInfo fi(path);
    if (!fi.exists()) return "Error: File not found: " + path;
    if (!fi.isFile()) return "Error: Not a file: " + path;

    const QString ext = fi.suffix().toLower();
    if (!imageSuffixes().contains(ext)) {
        QStringList known = imageSuffixes().values();
        known.sort();
        return "Error: " + path + " is not a recognized image file. "
               "Supported extensions: " + known.join(", ") +
               ". Use read_file for text.";
    }

    QImage probe(path);
    if (probe.isNull())
        return "Error: " + path + " could not be decoded as an image.";

    int    maxDim  = 0;
    double maxMb   = 0.0;
    int    quality = 0;
    {
        QMutexLocker lock(&g_mutex);
        maxDim  = g_imageMaxDimension;
        maxMb   = g_imageMaxMb;
        quality = g_imageQuality;
    }

    ImageResult result = imagePreprocess(path, maxDim, maxMb, quality);
    if (!result.ok) return "Error reading image: preprocessing failed for " + path;

    ctx->addPendingImage(path, result.mime, result.bytes_base64);

    // base64 inflates by 4/3; report the encoded byte count, not the string.
    const qint64 encoded = static_cast<qint64>(result.bytes_base64.size()) * 3 / 4;
    QString summary = QString("Loaded %1 — %2×%3, %4")
        .arg(fi.fileName())
        .arg(probe.width())
        .arg(probe.height())
        .arg(formatSize(fi.size()));
    if (encoded != fi.size())
        summary += QString(" → %1, %2 after preprocessing")
            .arg(result.mime, formatSize(encoded));
    return summary + ". The image is attached below; look at it directly.";
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

// ── sudo askpass helper ──────────────────────────────────────────────

// Temporary SUDO_ASKPASS helper script. The password itself never touches the
// disk — the script just echoes the PENGY_SUDO_PASSWORD environment variable
// handed to the child process. Directory and script are both mode 0700.
//
// Askpass replaces the older "pipe the password to the shell's stdin and
// rewrite the first sudo to `sudo -S`" approach, which broke whenever anything
// else in the command touched stdin: a pipeline (`echo x | sudo tee f`), a
// redirect (`sudo cmd < /dev/null`), an earlier command that reads stdin, or a
// second sudo after the single piped password had been consumed.
class AskpassHelper {
public:
    AskpassHelper() {
        qint64 nanos = QDateTime::currentMSecsSinceEpoch();
        qint64 pid   = QCoreApplication::applicationPid();
        m_dir = QDir::tempPath() + QString("/pengy-askpass-%1-%2").arg(pid).arg(nanos);
        if (!QDir().mkpath(m_dir)) return;
        m_path = m_dir + "/askpass.sh";
        QFile f(m_path);
        if (!f.open(QIODevice::WriteOnly)) return;
        f.write("#!/bin/sh\nprintf '%s\\n' \"$PENGY_SUDO_PASSWORD\"\n");
        f.close();
        QFile::setPermissions(m_dir,
            QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QFile::setPermissions(m_path,
            QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        m_valid = true;
    }

    ~AskpassHelper() {
        if (!m_path.isEmpty()) QFile::remove(m_path);
        if (!m_dir.isEmpty())  QDir().rmdir(m_dir);
    }

    AskpassHelper(const AskpassHelper&) = delete;
    AskpassHelper& operator=(const AskpassHelper&) = delete;

    bool valid() const { return m_valid; }
    QString path() const { return m_path; }

private:
    QString m_dir;
    QString m_path;
    bool    m_valid = false;
};

// Rewrite *every* word-boundary `sudo` to `sudo -A` so it authenticates via
// SUDO_ASKPASS rather than stdin. Any existing `-S` is dropped, since stdin no
// longer carries the password. Word-boundary matching leaves `sudoku` and
// `pseudo-tty` untouched.
QString rewriteSudoForAskpass(QString command) {
    static QRegularExpression stdinRx("\\bsudo\\s+-S\\b");
    static QRegularExpression rewriteRx("\\bsudo\\b(?!\\s+-A\\b)");
    command.replace(stdinRx, "sudo");
    command.replace(rewriteRx, "sudo -A");
    return command;
}

// ── Bash (with temp file output & process groups) ────────────────────

static QString toolRunBash(const QJsonObject& args, std::atomic<bool>* cancel,
                           ToolContext* ctx) {
    QString command = aStr(args, "command");
    if (command.isEmpty()) return "Error: command is required.";

    QString cwd = expandHome(aStr(args, "cwd"));
    if (!cwd.isEmpty() && !QFileInfo(cwd).isDir())
        return "Error: cwd not found or not a directory: " + cwd;

    int timeoutSecs = toolTimeout();

    // ── sudo detection ──────────────────────────────────────────────
    static QRegularExpression sudoRx("\\bsudo\\b");
    bool needsSudo = sudoRx.match(command).hasMatch();

    std::unique_ptr<AskpassHelper> askpass;
    if (needsSudo) {
        if (ctx->cachedSudoPassword().isEmpty()) {
            auto provider = ctx->sudoProvider();
            if (!provider) {
                return "Error: sudo detected but no password provider is configured.";
            }
            QString pw = provider();
            if (pw.isEmpty()) {
                return "Cancelled: sudo password not provided.";
            }
            ctx->setCachedSudoPassword(pw);
        }
        askpass = std::make_unique<AskpassHelper>();
        if (!askpass->valid()) {
            return "Error: Could not create sudo askpass helper.";
        }
        command = rewriteSudoForAskpass(command);
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
    // The command never inherits our stdin: the password goes via askpass, and
    // a child reading the terminal would hang the GUI/CLI.
    proc.setStandardInputFile(QProcess::nullDevice());
    if (!cwd.isEmpty()) proc.setWorkingDirectory(cwd);

    if (askpass) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("SUDO_ASKPASS", askpass->path());
        env.insert("PENGY_SUDO_PASSWORD", ctx->cachedSudoPassword());
        proc.setProcessEnvironment(env);
    }

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
    ctx->registerProcess(pid);


    int waitMs = timeoutSecs > 0 ? timeoutSecs * 1000 : -1;

    if (cancel) {
        int elapsed = 0;
        int step    = 100;
        while (!proc.waitForFinished(step)) {
            if (cancel->load()) {
                terminateProcessGroup(pid);
                proc.kill();
                proc.waitForFinished(2000);
                ctx->unregisterProcess(pid);
                removeOutputFiles(tmpFiles);
                return "Error: Command was cancelled.";
            }
            elapsed += step;
            if (waitMs > 0 && elapsed >= waitMs) {
                terminateProcessGroup(pid);
                proc.kill();
                proc.waitForFinished(2000);
                ctx->unregisterProcess(pid);
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
            ctx->unregisterProcess(pid);
            removeOutputFiles(tmpFiles);
            return QString("Error: Command timed out after %1 seconds.").arg(timeoutSecs);
        }
    }

    ctx->unregisterProcess(pid);

    QString out = readAndRemove(tmpFiles.stdoutPath);
    QString err = readAndRemove(tmpFiles.stderrPath);

    // Strip sudo password prompt lines from stderr only
    static QRegularExpression sudoPromptRx("^\\[sudo[^\\]]*\\].*\\n?", QRegularExpression::MultilineOption);
    err.remove(sudoPromptRx);
    err = err.trimmed();

    if (!err.isEmpty()) {
        out += "\n" + err;
    }

    if (proc.exitCode() != 0)
        out += QString("\n[Exit code: %1]").arg(proc.exitCode());

    return out.trimmed().isEmpty() ? "(No output)" : snipMiddle(out);
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
    static QRegularExpression splitRx("[^a-zA-Z0-9]+");
    QStringList tokens;
    for (const QString& word : query.split(splitRx)) {
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

/// Reduce *raw* to a bare filename inside ~/Downloads.
///
/// The model chooses this name and may be acting on instructions from a fetched
/// page, so a path component here must never escape the download directory —
/// "../../.bashrc" has to land as ".bashrc".  Backslashes are folded too so a
/// Windows-style path can't slip through on POSIX.
static QString safeDownloadName(const QString& raw) {
    QString name = QString(raw).replace('\\', '/').section('/', -1).trimmed();
    if (name.isEmpty() || name == "." || name == "..") return "download";
    return name;
}

QString safeDownloadNameForTest(const QString& raw) { return safeDownloadName(raw); }
QString snipMiddleForTest(const QString& text) { return snipMiddle(text); }

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

    if (filename.isEmpty())
        filename = urlStr.split('?').first().split('/').last();

    QString dest = downloads + "/" + safeDownloadName(filename);
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

    // Truncate to the configured output limit (or an explicit max_chars
    // override); 0/negative means no limit.
    int limit;
    if (args.contains("max_chars")) {
        limit = aInt(args, "max_chars", 0);
    } else {
        limit = g_toolOutputMaxChars;
    }

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
    QString textLower = text.toLower();
    bool isHtml = contentType.contains("html") ||
                  textLower.contains("<html") ||
                  textLower.contains("<!doctype");

    if (isHtml) {
        // Extract <body> content for cleaner text
        static QRegularExpression bodyRx("<body[^>]*>([\\s\\S]*)</body>",
                                  QRegularExpression::CaseInsensitiveOption);
        auto bm = bodyRx.match(text);
        QString bodyHtml = bm.hasMatch() ? bm.captured(1) : text;
        text = stripTags(bodyHtml);
        static QRegularExpression newlineRx("\\n{3,}");
        text.replace(newlineRx, "\n\n");
        text = text.trimmed();
    }

    if (limit > 0 && text.size() > limit) {
        text = text.left(limit) + QString("\n\n[... truncated at %1 characters — pass max_chars to adjust ...]").arg(limit);
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

static QString toolRunPython(const QJsonObject& args, ToolContext* ctx) {
    QString code = aStr(args, "code");
    if (code.isEmpty()) return "Error: code is required.";

    QString cwd = expandHome(aStr(args, "cwd"));
    if (!cwd.isEmpty() && !QFileInfo(cwd).isDir())
        return "Error: cwd not found or not a directory: " + cwd;

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
    if (!cwd.isEmpty()) proc.setWorkingDirectory(cwd);

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
    ctx->registerProcess(pid);

    int timeoutMs = toolTimeout() > 0 ? toolTimeout() * 1000 : -1;
    if (!proc.waitForFinished(timeoutMs)) {
        terminateProcessGroup(pid);
        proc.kill();
        proc.waitForFinished(2000);
        ctx->unregisterProcess(pid);
        removeOutputFiles(tmpFiles);
        return "Error: Python execution timed out.";
    }

    ctx->unregisterProcess(pid);

    QString out = readAndRemove(tmpFiles.stdoutPath);
    QString err = readAndRemove(tmpFiles.stderrPath);

    if (!err.trimmed().isEmpty()) {
        out += "\n" + err;
    }

    if (proc.exitCode() != 0)
        out += QString("\n[Exit code: %1]").arg(proc.exitCode());
    return out.trimmed().isEmpty() ? "(No output)" : snipMiddle(out);
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
    return snipMiddle(result);
}

// ── Read multiple files ──────────────────────────────────────────────

static QString toolReadMultipleFiles(const QJsonObject& args) {
    QJsonArray pathsArr = args["paths"].toArray();
    if (pathsArr.isEmpty()) return "Error: no paths provided.";

    const int MAX_FILES = 20;

    if (pathsArr.size() > MAX_FILES)
        return QString("Error: too many files (%1). Maximum is %2.").arg(pathsArr.size()).arg(MAX_FILES);

    // Derive per-file and total budgets from the tool output limit so the
    // single "max tool output" setting governs how much context a batch can
    // consume.  0 means "no limit".
    const int budget      = g_toolOutputMaxChars;
    const int perFile     = budget;
    const int totalBudget = budget > 0 ? budget * 5 : 0;

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

        // Same head-truncation and line-range reporting as read_file, so the
        // model can follow up with read_file(offset=...) on whichever file was cut.
        {
            const int totalLines = content.count('\n') + 1;
            int kept = 0;
            bool truncated = false;
            content = truncateHeadLines(content, perFile, &kept, &truncated);
            if (truncated)
                content += QString("\n\n[... showed lines 1-%1 of %2 — "
                                   "read_file with offset=%3 to continue ...]")
                               .arg(kept).arg(totalLines).arg(kept + 1);
        }

        QString block = header + "\n" + content;
        if (totalBudget > 0 && total + block.size() > totalBudget) {
            int remaining = totalBudget - total;
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
    static QRegularExpression braceRx(R"(^(.*)\{([^}]+)\}(.*)$)");
    auto m = braceRx.match(glob);
    if (m.hasMatch()) {
        QString pre  = m.captured(1);
        QString suf  = m.captured(3);
        for (const QString& choice : m.captured(2).split(',')) {
            QString pat = pre + choice + suf;
            pat.replace(QLatin1Char('.'), QLatin1String("\\."));
            pat.replace(QLatin1Char('*'), QLatin1String(".*"));
            pat.replace(QLatin1Char('?'), QLatin1String("."));
            if (QRegularExpression("^" + pat + "$").match(name).hasMatch())
                return true;
        }
        return false;
    }
    // Cache compiled glob regexes — the glob is constant for an entire
    // search_content call, so this avoids recompiling per file.
    static QHash<QString, QRegularExpression> globCache;
    auto it = globCache.constFind(glob);
    if (it == globCache.constEnd()) {
        QString pat = QString(glob);
        pat.replace(QLatin1Char('.'), QLatin1String("\\."));
        pat.replace(QLatin1Char('*'), QLatin1String(".*"));
        pat.replace(QLatin1Char('?'), QLatin1String("."));
        it = globCache.insert(glob, QRegularExpression("^" + pat + "$"));
    }
    return it.value().match(name).hasMatch();
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
    bool    regex        = aBool(args, "regex", false);

    if (pattern.isEmpty()) return "Error: pattern is required.";

    QFileInfo pathInfo(path);
    if (!pathInfo.exists()) return "Error: Path not found: " + path;

    // Literal by default so metacharacters in code symbols (".", "(", "[", "*",
    // ...) don't silently become regex syntax; regex=true opts into regex.
    QRegularExpression rx;
    if (regex) {
        rx = QRegularExpression(pattern);
        if (!rx.isValid())
            return "Error: Invalid regex pattern.";
    } else {
        rx = QRegularExpression(QRegularExpression::escape(pattern));
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
    return snipMiddle(summary + "\n" + QString(60, QChar(0x2500)) + "\n" + results.join("\n\n"));
}

static QString toolApplyChanges(const QJsonObject& args) {
    constexpr int maxFiles=20, maxOps=100, maxBlock=256000, maxResult=1000000;
    QJsonArray changes=args["changes"].toArray();
    if(changes.isEmpty()) return "Error: changes must be a non-empty list.";
    if(changes.size()>maxFiles) return QString("Error: too many files (%1). Maximum is %2.").arg(changes.size()).arg(maxFiles);
    struct Prepared { QString path, oldText, newText; };
    QList<Prepared> prepared; QStringList errors; QSet<QString> paths; int ops=0, bytes=0;
    for(int fi=0;fi<changes.size();++fi){
        QJsonObject file=changes[fi].toObject(); QString path=expandHome(file["path"].toString());
        if(path.isEmpty()){errors<<QString("file %1: path is required").arg(fi);continue;}
        QFileInfo info(path); if(!info.exists()){errors<<path+": file not found";continue;} if(!info.isFile()){errors<<path+": not a file";continue;}
        if(paths.contains(path)){errors<<path+": duplicate path";continue;} paths.insert(path);
        QFile in(path); if(!in.open(QIODevice::ReadOnly)){errors<<path+": binary or non-UTF-8 file";continue;} QByteArray raw=in.readAll();in.close(); QString old=QString::fromUtf8(raw);if(QString::fromUtf8(old.toUtf8())!=old){errors<<path+": binary or non-UTF-8 file";continue;}
        QString cur=old; QJsonArray operations=file["operations"].toArray(); if(operations.isEmpty()){errors<<path+": operations must be non-empty";continue;} ops+=operations.size();
        for(int oi=0;oi<operations.size();++oi){QJsonObject op=operations[oi].toObject();QString kind=op["kind"].toString();int expected=op["expected_matches"].toInt(1);QString needle,repl;
            if(kind=="replace"||kind=="delete"){needle=op["old"].toString();repl=kind=="delete"?QString():op["new"].toString();}
            else if(kind=="insert_after"){needle=op["anchor"].toString();repl=needle+op["text"].toString();}
            else {errors<<QString("%1 operation %2: unknown kind %3").arg(path).arg(oi).arg(kind);continue;}
            if(needle.isEmpty()){errors<<QString("%1 operation %2: match text must be non-empty").arg(path).arg(oi);continue;}
            if(needle.toUtf8().size()>maxBlock||repl.toUtf8().size()>maxBlock){errors<<QString("%1 operation %2: text block exceeds %3 bytes").arg(path).arg(oi).arg(maxBlock);continue;}
            int count=cur.count(needle);if(count!=expected){errors<<QString("%1 operation %2: matches %3 locations; expected %4").arg(path).arg(oi).arg(count).arg(expected);continue;}
            int pos=0;for(int n=0;n<expected;++n){pos=cur.indexOf(needle,pos);cur.replace(pos,needle.size(),repl);pos+=repl.size();}
        }
        bytes+=old.toUtf8().size()+cur.toUtf8().size();prepared.append({path,old,cur});
    }
    if(ops>maxOps)errors<<QString("too many operations; maximum is %1").arg(maxOps);if(bytes>maxResult)errors<<QString("result exceeds %1 bytes").arg(maxResult);
    QJsonArray conditions=args["postconditions"].toArray();for(int i=0;i<conditions.size();++i){QJsonObject c=conditions[i].toObject();QString path=expandHome(c["path"].toString()),content;for(const auto& p:prepared)if(p.path==path)content=p.newText;if(content.isEmpty()){QFile f(path);if(f.open(QIODevice::ReadOnly))content=QString::fromUtf8(f.readAll());}if(c.contains("contains")&&!content.contains(c["contains"].toString()))errors<<QString("postcondition %1: %2 does not contain expected text").arg(i).arg(path);if(c.contains("does_not_contain")&&content.contains(c["does_not_contain"].toString()))errors<<QString("postcondition %1: %2 still contains forbidden text").arg(i).arg(path);}
    if(!errors.isEmpty())return "Error: no changes applied.\n- "+errors.join("\n- ");
    QString diff;for(const auto& p:prepared)if(p.oldText!=p.newText)diff+=QString("--- %1\n+++ %1\n@@ changed content: %2 -> %3 bytes @@\n").arg(p.path).arg(p.oldText.toUtf8().size()).arg(p.newText.toUtf8().size());
    if(args["dry_run"].toBool(false))return QString("Dry run: no changes applied.\nFiles: %1\n\n%2").arg(prepared.size()).arg(diff).trimmed();
    QStringList temps;for(const auto& p:prepared){QString tmp=p.path+QString(".pengy-tmp-%1").arg(QCoreApplication::applicationPid());QFile f(tmp);if(!f.open(QIODevice::WriteOnly|QIODevice::Truncate)){for(const auto&t:temps)QFile::remove(t);return "Error: write failed; no changes applied.";}f.write(p.newText.toUtf8());f.close();temps<<tmp;}
    for(int i=0;i<prepared.size();++i){QFile::remove(prepared[i].path);if(!QFile::rename(temps[i],prepared[i].path))return "Error: rename failed after validation; changes may be partially applied.";}
    return QString("Applied changes to %1 file(s).\n\n%2").arg(prepared.size()).arg(diff).trimmed();
}

static QString toolGlob(const QJsonObject& args);
static QString toolTodowrite(const QJsonObject& args);

// ── Dispatcher ────────────────────────────────────────────────────────

QString execute(const QString& name, const QJsonObject& args,
                std::atomic<bool>* cancel, ToolContext* ctx) {
    if (!ctx) ctx = &g_defaultContext;
    if (name == "read_file")          return toolReadFile(args);
    if (name == "read_image")         return toolReadImage(args, ctx);
    if (name == "write_file")         return toolWriteFile(args);
    if (name == "replace_in_file")    return toolReplaceInFile(args);
    if (name == "apply_changes")      return toolApplyChanges(args);
    if (name == "run_bash")           return toolRunBash(args, cancel, ctx);
    if (name == "web_search")         return toolWebSearch(args);
    if (name == "download_file")      return toolDownloadFile(args);
    if (name == "fetch_url")          return toolFetchUrl(args);
    if (name == "run_python")         return toolRunPython(args, ctx);
    if (name == "directory_tree")     return toolDirectoryTree(args);
    if (name == "read_multiple_files") return toolReadMultipleFiles(args);
    if (name == "search_content")     return toolSearchContent(args);
    if (name == "glob")              return toolGlob(args);
    if (name == "todowrite")         return toolTodowrite(args);
    if (name == "ask_user_question") return "ask_user_question must be handled by the harness — it should never reach execute_tool directly.";
    return "Unknown tool: " + name;
}


// ── glob ──────────────────────────────────────────────────────────

static QString toolGlob(const QJsonObject& args) {
    QString pattern = args["pattern"].toString();
    QString pathStr = expandHome(args["path"].toString());

    // When no explicit path is given and the pattern contains '/',
    // extract the longest existing directory prefix from the pattern
    // so that e.g. "~/src/*.py" works without a separate path argument.
    if (pathStr.isEmpty() && pattern.contains('/')) {
        QString expanded = expandHome(pattern);
        QFileInfo efi(expanded);
        QDir current = efi.isDir() ? QDir(expanded) : efi.dir();
        // Walk up until we find an existing directory
        while (!current.exists() && !current.isRoot()) {
            current.cdUp();
        }
        if (current.exists()) {
            pathStr = current.path();
            QStringList parts = pattern.split('/');
            QString nameFilter = parts.last();
            if (pattern.contains("**/"))
                nameFilter = "**/" + nameFilter;
            pattern = nameFilter;
        }
    }

    QDir searchDir(pathStr.isEmpty() ? QDir::currentPath() : pathStr);
    if (!searchDir.exists())
        return QString("Error: Directory not found: %1").arg(searchDir.path());

    bool recursive = pattern.contains("**");
    QStringList parts = pattern.split('/');
    // The filename pattern is the last component of the glob
    QString nameFilter = parts.last();

    // A pattern whose final component starts with "." is asking for hidden
    // entries.  Testing the whole pattern would miss "**/.config" or "src/.env",
    // since those start with "*" and "s".
    const bool wantsHidden = nameFilter.startsWith('.');

    QStringList matches;
    QDirIterator::IteratorFlags flags = QDirIterator::NoIteratorFlags;
    if (recursive)
        flags |= QDirIterator::Subdirectories;

    // Convert glob wildcards to Qt wildcards: * → *, ? → ?
    // Qt already uses the same wildcard syntax; pass through as-is.
    QStringList nameFilters;
    nameFilters << nameFilter;

    QSet<QString> skipDirs = {".git", ".svn", ".hg", "__pycache__", "node_modules",
                               ".mypy_cache", ".pytest_cache", ".ruff_cache", ".tox", ".eggs",
                               ".venv", "venv", ".env", "build", "dist", "target"};

    // Hidden entries are not enumerated at all unless QDir::Hidden is set, so
    // the wantsHidden check below can only work if we ask for them here first.
    QDir::Filters dirFilters = QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot;
    if (wantsHidden) dirFilters |= QDir::Hidden;

    QDirIterator it(searchDir.path(), nameFilters, dirFilters, flags);
    while (it.hasNext()) {
        it.next();
        QString fname = it.fileName();
        QFileInfo fi = it.fileInfo();

        // Only directories are pruned by the skip set: ".env" and "target" are
        // in there as *directory* names, and matching them against files made a
        // plain ".env" file unfindable.
        if (fi.isDir() && skipDirs.contains(fname)) continue;
        if (fname.startsWith('.') && !wantsHidden) continue;

        QString absPath = fi.absoluteFilePath();
        // Also skip entries whose parent path goes through a skipped directory.
        // Ancestors only — the final component is the entry itself, checked above.
        QString rel = searchDir.relativeFilePath(absPath);
        QStringList relParts = rel.split('/');
        bool inSkipDir = false;
        for (int i = 0; i < relParts.size() - 1; ++i) {
            if (skipDirs.contains(relParts[i])) { inSkipDir = true; break; }
        }
        if (inSkipDir) continue;

        if (fi.isDir())
            matches.append(rel + "/");
        else
            matches.append(QString("%1  (%2 B)").arg(rel).arg(fi.size()));
    }

    if (matches.isEmpty())
        return QString("No files matching '%1' in %2").arg(pattern, searchDir.path());

    matches.sort();
    int maxResults = 200;
    QStringList result;
    for (int i = 0; i < qMin(matches.size(), maxResults); ++i)
        result.append(matches[i]);
    if (matches.size() > maxResults)
        result.append(QString("... and %1 more (truncated at %2)").arg(matches.size() - maxResults).arg(maxResults));

    return result.join("\n");
}

// ── todowrite ─────────────────────────────────────────────────────

static QString toolTodowrite(const QJsonObject& args) {
    QJsonArray todos = args["todos"].toArray();
    if (todos.isEmpty())
        return "Error: todos list is empty. Provide at least one task.";

    int inProgressCount = 0;
    QStringList errors;

    for (int i = 0; i < todos.size(); ++i) {
        QJsonObject t = todos[i].toObject();
        QString contentText = t["content"].toString();
        QString status = t["status"].toString();

        if (contentText.isEmpty())
            errors.append(QString("Item %1: content is empty").arg(i));
        if (status != "pending" && status != "in_progress" && status != "completed")
            errors.append(QString("Item %1: invalid status '%2'").arg(i).arg(status));
        if (status == "in_progress")
            inProgressCount++;
    }

    if (!errors.isEmpty())
        return "Error validating todos:\n" + errors.join("\n");

    if (inProgressCount > 1)
        return QString("Error: %1 tasks marked in_progress. Exactly one must be in_progress.").arg(inProgressCount);

    QStringList lines;
    for (const QJsonValue& v : todos) {
        QJsonObject t = v.toObject();
        QString contentText = t["content"].toString();
        QString status = t["status"].toString();
        QString icon;
        if (status == "pending")      icon = "[ ]";
        else if (status == "in_progress") icon = "[→]";
        else if (status == "completed")   icon = "[✓]";
        else icon = "[?]";
        lines.append(QString("%1 %2").arg(icon, contentText));
    }
    return lines.join("\n");
}

} // namespace Tools
