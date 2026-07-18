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
#  include <readline/readline.h>
#  include <readline/history.h>
#  include <termios.h>
#  include <unistd.h>
#endif

#include "../config.h"
#include "../chatmanager.h"
#include "../llmclient.h"
#include "../tools.h"
#include "version.h"

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
static inline QString blue(const QString& s)   { return clr("\033[34m", s); }
static inline QString red(const QString& s)    { return clr("\033[31m", s); }

static void out(const QString& s) {
    fputs(s.toUtf8().constData(), stdout);
    fflush(stdout);
}
static void outln(const QString& s = {}) { out(s + '\n'); }
static void prompt(const QString& p) { out(bold(p)); }

// ── Markdown-to-terminal renderer ────────────────────────────────────

static QString renderInline(const QString& text) {
    QString result = text;

    {
        QRegularExpression re("`([^`\\n]+)`");
        result.replace(re, g_color ? "\033[2;36m\\1\033[0m" : "\\1");
    }
    {
        QRegularExpression re("\\*\\*([^*\\n]+)\\*\\*");
        result.replace(re, g_color ? "\033[1m\\1\033[0m" : "\\1");
    }
    {
        QRegularExpression re("\\*([^*\\n]+)\\*");
        result.replace(re, g_color ? "\033[3m\\1\033[0m" : "\\1");
    }
    {
        QRegularExpression re("\\[([^\\]]+)\\]\\(([^)]+)\\)");
        result.replace(re, g_color ? "\033[36m\\1\033[0m (\\2)" : "\\1 (\\2)");
    }

    return result;
}

static void renderMarkdownToTerminal(const QString& text) {
    const QStringList lines = text.split('\n');
    bool inCodeBlock = false;
    bool inList = false;
    int listNum = 0;

    for (int i = 0; i < lines.size(); i++) {
        const QString& line = lines[i];
        const QString trimmed = line.trimmed();

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

        if (trimmed.isEmpty()) {
            if (inList) { outln(""); inList = false; }
            if (i + 1 < lines.size() && !lines[i + 1].trimmed().isEmpty())
                outln();
            continue;
        }

        if (trimmed == "---" || trimmed == "***" || trimmed == "___") {
            if (inList) { outln(""); inList = false; }
            QString rule = QString("─").repeated(60);
            outln(dim(rule));
            continue;
        }

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

        if (trimmed.startsWith("- ") || trimmed.startsWith("* ")) {
            if (!inList) inList = true;
            outln("  " + cyan("•") + " " + renderInline(trimmed.mid(2)));
            continue;
        }

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

        if (inList) { outln(""); inList = false; }
        outln(renderInline(trimmed));
    }

    if (inCodeBlock) outln(dim(""));
    if (inList) outln();
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

static QString truncate(const QString& text, int maxLen = 72) {
    QString preview = text.simplified();
    if (preview.length() <= maxLen) return preview;
    return preview.left(maxLen - 1) + QStringLiteral("…");
}

// ── Readline history ─────────────────────────────────────────────────

static QString g_histPath;

#ifdef Q_OS_UNIX

static void initReadline() {
    QString dataDir = QDir::homePath() + "/.local/state/pengy";
    QDir().mkpath(dataDir);
    g_histPath = dataDir + "/cli_history";
    rl_attempted_completion_function = nullptr;
    read_history(g_histPath.toUtf8().constData());
    stifle_history(1000);
}

static void saveReadlineHistory() {
    write_history(g_histPath.toUtf8().constData());
}

static QString readline_qstring(const char* prompt_str) {
    char* raw = readline(prompt_str);
    if (!raw) return {}; // EOF
    QString line = QString::fromUtf8(raw);
    QString trimmed = line.trimmed();
    if (!trimmed.isEmpty()) {
        add_history(trimmed.toUtf8().constData());
    }
    free(raw);
    return line;
}

#else
// Windows fallback: simple stdin input without readline history

static void initReadline() {}

static void saveReadlineHistory() {}

static QString readline_qstring(const char* prompt_str) {
    QTextStream out(stdout);
    out << prompt_str;
    out.flush();
    QTextStream in(stdin);
    QString line = in.readLine();
    if (line.isNull()) return {}; // EOF
    return line;
}
#endif

// ── CLI application ──────────────────────────────────────────────────

class PengyCliApp {
public:
    Config      cfg;
    QJsonObject chat;
    QJsonArray  m_runMsgs;
    bool        m_firstEventDone = false;
    QString     m_outputMode = "pretty";

    void exec(bool singleShot, const QString& singleShotMsg,
              bool noSave = false,
              const QString& modelOverride = {},
              const QString& systemOverride = {}) {
        cfg = configLoad();

        if (!modelOverride.isEmpty())
            cfg.model = modelOverride;
        if (!systemOverride.isEmpty())
            cfg.systemMessage = systemOverride;
        Tools::setUserAgent(cfg.userAgent);
        Tools::setTimeout(cfg.toolTimeout);

        if (singleShot && noSave) {
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

        // Startup summary
        {
            QJsonArray msgs = chat["messages"].toArray();
            int msgCount = msgs.size();
            QString title = chat["title"].toString();
            outln(dim("Chat: " + title + " (" + QString::number(msgCount) + " messages)"));

            // Show last user message as context
            for (int i = msgs.size() - 1; i >= 0; --i) {
                if (msgs[i].toObject()["role"].toString() == "user") {
                    QString last = msgs[i].toObject()["content"].toString();
                    if (!last.isEmpty()) {
                        outln(dim("Last: ") + truncate(last, 80));
                    }
                    break;
                }
            }
        }
        outln(dim("Model: " + cfg.model + "  Tool Confirm: " + cfg.toolConfirmation));
        outln();

        initReadline();
        for (;;) {
            QString title = truncate(chat["title"].toString(), 30);
            QString promptLine = title + " › You> ";
            QString line = readline_qstring(promptLine.toUtf8().constData());
            if (line.isNull()) break;
            line = line.trimmed();
            if (line.isEmpty()) continue;
            if (!handleCommand(line)) runLlm(line);
        }
        saveReadlineHistory();
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

        if (!m_firstEventDone) {
            m_firstEventDone = true;
            out("\r\033[K");
        }

        if (type == "assistant_tool_calls") {
            m_runMsgs.append(ev["message"].toObject());

        } else if (type == "tool_request") {
            outln();
            outln(cyan(bold("--- Tool: " + ev["name"].toString() + " ---")));
            QString argsText = QJsonDocument(ev["args"].toObject())
                        .toJson(QJsonDocument::Indented).trimmed();
            if (argsText.size() > 4000) argsText = argsText.left(4000) + "\n… [truncated]";
            outln(dim(argsText));

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

            if (m_outputMode == "silent") {
                // No output
            } else if (m_outputMode == "json") {
                QJsonObject result;
                result["content"] = content;
                result["usage"] = ev["usage"].toObject();
                outln(QJsonDocument(result).toJson(QJsonDocument::Indented));
            } else if (m_outputMode == "raw") {
                if (!content.trimmed().isEmpty())
                    outln(content);
            } else {
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
    }

    std::pair<bool,bool> onConfirm() {
        outln();
        outln("  " + bold("[1]") + " Execute   " +
              bold("[2]") + " Yes to all this turn   " +
              bold("[3]") + " Decline   " +
              bold("[4]") + " Abort run");
        for (;;) {
            QString c = readline_qstring("Choice [1]: ").trimmed();
            if (c.isEmpty() || c == "1") return {true, false};
            if (c == "2") return {true, true};
            if (c == "3") return {false, false};
            if (c == "4") {
                outln(red("Run aborted by user."));
                return {false, false};  // decline + don't yolo
            }
            outln(red("Please enter 1, 2, 3, or 4."));
        }
    }

    // ── Slash commands ───────────────────────────────────────────────

    bool handleCommand(const QString& line) {
        if (!line.startsWith('/')) return false;
        const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
        const QString cmd = parts.value(0).toLower();
        const QString arg = parts.size() > 1 ? parts.mid(1).join(' ') : QString();

        if (cmd == "/quit" || cmd == "/exit" || cmd == "/q") {
            saveReadlineHistory();
            outln("Goodbye!");
            exit(0);

        } else if (cmd == "/help") {
            printHelp();

        } else if (cmd == "/new") {
            chat = chatCreate("New Chat");
            outln(green("✓ New chat created."));

        } else if (cmd == "/show") {
            cmdShow(arg);

        } else if (cmd == "/tail") {
            cmdTail(arg);

        } else if (cmd == "/rename") {
            cmdRename(arg);

        } else if (cmd == "/clear") {
            out("\033[2J\033[H");
            outln(dim("Screen cleared. Use /show to see conversation."));

        } else if (cmd == "/export") {
            cmdExport(arg);

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

    // ── New command implementations ──────────────────────────────────

    void cmdShow(const QString& arg) {
        QJsonArray msgs = chat["messages"].toArray();
        int total = msgs.size();
        if (total == 0) {
            outln(dim("No messages in this chat."));
            return;
        }

        int limit = 0;
        if (!arg.isEmpty()) {
            bool ok; limit = arg.toInt(&ok);
            if (!ok || limit <= 0) {
                outln("Usage: /show [N]  — show last N messages");
                return;
            }
        }

        int start = limit > 0 ? qMax(0, total - limit) : 0;
        int showing = total - start;

        outln();
        outln(bold("Conversation: ") + bold(chat["title"].toString()) +
              dim(" (" + QString::number(total) + " messages total" +
                  (limit > 0 ? ", showing last " + QString::number(showing) : "") + ")"));
        outln(dim(QString("─").repeated(60)));

        for (int i = start; i < total; i++) {
            QJsonObject msg = msgs[i].toObject();
            QString role = msg["role"].toString();
            QString content = msg["content"].toString();
            int num = i + 1;

            if (role == "user") {
                outln(blue(bold("#" + QString::number(num) + " You:")) + " " + truncate(content, 200));
            } else if (role == "assistant") {
                QJsonArray toolCalls = msg["tool_calls"].toArray();
                QString suffix;
                if (!toolCalls.isEmpty()) {
                    QStringList tcNames;
                    for (const QJsonValue& tc : toolCalls)
                        tcNames << tc.toObject()["function"].toObject()["name"].toString();
                    suffix = dim(" (tool calls: " + tcNames.join(", ") + ")");
                }
                outln(green(bold("#" + QString::number(num) + " Assistant:")) + suffix);
                if (!content.isEmpty()) {
                    outln(dim("  " + truncate(content, 100)));
                }
            } else if (role == "tool") {
                QString tcId = msg["tool_call_id"].toString().left(8);
                outln(dim("#" + QString::number(num) + " Tool [" + tcId + "]: " + truncate(content, 80)));
            } else if (role == "system") {
                outln(dim("#" + QString::number(num) + " System: " + truncate(content, 100)));
            }
        }
        outln(dim(QString("─").repeated(60)));
    }

    void cmdTail(const QString& arg) {
        int n = 5;
        if (!arg.isEmpty()) {
            bool ok; int parsed = arg.toInt(&ok);
            if (ok && parsed > 0) n = parsed;
        }
        cmdShow(QString::number(n));
    }

    void cmdRename(const QString& arg) {
        if (arg.isEmpty()) {
            outln(dim("Usage: /rename <new title>"));
            return;
        }
        QString oldTitle = chat["title"].toString();
        chat["title"] = arg;
        chatSave(chat);
        outln(green("✓ Renamed: ") + bold(oldTitle) + " → " + bold(arg));
    }

    void cmdExport(const QString& arg) {
        QJsonArray msgs = chat["messages"].toArray();

        QString outPath = arg;
        if (outPath.isEmpty()) {
            QString safe = chat["title"].toString();
            safe.replace(QRegularExpression("[^a-zA-Z0-9 _-]"), "");
            safe = safe.trimmed().left(50);
            if (safe.isEmpty()) safe = "chat";
            outPath = QDir::homePath() + "/Downloads/" + safe + ".md";
        }

        QStringList lines;
        lines << "# " + chat["title"].toString();
        lines << "*Exported " + QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss") + "*";
        lines << "";

        for (const QJsonValue& v : msgs) {
            QJsonObject msg = v.toObject();
            QString role = msg["role"].toString();
            QString content = msg["content"].toString();

            if (role == "user") {
                lines << "### 🧑 You";
                lines << content;
                lines << "";
            } else if (role == "assistant") {
                QJsonArray toolCalls = msg["tool_calls"].toArray();
                if (!toolCalls.isEmpty()) {
                    lines << "### 🤖 Assistant (tool calls)";
                    for (const QJsonValue& tc : toolCalls) {
                        QJsonObject fn = tc.toObject()["function"].toObject();
                        lines << "- **" + fn["name"].toString() + "**";
                        lines << "  ```json\n  " + fn["arguments"].toString() + "\n  ```";
                    }
                    lines << "";
                }
                if (!content.isEmpty()) {
                    lines << "### 🤖 Assistant";
                    lines << content;
                    lines << "";
                }
            } else if (role == "tool") {
                QString tcId = msg["tool_call_id"].toString();
                lines << "#### 🔧 Tool result (`" + tcId + "`)";
                lines << "```";
                lines << content;
                lines << "```";
                lines << "";
            } else if (role == "system") {
                lines << "*System: " + truncate(content, 200) + "*";
                lines << "";
            }
        }

        QFileInfo fi(outPath);
        QDir().mkpath(fi.dir().absolutePath());
        QFile f(outPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(lines.join('\n').toUtf8());
            f.close();
            outln(green("✓ Exported to: ") + bold(outPath));
        } else {
            outln(red("Error exporting: ") + f.errorString());
        }
    }

    // ── Updated existing commands ────────────────────────────────────

    void printHelp() {
        struct Cmd { const char* c; const char* d; };
        static const Cmd cmds[] = {
            {"/help",                "Show this help"},
            {"/new",                 "Start a new chat"},
            {"/show [N]",            "Show full conversation (optional: last N messages)"},
            {"/tail [N]",            "Show the last N messages (default 5)"},
            {"/rename <title>",      "Rename the current chat"},
            {"/clear",               "Clear the terminal screen"},
            {"/export [path]",       "Export current chat as Markdown"},
            {"/config",              "Show current configuration"},
            {"/model [name]",        "Show or set model"},
            {"/models",              "Fetch available models from endpoint"},
            {"/baseurl [url]",       "Show or set API base URL"},
            {"/apikey [key]",        "Show or set API key"},
            {"/system [msg]",        "Show or set system message template"},
            {"/yolo [none|safe|all]","Cycle or set tool confirmation mode"},
            {"/context-keep <n>",    "Keep last N turns full (0 = keep all)"},
            {"/timeout <n>",         "Set tool execution timeout in seconds"},
            {"/agent [str]",         "Show or set user agent string"},
            {"/compact",             "Elide old tool results in current chat"},
            {"/list",                "List all chats"},
            {"/load <n>",            "Load chat by number"},
            {"/delete <n>",          "Delete chat by number"},
            {"/attach",              "Show file attachment help"},
            {"/quit",                "Exit"},
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
        outln(dim(QString("  %1  %2  %3  %4")
            .arg("#", -4)
            .arg("Title", -30)
            .arg("Msgs", 6)
            .arg("Preview")));

        QString currentId = chat["id"].toString();
        for (int i = 0; i < chats.size(); i++) {
            const QJsonObject c = chats[i].toObject();
            const bool cur = c["id"].toString() == currentId;
            int msgCount = c["messages"].toArray().size();

            // Find first user message as preview
            QString preview;
            const QJsonArray msgs = c["messages"].toArray();
            for (const QJsonValue& v : msgs) {
                if (v.toObject()["role"].toString() == "user") {
                    preview = truncate(v.toObject()["content"].toString(), 30);
                    break;
                }
            }

            outln(QString("  %1 %2 %3  %4")
                .arg((cur ? green("→") + QString::number(i + 1) : " " + QString::number(i + 1)), -4)
                .arg(truncate(c["title"].toString(), 28), -30)
                .arg(msgCount, 5)
                .arg(preview));
        }
    }

    void loadChat(int idx) {
        const QJsonArray chats = chatsLoad();
        if (idx < 0 || idx >= chats.size()) { outln(red("No chat at that index.")); return; }
        chat = chats[idx].toObject();
        int msgCount = chat["messages"].toArray().size();
        outln(dim("Loaded: " + chat["title"].toString() + " (" + QString::number(msgCount) + " messages)"));
        // Show tail for context
        cmdTail("3");
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
    QString outputMode = "pretty";
    QString configDir;
    QString modelOverride;
    QString systemOverride;
    QStringList promptArgs;
    for (int i = 0; i < args.size(); i++) {
        const QString& a = args[i];
        if (a == "--no-save") {
            noSave = true;
        } else if (a == "-v" || a == "--version") {
            outln(QString("Pengy v") + PENGY_VERSION);
            return 0;
        } else if (a == "-h" || a == "--help") {
            outln("Usage: pengy_cli [prompt...] [OPTIONS]");
            outln();
            outln("Arguments:");
            outln("  prompt      Optional prompt for single-shot mode. If omitted, starts interactive mode.");
            outln();
            outln("Options:");
            outln("  --no-save        Don't persist single-shot chats to history.");
            outln("  --model NAME     Set the model to use (overrides config).");
            outln("  --system MSG     Set the system message (overrides config).");
            outln("  --output FORMAT  Output format: pretty, raw, json, silent (default: pretty).");
            outln("  --config-dir PATH  Use a custom config directory.");
            outln("  -v, --version    Show version information and exit.");
            outln("  -h, --help       Show this help message and exit.");
            return 0;
        } else if (a == "--model" && i + 1 < args.size()) {
            modelOverride = args[++i];
        } else if (a == "--system" && i + 1 < args.size()) {
            systemOverride = args[++i];
        } else if (a == "--output" && i + 1 < args.size()) {
            outputMode = args[++i];
        } else if (a == "--config-dir" && i + 1 < args.size()) {
            configDir = args[++i];
        } else {
            promptArgs.append(a);
        }
    }
    const bool singleShot = !promptArgs.isEmpty();
    const QString msg     = singleShot ? promptArgs.join(' ') : QString();

    if (!configDir.isEmpty())
        setConfigDir(configDir);

    PengyCliApp cli;
    cli.m_outputMode = outputMode;
    cli.exec(singleShot, msg, noSave, modelOverride, systemOverride);
    return 0;
}
