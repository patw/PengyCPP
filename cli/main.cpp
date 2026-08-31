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
#  include <sys/ioctl.h>
#endif

#include "../config.h"
#include "../chatmanager.h"
#include "../llmclient.h"
#include "../taskmanager.h"
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
static inline QString yellow(const QString& s) { return clr("\033[33m", s); }

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

static int terminalWidth() {
#ifdef Q_OS_UNIX
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
#endif
    bool ok = false;
    const int columns = qEnvironmentVariable("COLUMNS").toInt(&ok);
    return ok && columns > 0 ? columns : 120;
}

static QStringList lastMessageLines(const QString& content, int maxLines = 10) {
    QString text = content.trimmed();
    if (text.isEmpty()) return {};
    const int width = qMax(20, terminalWidth() - 2);
    QStringList result;
    for (const QString& source : text.split('\n')) {
        QString line = source;
        while (line.size() > width) {
            result << line.left(width);
            line = line.mid(width);
        }
        result << line;
        if (result.size() >= maxLines) break;
    }
    if (result.size() > maxLines) result = result.mid(0, maxLines);
    if (text.split('\n').size() > maxLines || result.last().size() == width)
        result.last() = truncate(result.last(), width - 1) + QStringLiteral("…");
    return result;
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
    bool        m_firstEventDone = false;
    bool        m_noSave = false;
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
        Tools::setToolOutputMaxChars(cfg.toolOutputMaxChars);
        Tools::setDownloadMaxMb(cfg.downloadMaxMb);
        Tools::setImageLimits(cfg.imageMaxDimension, cfg.imageMaxMb, cfg.imageQuality);

        if (singleShot && noSave) {
            chat = QJsonObject{
                {"id",         QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {"title",      "New Chat"},
                {"messages",   QJsonArray()},
                {"created_at", QDateTime::currentDateTime().toString("yyyy-MM-ddTHH:mm:ss")}
            };
        } else {
            QJsonArray chats = chatsLoadIndex();
            chat = chats.isEmpty()
                       ? chatCreate("New Chat")
                       : chatGet(chats.first().toObject()["id"].toString());
            if (chat.isEmpty()) chat = chatCreate("New Chat");
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
                    const QStringList preview = lastMessageLines(last);
                    if (!preview.isEmpty()) {
                        outln(dim("Last:"));
                        for (const QString& line : preview) outln("  " + line);
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

        m_noSave = noSave;

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

        m_firstEventDone = false;

        // Persist the user message (and the derived title) before the run, so
        // the turn's tool calls have something to extend as they land.  Nothing
        // about the turn waits on the turn finishing to reach disk.
        {
            QJsonArray msgs = chat["messages"].toArray();
            msgs.append(QJsonObject{{"role","user"},{"content",input}});
            chat["messages"] = msgs;
            if (chat["title"].toString() == "New Chat" && msgs.size() <= 2)
                chat["title"] = input.left(60).replace('\n', ' ');
            saveProgress();
        }

        Tools::setSudoPasswordProvider([](){ return readPassword("Sudo password: "); });

        out(dim("Thinking..."));

        LlmClient client;
        client.run(
            LlmParams{cfg.baseUrl, cfg.apiKey, cfg.model, sendMsgs, cfg.toolConfirmation, cfg.reasoningEffort, cfg.preserveReasoning, cfg.llmTimeout},
            [this](const QJsonObject& ev) { onEvent(ev); },
            [this]() -> std::pair<bool,bool> { return onConfirm(); },
            []() -> bool { return false; },
            [this](const QJsonArray& questions) -> QStringList { return onQuestion(questions); }
        );

        Tools::clearSudoPasswordProvider();

        // An abort or an error ends the run mid-turn, where the last assistant
        // message can hold tool_calls with no result behind them (the API 400s
        // on that next request) -- repair before the final write.
        chat["messages"] = cleanDanglingToolCalls(chat["messages"].toArray());
        saveProgress();
    }

    // Persist mid-run, so a crash can't take the turn's tool calls with it.
    // One small per-chat file write; the whole store is not touched.
    void saveProgress() {
        if (!m_noSave) chatSave(chat);
    }

    // Append a message to the live chat and persist it immediately.
    void appendAndSave(const QJsonObject& msg) {
        QJsonArray msgs = chat["messages"].toArray();
        msgs.append(msg);
        chat["messages"] = msgs;
        saveProgress();
    }

    void onEvent(const QJsonObject& ev) {
        const QString type = ev["type"].toString();

        if (!m_firstEventDone) {
            m_firstEventDone = true;
            out("\r\033[K");
        }

        if (type == "assistant_tool_calls") {
            renderAssistantPreamble(ev["message"].toObject());
            appendAndSave(ev["message"].toObject());

        } else if (type == "retrying") {
            // 429/529 backoff — surface it instead of hanging silently.
            outln(yellow(QString("Overloaded (HTTP %1) — retrying in %2s (%3/%4)")
                .arg(ev["status_code"].toVariant().toString())
                .arg(ev["delay_secs"].toDouble(), 0, 'f', 1)
                .arg(ev["attempt"].toInt())
                .arg(ev["max_attempts"].toInt())));

        } else if (type == "tool_request") {
            outln();
            outln(cyan(bold("--- Tool: " + ev["name"].toString() + " ---")));
            QString argsText = QJsonDocument(ev["args"].toObject())
                        .toJson(QJsonDocument::Indented).trimmed();
            if (argsText.size() > 4000) argsText = argsText.left(4000) + "\n… [truncated]";
            outln(dim(argsText));

        } else if (type == "question_result") {
            // The LLM loop already has this on its own message list; persist it
            // too, or the assistant tool_calls message above is left dangling.
            appendAndSave(QJsonObject{
                {"role",         "tool"},
                {"tool_call_id", ev["tool_call_id"].toString()},
                {"content",      ev["content"].toString()}
            });
            outln(dim(ev["content"].toString()));

        } else if (type == "tool_result") {
            appendAndSave(QJsonObject{
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
                appendAndSave(QJsonObject{{"role","assistant"},{"content",content}});

            // Accumulate into chat["usage"] rather than overwrite: LlmClient
            // reports usage per turn only, and the running total across the
            // chat is a more useful "how much context has this chat burned
            // through" signal than the last turn alone -- the same pressure
            // /compact and /redact exist to relieve.
            chat = chatAddUsage(chat, ev["usage"].toObject());
            saveProgress();
            const QJsonObject totals = chat["usage"].toObject();

            if (m_outputMode == "silent") {
                // No output
            } else if (m_outputMode == "json") {
                QJsonObject result;
                result["content"] = content;
                result["usage"] = ev["usage"].toObject();
                result["cumulative_usage"] = totals;
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
                    outln(dim(QString("(%1 in / %2 out tokens this turn, %3 total this chat)")
                        .arg(usage["prompt_tokens"].toInt())
                        .arg(usage["completion_tokens"].toInt())
                        .arg(totals["total_tokens"].toInt())));
                }
                outln();
            }
        }
    }

    // Show the narration the model wrote alongside its tool calls.  It is
    // persisted with the turn and shows up on a later /show, so a live run that
    // skipped it looked like the model went straight to the tools with nothing
    // to say.  json mode stays silent: its output is a single object built from
    // the final response.
    void renderAssistantPreamble(const QJsonObject& message) {
        const QString content = message["content"].toString().trimmed();
        if (content.isEmpty() || m_outputMode == "silent" || m_outputMode == "json")
            return;

        if (m_outputMode == "raw") {
            outln(content);
            return;
        }

        outln();
        outln(green(bold("--- Pengy ---")));
        renderMarkdownToTerminal(content);
    }

    // ask_user_question always pauses for the user, whatever the confirmation
    // mode.  An empty return means "cancelled" to the harness.
    QStringList onQuestion(const QJsonArray& questions) {
        if (questions.isEmpty()) return QStringList();

        outln();
        outln(cyan(bold("--- The assistant needs your input ---")));

        QStringList answers;
        for (int qi = 0; qi < questions.size(); ++qi) {
            const QJsonObject q = questions[qi].toObject();
            const QString header = q["header"].toString(QString("Question %1").arg(qi + 1));
            const QJsonArray options = q["options"].toArray();

            outln();
            outln(bold(header));
            outln(dim(q["question"].toString()));
            outln();
            for (int oi = 0; oi < options.size(); ++oi) {
                const QJsonObject opt = options[oi].toObject();
                outln(QString("  [%1] %2  %3")
                          .arg(oi + 1)
                          .arg(opt["label"].toString(),
                               dim("— " + opt["description"].toString())));
            }
            outln(dim("  (blank = 1, 'c' = cancel, or type your own answer)"));

            for (;;) {
                const QByteArray prompt =
                    QString("  Choose [1-%1]: ").arg(options.size()).toUtf8();
                const QString c = readline_qstring(prompt.constData()).trimmed();
                if (c.compare("c", Qt::CaseInsensitive) == 0) {
                    outln(red("Question cancelled."));
                    return QStringList();
                }
                if (c.isEmpty()) {
                    if (options.isEmpty()) continue;
                    answers.append(options[0].toObject()["label"].toString());
                    break;
                }
                bool isNumber = false;
                const int idx = c.toInt(&isNumber) - 1;
                if (isNumber) {
                    if (idx >= 0 && idx < options.size()) {
                        answers.append(options[idx].toObject()["label"].toString());
                        break;
                    }
                    outln(red(QString("Please enter a number between 1 and %1.").arg(options.size())));
                    continue;
                }
                // Anything else is a free-text answer, like the GUI's "Other".
                answers.append(c);
                break;
            }
        }

        outln(green("Answers recorded."));
        return answers;
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
            outln("  llm_timeout:     " + QString::number(cfg.llmTimeout) + "s");
            outln("  tool_timeout:     " + QString::number(cfg.toolTimeout) + "s");
            outln("  download_max_mb:  " + QString::number(cfg.downloadMaxMb) + " MB");
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

        } else if (cmd == "/llm-timeout") {
            bool ok; int n = arg.toInt(&ok);
            if (!ok || n <= 0) outln("Usage: /llm-timeout <seconds>");
            else { cfg.llmTimeout = n; configSave(cfg);
                   outln(dim("LLM timeout → " + QString::number(n) + "s")); }

        } else if (cmd == "/timeout") {
            bool ok; int n = arg.toInt(&ok);
            if (!ok || n <= 0) outln("Usage: /timeout <seconds>");
            else { cfg.toolTimeout = n; Tools::setTimeout(n); configSave(cfg);
                   outln(dim("Timeout → " + QString::number(n) + "s")); }

        } else if (cmd == "/download-max") {
            bool ok; int n = arg.toInt(&ok);
            if (!ok || n < 0) outln("Usage: /download-max <mb> (0 = no limit)");
            else { cfg.downloadMaxMb = n; Tools::setDownloadMaxMb(n); configSave(cfg);
                   outln(dim("Download max → " + QString::number(n) + " MB")); }

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

        } else if (cmd == "/redact") {
            cmdRedact(arg);

        } else if (cmd == "/tasks") {
            cmdTasks();

        } else if (cmd == "/task") {
            cmdTask(arg);

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

    // Delete the last N raw messages (default 1) from the current chat.
    //
    // This is the "undo the model's last step" button: repeatable all the
    // way to an empty chat. It edits chats.json directly, so it wrecks
    // prompt caching on most backends -- that's the expected trade-off for
    // pruning a wrong path out of context.
    void cmdRedact(const QString& arg) {
        QJsonArray msgs = chat["messages"].toArray();
        if (msgs.isEmpty()) {
            outln(dim("Chat is already empty."));
            return;
        }

        int n = 1;
        if (!arg.isEmpty()) {
            bool ok; n = arg.toInt(&ok);
            if (!ok || n < 1) {
                outln("Usage: /redact [n]  — delete the last n messages (default 1)");
                return;
            }
        }

        int before = msgs.size();
        for (int i = 0; i < n && !msgs.isEmpty(); ++i)
            msgs = messagesRedactLast(msgs);
        chat["messages"] = msgs;
        chatSave(chat);

        int removed = before - msgs.size();
        outln(green(QString("✓ Redacted %1 message(s).").arg(removed)) +
              QString(" (%1 -> %2)").arg(before).arg(msgs.size()));
        if (!msgs.isEmpty()) cmdShow("3");
    }

    // List saved prompt-template Tasks (shared with the GUI's Tasks dialog).
    void cmdTasks() {
        QJsonArray tasks = tasksLoad();
        if (tasks.isEmpty()) {
            outln(dim("No tasks defined yet. Create one in the GUI's Tasks dialog "
                      "(or add one to tasks.json), then run it here with /task <n>."));
            return;
        }
        outln(bold("Tasks:"));
        for (int i = 0; i < tasks.size(); ++i) {
            QJsonObject task = tasks[i].toObject();
            QString preview = task["template"].toString().replace('\n', ' ');
            if (preview.size() > 60) preview = preview.left(57) + "...";
            outln(QString("  %1  %2  %3")
                .arg(QString::number(i + 1) + ".", -4)
                .arg(task["title"].toString(), -24)
                .arg(preview));
        }
        outln(dim("Run one with /task <n>"));
    }

    // Run a Task: fill in its %placeholders%, then send it like a normal message.
    void cmdTask(const QString& arg) {
        if (arg.isEmpty()) { cmdTasks(); return; }

        QJsonArray tasks = tasksLoad();
        bool ok; int n = arg.toInt(&ok);
        if (!ok || n < 1 || n > tasks.size()) {
            outln("Usage: /task <n>  (use /tasks to see indices)");
            return;
        }

        QJsonObject task = tasks[n - 1].toObject();
        const QString templ = task["template"].toString();
        const QStringList placeholders = extractPlaceholders(templ);
        QMap<QString, QString> values;
        for (const QString& name : placeholders) {
            const QByteArray prompt = ("  " + name + ": ").toUtf8();
            values[name] = readline_qstring(prompt.constData());
        }

        const QString rendered = renderTaskTemplate(templ, values).trimmed();
        if (rendered.isEmpty()) {
            outln(dim("This task produced an empty prompt."));
            return;
        }
        runLlm(rendered);
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
            {"/llm-timeout <n>",      "Set LLM API request timeout in seconds"},
            {"/timeout <n>",         "Set tool execution timeout in seconds"},
            {"/download-max <mb>",    "Set default download size limit in MB (0 = no limit)"},
            {"/agent [str]",         "Show or set user agent string"},
            {"/compact",             "Elide old tool results in current chat"},
            {"/redact [n]",          "Delete the last n messages (default 1) — repeatable up to the top"},
            {"/tasks",               "List saved prompt-template Tasks"},
            {"/task <n>",            "Run a Task by its /tasks index, prompting for any %placeholders%"},
            {"/list",                "List all chats"},
            {"/load <n>",            "Load chat by number"},
            {"/delete <n>",          "Delete chat by number"},
            {"/attach",              "Show file attachment help"},
            {"/quit",                "Exit"},
        };
        outln(bold("Commands:"));
        for (const auto& cmd : cmds)
            outln(QString("  %1%2").arg(QString(cmd.c).leftJustified(28), cmd.d));
        outln();
        outln(dim("  @/path/to/file  — attach file content to your message"));
        outln(dim("  {date} {username} {hostname} {osinfo}  — system message variables"));
    }

    void listChats() {
        const QJsonArray chats = chatsLoadIndex();
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
            const int msgCount = c["msg_count"].toInt();
            const QString preview = truncate(c["preview"].toString(), 30);

            outln(QString("  %1 %2 %3  %4")
                .arg((cur ? green("→") + QString::number(i + 1) : " " + QString::number(i + 1)), -4)
                .arg(truncate(c["title"].toString(), 28), -30)
                .arg(msgCount, 5)
                .arg(preview));
        }
    }

    void loadChat(int idx) {
        const QJsonArray chats = chatsLoadIndex();
        if (idx < 0 || idx >= chats.size()) { outln(red("No chat at that index.")); return; }
        const QJsonObject loaded = chatGet(chats[idx].toObject()["id"].toString());
        if (loaded.isEmpty()) { outln(red("Chat could not be loaded.")); return; }
        chat = loaded;
        int msgCount = chat["messages"].toArray().size();
        outln(dim("Loaded: " + chat["title"].toString() + " (" + QString::number(msgCount) + " messages)"));
        // Show tail for context
        cmdTail("3");
    }

    void deleteChat(int idx) {
        const QJsonArray chats = chatsLoadIndex();
        if (idx < 0 || idx >= chats.size()) { outln(red("No chat at that index.")); return; }
        const QString id    = chats[idx].toObject()["id"].toString();
        const QString title = chats[idx].toObject()["title"].toString();

        // Deletion is immediate and unrecoverable; ask first.
        out(QString("Delete \"%1\"? This cannot be undone. [y/N] ").arg(title));
        QTextStream in(stdin);
        QString answer = in.readLine().trimmed();
        if (answer.toLower() != "y" && answer.toLower() != "yes") {
            outln(dim("Cancelled."));
            return;
        }

        chatDelete(id);
        outln(dim("Deleted: " + title));
        if (id == chat["id"].toString()) {
            chat = chatCreate("New Chat");
            outln(dim("Started new chat."));
        }
    }
};

// ── Entry point ───────────────────────────────────────────────────────

static const QStringList kOutputModes = {"pretty", "raw", "json", "silent"};

/// Report a command-line usage error and exit 2, matching the other frontends.
[[noreturn]] static void argError(const QString& msg) {
    QTextStream(stderr) << "error: " << msg << "\n"
                        << "Try 'pengy-cli --help' for more information.\n";
    std::exit(2);
}

/// Consume the value following a flag, or fail if it is missing.
static QString requireValue(const QStringList& args, int& i, const QString& flag) {
    if (i + 1 >= args.size())
        argError(QString("option '%1' requires a value").arg(flag));
    return args[++i];
}

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
            outln("Usage: pengy-cli [prompt...] [OPTIONS]");
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
            outln("  --               Treat all remaining arguments as prompt text.");
            outln("  -v, --version    Show version information and exit.");
            outln("  -h, --help       Show this help message and exit.");
            return 0;
        } else if (a == "--model") {
            modelOverride = requireValue(args, i, "--model");
        } else if (a == "--system") {
            systemOverride = requireValue(args, i, "--system");
        } else if (a == "--output") {
            outputMode = requireValue(args, i, "--output");
            if (!kOutputModes.contains(outputMode))
                argError(QString("invalid --output value '%1' (expected: %2)")
                             .arg(outputMode, kOutputModes.join(", ")));
        } else if (a == "--config-dir") {
            configDir = requireValue(args, i, "--config-dir");
        } else if (a == "--") {
            // Everything after -- is prompt text, even if it looks like a flag.
            for (int j = i + 1; j < args.size(); j++)
                promptArgs.append(args[j]);
            break;
        } else if (a.startsWith('-')) {
            // Unrecognised flags used to be appended to the prompt, so a typo
            // was silently sent to the model as part of the question.
            argError(QString("unknown option '%1'").arg(a));
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
