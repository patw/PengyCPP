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
#include <QStorageInfo>
#include <QElapsedTimer>
#include <QUuid>
#include <QCoreApplication>
#include <QThread>
#include <QUrlQuery>
#include <QImage>
#include "image_utils.h"
#include <QJsonDocument>
#include <QAtomicInteger>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <vector>
#include <functional>
#include <tuple>
#include <algorithm>
#include <climits>
#include <cerrno>
#include <cstring>

#ifdef Q_OS_UNIX
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#endif

#ifdef Q_OS_WIN
// shell32 (linked by default); declared here to keep <windows.h> and its
// min/max macros out of this file.
extern "C" __declspec(dllimport) int __stdcall IsUserAnAdmin(void);
#endif

namespace Tools {

static QString   g_userAgent = "PengyAgent/1.0";
static int       g_timeout   = 300;
static int       g_imageMaxDimension = 4096;
static double    g_imageMaxMb        = 4.5;
static int       g_imageQuality      = 85;
static int       g_toolOutputMaxChars = 250000;
static int       g_downloadMaxMb      = 100;
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
    m_cachedSudoPasswords.clear();
}
SudoPasswordFn ToolContext::sudoProvider() {
    QMutexLocker lock(&m_mutex);
    return m_sudoProvider;
}
QString ToolContext::cachedSudoPassword(const QString& host) {
    QMutexLocker lock(&m_mutex);
    return m_cachedSudoPasswords.value(host);
}
bool ToolContext::hasCachedSudoPassword(const QString& host) {
    QMutexLocker lock(&m_mutex);
    return m_cachedSudoPasswords.contains(host);
}
void ToolContext::setCachedSudoPassword(const QString& pw, const QString& host) {
    QMutexLocker lock(&m_mutex);
    m_cachedSudoPasswords.insert(host, pw);
}
void ToolContext::forgetSudoPassword(const QString& host) {
    QMutexLocker lock(&m_mutex);
    m_cachedSudoPasswords.remove(host);
}
void ToolContext::clearSudo() {
    QMutexLocker lock(&m_mutex);
    m_cachedSudoPasswords.clear();
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
void setDownloadMaxMb(int mb) {
    QMutexLocker lock(&g_mutex);
    g_downloadMaxMb = mb;
}
static QString userAgent() {
    QMutexLocker lock(&g_mutex);
    return g_userAgent;
}
static int toolTimeout() {
    QMutexLocker lock(&g_mutex);
    return g_timeout;
}
static int downloadMaxMb() {
    QMutexLocker lock(&g_mutex);
    return g_downloadMaxMb;
}

// ── Process group management ──────────────────────────────────────────

static QString systemRoot() {
    QString root = qEnvironmentVariable("SystemRoot");
    return root.isEmpty() ? QStringLiteral("C:/Windows") : QDir::fromNativeSeparators(root);
}

static void terminateProcessGroup(qint64 pid) {
#ifdef Q_OS_UNIX
    QProcess::execute("kill", {"-9", QString("-%1").arg(pid)});
#else
    // taskkill /T walks the parent-PID tree (Windows has no process group to
    // signal).  Absolute path so a taskkill.exe earlier on PATH can't stand in
    // for it.  From a GUI parent QProcess already starts console children with
    // CREATE_NO_WINDOW, so nothing flashes.
    QProcess::execute(systemRoot() + "/System32/taskkill.exe",
                      {"/PID", QString::number(pid), "/T", "/F"});
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

const QJsonArray& baseToolDefinitions() {
    // Built once; QJsonArray is implicitly shared so callers copy cheaply.
    static const QJsonArray defs = QJsonArray{
        td("read_file", "Read a text file. Long files are truncated with a header saying how to continue using offset/limit. Use read_image for images.",
            QJsonObject{
                {"path",   prop("string", "File path")},
                {"offset", prop("integer", "First line to read (1-based)")},
                {"limit",  prop("integer", "Maximum number of lines")}},
            QJsonArray{"path"}),

        td("read_image", "Look at an image file (PNG, JPEG, GIF, WebP, BMP, TIFF); it is attached so you can see it.",
            QJsonObject{{"path", prop("string", "Image file path")}},
            QJsonArray{"path"}),

        td("write_file", "Create or overwrite a file. Parent directories are created.",
            QJsonObject{
                {"path",    prop("string", "File path")},
                {"content", prop("string", "Full file content")}},
            QJsonArray{"path", "content"}),

        td("replace_in_file",
            "Replace exact text in a file. old_str must match exactly one place (include surrounding lines to make it unique) unless replace_all=true.",
            QJsonObject{
                {"path",    prop("string", "File path")},
                {"old_str", prop("string", "Exact text to replace, including whitespace")},
                {"new_str", prop("string", "Replacement text (empty to delete)")},
                {"replace_all", prop("boolean", "Replace every occurrence")}},
            QJsonArray{"path", "old_str", "new_str"}),

        td("apply_changes",
            "Apply several exact-text edits across existing files, all or nothing. dry_run=true returns the unified diff without writing.",
            QJsonObject{
                {"changes", QJsonObject{
                    {"type", "array"},
                    {"description", "Per file: path and operations. An operation is replace (old, new), delete (old) or insert_after (anchor, text); old/anchor must match expected_matches times (default 1)"},
                    {"items", QJsonObject{
                        {"type", "object"},
                        {"properties", QJsonObject{
                            {"path", QJsonObject{
                                {"type", "string"},
                                {"description", "File path"}
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
                                            {"description", "Exact text for replace/delete"}
                                        }},
                                        {"anchor", QJsonObject{
                                            {"type", "string"},
                                            {"description", "Exact text to insert after"}
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
                                            {"description", "Required match count (default 1)"}
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
                    {"description", "Preview only; nothing is written"}
                }},
                {"postconditions", QJsonObject{
                    {"type", "array"},
                    {"description", "Optional {path, contains, does_not_contain} checks on the edited text; a failed check cancels every edit"},
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

        td("run_bash", "Run a bash command and return its output. Non-interactive: stdin is closed. For root, write sudo in the command and set elevated=true. To run on another machine set host (don't call ssh yourself). Killed after the tool timeout.",
            QJsonObject{
                {"command", prop("string", "Bash command")},
                {"cwd",     prop("string", "Working directory")},
                {"host",    prop("string", "ssh destination (key login must already work)")},
                {"elevated", prop("boolean", "true when the command uses sudo")}},
            QJsonArray{"command"}),

        td("web_search",
            "Search the web; returns titles, URLs and snippets.",
            QJsonObject{
                {"query",       prop("string",  "Search query")},
                {"max_results", prop("integer", "Number of results (default 5)")}},
            QJsonArray{"query"}),

        td("download_file", "Download a URL to a file and return the saved path.",
            QJsonObject{
                {"url",         prop("string",  "URL to download")},
                {"filename",    prop("string",  "File name (default: from the URL)")},
                {"dir",         prop("string",  "Directory to save into (default ~/Downloads)")},
                {"max_size_mb", prop("integer", "Size limit in MB (default: the configured limit; 0 = none)")}},
            QJsonArray{"url"}),

        td("fetch_url", "Fetch a URL and return the body as text (HTML is converted to text).",
            QJsonObject{
                {"url",       prop("string",  "URL to fetch")},
                {"max_chars", prop("integer", "Maximum characters returned (0 = no limit)")}},
            QJsonArray{"url"}),

        td("run_python", "Run Python code in a fresh process; only printed output comes back. Killed after the tool timeout.",
            QJsonObject{
                {"code", prop("string", "Python source")},
                {"cwd",  prop("string", "Working directory")}},
            QJsonArray{"code"}),

        td("directory_tree",
            "Show a directory tree; use it to get oriented in an unfamiliar project. Skips .git, node_modules and similar.",
            QJsonObject{
                {"path",        prop("string",  "Directory")},
                {"max_depth",   prop("integer", "Depth limit (default 3)")},
                {"show_hidden", prop("boolean", "Include dot-files")}},
            QJsonArray{"path"}),

        td("read_multiple_files", "Read several text files in one call.",
            QJsonObject{{"paths", QJsonObject{
                {"type", "array"},
                {"description", "File paths"},
                {"items", QJsonObject{{"type", "string"}}}}}},
            QJsonArray{"paths"}),

        td("search_content",
            "Search file contents under a directory or in a file. Literal text unless regex=true. Skips binary files and .git, node_modules and similar.",
            QJsonObject{
                {"pattern",       prop("string",  "Text (or regex) to find")},
                {"regex",         prop("boolean", "Treat pattern as a regular expression")},
                {"path",          prop("string",  "Directory or file to search")},
                {"file_glob",     prop("string",  "Only files matching this glob, e.g. *.py or src/**/*.ts")},
                {"context_lines", prop("integer", "Lines of context around each match")},
                {"max_results",   prop("integer", "Maximum matches (default 50)")},
                {"ignore_case",   prop("boolean", "Case-insensitive match")}},
            QJsonArray{"pattern", "path"}),

        td("glob", "Find files by glob pattern; ** matches any depth. Skips hidden and build/vendor directories; at most 200 results.",
            QJsonObject{
                {"pattern", prop("string", "Glob, e.g. src/**/*.py")},
                {"path",    prop("string", "Directory to search (default: current)")}},
            QJsonArray{"pattern"}),

        td("todowrite",
            "Keep a task list for multi-step work. Send the whole list each time; at most one task in_progress.",
            QJsonObject{
                {"todos", QJsonObject{
                    {"type", "array"},
                    {"description", "All tasks with their status"},
                    {"items", QJsonObject{
                        {"type", "object"},
                        {"properties", QJsonObject{
                            {"content", QJsonObject{
                                {"type", "string"},
                                {"description", "Task, imperative (e.g. 'Run the tests')"}
                            }},
                            {"status", QJsonObject{
                                {"type", "string"},
                                {"enum", QJsonArray{"pending", "in_progress", "completed"}},
                                {"description", "pending, in_progress or completed"}
                            }}
                        }},
                        {"required", QJsonArray{"content", "status"}}
                    }}
                }}
            },
            QJsonArray{"todos"}),

        td("ask_user_question", "Ask the user multiple-choice questions. Use it instead of guessing when the request is ambiguous, or when what you found contradicts what the user said.",
            QJsonObject{{"questions", QJsonObject{
                {"type", "array"},
                {"description", "Questions, each with a header, question text and options"},
                {"items", QJsonObject{
                    {"type", "object"},
                    {"properties", QJsonObject{
                        {"header", QJsonObject{
                            {"type", "string"},
                            {"description", "Short label, e.g. 'Auth'"}
                        }},
                        {"question", QJsonObject{
                            {"type", "string"},
                            {"description", "The question"}
                        }},
                        {"options", QJsonObject{
                            {"type", "array"},
                            {"description", "The choices"},
                            {"items", QJsonObject{
                                {"type", "object"},
                                {"properties", QJsonObject{
                                    {"label", QJsonObject{
                                        {"type", "string"},
                                        {"description", "Choice label"}
                                    }},
                                    {"description", QJsonObject{
                                        {"type", "string"},
                                        {"description", "One-line explanation of the choice"}
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

// ── Platform-specific tool surface ────────────────────────────────────
//
// One local-shell tool per platform, named for the shell it really runs: the
// tool name is the strongest hint a model gets about which syntax to write.
// POSIX keeps run_bash exactly as defined above.  Windows gets run_powershell
// for local commands, and run_bash survives only for remote hosts (host=),
// since an ssh target really does run a POSIX shell.  No shell translation
// either way.  Keep the wording identical to the Python and Rust editions.

static bool isWindowsHost() {
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

// PowerShell 7 (pwsh) is preferred when installed, but a clean Windows 11
// ships only Windows PowerShell 5.1, so that is the supported floor.  The
// absolute System32 path covers a PATH that has lost the v1.0 directory.
// Never falls back to cmd.exe.  Resolved once; empty off Windows.
static QString powershellPath() {
    static const QString path = []() -> QString {
        if (!isWindowsHost()) return {};
        for (const char* name : {"pwsh", "powershell"}) {
            QString found = QStandardPaths::findExecutable(name);
            if (!found.isEmpty()) return found;
        }
        QString builtin = systemRoot() + "/System32/WindowsPowerShell/v1.0/powershell.exe";
        return QFileInfo(builtin).isFile() ? builtin : QString();
    }();
    return path;
}

static bool windowsIsAdmin() {
#ifdef Q_OS_WIN
    return IsUserAnAdmin() != 0;
#else
    return false;
#endif
}

QString powershellLabel(const QString& path) {
    QString name = QString(path).replace('\\', '/').section('/', -1).toLower();
    if (name == "pwsh" || name == "pwsh.exe") return "PowerShell 7";
    return "Windows PowerShell 5.1";
}

static QJsonObject runPowershellDefinition(const QString& label, bool isAdmin) {
    const QString privilege = isAdmin
        ? QStringLiteral("Pengy is running as Administrator, so commands already have full administrative rights (HKLM registry, services, scheduled tasks, firewall, Windows features); no elevation step is needed.")
        : QStringLiteral("Pengy is NOT running as Administrator. Commands that need admin rights (writing HKLM, managing services, scheduled tasks that run as SYSTEM or with highest privileges, firewall rules, Windows features, machine-wide installs) fail with access denied. Do not try to self-elevate with Start-Process -Verb RunAs or sudo: the elevated process cannot be captured here. Instead tell the user the step needs admin rights: they can restart Pengy with Run as administrator, or run the command themselves.");
    const QString dialect = label == "Windows PowerShell 5.1"
        ? QStringLiteral(" This is Windows PowerShell 5.1, not PowerShell 7: there are no && / || chain operators and no ternary operator, and curl/wget are aliases for Invoke-WebRequest (use curl.exe for real curl).")
        : QString();
    const QString desc =
        QStringLiteral("Run a PowerShell script on this Windows machine with ") + label +
        QStringLiteral(". Use PowerShell syntax and cmdlets; this is not bash. The script may span multiple lines. It is non-interactive: stdin is closed, so anything that prompts (Read-Host, Get-Credential, confirmation prompts, an editor) fails rather than waits; pass -Force, -Confirm:$false or other non-interactive flags. Set cwd to run in a specific directory. Default table formatting is cut to a narrow width, so for wide or detailed objects pipe to Format-List, ConvertTo-Json, or Out-String -Width 4096. The exit code is the last native command's exit code, or 1 if the script throws or fails to parse; non-terminating errors are shown but do not change it. ") +
        privilege + dialect +
        QStringLiteral(" To run on a remote Linux or macOS machine, use run_bash with host. Commands are killed once the configured tool timeout elapses.");
    return td("run_powershell", desc,
        QJsonObject{
            {"command", prop("string", "The PowerShell script to execute")},
            {"cwd",     prop("string", "Optional working directory to run the script in")}},
        QJsonArray{"command"});
}

// Windows variant of run_bash: same parameters, host required.
static QJsonObject remoteOnlyRunBash(QJsonObject def) {
    QJsonObject fn = def["function"].toObject();
    fn["description"] = QStringLiteral("Run a bash command on a remote Linux or macOS host over ssh. On this Windows machine run_bash is only for remote hosts: host is required, and local commands go through run_powershell. Use the ssh destination the user uses (an ~/.ssh/config alias, host, or user@host); key-based login must already work. The command is non-interactive: stdin is closed, so anything that prompts or waits for input fails rather than waits; pass non-interactive flags instead. cwd, if given, is a path on the remote host. To run something as root there, include an explicit `sudo ...` in the command AND set elevated=true; Pengy then prompts for that host's sudo password. elevated=true does NOT elevate on its own: a command with elevated=true but no `sudo` is rejected. Do not wrap the command in ssh yourself. Commands are killed once the configured tool timeout elapses.");
    QJsonObject params = fn["parameters"].toObject();
    params["required"] = QJsonArray{"command", "host"};
    fn["parameters"] = params;
    def["function"] = fn;
    return def;
}

QJsonArray platformTools(const QJsonArray& base, bool windows,
                         const QString& powershellLabel, bool isAdmin) {
    if (!windows) return base;
    QJsonArray tools;
    for (const QJsonValue& v : base) {
        QJsonObject def = v.toObject();
        QJsonObject fn = def["function"].toObject();
        const QString name = fn["name"].toString();
        if (name == "run_bash") {
            tools.append(runPowershellDefinition(powershellLabel, isAdmin));
            tools.append(remoteOnlyRunBash(def));
            continue;
        }
        tools.append(def);
    }
    return tools;
}

const QJsonArray& toolDefinitions() {
    static const QJsonArray defs = platformTools(
        baseToolDefinitions(), isWindowsHost(), powershellLabel(powershellPath()), windowsIsAdmin());
    return defs;
}

QString windowsLocalRunBashError(bool windows, const QString& host) {
    if (windows && host.isEmpty())
        return "Error: on Windows, run_bash only runs on remote hosts (set host). "
               "Use run_powershell for commands on this machine.";
    return {};
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

// ── Argument normalization (see pengy/core/tool_args.py) ─────────────
//
// Models routinely send arguments that are *almost* right: "3" for an
// integer, "true" for a boolean, a bare string where a list is expected, or a
// key in the wrong place.  Each handler used to read only the keys it knew,
// so a misplaced key (dry_run inside a per-file change) or a mistyped value
// (dry_run: "true") was silently replaced by its default — and a "preview"
// wrote files.  normalizeArguments() runs once in execute() before any
// handler: unambiguous slips are coerced, null is an absent key, and anything
// else rejects the call with an error naming the key.  Keep the semantics and
// messages identical to tool_args.py and tool_args.rs.

static QString describeJson(const QJsonValue& v) {
    switch (v.type()) {
    case QJsonValue::Bool:   return "boolean";
    case QJsonValue::Double: return "number";
    case QJsonValue::String: return "string";
    case QJsonValue::Array:  return "array";
    case QJsonValue::Object: return "object";
    default:                 return "null";
    }
}

static QString previewJson(const QJsonValue& v) {
    QString text = QString::fromUtf8(QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact));
    text = text.mid(1, text.size() - 2);
    return text.size() <= 40 ? text : text.left(37) + "...";
}

static bool isIntegral(double d) { return std::isfinite(d) && d == std::floor(d); }

static QJsonValue coerceArg(const QJsonObject& schema, const QJsonValue& value, const QString& at,
                            QStringList& errors, const QSet<QString>& top);

static QJsonObject coerceObject(const QJsonObject& schema, const QJsonObject& value, const QString& at,
                                QStringList& errors, const QSet<QString>& top) {
    if (!schema.contains("properties")) return value;
    const QJsonObject props = schema["properties"].toObject();
    const QString prefix = at.isEmpty() ? QString() : at + ".";
    QJsonObject out;
    for (auto it = value.begin(); it != value.end(); ++it) {
        const QString key = it.key();
        if (!props.contains(key)) {
            if (!at.isEmpty() && top.contains(key)) {
                errors << QString("%1%2: not allowed here — %2 is a top-level argument").arg(prefix, key);
            } else {
                QString valid = props.keys().join(", ");
                errors << QString("%1%2: unknown key (valid keys: %3)")
                              .arg(prefix, key, valid.isEmpty() ? QStringLiteral("none") : valid);
            }
            continue;
        }
        if (it.value().isNull() || it.value().isUndefined()) continue;
        out[key] = coerceArg(props[key].toObject(), it.value(), prefix + key, errors, top);
    }
    for (const QJsonValue& r : schema["required"].toArray()) {
        if (!out.contains(r.toString()))
            errors << QString("%1%2: missing required key").arg(prefix, r.toString());
    }
    return out;
}

static QJsonValue coerceArg(const QJsonObject& schema, const QJsonValue& value, const QString& at,
                            QStringList& errors, const QSet<QString>& top) {
    const QString kind = schema["type"].toString();
    if (kind == "string") {
        QString out;
        if (value.isString()) out = value.toString();
        else if (value.isDouble() && isIntegral(value.toDouble())) out = QString::number(qint64(value.toDouble()));
        else {
            errors << QString("%1: expected a string, got %2 %3").arg(at, describeJson(value), previewJson(value));
            return {};
        }
        if (schema.contains("enum")) {
            QStringList names;
            for (const QJsonValue& e : schema["enum"].toArray()) names << e.toString();
            if (!names.contains(out)) {
                const QString folded = out.trimmed().toLower();
                for (const QString& n : names)
                    if (n.toLower() == folded) return n;
                errors << QString("%1: must be one of %2 (got %3)").arg(at, names.join(", "), previewJson(out));
                return {};
            }
        }
        return out;
    }
    if (kind == "integer") {
        if (value.isDouble() && isIntegral(value.toDouble())) return qint64(value.toDouble());
        if (value.isString()) {
            static const QRegularExpression intRx(R"(^[+-]?\d+$)");
            const QString t = value.toString().trimmed();
            if (intRx.match(t).hasMatch()) return t.toLongLong();
        }
        errors << QString("%1: expected an integer, got %2 %3").arg(at, describeJson(value), previewJson(value));
        return {};
    }
    if (kind == "boolean") {
        if (value.isBool()) return value;
        if (value.isString()) {
            const QString t = value.toString().trimmed().toLower();
            if (t == "true") return true;
            if (t == "false") return false;
        }
        if (value.isDouble() && (value.toDouble() == 0 || value.toDouble() == 1)) return value.toDouble() == 1;
        errors << QString("%1: expected true or false, got %2 %3").arg(at, describeJson(value), previewJson(value));
        return {};
    }
    if (kind == "array") {
        QJsonValue v = value;
        if (v.isString() && v.toString().trimmed().startsWith('[')) {
            QJsonDocument d = QJsonDocument::fromJson(v.toString().toUtf8());
            if (d.isArray()) v = d.array();
        }
        QJsonArray items = v.isArray() ? v.toArray() : QJsonArray{v};
        if (!schema.contains("items")) return items;
        const QJsonObject itemSchema = schema["items"].toObject();
        QJsonArray out;
        for (int i = 0; i < items.size(); ++i)
            out.append(coerceArg(itemSchema, items[i], QString("%1[%2]").arg(at).arg(i), errors, top));
        return out;
    }
    if (kind == "object") {
        QJsonValue v = value;
        if (v.isString() && v.toString().trimmed().startsWith('{')) {
            QJsonDocument d = QJsonDocument::fromJson(v.toString().toUtf8());
            if (d.isObject()) v = d.object();
        }
        if (!v.isObject()) {
            errors << QString("%1: expected an object, got %2 %3").arg(at, describeJson(value), previewJson(value));
            return {};
        }
        return coerceObject(schema, v.toObject(), at, errors, top);
    }
    return value;
}

QString normalizeArguments(const QString& name, const QJsonObject& args,
                           const QJsonObject* parameters, QJsonObject& out) {
    out = args;
    if (!parameters) return {};
    if (args.size() == 1 && args.contains("_raw_arguments")) {
        QString raw = args["_raw_arguments"].toString();
        if (raw.size() > 200) raw = raw.left(200) + "...";
        return QString("Error: the arguments for %1 were not valid JSON, so nothing was run. "
                       "Resend the call with a single JSON object. Received: %2").arg(name, raw);
    }
    QSet<QString> top;
    for (const QString& k : (*parameters)["properties"].toObject().keys()) top.insert(k);
    QStringList errors;
    out = coerceObject(*parameters, args, QString(), errors, top);
    if (errors.isEmpty()) return {};
    out = QJsonObject();
    return QString("Error: invalid arguments for %1; nothing was run.\n- %2").arg(name, errors.join("\n- "));
}

QString validateQuestionArguments(const QJsonObject& args, QJsonObject& out) {
    static const QJsonObject schema = [] {
        for (const QJsonValue& t : toolDefinitions()) {
            const QJsonObject fn = t.toObject()["function"].toObject();
            if (fn["name"].toString() == "ask_user_question") return fn["parameters"].toObject();
        }
        return QJsonObject();
    }();
    const QString err = normalizeArguments("ask_user_question", args, &schema, out);
    if (!err.isEmpty()) return err;
    if (out["questions"].toArray().isEmpty())
        return "Error: invalid arguments for ask_user_question; nothing was run.\n"
               "- questions: must contain at least one question";
    return {};
}

QJsonObject parseRawArguments(const QString& raw) {
    if (raw.trimmed().isEmpty()) return {};
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        // One level of double encoding: a JSON string holding a JSON object.
        QJsonDocument wrapped = QJsonDocument::fromJson(("[" + raw + "]").toUtf8());
        if (wrapped.isArray() && wrapped.array().size() == 1 && wrapped.array()[0].isString())
            doc = QJsonDocument::fromJson(wrapped.array()[0].toString().toUtf8(), &err);
    }
    if (err.error == QJsonParseError::NoError && doc.isObject()) return doc.object();
    return QJsonObject{{"_raw_arguments", raw}};
}

// ── Staged (atomic) writes ───────────────────────────────────────────
//
// Every edit tool writes through StagedWrite so a failed write (disk full,
// quota, signal) can never truncate or delete the file being edited: the
// target is only ever replaced by an atomic rename of a complete, synced temp
// file beside it.  The rename keeps the target's permission bits (and owner,
// where allowed) and follows symlinks so the link itself survives and its
// target is edited.  If the directory doesn't allow creating a temp file but
// the target is writable, it falls back to a checked in-place write.  Keep
// identical to _StagedWrite (Python) and StagedWrite (Rust).

static std::filesystem::path toFsPath(const QString& p) {
#ifdef Q_OS_WIN
    return std::filesystem::path(p.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(p).toStdString());
#endif
}

static QString resolveWriteTarget(const QString& path) {
    QString p = path;
    for (int i = 0; i < 40 && QFileInfo(p).isSymLink(); ++i) {
        const QString next = QFileInfo(p).symLinkTarget();
        if (next.isEmpty()) break;
        p = next;
    }
    return p;
}

static QAtomicInteger<quint64> g_stageCounter = 0;

class StagedWrite {
public:
    StagedWrite(const QString& path, const QByteArray& data)
        : m_path(path), m_target(resolveWriteTarget(path)), m_data(data) {}
    ~StagedWrite() { discard(); }
    StagedWrite(const StagedWrite&) = delete;
    StagedWrite& operator=(const StagedWrite&) = delete;
    StagedWrite(StagedWrite&& o) noexcept
        : m_path(o.m_path), m_target(o.m_target), m_temp(o.m_temp), m_data(o.m_data), m_inPlace(o.m_inPlace) {
        o.m_temp.clear();
    }

    /// Write the temp file; returns an error message, or empty on success.
    QString stage() {
        QFileInfo info(m_target);
        if (info.isDir()) return m_path + " is a directory";
        const QString dir = info.absolutePath();
        if (!QDir().mkpath(dir)) return "cannot create directory " + dir;
        const bool existed = info.exists();
        // A rename only needs a writable directory; still refuse a target the
        // user made read-only, as a plain open() for writing would.
        if (existed && !info.isWritable()) return "Permission denied: " + m_path;
        m_temp = dir + "/." + info.fileName() + QString(".pengy-tmp-%1-%2")
                     .arg(QCoreApplication::applicationPid()).arg(g_stageCounter.fetchAndAddRelaxed(1));
        QFile f(m_temp);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            const QString why = f.errorString();
            m_temp.clear();
            if (existed && !QFileInfo(dir).isWritable()) {
                m_inPlace = true;
                return {};
            }
            return why;
        }
        errno = 0;
        bool ok = f.write(m_data) == m_data.size() && f.flush();
#ifdef Q_OS_UNIX
        ok = ok && ::fsync(f.handle()) == 0;
#endif
        // Qt reports a failed buffered write as "Unknown error"; errno has the
        // real cause (ENOSPC, EFBIG, EDQUOT...).
        const int savedErrno = errno;
        QString why = f.errorString();
        f.close();
        if (!ok || f.error() != QFileDevice::NoError) {
            if (savedErrno != 0) why = QString::fromLocal8Bit(strerror(savedErrno));
            discard();
            return why.isEmpty() ? QStringLiteral("write failed") : why;
        }
        if (existed) {
#ifdef Q_OS_UNIX
            struct stat st;
            const QByteArray tgt = QFile::encodeName(m_target), tmp = QFile::encodeName(m_temp);
            if (::stat(tgt.constData(), &st) == 0) {
                ::chmod(tmp.constData(), st.st_mode & 07777);
                if (::chown(tmp.constData(), st.st_uid, st.st_gid) != 0) { /* not ours to change */ }
            }
#else
            QFile::setPermissions(m_temp, QFile::permissions(m_target));
#endif
        }
        return {};
    }

    /// Replace the target; returns an error message, or empty on success.
    QString commit() {
        if (m_inPlace) {
            QFile f(m_target);
            if (!f.open(QIODevice::ReadWrite)) return f.errorString();
            bool ok = f.write(m_data) == m_data.size() && f.resize(m_data.size()) && f.flush();
#ifdef Q_OS_UNIX
            ok = ok && ::fsync(f.handle()) == 0;
#endif
            return ok ? QString() : f.errorString();
        }
        if (m_temp.isEmpty()) return {};
        std::error_code ec;
        std::filesystem::rename(toFsPath(m_temp), toFsPath(m_target), ec);
        if (ec) return QString::fromStdString(ec.message());
        m_temp.clear();
        return {};
    }

    void discard() {
        if (!m_temp.isEmpty()) {
            QFile::remove(m_temp);
            m_temp.clear();
        }
    }

private:
    QString m_path, m_target, m_temp;
    QByteArray m_data;
    bool m_inPlace = false;
};

/// Stage and commit one text write; returns an error message or empty.
static QString writeAtomic(const QString& path, const QString& text) {
    StagedWrite w(path, text.toUtf8());
    QString err = w.stage();
    if (err.isEmpty()) err = w.commit();
    return err;
}

/// Read a file as exact UTF-8 (no newline translation); false if not UTF-8.
static bool readTextExact(const QString& path, QString& out, QString& error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { error = f.errorString(); return false; }
    const QByteArray raw = f.readAll();
    out = QString::fromUtf8(raw);
    if (out.toUtf8() != raw) { error = "binary"; return false; }
    return true;
}

/// Adapt LF-typed old/new to a CRLF file (see _match_newlines in Python):
/// when old has no match as typed, has no CR of its own, and the file uses
/// CRLF, retry with CRLF line endings and write new the same way.
static void matchNewlines(const QString& content, QString& oldText, QString& newText) {
    if (content.contains(oldText) || !oldText.contains('\n') || oldText.contains('\r')
        || !content.contains("\r\n"))
        return;
    QString crlfOld = oldText;
    crlfOld.replace("\n", "\r\n");
    if (!content.contains(crlfOld)) return;
    oldText = crlfOld;
    if (!newText.contains('\r')) newText.replace("\n", "\r\n");
}

// ── Unified diff (same algorithm as unified_diff in tools.rs) ────────

struct DiffOp { char tag; int i1, i2, j1, j2; };

static QList<DiffOp> diffOpcodes(const QStringList& a, const QStringList& b) {
    int pre = 0;
    while (pre < a.size() && pre < b.size() && a[pre] == b[pre]) ++pre;
    int suf = 0;
    while (suf < a.size() - pre && suf < b.size() - pre && a[a.size() - 1 - suf] == b[b.size() - 1 - suf]) ++suf;
    const int n = a.size() - pre - suf, m = b.size() - pre - suf;
    QList<QPair<int, int>> pairs;
    if (n > 0 && m > 0 && qint64(n) * m <= 4000000) {
        std::vector<quint32> dp(size_t(n + 1) * (m + 1), 0);
        auto at = [&](int i, int j) -> quint32& { return dp[size_t(i) * (m + 1) + j]; };
        for (int i = n - 1; i >= 0; --i)
            for (int j = m - 1; j >= 0; --j)
                at(i, j) = a[pre + i] == b[pre + j] ? at(i + 1, j + 1) + 1 : qMax(at(i + 1, j), at(i, j + 1));
        int i = 0, j = 0;
        while (i < n && j < m) {
            if (a[pre + i] == b[pre + j]) { pairs.append({i, j}); ++i; ++j; }
            else if (at(i + 1, j) >= at(i, j + 1)) ++i;
            else ++j;
        }
    }
    QList<DiffOp> ops;
    if (pre > 0) ops.append({'=', 0, pre, 0, pre});
    int ai = 0, bj = 0, k = 0;
    while (k <= pairs.size()) {
        const int ni = k < pairs.size() ? pairs[k].first : n;
        const int nj = k < pairs.size() ? pairs[k].second : m;
        if (ai < ni || bj < nj) {
            const char tag = (ai < ni && bj < nj) ? 'r' : (ai < ni ? 'd' : 'i');
            ops.append({tag, pre + ai, pre + ni, pre + bj, pre + nj});
        }
        if (k < pairs.size()) {
            int run = 1;
            while (k + run < pairs.size() && pairs[k + run] == QPair<int, int>(ni + run, nj + run)) ++run;
            ops.append({'=', pre + ni, pre + ni + run, pre + nj, pre + nj + run});
            ai = ni + run; bj = nj + run; k += run;
        } else {
            ++k;
        }
    }
    if (suf > 0) ops.append({'=', int(a.size()) - suf, int(a.size()), int(b.size()) - suf, int(b.size())});
    QList<DiffOp> merged;
    for (const DiffOp& op : ops) {
        if (!merged.isEmpty() && merged.last().tag == '=' && op.tag == '=' && merged.last().i2 == op.i1) {
            merged.last().i2 = op.i2;
            merged.last().j2 = op.j2;
            continue;
        }
        merged.append(op);
    }
    return merged;
}

static QString unifiedRange(int start, int stop) {
    const int len = stop - start;
    if (len == 1) return QString::number(start + 1);
    if (len == 0) return QString("%1,0").arg(start);
    return QString("%1,%2").arg(start + 1).arg(len);
}

static QStringList splitKeepEnds(const QString& text) {
    QStringList out;
    int start = 0;
    for (int i = 0; i < text.size(); ++i)
        if (text[i] == '\n') { out << text.mid(start, i - start + 1); start = i + 1; }
    if (start < text.size()) out << text.mid(start);
    return out;
}

/// A unified diff (3 lines of context) of oldText -> newText, in difflib's format.
static QString unifiedDiff(const QString& path, const QString& oldText, const QString& newText) {
    const int ctx = 3;
    const QStringList a = splitKeepEnds(oldText), b = splitKeepEnds(newText);
    QList<DiffOp> codes = diffOpcodes(a, b);
    if (codes.isEmpty()) return {};
    if (codes.first().tag == '=') {
        DiffOp& c = codes.first();
        c.i1 = qMax(c.i1, c.i2 - ctx);
        c.j1 = qMax(c.j1, c.j2 - ctx);
    }
    if (codes.last().tag == '=') {
        DiffOp& c = codes.last();
        c.i2 = qMin(c.i2, c.i1 + ctx);
        c.j2 = qMin(c.j2, c.j1 + ctx);
    }
    QList<QList<DiffOp>> groups;
    QList<DiffOp> group;
    for (DiffOp c : codes) {
        if (c.tag == '=' && c.i2 - c.i1 > 2 * ctx) {
            group.append({'=', c.i1, qMin(c.i2, c.i1 + ctx), c.j1, qMin(c.j2, c.j1 + ctx)});
            groups.append(group);
            group.clear();
            c.i1 = qMax(c.i1, c.i2 - ctx);
            c.j1 = qMax(c.j1, c.j2 - ctx);
        }
        group.append(c);
    }
    if (!group.isEmpty() && !(group.size() == 1 && group.first().tag == '=')) groups.append(group);
    if (groups.isEmpty()) return {};
    QString out = QString("--- %1\n+++ %1\n").arg(path);
    auto line = [&out](QChar prefix, const QString& text) {
        out += prefix;
        out += text;
        if (!text.endsWith('\n')) out += '\n';
    };
    for (const QList<DiffOp>& g : groups) {
        out += QString("@@ -%1 +%2 @@\n").arg(unifiedRange(g.first().i1, g.last().i2),
                                               unifiedRange(g.first().j1, g.last().j2));
        for (const DiffOp& op : g) {
            if (op.tag == '=') {
                for (int i = op.i1; i < op.i2; ++i) line(' ', a[i]);
                continue;
            }
            for (int i = op.i1; i < op.i2; ++i) line('-', a[i]);
            for (int j = op.j1; j < op.j2; ++j) line('+', b[j]);
        }
    }
    return out;
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

static const int kBinarySampleChars = 4096;
static const double kBinaryNonprintableRatio = 0.25;

static bool isNonprintable(QChar c) {
    return c != '\n' && c != '\r' && c != '\t' && (c.category() == QChar::Other_Control);
}

/// Heuristically detect binary blobs that decoded as text without erroring.
///
/// readAndRemove() already decodes leniently (QString::fromUtf8 substitutes
/// U+FFFD for invalid sequences rather than failing), so a hard-invalid byte
/// sequence never crashes or vanishes here. What this catches is the case
/// that never raised in the first place: bytes that happen to form valid
/// text (a UTF-16 file decoded as UTF-8 leaves a NUL between every ASCII
/// byte; a core dump or compiled binary can have long printable-looking
/// runs) but aren't meaningful for the model to read and can blow out the
/// context window.
///
/// Returns the non-printable ratio via *outRatio* and whether the sample
/// looks binary, computed over a leading sample.
static bool looksBinary(const QString& text, double* outRatio) {
    if (text.isEmpty()) {
        if (outRatio) *outRatio = 0.0;
        return false;
    }
    QString sample = text.left(kBinarySampleChars);
    int nonprintable = 0;
    bool hasNull = false;
    for (const QChar& c : sample) {
        if (c == QChar(u'\0')) hasNull = true;
        if (isNonprintable(c)) ++nonprintable;
    }
    double ratio = static_cast<double>(nonprintable) / sample.size();
    if (outRatio) *outRatio = ratio;
    return hasNull || ratio > kBinaryNonprintableRatio;
}

/// Tail-biased truncation for *command* output (run_bash, run_python).
///
/// Keeps the head (~20%) and tail (~80%) and snips the middle: a command echo
/// sits at the start and the error that matters usually sits at the end, so the
/// middle of a build log is the disposable part.  File reads use
/// truncateHeadLines instead — a gap in the middle of a source file is not
/// disposable, and unlike a log it can be paged around.
///
/// Runs the binary guard first: output that looks like a binary blob rather
/// than text is blocked outright rather than truncated, since truncating a
/// binary dump still floods the context with useless bytes.
static QString snipMiddle(const QString& text) {
    double ratio = 0.0;
    if (looksBinary(text, &ratio)) {
        return QString(
            "[Binary output blocked: %1 chars, ~%2% non-printable/control characters. "
            "Refusing to load this into context. If you need the data, redirect it to a "
            "file and use read_file/search_content on it, or use download_file for URLs.]")
            .arg(text.size())
            .arg(qRound(ratio * 100));
    }

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

    // QString::fromUtf8 is lenient (U+FFFD for invalid sequences), so a
    // hard-invalid byte sequence never fails here. This catches the other half
    // of the binary problem — valid UTF-8 bytes that aren't text (a UTF-16
    // file leaves a NUL between every ASCII byte).
    if (looksBinary(text, nullptr))
        return "Error: File appears to be binary (not text): " + path;

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
    const QString err = writeAtomic(path, content);
    if (!err.isEmpty()) return "Error writing file: " + err;
    return "Successfully wrote to " + aStr(args, "path");
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

    QString content, readError;
    if (!readTextExact(path, content, readError)) {
        if (readError == "binary") return "Error: " + path + " is a binary file (not UTF-8).";
        return "Error reading file: " + readError;
    }
    matchNewlines(content, oldStr, newStr);

    int count = content.count(oldStr);
    if (count == 0) {
        return "Error: old_str not found in " + path +
               ".\n\nTip: read the file first to get the exact text.";
    }
    if (aBool(args, "replace_all", false)) {
        content.replace(oldStr, newStr);
        const QString writeError = writeAtomic(path, content);
        if (!writeError.isEmpty()) return "Error writing file: " + writeError;
        return QString("✅ Replaced %1 occurrence(s) in %2.").arg(count).arg(aStr(args, "path"));
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
                       "Matches found on lines: [%3]\n\nMake old_str longer or more specific, or set replace_all=true to replace every occurrence.")
               .arg(count).arg(path).arg(lineStrs.join(", "));
    }

    int oldLine  = content.left(content.indexOf(oldStr)).count('\n') + 1;
    int oldLines = oldStr.count('\n') + 1;
    int newLines = newStr.count('\n') + 1;

    content.replace(content.indexOf(oldStr), oldStr.size(), newStr);

    const QString writeError = writeAtomic(path, content);
    if (!writeError.isEmpty()) return "Error writing file: " + writeError;

    return QString("✅ Successfully replaced in %1:\n   Lines %2–%3 → "
                   "%4 line(s) replaced with %5 line(s)")
           .arg(path).arg(oldLine).arg(oldLine + oldLines - 1)
           .arg(oldLines).arg(newLines);
}

// ── sudo askpass helper ──────────────────────────────────────────────

// Temporary SUDO_ASKPASS helper script. The password is written to a private
// 0600 file that only the askpass script reads — it is deliberately *not*
// placed in the child process's environment, where a `printenv`/`env` or any
// grandchild (a build script, a package post-install) could observe it and leak
// it back into tool output. Directory/script/password file are all single-use
// and removed when the command exits.
//
// Askpass replaces the older "pipe the password to the shell's stdin and
// rewrite the first sudo to `sudo -S`" approach, which broke whenever anything
// else in the command touched stdin: a pipeline (`echo x | sudo tee f`), a
// redirect (`sudo cmd < /dev/null`), an earlier command that reads stdin, or a
// second sudo after the single piped password had been consumed.
class AskpassHelper {
public:
    AskpassHelper(const QString& password) {
        qint64 nanos = QDateTime::currentMSecsSinceEpoch();
        qint64 pid   = QCoreApplication::applicationPid();
        m_dir = QDir::tempPath() + QString("/pengy-askpass-%1-%2").arg(pid).arg(nanos);
        if (!QDir().mkpath(m_dir)) return;
        m_pwPath = m_dir + "/pw";
        QFile pwf(m_pwPath);
        if (!pwf.open(QIODevice::WriteOnly)) return;
        pwf.write(password.toUtf8());
        pwf.close();
        m_path = m_dir + "/askpass.sh";
        QFile f(m_path);
        if (!f.open(QIODevice::WriteOnly)) return;
        f.write(QString("#!/bin/sh\ncat '%1'\n").arg(m_pwPath).toUtf8());
        f.close();
        QFile::setPermissions(m_dir,
            QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QFile::setPermissions(m_path,
            QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QFile::setPermissions(m_pwPath,
            QFile::ReadOwner | QFile::WriteOwner);
        m_valid = true;
    }

    ~AskpassHelper() {
        if (!m_path.isEmpty())  QFile::remove(m_path);
        if (!m_pwPath.isEmpty()) QFile::remove(m_pwPath);
        if (!m_dir.isEmpty())   QDir().rmdir(m_dir);
    }

    AskpassHelper(const AskpassHelper&) = delete;
    AskpassHelper& operator=(const AskpassHelper&) = delete;

    bool valid() const { return m_valid; }
    QString path() const { return m_path; }

private:
    QString m_dir;
    QString m_path;
    QString m_pwPath;
    bool    m_valid = false;
};

// Find unquoted sudo command words. A textual mention, quote, or comment must
// never request a password or alter the source command.
static QList<QPair<int, int>> sudoInvocationSpans(const QString& command) {
    QList<QPair<int, int>> spans; int i = 0; bool commandStart = true;
    while (i < command.size()) {
        QChar c = command[i];
        if (c.isSpace()) { if (c == '\n') commandStart = true; ++i; continue; }
        if (c == '#' && (i == 0 || command[i - 1].isSpace() || QString(";|&()\n").contains(command[i - 1]))) {
            i = command.indexOf('\n', i); if (i < 0) break; ++i; commandStart = true; continue;
        }
        if (QString(";|&()").contains(c)) { commandStart = true; ++i; continue; }
        int start = i; bool quoted = false;
        while (i < command.size() && !command[i].isSpace() && !QString(";|&()").contains(command[i])) {
            if (command[i] == '\\') { i += 2; continue; }
            if (command[i] == '\'' || command[i] == '"') { quoted = true; QChar q = command[i++]; while (i < command.size() && command[i] != q) ++i; if (i < command.size()) ++i; continue; }
            ++i;
        }
        QString word = command.mid(start, i - start);
        if (commandStart && !quoted && (word == "sudo" || word == "/usr/bin/sudo" || word == "/bin/sudo")) spans.append({start, i});
        commandStart = false;
    }
    return spans;
}

QString rewriteSudoForAskpass(QString command) {
    const auto spans = sudoInvocationSpans(command); QString out; int last = 0;
    for (const auto& span : spans) {
        out += command.mid(last, span.first - last) + command.mid(span.first, span.second - span.first);
        static QRegularExpression flagRx("^\\s+-([AS])\\b");
        auto m = flagRx.match(command.mid(span.second));
        if (m.hasMatch() && m.captured(1) == "S") { out += " -A"; last = span.second + m.capturedEnd(); }
        else if (m.hasMatch()) { last = span.second; }
        else { out += " -A"; last = span.second; }
    }
    return out + command.mid(last);
}

// ── Remote execution (run_bash host=) ───────────────────────────────

// Remote-execution wrapper for run_bash(host=...).  Sent over ssh's stdin to
// `sh -s`, so the only thing on the ssh command line is `sh -s` (independent of
// the remote login shell) and the password never touches an argv.  The whole
// script is one `{ ... }` compound command so the shell reads all of it before
// running any of it; after that, stdin holds only the still-open channel.
//
//   - The askpass helper mirrors AskpassHelper: a 0600 password file in a 0700
//     mktemp dir, read by a 0700 script.  `pw` is never exported and is unset
//     before the command starts, so `env` inside the command can't leak it.
//     The dir must be executable (sudo execs the helper), hence the candidate
//     list — $XDG_RUNTIME_DIR is tmpfs, /tmp is often noexec.
//   - Stop: the Pengy side holds ssh's stdin open for the whole run.  Killing
//     the local ssh closes the channel; the watcher's `cat` sees EOF and
//     SIGTERMs the command's process group (setsid).  `sudo` relays SIGTERM to
//     its root child.  No pty, so there is no SIGHUP to rely on.
//   - `exec 3<&0`: background jobs get /dev/null as stdin when job control is
//     off, so the watcher must read the channel through a saved fd.
//   - `trap ... PIPE`: after the client disconnects, dash writes a "Terminated"
//     notice to the dead channel; without a handler it dies of SIGPIPE before
//     the EXIT trap removes the password dir.  Handler traps (unlike ignored
//     signals) reset to default in the child, so the command's own SIGPIPE
//     semantics are unchanged.
//
// Keep byte-identical with the Python and Rust editions.
static const char* const kRemoteWrapper = R"PENGYWRAP({
umask 077
use_sudo=__USE_SUDO__
pw=__PASSWORD__
cmd=__COMMAND__
cwd=__CWD__
d=
if [ "$use_sudo" = 1 ]; then
  for base in "${XDG_RUNTIME_DIR:-}" "${HOME:-}/.cache" /tmp; do
    [ -n "$base" ] && [ -d "$base" ] && [ -w "$base" ] || continue
    t=$(mktemp -d "$base/pengy-askpass.XXXXXX" 2>/dev/null) || continue
    printf '#!/bin/sh\ncat "%s/pw"\n' "$t" > "$t/askpass"
    chmod 700 "$t/askpass"
    : > "$t/pw"
    if "$t/askpass" >/dev/null 2>&1; then d=$t; break; fi
    rm -rf "$t"
  done
  if [ -z "$d" ]; then
    unset pw
    echo "pengy: no writable, executable private directory for SUDO_ASKPASS on the remote host" >&2
    exit 125
  fi
  trap 'rm -rf "$d"' EXIT
  printf '%s\n' "$pw" > "$d/pw"
fi
unset pw
trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM; trap 'exit 141' PIPE
if [ -n "$cwd" ]; then cd "$cwd" || exit 126; fi
if command -v bash >/dev/null 2>&1; then run=bash; else run=sh; fi
exec 3<&0
if [ -n "$d" ]; then
  SUDO_ASKPASS="$d/askpass" setsid "$run" -c "$cmd" </dev/null 3<&- &
else
  setsid "$run" -c "$cmd" </dev/null 3<&- &
fi
child=$!
( cat >/dev/null; kill -TERM -"$child" 2>/dev/null ) <&3 >/dev/null 2>&1 &
watcher=$!
exec 3<&-
wait "$child"; rc=$?
kill "$watcher" 2>/dev/null
exit "$rc"
}
)PENGYWRAP";

QString remoteWrapper() { return QString::fromUtf8(kRemoteWrapper); }

QString validateHost(const QString& host) {
    // ssh destinations: hostnames, ~/.ssh/config aliases, user@host, IPv6
    // literals.  No leading '-' (ssh option injection such as
    // -oProxyCommand=...), no whitespace/quotes/shell metacharacters/slashes.
    static const QRegularExpression hostRx("^[A-Za-z0-9._@:%-]+$");
    if (host.isEmpty() || host.startsWith('-') || !hostRx.match(host).hasMatch())
        return QString("Error: invalid host '%1'. Use an ssh destination such as "
                       "`web1`, `user@web1.example.com`, or an ~/.ssh/config alias.").arg(host);
    return {};
}

QString shellQuote(const QString& s) {
    if (s.isEmpty()) return "''";
    static const QString safe = "_@%+=:,./-";
    bool plain = true;
    for (QChar c : s) {
        if (!(c.unicode() < 128 && (c.isLetterOrNumber() || safe.contains(c)))) { plain = false; break; }
    }
    if (plain) return s;
    QString body = s;
    body.replace("'", "'\"'\"'");
    return "'" + body + "'";
}

QString buildRemoteScript(const QString& command, bool useSudo,
                          const QString& password, const QString& cwd) {
    // One pass over the template: substituting placeholders one after another
    // would also rewrite placeholder text inside an already-inserted value (a
    // password containing `__COMMAND__`), breaking out of its quoting.
    static const QRegularExpression placeholderRx("__(USE_SUDO|PASSWORD|COMMAND|CWD)__");
    const QString tmpl = remoteWrapper();
    QString out;
    int last = 0;
    auto it = placeholderRx.globalMatch(tmpl);
    while (it.hasNext()) {
        auto m = it.next();
        out += tmpl.mid(last, m.capturedStart() - last);
        const QString name = m.captured(1);
        if (name == "USE_SUDO")      out += useSudo ? "1" : "0";
        else if (name == "PASSWORD") out += shellQuote(useSudo ? password : QString());
        else if (name == "COMMAND")  out += shellQuote(command);
        else                         out += shellQuote(cwd);
        last = m.capturedEnd();
    }
    return out + tmpl.mid(last);
}

// Failed-authentication messages from classic sudo and sudo-rs.  On a match the
// cached password for that host is discarded so the next elevated call prompts
// again instead of replaying a bad password until the account locks.
static QString sudoAuthFailureNote(ToolContext* ctx, const QString& host, const QString& err) {
    static const QRegularExpression authFailRx(
        "Sorry, try again\\.|incorrect password attempt|"
        "Authentication failed, try again\\.|incorrect authentication attempt",
        QRegularExpression::CaseInsensitiveOption);
    if (!authFailRx.match(err).hasMatch()) return {};
    ctx->forgetSudoPassword(host);
    QString where = host.isEmpty() ? QString() : " on " + host;
    return QString("\n[sudo authentication failed%1; the cached password was discarded]").arg(where);
}

// ── Bash (with temp file output & process groups) ────────────────────

// Wait for a command started in its own process group.  Returns an empty
// string when it finished; otherwise the tool result for a timeout or cancel
// (the process group has been killed and the output files consumed).
static QString waitForCommand(QProcess& proc, qint64 pid, int timeoutSecs,
                              std::atomic<bool>* cancel, ToolContext* ctx,
                              const TempOutputFiles& tmpFiles) {
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
    return {};
}

// Strip sudo password prompt lines from stderr only.
static QString stripSudoPrompts(QString err) {
    static QRegularExpression sudoPromptRx("^\\[sudo[^\\]]*\\].*\\n?", QRegularExpression::MultilineOption);
    err.remove(sudoPromptRx);
    return err.trimmed();
}

static QString joinCommandOutput(QString out, const QString& err, int exitCode) {
    if (!err.isEmpty()) {
        out += "\n" + err;
    }
    if (exitCode != 0)
        out += QString("\n[Exit code: %1]").arg(exitCode);
    return out;
}

// Run *command* on *host* through the remote wrapper over ssh.
static QString runBashRemote(const QString& command, const QString& cwd,
                             bool useSudo, const QString& password,
                             const QString& host, std::atomic<bool>* cancel,
                             ToolContext* ctx) {
    const QString ssh = QStandardPaths::findExecutable("ssh");
    if (ssh.isEmpty())
        return "Error: run_bash host= requires the `ssh` client, which was not found on PATH.";

    auto tmpFiles = createOutputFiles("bash");
    if (!tmpFiles.valid) {
        return "Error: Could not create temp output files.";
    }

    QProcess proc;
    proc.setProgram(ssh);
    // -T: no pty (separate stdout/stderr, no echo, and the stdin-EOF watcher
    // works).  BatchMode: never block on a login/passphrase/host-key prompt.
    // `--` before the host as a second guard against option injection.
    proc.setArguments({"-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15",
                       "-o", "ServerAliveInterval=15", "--", host, "sh", "-s"});
    // Output goes to files and we wait on ssh itself rather than on output
    // EOF: if some other process inherited the output (a ProxyCommand helper),
    // EOF never comes while the channel stays open — a deadlock on Stop.
    proc.setStandardOutputFile(tmpFiles.stdoutPath);
    proc.setStandardErrorFile(tmpFiles.stderrPath);

#ifdef Q_OS_UNIX
    proc.setChildProcessModifier([]() {
        setsid();
    });
#endif

    // stdin stays a pipe for the whole run: its EOF is what tells the remote
    // watcher to kill the command, and QProcess closes it only once ssh has
    // exited.  The script is buffered here and flushed by the waitFor* calls,
    // so a command larger than the pipe buffer cannot deadlock.
    proc.start(QIODevice::ReadWrite);
    if (!proc.waitForStarted(5000)) {
        removeOutputFiles(tmpFiles);
        return QString("Error running command on %1: %2").arg(host, proc.errorString());
    }

    qint64 pid = proc.processId();
    ctx->registerProcess(pid);
    proc.write(buildRemoteScript(command, useSudo, password, cwd).toUtf8());

    QString early = waitForCommand(proc, pid, toolTimeout(), cancel, ctx, tmpFiles);
    // Close the channel now (not at QProcess destruction) so the remote
    // watcher kills anything still running as soon as ssh is gone.
    proc.closeWriteChannel();
    if (!early.isEmpty()) return early;

    QString out = readAndRemove(tmpFiles.stdoutPath);
    QString err = stripSudoPrompts(readAndRemove(tmpFiles.stderrPath));
    out = joinCommandOutput(out, err, proc.exitCode());

    // stderr shapes that mean ssh itself failed (exit 255 is ambiguous: a
    // remote command can exit 255 too).
    static const QRegularExpression sshFailRx(
        "^ssh: |Permission denied \\(|Host key verification failed",
        QRegularExpression::MultilineOption);
    if (proc.exitCode() == 255 && sshFailRx.match(err).hasMatch())
        out += QString("\n[ssh to %1 failed. run_bash host= requires key-based "
                       "login and an existing known_hosts entry]").arg(host);
    if (useSudo)
        out += sudoAuthFailureNote(ctx, host, err);

    return out.trimmed().isEmpty() ? "(No output)" : snipMiddle(out);
}

static QString toolRunBash(const QJsonObject& args, std::atomic<bool>* cancel,
                           ToolContext* ctx) {
    QString command = aStr(args, "command");
    if (command.isEmpty()) return "Error: command is required.";

    // Remote run: cwd is a path on the remote host; the wrapper cds into it.
    const QString host = aStr(args, "host");
    QString cwd;
    if (!host.isEmpty()) {
        QString hostErr = validateHost(host);
        if (!hostErr.isEmpty()) return hostErr;
        cwd = aStr(args, "cwd");
    } else {
        cwd = expandHome(aStr(args, "cwd"));
        if (!cwd.isEmpty() && !QFileInfo(cwd).isDir())
            return "Error: cwd not found or not a directory: " + cwd;
    }

    int timeoutSecs = toolTimeout();

    // ── sudo detection ──────────────────────────────────────────────
    bool needsSudo = !sudoInvocationSpans(command).isEmpty();
    if (needsSudo && !args.value("elevated").toBool(false))
        return "Elevation required: this command invokes sudo. Retry run_bash with elevated=true to request sudo access.";
    if (!needsSudo && args.value("elevated").toBool(false))
        // Fail loudly instead of silently running unprivileged. A caller that
        // asked for elevation must actually contain a `sudo` invocation, so
        // the escalation is explicit and auditable.
        return "Error: elevated=true was set, but the command does not invoke sudo. "
               "Add an explicit `sudo ...` to the command (so the elevation is an "
               "auditable sudo call), or omit elevated=true if no root is needed.";

    // Passwords are cached per host (empty = local) and never offered to a
    // host they weren't entered for.
    QString password;
    if (needsSudo) {
        if (!ctx->hasCachedSudoPassword(host)) {
            auto provider = ctx->sudoProvider();
            if (!provider) {
                return "Error: sudo requested but no password provider is configured.";
            }
            QString pw = provider(host);
            if (pw.isEmpty()) {
                return "Cancelled: sudo password not provided.";
            }
            ctx->setCachedSudoPassword(pw, host);
        }
        password = ctx->cachedSudoPassword(host);
        // Only parsed command words get -A — never a mention in data, a
        // comment, or a quoted string.
        command = rewriteSudoForAskpass(command);
    }

    if (!host.isEmpty())
        return runBashRemote(command, cwd, needsSudo, password, host, cancel, ctx);

    std::unique_ptr<AskpassHelper> askpass;
    if (needsSudo) {
        askpass = std::make_unique<AskpassHelper>(password);
        if (!askpass->valid()) {
            return "Error: Could not create sudo askpass helper.";
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
    // The command never inherits our stdin: the password goes via askpass, and
    // a child reading the terminal would hang the GUI/CLI.
    proc.setStandardInputFile(QProcess::nullDevice());
    if (!cwd.isEmpty()) proc.setWorkingDirectory(cwd);

    if (askpass) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("SUDO_ASKPASS", askpass->path());
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

    QString early = waitForCommand(proc, pid, timeoutSecs, cancel, ctx, tmpFiles);
    if (!early.isEmpty()) return early;

    QString out = readAndRemove(tmpFiles.stdoutPath);
    QString err = stripSudoPrompts(readAndRemove(tmpFiles.stderrPath));
    out = joinCommandOutput(out, err, proc.exitCode());
    if (needsSudo)
        out += sudoAuthFailureNote(ctx, QString(), err);

    return out.trimmed().isEmpty() ? "(No output)" : snipMiddle(out);
}

// ── run_powershell (Windows) ─────────────────────────────────────────
//
// Fixed prelude.  The model's script is written to a UTF-8 temp file and
// compiled with [ScriptBlock]::Create rather than run via -File (blocked by
// the default Restricted execution policy on Windows client SKUs, and
// -ExecutionPolicy Bypass is a pattern EDR flags) or -EncodedCommand (a
// classic malware IOC that corporate EDR blocks outright).  Reading the file
// with an explicit UTF-8 encoding sidesteps 5.1's ANSI default for BOM-less
// scripts, and nothing model-authored ever crosses the command line, so there
// is no quoting to get wrong.
//   - ProgressPreference: progress records otherwise leak onto redirected
//     output (as CLIXML on 5.1).
//   - PSStyle.OutputRendering (7.2+): pwsh emits ANSI colour even into a pipe.
//   - OutputEncoding: UTF-8 both ways so non-ASCII output survives; the
//     Console setter can throw without a console, hence the try.
//   - A parse error surfaces from Create() and must exit non-zero, otherwise
//     the run reports success.
//   - Exit code: the last native exit code (LASTEXITCODE), or 1 when the
//     script throws; `exit N` inside the script ends the process directly.
// Must stay free of double quotes: it is one argv element, and Windows argv
// quoting of embedded quotes is the fragile part.
// Keep byte-identical with the Python and Rust editions.
static const char kPowershellPrelude[] =
    "$ProgressPreference = 'SilentlyContinue'; "
    "if ($PSStyle) { $PSStyle.OutputRendering = 'PlainText' }; "
    "try { [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false) } catch {}; "
    "$OutputEncoding = [Text.UTF8Encoding]::new($false); "
    "try { $__pengy = [ScriptBlock]::Create([IO.File]::ReadAllText('{path}', [Text.Encoding]::UTF8)) } "
    "catch { $e = $_.Exception; if ($e.InnerException) { $e = $e.InnerException }; "
    "[Console]::Error.WriteLine($e.Message); exit 1 }; "
    "$global:LASTEXITCODE = 0; "
    "& $__pengy; "
    "exit $LASTEXITCODE";

QString powershellPrelude(const QString& scriptPath) {
    // PowerShell single-quoted strings escape ' by doubling it.
    QString escaped = scriptPath;
    escaped.replace("'", "''");
    return QString::fromUtf8(kPowershellPrelude).replace("{path}", escaped);
}

QString runPowershellWith(const QString& exe, const QJsonObject& args,
                          std::atomic<bool>* cancel, ToolContext* ctx) {
    if (!ctx) ctx = &g_defaultContext;
    if (exe.isEmpty())
        return "Error: PowerShell was not found (looked for pwsh and powershell "
               "on PATH and Windows PowerShell under System32).";
    QString command = aStr(args, "command");
    if (command.isEmpty()) return "Error: command is required.";
    QString cwd = expandHome(aStr(args, "cwd"));
    if (!cwd.isEmpty() && !QFileInfo(cwd).isDir())
        return "Error: cwd not found or not a directory: " + cwd;

    QTemporaryFile script;
    script.setFileTemplate(QDir::tempPath() + "/pengy-XXXXXX.ps1");
    if (!script.open()) return "Error: Could not create temp file.";
    script.write(command.toUtf8());
    // Close (the file stays until `script` is destroyed): .NET's ReadAllText
    // opens with FileShare.Read, which fails on Windows while our write
    // handle is still open.
    script.close();

    auto tmpFiles = createOutputFiles("powershell");
    if (!tmpFiles.valid) {
        return "Error: Could not create temp output files.";
    }

    QProcess proc;
    proc.setProgram(exe);
    proc.setArguments({"-NoLogo", "-NoProfile", "-NonInteractive", "-Command",
                       powershellPrelude(QDir::toNativeSeparators(script.fileName()))});
    proc.setStandardOutputFile(tmpFiles.stdoutPath);
    proc.setStandardErrorFile(tmpFiles.stderrPath);
    proc.setStandardInputFile(QProcess::nullDevice());
    if (!cwd.isEmpty()) proc.setWorkingDirectory(cwd);

#ifdef Q_OS_UNIX
    // POSIX pwsh (tests): own group so a timeout's group kill can't reach us.
    proc.setChildProcessModifier([]() {
        setsid();
    });
#endif

    proc.start();
    if (!proc.waitForStarted(5000)) {
        removeOutputFiles(tmpFiles);
        return "Error running PowerShell: " + proc.errorString();
    }

    qint64 pid = proc.processId();
    ctx->registerProcess(pid);

    QString early = waitForCommand(proc, pid, toolTimeout(), cancel, ctx, tmpFiles);
    if (!early.isEmpty()) return early;

    QString out = readAndRemove(tmpFiles.stdoutPath);
    QString err = readAndRemove(tmpFiles.stderrPath);
    out = joinCommandOutput(out, err, proc.exitCode());
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
    QString dir      = aStr(args, "dir");
    if (urlStr.isEmpty()) return "Error: url is required.";

    QUrl url(urlStr);
    if (!url.isValid())   return "Error: Invalid URL: " + urlStr;
    QString scheme = url.scheme();
    if (scheme != "http" && scheme != "https")
        return QString("Error: Only http/https URLs are supported (got '%1').").arg(scheme);

    QString targetDir = dir.isEmpty() ? QDir::homePath() + "/Downloads" : expandHome(dir);
    QDir().mkpath(targetDir);
    if (!QFileInfo(targetDir).isDir())
        return "Error: dir is not a directory: " + targetDir;

    if (filename.isEmpty())
        filename = urlStr.split('?').first().split('/').last();
    QString dest = targetDir + "/" + safeDownloadName(filename);

    int limitMb = args.contains("max_size_mb") ? aInt(args, "max_size_mb", 0) : downloadMaxMb();
    qint64 limitBytes = limitMb <= 0 ? 0 : (qint64)limitMb * 1024 * 1024;

    if (limitBytes > 0) {
        QStorageInfo si(targetDir);
        qint64 avail = si.isValid() ? (qint64)si.bytesAvailable() : -1;
        if (avail >= 0 && avail < limitBytes)
            return QString("Error: not enough disk space — need %1 MB, have %2 MB free in %3")
                .arg(limitMb).arg(avail / (1024.0 * 1024.0), 0, 'f', 0).arg(targetDir);
    }

    QNetworkAccessManager mgr;
    mgr.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    req.setTransferTimeout(120000); // 120s stall, not a total cap

    QNetworkReply* reply = mgr.get(req);

    // Stream into a hidden temp file renamed over dest only once complete, so
    // a failed download never touches an existing file of the same name.
    const QString temp = targetDir + "/." + QFileInfo(dest).fileName()
                         + QString(".pengy-dl-%1").arg(QCoreApplication::applicationPid());
    QFile out(temp);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        reply->deleteLater();
        return "Error writing file: " + out.errorString();
    }
    bool writeFailed = false;

    QEventLoop loop;
    QTimer stallTimer;
    stallTimer.setSingleShot(true);
    qint64 total = 0;
    bool exceeded = false;
    bool stalled = false;

    QObject::connect(reply, &QNetworkReply::readyRead, [&]() {
        QByteArray chunk = reply->readAll();
        total += chunk.size();
        if (limitBytes > 0 && total > limitBytes) {
            exceeded = true;
            loop.quit();
            return;
        }
        if (out.write(chunk) != chunk.size()) {
            writeFailed = true;
            reply->abort();
            loop.quit();
            return;
        }
        stallTimer.start(120000);
    });
    QObject::connect(&stallTimer, &QTimer::timeout, [&]() {
        stalled = true;
        reply->abort();
        loop.quit();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    stallTimer.start(120000);
    loop.exec();

    if (!out.flush()) writeFailed = true;
    const QString writeError = out.errorString();
    out.close();
    QNetworkReply::NetworkError nerr = reply->error();
    QString nerrStr = reply->errorString();
    reply->deleteLater();

    if (exceeded) {
        QFile::remove(temp);
        return QString("Error: Download exceeds maximum size of %1.").arg(formatSize(limitBytes));
    }
    if (stalled) {
        QFile::remove(temp);
        return "Error downloading: no data for 120 seconds";
    }
    if (writeFailed) {
        QFile::remove(temp);
        return "Error writing file: " + writeError;
    }
    if (nerr != QNetworkReply::NoError) {
        QFile::remove(temp);
        return "Error downloading file: " + nerrStr;
    }
    std::error_code ec;
    std::filesystem::rename(toFsPath(temp), toFsPath(dest), ec);
    if (ec) {
        QFile::remove(temp);
        return "Error writing file: " + QString::fromStdString(ec.message());
    }
    return QString("Downloaded to %1 (%2 bytes)").arg(dest).arg(total);
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

    if (looksBinary(text, nullptr))
        return "Error fetching URL: response appears to be binary data, not text";

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

        // Same binary guard as read_file: valid-UTF-8 bytes that aren't text
        // (e.g. a UTF-16 file's NUL-interleaved ASCII) must not flood context.
        if (looksBinary(content, nullptr)) {
            parts.append(header + "\n  ❌ Binary file (not text).");
            continue;
        }

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

// search_content decides text vs binary by content (a NUL byte in the first
// 8 KB), not by an extension allowlist: an allowlist silently hid whole
// languages (.cs, .vue, .tf, .ps1), logs and extensionless scripts, and the
// model got "No matches" with no hint that files were never searched.  The
// extensions below are only a fast path to skip known binaries unread.
static const QSet<QString> SEARCH_BINARY_EXTS{
    "png","jpg","jpeg","gif","bmp","ico","webp","tif","tiff","pdf","zip","gz","tgz",
    "bz2","xz","zst","7z","rar","tar","jar","war","class","pyc","pyo","o","obj","a",
    "lib","so","dylib","dll","exe","bin","wasm","whl","deb","rpm","woff","woff2","ttf",
    "otf","eot","mp3","mp4","wav","flac","ogg","mkv","mov","avi","webm","sqlite","db",
    "iso","dmg"
};
static constexpr qint64 SEARCH_MAX_FILE_BYTES = 10LL * 1024 * 1024;

/// "binary"/"large" if *fi* shouldn't be searched, else an empty string.
static QString searchSkipReason(const QFileInfo& fi) {
    if (SEARCH_BINARY_EXTS.contains(fi.suffix().toLower())) return "binary";
    if (fi.size() > SEARCH_MAX_FILE_BYTES) return "large";
    QFile f(fi.filePath());
    if (!f.open(QIODevice::ReadOnly)) return "binary";
    if (f.read(8192).contains('\0')) return "binary";
    return {};
}

static bool matchesGlob(const QString& name, const QString& glob);

/// A file_glob without '/' matches the file name ("*.py"); one with '/'
/// matches the path relative to the search root ("src/*.py", "**/test_*.py"),
/// with a leading "**/" also matching at the root.  Mirrors _matches_glob.
static bool matchesFileGlob(const QString& name, const QString& rel, const QString& glob) {
    static QRegularExpression braceRx(R"(^(.*)\{([^}]+)\}(.*)$)");
    QStringList patterns;
    auto m = braceRx.match(glob);
    if (m.hasMatch()) {
        for (const QString& c : m.captured(2).split(','))
            patterns << m.captured(1) + c.trimmed() + m.captured(3);
    } else {
        patterns << glob;
    }
    for (const QString& p : patterns) {
        if (!p.contains('/')) {
            if (matchesGlob(name, p)) return true;
            continue;
        }
        if (matchesGlob(rel, p)) return true;
        if (p.startsWith("**/") && matchesGlob(rel, p.mid(3))) return true;
    }
    return false;
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
    const auto caseOpt   = aBool(args, "ignore_case", false)
                               ? QRegularExpression::CaseInsensitiveOption
                               : QRegularExpression::NoPatternOption;

    if (pattern.isEmpty()) return "Error: pattern is required.";

    QFileInfo pathInfo(path);
    if (!pathInfo.exists()) return "Error: Path not found: " + path;

    // Literal by default so metacharacters in code symbols (".", "(", "[", "*",
    // ...) don't silently become regex syntax; regex=true opts into regex.
    QRegularExpression rx;
    if (regex) {
        rx = QRegularExpression(pattern, caseOpt);
        if (!rx.isValid())
            return "Error: Invalid regex pattern.";
    } else {
        rx = QRegularExpression(QRegularExpression::escape(pattern), caseOpt);
    }

    QStringList results;
    int filesSearched = 0, skippedBinary = 0, skippedLarge = 0;
    bool truncated = false;

    if (pathInfo.isFile()) {
        searchOneFile(path, rx, contextLines, path, results, maxResults);
        if (results.isEmpty()) return QString("No matches found for '%1' in %2").arg(pattern, path);
        return results.join("\n\n");
    }

    // Hidden directories are searched (.github, .config and .vscode hold real
    // content); only the known-noise list is pruned, before descending, so a
    // big .git or node_modules is never walked.  Files are visited in sorted
    // order so results are deterministic.  Mirrors _should_skip_search_dir.
    static const QSet<QString> SEARCH_EXTRA_SKIP{".venv", ".cache"};
    QStringList files;
    std::function<void(const QString&)> walk = [&](const QString& dir) {
        const QFileInfoList entries = QDir(dir).entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
        for (const QFileInfo& e : entries) {
            if (e.isDir()) {
                const QString n = e.fileName();
                if (!e.isSymLink() && !ALWAYS_SKIP.contains(n) && !SEARCH_EXTRA_SKIP.contains(n)
                    && !n.endsWith(".egg-info"))
                    walk(e.absoluteFilePath());
            } else {
                files << e.filePath();
            }
        }
    };
    walk(path);
    files.sort();
    for (const QString& fp : files) {
        if (truncated) break;
        QFileInfo fi(fp);
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

        if (!fileGlob.isEmpty() && !matchesFileGlob(name, rel, fileGlob)) continue;

        const QString reason = searchSkipReason(fi);
        if (reason == "large") { ++skippedLarge; continue; }
        if (!reason.isEmpty()) { ++skippedBinary; continue; }
        ++filesSearched;

        if (searchOneFile(fp, rx, contextLines,
                          QDir(path).relativeFilePath(fp),
                          results, maxResults)) {
            truncated = true;
        }
    }

    if (results.isEmpty()) {
        QString summary = QString("No matches found for '%1' in %2 (searched %3 files")
                              .arg(pattern, aStr(args, "path")).arg(filesSearched);
        if (skippedBinary > 0) summary += QString("; skipped %1 binary").arg(skippedBinary);
        if (skippedLarge > 0) summary += QString("; skipped %1 over 10 MB").arg(skippedLarge);
        summary += ')';
        if (!fileGlob.isEmpty() && filesSearched == 0 && skippedBinary == 0 && skippedLarge == 0)
            summary += QString(". No file matched file_glob '%1'.").arg(fileGlob);
        return summary;
    }

    QString summary = QString("Found %1 match(es) for '%2' across %3 file(s)")
                      .arg(results.size()).arg(pattern).arg(filesSearched);
    if (truncated) summary += " (results truncated)";
    return snipMiddle(summary + "\n" + QString(60, QChar(0x2500)) + "\n" + results.join("\n\n"));
}

static QString toolApplyChanges(const QJsonObject& args) {
    constexpr int maxFiles = 20, maxOps = 100, maxBlock = 256000, maxResult = 1000000;
    const QJsonArray changes = args["changes"].toArray();
    if (changes.isEmpty()) return "Error: changes must be a non-empty list.";
    if (changes.size() > maxFiles)
        return QString("Error: too many files (%1). Maximum is %2.").arg(changes.size()).arg(maxFiles);

    // Paths are resolved (symlinks followed) before the duplicate check, so
    // "t.txt", "./t.txt" and a link to it are one file — two entries for the
    // same file used to race two temp files and could delete the target.
    struct Prepared { QString path, oldText, newText; };
    QList<Prepared> prepared;
    QStringList errors;
    QSet<QString> seen;
    int ops = 0, bytes = 0;
    for (int fi = 0; fi < changes.size(); ++fi) {
        const QJsonObject file = changes[fi].toObject();
        const QString raw = file["path"].toString();
        if (raw.isEmpty()) { errors << QString("file %1: path is required").arg(fi); continue; }
        QFileInfo info(QDir::current().absoluteFilePath(expandHome(raw)));
        if (!info.exists()) { errors << raw + ": file not found"; continue; }
        if (!info.isFile()) { errors << raw + ": not a file"; continue; }
        const QString path = info.canonicalFilePath();
        if (seen.contains(path)) { errors << raw + ": duplicate path"; continue; }
        seen.insert(path);
        QString old, readError;
        if (!readTextExact(path, old, readError)) { errors << raw + ": binary or non-UTF-8 file"; continue; }
        const QJsonArray operations = file["operations"].toArray();
        if (operations.isEmpty()) { errors << raw + ": operations must be non-empty"; continue; }
        ops += operations.size();
        QString cur = old;
        for (int oi = 0; oi < operations.size(); ++oi) {
            const QJsonObject op = operations[oi].toObject();
            const QString kind = op["kind"].toString();
            const int expected = op["expected_matches"].toInt(1);
            if (expected < 1) { errors << QString("%1 operation %2: expected_matches must be positive").arg(raw).arg(oi); continue; }
            QString needle, text;
            if (kind == "replace" || kind == "delete") {
                needle = op["old"].toString();
                text = kind == "delete" ? QString() : op["new"].toString();
            } else if (kind == "insert_after") {
                needle = op["anchor"].toString();
                text = op["text"].toString();
            } else {
                errors << QString("%1 operation %2: unknown kind %3").arg(raw).arg(oi).arg(kind);
                continue;
            }
            if (needle.isEmpty()) { errors << QString("%1 operation %2: match text must be non-empty").arg(raw).arg(oi); continue; }
            if (needle.toUtf8().size() > maxBlock || text.toUtf8().size() > maxBlock) {
                errors << QString("%1 operation %2: text block exceeds %3 bytes").arg(raw).arg(oi).arg(maxBlock);
                continue;
            }
            matchNewlines(cur, needle, text);
            const int count = cur.count(needle);
            if (count != expected) {
                errors << QString("%1 operation %2: matches %3 locations; expected %4").arg(raw).arg(oi).arg(count).arg(expected);
                continue;
            }
            const QString repl = kind == "insert_after" ? needle + text : text;
            int pos = 0;
            for (int n = 0; n < expected; ++n) {
                pos = cur.indexOf(needle, pos);
                cur.replace(pos, needle.size(), repl);
                pos += repl.size();
            }
        }
        bytes += old.toUtf8().size() + cur.toUtf8().size();
        prepared.append({path, old, cur});
    }
    if (ops > maxOps) errors << QString("too many operations; maximum is %1").arg(maxOps);
    if (bytes > maxResult) errors << QString("result exceeds %1 bytes").arg(maxResult);

    // Postconditions inspect proposed contents, not the files on disk.
    const QJsonArray conditions = args["postconditions"].toArray();
    for (int i = 0; i < conditions.size(); ++i) {
        const QJsonObject c = conditions[i].toObject();
        const QString raw = c["path"].toString();
        const QString path = QFileInfo(QDir::current().absoluteFilePath(expandHome(raw))).canonicalFilePath();
        QString content;
        bool found = false;
        for (const auto& p : prepared)
            if (p.path == path) { content = p.newText; found = true; }
        if (!found) {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly)) content = QString::fromUtf8(f.readAll());
        }
        if (c.contains("contains") && !content.contains(c["contains"].toString()))
            errors << QString("postcondition %1: %2 does not contain expected text").arg(i).arg(raw);
        if (c.contains("does_not_contain") && content.contains(c["does_not_contain"].toString()))
            errors << QString("postcondition %1: %2 still contains forbidden text").arg(i).arg(raw);
    }
    if (!errors.isEmpty()) return "Error: no changes applied.\n- " + errors.join("\n- ");

    QString diff;
    for (const auto& p : prepared)
        if (p.oldText != p.newText) diff += unifiedDiff(p.path, p.oldText, p.newText);
    if (args["dry_run"].toBool(false))
        return QString("Dry run: no changes applied.\nFiles: %1\n\n%2").arg(prepared.size()).arg(diff).trimmed();

    // Stage every file before replacing any, so a write failure (disk full,
    // permissions) leaves all targets untouched; only the renames remain after.
    std::vector<StagedWrite> staged;
    for (const auto& p : prepared)
        if (p.oldText != p.newText) staged.emplace_back(p.path, p.newText.toUtf8());
    for (auto& w : staged) {
        const QString err = w.stage();
        if (!err.isEmpty()) {
            for (auto& d : staged) d.discard();
            return "Error: write failed; no changes applied: " + err;
        }
    }
    for (auto& w : staged) {
        const QString err = w.commit();
        if (!err.isEmpty())
            return "Error: rename failed after validation; changes may be partially applied: " + err;
    }
    return QString("Applied changes to %1 file(s).\n\n%2").arg(prepared.size()).arg(diff).trimmed();
}

static QString toolGlob(const QJsonObject& args);
static QString toolTodowrite(const QJsonObject& args);

// ── Dispatcher ────────────────────────────────────────────────────────

QString execute(const QString& name, const QJsonObject& rawArgs,
                std::atomic<bool>* cancel, ToolContext* ctx) {
    if (!ctx) ctx = &g_defaultContext;
    if (name == "ask_user_question") return "ask_user_question must be handled by the harness — it should never reach execute_tool directly.";
    static const QHash<QString, QJsonObject> parameters = [] {
        QHash<QString, QJsonObject> h;
        for (const QJsonValue& t : toolDefinitions()) {
            const QJsonObject fn = t.toObject()["function"].toObject();
            h.insert(fn["name"].toString(), fn["parameters"].toObject());
        }
        return h;
    }();
    QJsonObject args;
    const auto found = parameters.constFind(name);
    const QString argError = normalizeArguments(
        name, rawArgs, found == parameters.constEnd() ? nullptr : &found.value(), args);
    if (!argError.isEmpty()) return argError;
    if (name == "read_file")          return toolReadFile(args);
    if (name == "read_image")         return toolReadImage(args, ctx);
    if (name == "write_file")         return toolWriteFile(args);
    if (name == "replace_in_file")    return toolReplaceInFile(args);
    if (name == "apply_changes")      return toolApplyChanges(args);
    if (name == "run_powershell")     return runPowershellWith(powershellPath(), args, cancel, ctx);
    if (name == "run_bash") {
        QString e = windowsLocalRunBashError(isWindowsHost(), aStr(args, "host"));
        if (!e.isEmpty()) return e;
        return toolRunBash(args, cancel, ctx);
    }
    if (name == "web_search")         return toolWebSearch(args);
    if (name == "download_file")      return toolDownloadFile(args);
    if (name == "fetch_url")          return toolFetchUrl(args);
    if (name == "run_python")         return toolRunPython(args, ctx);
    if (name == "directory_tree")     return toolDirectoryTree(args);
    if (name == "read_multiple_files") return toolReadMultipleFiles(args);
    if (name == "search_content")     return toolSearchContent(args);
    if (name == "glob")              return toolGlob(args);
    if (name == "todowrite")         return toolTodowrite(args);
    return "Unknown tool: " + name;
}


// ── glob ──────────────────────────────────────────────────────────

/// Translate a glob into an anchored regex over a '/'-separated relative path:
/// "**/" = any number of directories (including none), "**" = anything,
/// "*" = anything within one segment, "?" = one character, "{a,b}" = either.
static QString globToRegex(const QString& glob) {
    QString re = "^";
    bool inBrace = false;
    for (int i = 0; i < glob.size(); ++i) {
        const QChar c = glob[i];
        if (c == '*' && i + 1 < glob.size() && glob[i + 1] == '*') {
            if (i + 2 < glob.size() && glob[i + 2] == '/') { re += "(?:.*/)?"; i += 2; }
            else { re += ".*"; i += 1; }
            continue;
        }
        if (c == '*') re += "[^/]*";
        else if (c == '?') re += "[^/]";
        else if (c == '{') { inBrace = true; re += "(?:"; }
        else if (c == '}' && inBrace) { inBrace = false; re += ")"; }
        else if (c == ',' && inBrace) re += "|";
        else re += QRegularExpression::escape(QString(c));
    }
    return re + "$";
}

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

    // Match the whole relative path with real glob semantics: "*" and "?" stay
    // within one path segment, "**/" spans any number of directories.  (Matching
    // only the last component ignored any prefix, so "src/**/*.py" also
    // returned files outside src/.)  Same rules as glob_to_regex in tools.rs.
    const QString norm = QString(pattern).replace('\\', '/');
    const QRegularExpression matcher(globToRegex(norm));
    if (!matcher.isValid()) return QString("Error with glob pattern '%1'").arg(pattern);
    QString trimmedPattern = norm;
    while (trimmedPattern.endsWith('/')) trimmedPattern.chop(1);
    const QStringList segs = trimmedPattern.split('/');
    const bool wantsHidden = segs.last().startsWith('.');
    bool wantsHiddenDirs = false;
    for (int k = 0; k + 1 < segs.size(); ++k)
        if (segs[k].startsWith('.') && segs[k] != "." && segs[k] != "..") wantsHiddenDirs = true;
    const int maxDepth = pattern.contains("**") ? INT_MAX : segs.size();

    static const QSet<QString> skipDirs = {".git", ".svn", ".hg", "__pycache__", "node_modules",
                               ".mypy_cache", ".pytest_cache", ".ruff_cache", ".tox", ".eggs",
                               ".venv", "venv", ".env", "build", "dist", "target"};

    // (isDir, relative path, display line)
    QList<std::tuple<bool, QString, QString>> found;
    std::function<void(const QString&, const QString&, int)> walk =
        [&](const QString& dir, const QString& prefix, int depth) {
        const QFileInfoList entries = QDir(dir).entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
        for (const QFileInfo& fi : entries) {
            const QString name = fi.fileName();
            const bool isDir = fi.isDir() && !fi.isSymLink();
            // Hidden directories are walked only when the pattern names one
            // (".github/**/*.yml"); hidden files only when the last segment
            // asks for them ("**/.env").
            if (name.startsWith('.') && !(isDir ? wantsHiddenDirs : wantsHidden)) continue;
            // Only directories are pruned by the skip set: ".env" and "target"
            // are in there as *directory* names; a plain ".env" file is fine.
            if (isDir && skipDirs.contains(name)) continue;
            const QString rel = prefix + name;
            if (matcher.match(rel).hasMatch() && !(isDir && name.startsWith('.') && !wantsHidden))
                found.append({isDir, rel, isDir ? rel + "/" : QString("%1  (%2)").arg(rel, formatSize(fi.size()))});
            if (isDir && depth < maxDepth) walk(fi.absoluteFilePath(), rel + "/", depth + 1);
        }
    };
    walk(searchDir.path(), QString(), 1);
    // Directories first, then by path.
    std::sort(found.begin(), found.end(), [](const auto& x, const auto& y) {
        if (std::get<0>(x) != std::get<0>(y)) return std::get<0>(x);
        return std::get<1>(x) < std::get<1>(y);
    });
    QStringList matches;
    for (const auto& f : found) matches << std::get<2>(f);

    if (matches.isEmpty())
        return QString("No files matching '%1' in %2").arg(pattern, searchDir.path());

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
        return QString("Error: %1 tasks marked in_progress. At most one task may be in_progress at a time.").arg(inProgressCount);

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
