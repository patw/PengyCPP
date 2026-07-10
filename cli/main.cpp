#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QUuid>
#include <QTextStream>
#include <QRegularExpression>
#include <atomic>
#include <cstdio>
#include <functional>

#ifdef Q_OS_UNIX
#  include <termios.h>
#  include <unistd.h>
#endif

#include "../config.h"
#include "../chatmanager.h"
#include "../llmclient.h"
#include "../tools.h"
#include "../version.h"

// ── Terminal colors ──────────────────────────────────────────────────

#ifdef Q_OS_UNIX
static bool g_color = false;
#else
static bool g_color = false;
#endif

static inline QString clr(const char* code, const QString& s) {
    return g_color ? (QLatin1String(code) + s + QLatin1String("\033[0m")) : s;
}
static inline QString bold(const QString& s)   { return clr("\033[1m",  s); }
static inline QString dim(const QString& s)    { return clr("\033[2m",  s); }
static inline QString green(const QString& s)  { return clr("\033[32m", s); }
static inline QString cyan(const QString& s)   { return clr("\033[36m", s); }
static inline QString red(const QString& s)    { return clr("\033[31m", s); }

static void out(const QString& s) {
    fputs(s.toUtf8().constData(), stdout);
    fflush(stdout);
}
static void outln(const QString& s = {}) { out(s + '\n'); }
static void prompt(const QString& p) { out(bold(p)); }

// ── Markdown-to-terminal renderer ────────────────────────────────────

/// Render inline markdown: **bold**, *italic*, `code`, [links](url).
static QString renderInline(const QString& text) {
    QString result = text;

    // Inline code: `...` → cyan+dim
    {
        QRegularExpression re("`([^`\\n]+)`");
        result.replace(re, g_color ? "\033[2;36m\\1\033[0m" : "\\1");
    }
    // Bold: **...**  (process before italic)
    {
        QRegularExpression re("\\*\\*([^*\\n]+)\\*\\*");
        result.replace(re, g_color ? "\033[1m\\1\033[0m" : "\\1");
    }
    // Italic: *...*
    {
        QRegularExpression re("\\*([^*\\n]+)\\*");
        result.replace(re, g_color ? "\033[3m\\1\033[0m" : "\\1");
    }
    // Links: [text](url) → text (url)
    {
        QRegularExpression re("\\[([^\\]]+)\\]\\(([^)]+)\\)");
        result.replace(re, g_color ? "\033[36m\\1\033[0m (\\2)" : "\\1 (\\2)");
    }

    return result;
}

/// Render markdown text with ANSI styling for the terminal.
/// Handles: fenced code blocks, inline code, bold, italic, headers,
/// lists, blockquotes, horizontal rules, and links.
static void renderMarkdownToTerminal(const QString& text) {
    const QStringList lines = text.split('\n');
    bool inCodeBlock = false;
    bool inList = false;
    int listNum = 0;

    for (int i = 0; i < lines.size(); i++) {
        const QString& line = lines[i];
        const QString trimmed = line.trimmed();

        // Code block fence
        if (trimmed.startsWith("```")) {
            if (inCodeBlock) {
                inCodeBlock = false;
                outln(dim(""));
            } else {
                if (inList) { outln(""); inList = false; }
                inCodeBlock = true;
            }
            continue;
        }

        if (inCodeBlock) {
            outln(dim(line));
            continue;
        }

        // Empty line
        if (trimmed.isEmpty()) {
            if (inList) { outln(""); inList = false; }
            if (i + 1 < lines.size() && !lines[i + 1].trimmed().isEmpty())
                outln();
            continue;
        }

        // Horizontal rule
        if (trimmed == "---" || trimmed == "***" || trimmed == "___") {
            if (inList) { outln(""); inList = false; }
            QString rule = QString("─").repeated(60);
            outln(dim(rule));
            continue;
        }

        // Headers
        if (trimmed.startsWith("### ")) {
            if (inList) { outln(""); inList = false; }
            outln(bold(renderInline(trimmed.mid(4))));
            outln();
            continue;
        }
        if (trimmed.startsWith("## ")) {
            if (inList) { outln(""); inList = false; }
            outln(bold(renderInline(trimmed.mid(3))));
            outln();
            continue;
        }
        if (trimmed.startsWith("# ")) {
            if (inList) { outln(""); inList = false; }
            outln(bold(renderInline(trimmed.mid(2))));
            outln();
            continue;
        }

        // Blockquote
        if (trimmed.startsWith("> ")) {
            if (inList) { outln(""); inList = false; }
            outln("  " + dim("│") + " " + renderInline(trimmed.mid(2)));
            continue;
        }
        if (trimmed == ">") {
            if (inList) { outln(""); inList = false; }
            outln("  " + dim("│"));
            continue;
        }

        // Unordered list
        if (trimmed.startsWith("- ") || trimmed.startsWith("* ")) {
            if (!inList) inList = true;
            outln("  " + cyan("•") + " " + renderInline(trimmed.mid(2)));
            continue;
        }

        // Ordered list (simple: starts with "N. ")
        {
            QRegularExpression re("^(\\d+)\\. (.+)");
            auto m = re.match(trimmed);
            if (m.hasMatch()) {
                if (!inList) { inList = true; listNum = 0; }
                listNum++;
                outln("  " + cyan(QString::number(listNum) + ".") + " " + renderInline(m.captured(2)));
                continue;
            }
        }

        // Regular paragraph
        if (inList) { outln(""); inList = false; }
        outln(renderInline(trimmed));
    }

    if (inCodeBlock)
        outln(dim(""));
    if (inList)
        outln();
}

// ── Secure password input ────────────────────────────────────────────

static QString readPassword(const QString& promptStr) {
    out(promptStr);
#ifdef Q_OS_UNIX
    termios old{}, noecho{};
    tcgetattr(STDIN_FILENO, &old);
    noecho = old;
    noecho.c_lflag &= ~(tcflag_t)ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &noecho);
    QTextStream in(stdin);
    QString pw = in.readLine();
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    outln();
    return pw;
#else
    QTextStream in(stdin);
    return in.readLine();
#endif
}

// ── File attachment expansion ────────────────────────────────────────

static QString expandAttachments(const QString& input) {
    static const QString term = QStringLiteral(",;:.!?)]}'\"");
    QString result;
    bool first = true;
    for (const QString& word : input.split(' ', Qt::SkipEmptyParts)) {
        if (!first) result += ' ';
        first = false;
        if (word.startsWith('@') && word.size() > 1) {
            QString path = word.mid(1);
            while (!path.isEmpty() && term.contains(path.back())) path.chop(1);
            if (path.startsWith('~')) path = QDir::homePath() + path.mid(1);
            QFile f(path);
            if (f.open(QIODevice::ReadOnly)) {
                result += QString("\n[File: %1]\n```\n%2\n```\n")
                    .arg(QFileInfo(path).fileName(), QString::fromUtf8(f.readAll()));
                continue;
            }
        }
        result += word;
    }
    return result.trimmed();
}

// ── Model list fetch ─────────────────────────────────────────────────

static QStringList fetchModels(const Config& cfg) {
    QNetworkAccessManager mgr;
    QNetworkRequest req(QUrl(cfg.baseUrl + "/models"));
    req.setRawHeader("Authorization", ("Bearer " + cfg.apiKey).toUtf8());
    req.setRawHeader("User-Agent",    cfg.userAgent.toUtf8());
    QEventLoop loop;
    auto* reply = mgr.get(req);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    reply->deleteLater();
    QStringList models;
    for (const QJsonValue& v : doc["data"].toArray())
        models << v.toObject()["id"].toString();
    models.sort();
    return models;
}

// ── CLI application ──────────────────────────────────────────────────

class PengyCliApp {
public:
    Config      cfg;
    QJsonObject chat;
    QJsonArray  m_runMsgs;
    bool        m_firstEventDone = false;

    void exec(bool singleShot, const QString& singleShotMsg, bool noSave = false) {
        cfg = configLoad();
        Tools::setUserAgent(cfg.userAgent);
        Tools::setTimeout(cfg.toolTimeout);

        if (singleShot && noSave) {
            // Ephemeral chat: never touches chats.json.
            chat = QJsonObject{
                {"id",         QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {"title",      "New Chat"},
                {"messages",   QJsonArray()},
                {"created_at", QDateTime::currentDateTime().toString("yyyy-MM-ddTHH:mm:ss")}
            };
        } else {
            QJsonArray chats = chatsLoad();
            chat = chats.isEmpty() ? chatCreate("New Chat") : chats.first().toObject();
        }

        if (singleShot) {
            runLlm(singleShotMsg, noSave);
            return;
        }

        outln(bold("Pengy") + " — type " + cyan("/help") + " for commands, Ctrl-D to quit");
        outln(dim("Chat: " + chat["title"].toString()));
        outln();

        QTextStream in(stdin);
        while (!in.atEnd()) {
            prompt("You> ");
            QString line = in.readLine();
            if (line.isNull()) break;
            line = line.trimmed();
            if (line.isEmpty()) continue;
            if (!handleCommand(line)) runLlm(line);
        }
        outln();
    }

private:
    // ── LLM run ─────────────────────────────────────────────────────

    void runLlm(const QString& rawInput, bool noSave = false) {
        const QString input = expandAttachments(rawInput);
        if (input.isEmpty()) return;

        QJsonArray hist = chat["messages"].toArray();
        hist.append(QJsonObject{{"role","user"},{"content",input}});
        hist = cleanDanglingToolCalls(hist);
        if (cfg.contextKeepTurns > 0)
            hist = elideOldToolResults(hist, cfg.contextKeepTurns);

        QJsonArray sendMsgs;
        if (!cfg.systemMessage.isEmpty())
            sendMsgs.append(QJsonObject{
                {"role","system"},
                {"content", configRenderSystemMessage(cfg.systemMessage)}
            });
        for (const QJsonValue& v : hist) sendMsgs.append(v);

        // Clear accumulated messages
        m_runMsgs = QJsonArray();
        m_firstEventDone = false;

        Tools::setSudoPasswordProvider([](){ return readPassword("Sudo password: "); });

        out(dim("Thinking..."));

        LlmClient client;
        client.run(
            LlmParams{cfg.baseUrl, cfg.apiKey, cfg.model, sendMsgs, cfg.toolConfirmation, cfg.reasoningEffort, cfg.preserveReasoning},
            [this](const QJsonObject& ev) { onEvent(ev); },
            [this]() -> std::pair<bool,bool> { return onConfirm(); },
            []() -> bool { return false; }
        );

        Tools::clearSudoPasswordProvider();

        // Persist to chat
        QJsonArray msgs = chat["messages"].toArray();
        msgs.append(QJsonObject{{"role","user"},{"content",input}});
        for (const QJsonValue& v : m_runMsgs) msgs.append(v);
        chat["messages"] = msgs;

        if (chat["title"].toString() == "New Chat" && msgs.size() <= 2)
            chat["title"] = input.left(60).replace('\n', ' ');

        if (!noSave) chatSave(chat);
    }

    void onEvent(const QJsonObject& ev) {
        const QString type = ev["type"].toString();

        // Clear thinking indicator on first event
        if (!m_firstEventDone) {
            m_firstEventDone = true;
            out("\r\033[K"); // clear line
        }

        if (type == "assistant_tool_calls") {
            m_runMsgs.append(ev["message"].toObject());

        } else if (type == "tool_request") {
            outln();
            outln(cyan(bold("--- Tool: " + ev["name"].toString() + " ---")));
            outln(dim(QJsonDocument(ev["args"].toObject())
                        .toJson(QJsonDocument::Indented).trimmed()));

        } else if (type == "tool_result") {
            m_runMsgs.append(QJsonObject{
                {"role",         "tool"},
                {"tool_call_id", ev["tool_call_id"].toString()},
                {"content",      ev["content"].toString()}
            });
            if (ev["declined"].toBool()) {
                outln(dim("  (declined)"));
            } else {
                QString result = ev["content"].toString();
                if (result.size() > 2000) result = result.left(2000) + "\n… [truncated]";
                outln(dim("--- Output ---"));
                outln(dim(result));
            }

        } else if (type == "final_response") {
            const QString content = ev["content"].toString();
            if (!content.isEmpty())
                m_runMsgs.append(QJsonObject{{"role","assistant"},{"content",content}});
            outln();
            outln(green(bold("--- Pengy ---")));
            if (content.trimmed().isEmpty()) {
                outln(dim("(empty response)"));
            } else {
                renderMarkdownToTerminal(content);
            }
            const QJsonObject usage = ev["usage"].toObject();
            if (usage["total_tokens"].toInt() > 0) {
                outln(dim(QString("(%1 in / %2 out tokens)")
                    .arg(usage["prompt_tokens"].toInt())
                    .arg(usage["completion_tokens"].toInt())));
            }
            outln();
        }
    }

    std::pair<bool,bool> onConfirm() {
        outln();
        outln("  " + bold("[1]") + " Execute   " +
              bold("[2]") + " Yes to all this turn   " +
              bold("[3]") + " Decline");
        prompt("Choice [1]: ");
        QTextStream in(stdin);
        const QString c = in.readLine().trimmed();
        if (c == "3") return {false, false};
        if (c == "2") return {true,  true};
        return {true, false};
    }

    // ── Slash commands ───────────────────────────────────────────────

    bool handleCommand(const QString& line) {
        if (!line.startsWith('/')) return false;
        const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
        const QString cmd = parts.value(0).toLower();
        const QString arg = parts.size() > 1 ? parts.mid(1).join(' ') : QString();

        if (cmd == "/quit" || cmd == "/exit" || cmd == "/q") {
            outln("Goodbye!");
            exit(0);

        } else if (cmd == "/help") {
            printHelp();

        } else if (cmd == "/new") {
            chat = chatCreate("New Chat");
            outln(dim("New chat started."));

        } else if (cmd == "/config") {
            outln(bold("Configuration:"));
            outln("  base_url:         " + cfg.baseUrl);
            outln("  model:            " + cfg.model);
            outln("  tool_confirm:     " + cfg.toolConfirmation);
            outln("  context_keep:     " + QString::number(cfg.contextKeepTurns));
            outln("  tool_timeout:     " + QString::number(cfg.toolTimeout) + "s");
            outln("  api_key:          " + (cfg.apiKey.isEmpty() ? dim("(not set)") : dim("***")));
            outln("  system_message:   " + (cfg.systemMessage.isEmpty()
                                           ? dim("(not set)") : cfg.systemMessage.left(60)));

        } else if (cmd == "/model") {
            if (arg.isEmpty()) outln("Current model: " + bold(cfg.model));
            else { cfg.model = arg; configSave(cfg); outln(dim("Model → " + cfg.model)); }

        } else if (cmd == "/models") {
            outln(dim("Fetching models…"));
            const QStringList models = fetchModels(cfg);
            if (models.isEmpty()) outln(red("Failed to fetch models."));
            else for (const QString& m : models) outln("  " + m);

        } else if (cmd == "/baseurl") {
            if (arg.isEmpty()) outln("base_url: " + cfg.baseUrl);
            else { cfg.baseUrl = arg; configSave(cfg); outln(dim("Base URL updated.")); }

        } else if (cmd == "/apikey") {
            if (arg.isEmpty()) outln("api_key: " + (cfg.apiKey.isEmpty() ? dim("(not set)") : dim("***")));
            else { cfg.apiKey = arg; configSave(cfg); outln(dim("API key updated.")); }

        } else if (cmd == "/timeout") {
            bool ok; int n = arg.toInt(&ok);
            if (!ok || n <= 0) outln("Usage: /timeout <seconds>");
            else { cfg.toolTimeout = n; Tools::setTimeout(n); configSave(cfg);
                   outln(dim("Timeout → " + QString::number(n) + "s")); }

        } else if (cmd == "/agent") {
            if (arg.isEmpty()) outln("user_agent: " + cfg.userAgent);
            else { cfg.userAgent = arg; Tools::setUserAgent(arg); configSave(cfg);
                   outln(dim("User agent updated.")); }

        } else if (cmd == "/context-keep") {
            bool ok; int n = arg.toInt(&ok);
            if (!ok || n < 0) outln("Usage: /context-keep <n>  (0 = keep all)");
            else { cfg.contextKeepTurns = n; configSave(cfg);
                   outln(dim("context_keep_turns → " + QString::number(n))); }

        } else if (cmd == "/yolo") {
            static const QStringList modes = {"none","safe","all"};
            if (arg.isEmpty()) {
                int i = (modes.indexOf(cfg.toolConfirmation) + 1) % modes.size();
                cfg.toolConfirmation = modes[i];
            } else if (modes.contains(arg)) {
                cfg.toolConfirmation = arg;
            } else {
                outln("Usage: /yolo [none|safe|all]"); return true;
            }
            configSave(cfg);
            outln(dim("tool_confirmation → " + cfg.toolConfirmation));

        } else if (cmd == "/system") {
            if (arg.isEmpty()) {
                QString rendered = cfg.systemMessage.isEmpty()
                    ? dim("(not set)")
                    : configRenderSystemMessage(cfg.systemMessage);
                outln(bold("Template: ") + cfg.systemMessage);
                outln(bold("Rendered: ") + rendered);
            } else { cfg.systemMessage = arg; configSave(cfg);
                outln(green("System message updated."));
                outln(bold("Rendered: ") + configRenderSystemMessage(arg));
            }

        } else if (cmd == "/compact") {
            int turns = cfg.contextKeepTurns > 0 ? cfg.contextKeepTurns : 3;
            int oldCount = chat["messages"].toArray().size();
            QJsonArray msgs = elideOldToolResults(chat["messages"].toArray(), turns);
            chat["messages"] = msgs;
            int newCount = msgs.size();
            chatSave(chat);
            outln(green("Compacted: ") + "elided tool results older than " +
                  QString::number(turns) + " turns. (" +
                  QString::number(oldCount) + " -> " + QString::number(newCount) + " messages)");

        } else if (cmd == "/list") {
            listChats();

        } else if (cmd == "/load") {
            bool ok; int n = arg.toInt(&ok);
            if (!ok || n < 1) outln("Usage: /load <n>  (see /list)");
            else loadChat(n - 1);

        } else if (cmd == "/delete") {
            bool ok; int n = arg.toInt(&ok);
            if (!ok || n < 1) outln("Usage: /delete <n>  (see /list)");
            else deleteChat(n - 1);

        } else if (cmd == "/attach") {
            outln(bold("File attachment:"));
            outln("  Use " + cyan("@path/to/file") + " anywhere in your message to attach a text file.");
            outln("  Example: " + dim("Look at @src/main.cpp and fix the bug"));

        } else {
            outln(red("Unknown command: " + cmd + "  — type /help"));
        }
        return true;
    }

    void printHelp() {
        struct Cmd { const char* c; const char* d; };
        static const Cmd cmds[] = {
            {"/new",                "Start a new chat"},
            {"/list",               "List all chats"},
            {"/load <n>",           "Load chat by number"},
            {"/delete <n>",         "Delete chat by number"},
            {"/compact",            "Elide old tool results in current chat"},
            {"/model [name]",       "Show or set model"},
            {"/models",             "Fetch available models from endpoint"},
            {"/baseurl [url]",      "Show or set API base URL"},
            {"/apikey [key]",       "Show or set API key"},
            {"/system [msg]",       "Show or set system message template"},
            {"/yolo [none|safe|all]","Cycle or set tool confirmation mode"},
            {"/context-keep <n>",   "Keep last N turns full (0 = keep all)"},
            {"/timeout <n>",        "Set tool execution timeout in seconds"},
            {"/agent [str]",        "Show or set user agent string"},
            {"/attach",             "Show file attachment help"},
            {"/config",             "Show current configuration"},
            {"/help",               "Show this help"},
            {"/quit",               "Exit"},
        };
        outln(bold("Commands:"));
        for (const auto& cmd : cmds)
            outln(QString("  %-28s %2").arg(cmd.c).arg(cmd.d));
        outln();
        outln(dim("  @/path/to/file  — attach file content to your message"));
        outln(dim("  {date} {username} {hostname} {osinfo}  — system message variables"));
    }

    void listChats() {
        const QJsonArray chats = chatsLoad();
        if (chats.isEmpty()) { outln(dim("No chats.")); return; }
        outln(bold("Chats:"));
        for (int i = 0; i < chats.size(); i++) {
            const QJsonObject c = chats[i].toObject();
            const bool cur = c["id"].toString() == chat["id"].toString();
            outln(QString("  %1. %2%3")
                .arg(i + 1)
                .arg(c["title"].toString())
                .arg(cur ? green("  ◀ current") : ""));
        }
    }

    void loadChat(int idx) {
        const QJsonArray chats = chatsLoad();
        if (idx < 0 || idx >= chats.size()) { outln(red("No chat at that index.")); return; }
        chat = chats[idx].toObject();
        outln(dim("Loaded: " + chat["title"].toString()));
    }

    void deleteChat(int idx) {
        const QJsonArray chats = chatsLoad();
        if (idx < 0 || idx >= chats.size()) { outln(red("No chat at that index.")); return; }
        const QString id    = chats[idx].toObject()["id"].toString();
        const QString title = chats[idx].toObject()["title"].toString();
        chatDelete(id);
        outln(dim("Deleted: " + title));
        if (id == chat["id"].toString()) {
            chat = chatCreate("New Chat");
            outln(dim("Started new chat."));
        }
    }
};

// ── Entry point ───────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

#ifdef Q_OS_UNIX
    g_color = isatty(STDOUT_FILENO);
#endif

    const QStringList args = app.arguments().mid(1);
    bool noSave = false;
    QStringList promptArgs;
    for (const QString& a : args) {
        if (a == "--no-save") {
            noSave = true;
        } else if (a == "-v" || a == "--version") {
            outln(QString("Pengy v") + PENGY_VERSION);
            return 0;
        } else if (a == "-h" || a == "--help") {
            outln("Usage: pengy_cli [prompt...] [--no-save]");
            outln("  prompt      Optional prompt for single-shot mode. If omitted, starts interactive mode.");
            outln("  --no-save   Don't persist single-shot chats to history.");
            outln("  -v, --version  Show version information and exit.");
            outln("  -h, --help     Show this help message and exit.");
            return 0;
        } else {
            promptArgs.append(a);
        }
    }
    const bool singleShot = !promptArgs.isEmpty();
    const QString msg     = singleShot ? promptArgs.join(' ') : QString();

    PengyCliApp cli;
    cli.exec(singleShot, msg, noSave);
    return 0;
}
