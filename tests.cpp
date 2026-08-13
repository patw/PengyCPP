#include <QtTest>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QDir>
#include <QSysInfo>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QTcpSocket>
#include <QTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QEventLoop>

#include "config.h"
#include "chatmanager.h"
#include <QImage>
#include "tools.h"
#include "llmclient.h"
#include "web/webserver.h"
#include <QTcpServer>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

// ── Test helpers ────────────────────────────────────────────────────

static QJsonObject userMsg(const QString& content) {
    return QJsonObject{{"role", "user"}, {"content", content}};
}

static QJsonObject assistantMsg(const QString& content) {
    return QJsonObject{{"role", "assistant"}, {"content", content}};
}

static QJsonObject assistantWithTools(const QStringList& ids) {
    QJsonArray tcs;
    for (const QString& id : ids) {
        tcs.append(QJsonObject{
            {"id", id},
            {"type", "function"},
            {"function", QJsonObject{
                {"name", "test_tool"},
                {"arguments", "{}"}
            }}
        });
    }
    return QJsonObject{
        {"role", "assistant"},
        {"content", ""},
        {"tool_calls", tcs}
    };
}

static QJsonObject toolMsg(const QString& toolCallId, const QString& content) {
    return QJsonObject{
        {"role", "tool"},
        {"tool_call_id", toolCallId},
        {"content", content}
    };
}

// ── Test class ──────────────────────────────────────────────────────

// ── HTTP response helper ─────────────────────────────────────────────

struct WebResp {
    int        status = 0;
    QByteArray body;
    QString    location;
    QString    contentType;
    QString    disposition;
};

static WebResp webRequest(const QString& method, quint16 port,
                           const QString& path,
                           const QByteArray& body = {},
                           const QString& ct = {})
{
    QNetworkAccessManager mgr;
    QNetworkRequest req(QUrl("http://127.0.0.1:" + QString::number(port) + path));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);
    if (!ct.isEmpty()) req.setHeader(QNetworkRequest::ContentTypeHeader, ct);

    QEventLoop loop;
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    QNetworkReply* reply = (method == "POST") ? mgr.post(req, body) : mgr.get(req);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    WebResp r;
    r.status      = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    r.body        = reply->readAll();
    // rawHeader works for relative Location values; parsed LocationHeader may be empty
    r.location    = QString::fromUtf8(reply->rawHeader("Location"));
    r.contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
    r.disposition = QString::fromUtf8(reply->rawHeader("Content-Disposition"));
    reply->deleteLater();
    return r;
}

// ── CLI subprocess helper ────────────────────────────────────────────

static QString cliBin() {
    return QCoreApplication::applicationDirPath() + "/pengy_cli";
}

// ── Stub LLM server ──────────────────────────────────────────────────
// Replays queued /chat/completions responses and records request bodies.
// LlmClient::run()'s inner QEventLoop pumps this server's slots, so both
// can live on the test thread.

class StubLlmServer : public QObject {
public:
    QList<QByteArray>  responses;   // JSON bodies served in order
    QList<int>         statuses;    // optional per-response HTTP status
    QList<QJsonObject> requests;    // recorded request payloads

    StubLlmServer() {
        m_server.listen(QHostAddress::LocalHost, 0);
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (m_server.hasPendingConnections()) {
                QTcpSocket* sock = m_server.nextPendingConnection();
                m_bufs[sock] = QByteArray();
                connect(sock, &QTcpSocket::readyRead, this, [this, sock]() { onData(sock); });
                connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
            }
        });
    }

    QString baseUrl() const {
        return "http://127.0.0.1:" + QString::number(m_server.serverPort());
    }

private:
    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_bufs;

    void onData(QTcpSocket* sock) {
        QByteArray& buf = m_bufs[sock];
        buf += sock->readAll();
        int headerEnd = buf.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        int contentLength = 0;
        for (const QByteArray& line : buf.left(headerEnd).split('\n')) {
            if (line.toLower().trimmed().startsWith("content-length:"))
                contentLength = line.mid(line.indexOf(':') + 1).trimmed().toInt();
        }
        if (buf.size() < headerEnd + 4 + contentLength) return;

        requests.append(QJsonDocument::fromJson(
            buf.mid(headerEnd + 4, contentLength)).object());
        m_bufs[sock].clear();

        QByteArray body = responses.isEmpty()
            ? QByteArray(R"({"error": {"message": "stub exhausted"}})")
            : responses.takeFirst();
        int status = statuses.isEmpty() ? 200 : statuses.takeFirst();
        QByteArray resp = QString(
            "HTTP/1.1 %1 %2\r\nContent-Type: application/json\r\n"
            "Content-Length: %3\r\nConnection: close\r\n\r\n")
            .arg(status).arg(status == 200 ? "OK" : "Error").arg(body.size())
            .toUtf8() + body;
        sock->write(resp);
        sock->flush();
        sock->disconnectFromHost();
    }
};

static QByteArray llmCompletion(const QString& content,
                                const QJsonArray& toolCalls = {},
                                int promptToks = 10, int complToks = 5,
                                const QJsonObject& msgExtra = {}) {
    QJsonObject message{{"role", "assistant"}, {"content", content}};
    if (!toolCalls.isEmpty()) message["tool_calls"] = toolCalls;
    for (auto it = msgExtra.begin(); it != msgExtra.end(); ++it)
        message[it.key()] = it.value();
    QJsonObject payload{
        {"choices", QJsonArray{QJsonObject{
            {"index", 0}, {"message", message}, {"finish_reason", "stop"}}}},
        {"usage", QJsonObject{
            {"prompt_tokens", promptToks},
            {"completion_tokens", complToks},
            {"total_tokens", promptToks + complToks}}},
    };
    return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

static QJsonObject llmToolCall(const QString& id, const QString& name,
                               const QJsonObject& args) {
    return QJsonObject{
        {"id", id},
        {"type", "function"},
        {"function", QJsonObject{
            {"name", name},
            {"arguments", QString::fromUtf8(
                QJsonDocument(args).toJson(QJsonDocument::Compact))}}},
    };
}

static QString runCli(const QStringList& commands, int timeoutMs = 5000) {
    QProcess proc;
    proc.setProgram(cliBin());
    proc.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    proc.start();
    if (!proc.waitForStarted(2000)) return {};
    for (const QString& cmd : commands)
        proc.write((cmd + "\n").toUtf8());
    proc.closeWriteChannel(); // EOF on stdin → CLI's while(!in.atEnd()) loop exits
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(1000);
    }
    return QString::fromUtf8(proc.readAllStandardOutput());
}

// ── Test class ──────────────────────────────────────────────────────

class PengyTests : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_xdgDir; // test-isolated config directory

private slots:
    // ── Test lifecycle ───────────────────────────────────────────────

    void initTestCase() {
        QVERIFY(m_xdgDir.isValid());
        // Override XDG config home so tests don't read/write ~/.config/pengy
        qputenv("XDG_CONFIG_HOME", m_xdgDir.path().toUtf8());
        QDir(m_xdgDir.path()).mkpath("pengy");
    }

    void cleanupTestCase() {
        qunsetenv("XDG_CONFIG_HOME");
    }

    void init() {
        // Fresh config/chat state before each test. Chats live one per file in
        // pengy/chats/, so the whole directory goes; chats.json is the legacy
        // store, which is still read and so must be cleared too.
        QDir(m_xdgDir.path() + "/pengy/chats").removeRecursively();
        QFile(m_xdgDir.path() + "/pengy/chats.json").remove();
        QFile(m_xdgDir.path() + "/pengy/settings.json").remove();
    }

    // ── Config ──────────────────────────────────────────────────────

    void configDefaultValues() {
        Config c;
        QCOMPARE(c.baseUrl, "https://api.openai.com/v1");
        QCOMPARE(c.model, "gpt-4o");
        QCOMPARE(c.toolConfirmation, "none");
        QCOMPARE(c.uiScale, 100);
        QCOMPARE(c.toolTimeout, 300);
        QCOMPARE(c.llmTimeout, 300);
        QCOMPARE(c.contextKeepTurns, 0);
        QVERIFY(c.apiKey.isEmpty());
    }

    void configJsonRoundTrip() {
        Config c;
        c.baseUrl = "http://localhost:8080/v1";
        c.apiKey = "sk-test";
        c.model = "llama3";
        c.toolConfirmation = "safe";
        c.contextKeepTurns = 5;
        c.uiScale = 150;
        c.userAgent = "TestAgent/1.0";
        c.toolTimeout = 120;

        QJsonObject json = c.toJson();
        Config c2 = Config::fromJson(json);

        QCOMPARE(c2.baseUrl, c.baseUrl);
        QCOMPARE(c2.apiKey, c.apiKey);
        QCOMPARE(c2.model, c.model);
        QCOMPARE(c2.toolConfirmation, c.toolConfirmation);
        QCOMPARE(c2.contextKeepTurns, c.contextKeepTurns);
        QCOMPARE(c2.uiScale, c.uiScale);
        QCOMPARE(c2.userAgent, c.userAgent);
        QCOMPARE(c2.toolTimeout, c.toolTimeout);
    }

    void configFromJsonPartial() {
        QJsonObject json{{"api_key", "sk-test"}, {"model", "custom-model"}};
        Config c = Config::fromJson(json);
        QCOMPARE(c.apiKey, "sk-test");
        QCOMPARE(c.model, "custom-model");
        QCOMPARE(c.baseUrl, "https://api.openai.com/v1");
        QCOMPARE(c.toolConfirmation, "none");
        QCOMPARE(c.uiScale, 100);
        QCOMPARE(c.toolTimeout, 300);
        QCOMPARE(c.llmTimeout, 300);
    }

    void configFromJsonEmpty() {
        Config c = Config::fromJson(QJsonObject());
        Config d;
        QCOMPARE(c.baseUrl, d.baseUrl);
        QCOMPARE(c.model, d.model);
        QCOMPARE(c.toolConfirmation, d.toolConfirmation);
        QCOMPARE(c.uiScale, d.uiScale);
    }

    void configToJsonHasAllFields() {
        Config c;
        QJsonObject json = c.toJson();
        QVERIFY(json.contains("base_url"));
        QVERIFY(json.contains("api_key"));
        QVERIFY(json.contains("model"));
        QVERIFY(json.contains("system_message"));
        QVERIFY(json.contains("tool_confirmation"));
        QVERIFY(json.contains("context_keep_turns"));
        QVERIFY(json.contains("ui_scale"));
        QVERIFY(json.contains("user_agent"));
        QVERIFY(json.contains("tool_timeout"));
    }

    void configRenderReplacesAllPlaceholders() {
        QString tmpl = "Date: {date}, User: {username}, Host: {hostname}, OS: {osinfo}";
        QString result = configRenderSystemMessage(tmpl);
        QVERIFY(!result.contains("{date}"));
        QVERIFY(!result.contains("{username}"));
        QVERIFY(!result.contains("{hostname}"));
        QVERIFY(!result.contains("{osinfo}"));
    }

    void configRenderNoPlaceholders() {
        QCOMPARE(configRenderSystemMessage("Hello, world!"), "Hello, world!");
    }

    void configRenderEmpty() {
        QCOMPARE(configRenderSystemMessage(""), "");
    }

    void configRenderContainsOsInfo() {
        QString result = configRenderSystemMessage("{osinfo}");
        QVERIFY(result.contains(QSysInfo::currentCpuArchitecture()));
    }

    void configFileSaveAndLoad() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString path = dir.path() + "/settings.json";
        Config c;
        c.baseUrl = "http://test:1234/v1";
        c.apiKey = "sk-round-trip";
        c.model = "test-model";
        QByteArray json = QJsonDocument(c.toJson()).toJson(QJsonDocument::Indented);
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(json);
        f.close();

        QFile f2(path);
        QVERIFY(f2.open(QIODevice::ReadOnly));
        Config c2 = Config::fromJson(QJsonDocument::fromJson(f2.readAll()).object());
        QCOMPARE(c2.baseUrl, "http://test:1234/v1");
        QCOMPARE(c2.apiKey, "sk-round-trip");
        QCOMPARE(c2.model, "test-model");
    }

    // ── ChatManager: cleanDanglingToolCalls ─────────────────────────

    void cleanNoToolCallsUnchanged() {
        QJsonArray msgs{userMsg("hi"), assistantMsg("hello")};
        QJsonArray cleaned = cleanDanglingToolCalls(msgs);
        QCOMPARE(cleaned.size(), 2);
    }

    void cleanCompleteToolCallUnchanged() {
        QJsonArray msgs{
            userMsg("do something"),
            assistantWithTools({"tc-1"}),
            toolMsg("tc-1", "result"),
            assistantMsg("done")
        };
        QJsonArray cleaned = cleanDanglingToolCalls(msgs);
        QCOMPARE(cleaned.size(), 4);
        QCOMPARE(cleaned[2].toObject()["role"].toString(), "tool");
        QCOMPARE(cleaned[2].toObject()["content"].toString(), "result");
    }

    void cleanDanglingSynthesizesCancelled() {
        QJsonArray msgs{
            userMsg("do something"),
            assistantWithTools({"tc-1"}),
            userMsg("next question")
        };
        QJsonArray cleaned = cleanDanglingToolCalls(msgs);
        QCOMPARE(cleaned.size(), 4);
        QCOMPARE(cleaned[2].toObject()["role"].toString(), "tool");
        QCOMPARE(cleaned[2].toObject()["tool_call_id"].toString(), "tc-1");
        QVERIFY(cleaned[2].toObject()["content"].toString().contains("cancelled"));
    }

    void cleanOrphanToolMessageDropped() {
        QJsonArray msgs{
            userMsg("hi"),
            toolMsg("orphan-id", "stale result"),
            assistantMsg("hello")
        };
        QJsonArray cleaned = cleanDanglingToolCalls(msgs);
        QCOMPARE(cleaned.size(), 2);
        QCOMPARE(cleaned[0].toObject()["role"].toString(), "user");
        QCOMPARE(cleaned[1].toObject()["role"].toString(), "assistant");
    }

    void cleanMultipleToolCallsPartialResults() {
        QJsonArray msgs{
            userMsg("do two things"),
            assistantWithTools({"tc-1", "tc-2"}),
            toolMsg("tc-1", "result 1")
        };
        QJsonArray cleaned = cleanDanglingToolCalls(msgs);
        QCOMPARE(cleaned.size(), 4);
        QCOMPARE(cleaned[2].toObject()["tool_call_id"].toString(), "tc-1");
        QCOMPARE(cleaned[3].toObject()["role"].toString(), "tool");
        QCOMPARE(cleaned[3].toObject()["tool_call_id"].toString(), "tc-2");
        QVERIFY(cleaned[3].toObject()["content"].toString().contains("cancelled"));
    }

    void cleanMultipleToolCallsAllSatisfied() {
        QJsonArray msgs{
            assistantWithTools({"tc-1", "tc-2", "tc-3"}),
            toolMsg("tc-1", "r1"),
            toolMsg("tc-2", "r2"),
            toolMsg("tc-3", "r3")
        };
        QJsonArray cleaned = cleanDanglingToolCalls(msgs);
        QCOMPARE(cleaned.size(), 4);
    }

    void cleanEmptyMessages() {
        QJsonArray cleaned = cleanDanglingToolCalls(QJsonArray());
        QVERIFY(cleaned.isEmpty());
    }

    // ── ChatManager: elideOldToolResults ────────────────────────────

    void elideKeepZeroReturnsAll() {
        QJsonArray msgs{
            userMsg("q1"),
            assistantWithTools({"tc-1"}),
            toolMsg("tc-1", "long result data"),
            assistantMsg("done")
        };
        QJsonArray elided = elideOldToolResults(msgs, 0);
        QCOMPARE(elided.size(), msgs.size());
        QCOMPARE(elided[2].toObject()["content"].toString(), "long result data");
    }

    void elideKeepsRecentTurnIntact() {
        QJsonArray msgs{
            userMsg("old question"),
            assistantWithTools({"tc-old"}),
            toolMsg("tc-old", "old tool output"),
            assistantMsg("old answer"),
            userMsg("new question"),
            assistantWithTools({"tc-new"}),
            toolMsg("tc-new", "new tool output"),
            assistantMsg("new answer")
        };
        QJsonArray elided = elideOldToolResults(msgs, 1);
        QVERIFY(elided[2].toObject()["content"].toString().contains("elided"));
        QCOMPARE(elided[6].toObject()["content"].toString(), "new tool output");
    }

    void elideNoUserMessagesReturnsAll() {
        QJsonArray msgs{assistantMsg("system init")};
        QJsonArray elided = elideOldToolResults(msgs, 1);
        QCOMPARE(elided.size(), 1);
    }

    void elideKeepAllTurns() {
        QJsonArray msgs{
            userMsg("q1"), toolMsg("tc-1", "result 1"),
            userMsg("q2"), toolMsg("tc-2", "result 2")
        };
        QJsonArray elided = elideOldToolResults(msgs, 10);
        QCOMPARE(elided[1].toObject()["content"].toString(), "result 1");
        QCOMPARE(elided[3].toObject()["content"].toString(), "result 2");
    }

    void elideNonToolNeverModified() {
        QJsonArray msgs{
            userMsg("old"), assistantMsg("old answer"),
            userMsg("new"), assistantMsg("new answer")
        };
        QJsonArray elided = elideOldToolResults(msgs, 1);
        QCOMPARE(elided[1].toObject()["content"].toString(), "old answer");
    }

    // ── Tools: classification ───────────────────────────────────────

    void readonlyToolsCorrect() {
        QVERIFY(Tools::isReadOnly("read_file"));
        QVERIFY(Tools::isReadOnly("read_image"));
        QVERIFY(Tools::isReadOnly("read_multiple_files"));
        QVERIFY(Tools::isReadOnly("directory_tree"));
        QVERIFY(Tools::isReadOnly("search_content"));
        QVERIFY(Tools::isReadOnly("web_search"));
        QVERIFY(Tools::isReadOnly("fetch_url"));
    }

    void applyChangesIsRegisteredAndWriteOnly() {
        bool found = false;
        for (const auto& v : Tools::toolDefinitions()) {
            if (v.toObject()["function"].toObject()["name"].toString() == "apply_changes") found = true;
        }
        QVERIFY(found);
        QVERIFY(!Tools::isReadOnly("apply_changes"));
    }

    void writeToolsNotReadonly() {
        QVERIFY(!Tools::isReadOnly("write_file"));
        QVERIFY(!Tools::isReadOnly("replace_in_file"));
        QVERIFY(!Tools::isReadOnly("run_bash"));
        QVERIFY(!Tools::isReadOnly("run_python"));
        QVERIFY(!Tools::isReadOnly("download_file"));
    }

    void unknownToolNotReadonly() {
        QVERIFY(!Tools::isReadOnly("nonexistent_tool"));
        QVERIFY(!Tools::isReadOnly(""));
    }

    // ── Tools: definitions ──────────────────────────────────────────

    void toolDefinitionsHasSixteen() {
        QCOMPARE(Tools::toolDefinitions().size(), 16);
    }

    void toolDefinitionsAllFunctionType() {
        for (const QJsonValue& v : Tools::toolDefinitions()) {
            QCOMPARE(v.toObject()["type"].toString(), "function");
        }
    }

    void toolDefinitionsUniqueNames() {
        QSet<QString> names;
        for (const QJsonValue& v : Tools::toolDefinitions()) {
            names.insert(v.toObject()["function"].toObject()["name"].toString());
        }
        QCOMPARE(names.size(), 16);
    }

    void toolDefinitionsAllHaveRequired() {
        for (const QJsonValue& v : Tools::toolDefinitions()) {
            QJsonObject fn = v.toObject()["function"].toObject();
            QVERIFY(!fn["name"].toString().isEmpty());
            QVERIFY(!fn["description"].toString().isEmpty());
            QJsonObject params = fn["parameters"].toObject();
            QCOMPARE(params["type"].toString(), "object");
            QVERIFY(!params["required"].toArray().isEmpty());
        }
    }

    void toolDefinitionsSerializesToJson() {
        QJsonArray defs = Tools::toolDefinitions();
        QByteArray json = QJsonDocument(defs).toJson();
        QJsonDocument parsed = QJsonDocument::fromJson(json);
        QVERIFY(parsed.isArray());
        QCOMPARE(parsed.array().size(), 16);
    }

    // ── Tools: read_file ────────────────────────────────────────────

    void readFileExisting() {
        QTemporaryDir dir;
        QString path = dir.path() + "/test.txt";
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write("hello world");
        f.close();

        QString result = Tools::execute("read_file", QJsonObject{{"path", path}});
        QCOMPARE(result, "hello world");
    }

    void readFileNotFound() {
        QString result = Tools::execute("read_file",
            QJsonObject{{"path", "/tmp/pengy_nonexistent_file_12345.txt"}});
        QVERIFY(result.contains("not found") || result.contains("Not found"));
    }

    // offset/limit page through a file; the header states where you are so the
    // model can request the next page.  Mirrors Python's
    // test_read_file_offset_and_limit and Rust's read_file_offset_and_limit.
    void readFileOffsetAndLimit() {
        QTemporaryDir dir;
        QString path = dir.path() + "/many.txt";
        QStringList body;
        for (int i = 1; i <= 20; ++i) body << QString("line %1").arg(i);
        { QFile f(path); f.open(QIODevice::WriteOnly);
          f.write(body.join("\n").toUtf8()); }

        auto call = [&](const QJsonObject& extra) {
            QJsonObject a{{"path", path}};
            for (auto it = extra.begin(); it != extra.end(); ++it) a[it.key()] = it.value();
            return Tools::execute("read_file", a);
        };

        QStringList ranged = call({{"offset", 5}, {"limit", 3}}).split('\n');
        QCOMPARE(ranged[0], QString("[Lines 5-7 of 20 in %1]").arg(path));
        QCOMPARE(ranged.mid(1), QStringList({"line 5", "line 6", "line 7"}));

        // limit alone starts at line 1; offset alone runs to the end.
        QCOMPARE(call({{"limit", 2}}).split('\n').mid(1),
                 QStringList({"line 1", "line 2"}));
        QCOMPARE(call({{"offset", 19}}).split('\n').mid(1),
                 QStringList({"line 19", "line 20"}));

        // A limit past the end clamps instead of erroring.
        QCOMPARE(call({{"offset", 19}, {"limit", 100}}).split('\n')[0],
                 QString("[Lines 19-20 of 20 in %1]").arg(path));

        // No offset/limit keeps the plain whole-file behaviour (no header).
        QVERIFY(call({}).startsWith("line 1"));
    }

    // Files truncate from the head, not the middle: the head holds imports and
    // declarations, and unlike a log the rest can be paged to.  Mirrors Python's
    // test_read_file_truncates_from_head_with_continuation.
    void readFileTruncatesFromHeadWithContinuation() {
        QTemporaryDir dir;
        QString path = dir.path() + "/big.txt";
        QStringList body;
        for (int i = 1; i <= 5000; ++i) body << QString("line %1").arg(i);
        { QFile f(path); f.open(QIODevice::WriteOnly); f.write(body.join("\n").toUtf8()); }

        Tools::setToolOutputMaxChars(2000);
        QString result = Tools::execute("read_file", QJsonObject{{"path", path}});
        Tools::setToolOutputMaxChars(250000);

        QStringList lines = result.split('\n');
        QString header = lines.takeFirst();

        // No middle gap — content runs contiguously from line 1.
        QVERIFY2(!result.contains("snipped"), qPrintable(header));
        QCOMPARE(lines.first(), QString("line 1"));
        QVERIFY2(header.contains("of 5000 in"), qPrintable(header));
        QVERIFY2(header.contains("output limit reached"), qPrintable(header));

        // The stated continuation offset is the next unseen line.
        int lastShown = lines.last().split(' ').at(1).toInt();
        int offset = header.section("offset=", 1).section(' ', 0, 0).toInt();
        QCOMPARE(offset, lastShown + 1);
    }

    // run_bash output keeps head+tail: the command echo is at the start and the
    // error that matters is usually at the end.
    void commandOutputStaysTailBiased() {
        QStringList body;
        for (int i = 1; i <= 5000; ++i) body << QString("line %1").arg(i);
        QString text = body.join("\n");

        Tools::setToolOutputMaxChars(2000);
        QString out = Tools::snipMiddleForTest(text);
        Tools::setToolOutputMaxChars(250000);

        QVERIFY(out.startsWith("line 1"));
        QVERIFY(out.trimmed().endsWith("line 5000"));
        QVERIFY(out.contains("snipped"));
    }

    // Character-index cuts left a broken half-line at each seam, which on source
    // code is a fragment the model may try to "fix".
    void truncationNeverSplitsALine() {
        QStringList body;
        for (int i = 1; i <= 5000; ++i) body << QString("line %1").arg(i);
        QString text = body.join("\n");

        Tools::setToolOutputMaxChars(2000);
        QString out = Tools::snipMiddleForTest(text);
        Tools::setToolOutputMaxChars(250000);

        QString head = out.section("[... snipped", 0, 0);
        QString tail = out.section("]", 1);
        QRegularExpression whole("^line \\d+$");
        for (const QString& frag : (head.trimmed().split('\n') + tail.trimmed().split('\n')))
            QVERIFY2(whole.match(frag).hasMatch(), qPrintable("broken seam: " + frag));
    }

    void readFileOffsetPastEndErrors() {
        QTemporaryDir dir;
        QString path = dir.path() + "/short.txt";
        { QFile f(path); f.open(QIODevice::WriteOnly); f.write("a\nb\nc"); }
        QString result = Tools::execute("read_file",
            QJsonObject{{"path", path}, {"offset", 99}});
        QVERIFY(result.contains("Error"));
        QVERIFY(result.contains("3 lines"));
    }

    // ── Tools: write_file ───────────────────────────────────────────

    void writeFileCreatesAndWrites() {
        QTemporaryDir dir;
        QString path = dir.path() + "/output.txt";
        QString result = Tools::execute("write_file",
            QJsonObject{{"path", path}, {"content", "content"}});
        QVERIFY(result.contains("Successfully"));

        QFile f(path);
        f.open(QIODevice::ReadOnly);
        QCOMPARE(QString::fromUtf8(f.readAll()), "content");
    }

    void writeFileCreatesParentDirs() {
        QTemporaryDir dir;
        QString path = dir.path() + "/a/b/c/file.txt";
        QString result = Tools::execute("write_file",
            QJsonObject{{"path", path}, {"content", "nested"}});
        QVERIFY(result.contains("Successfully"));

        QFile f(path);
        f.open(QIODevice::ReadOnly);
        QCOMPARE(QString::fromUtf8(f.readAll()), "nested");
    }

    // ── Tools: replace_in_file ──────────────────────────────────────

    void replaceInFileSingleMatch() {
        QTemporaryDir dir;
        QString path = dir.path() + "/replace.txt";
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write("hello world foo bar");
        f.close();

        QString result = Tools::execute("replace_in_file",
            QJsonObject{{"path", path}, {"old_str", "world"}, {"new_str", "universe"}});
        QVERIFY(result.contains("Successfully"));

        QFile f2(path);
        f2.open(QIODevice::ReadOnly);
        QCOMPARE(QString::fromUtf8(f2.readAll()), "hello universe foo bar");
    }

    void replaceInFileNoMatch() {
        QTemporaryDir dir;
        QString path = dir.path() + "/replace.txt";
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write("hello world");
        f.close();

        QString result = Tools::execute("replace_in_file",
            QJsonObject{{"path", path}, {"old_str", "nonexistent"}, {"new_str", "x"}});
        QVERIFY(result.contains("not found"));
    }

    void replaceInFileMultipleMatches() {
        QTemporaryDir dir;
        QString path = dir.path() + "/replace.txt";
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write("aaa bbb aaa");
        f.close();

        QString result = Tools::execute("replace_in_file",
            QJsonObject{{"path", path}, {"old_str", "aaa"}, {"new_str", "x"}});
        QVERIFY(result.contains("matches 2 locations"));
    }

    void replaceInFileEmptyOldStr() {
        QString result = Tools::execute("replace_in_file",
            QJsonObject{{"path", "/tmp/x"}, {"old_str", ""}, {"new_str", "y"}});
        QVERIFY(result.contains("old_str is empty"));
    }

    void replaceInFileNotFound() {
        QString result = Tools::execute("replace_in_file",
            QJsonObject{{"path", "/tmp/pengy_nonexistent_12345.txt"},
                        {"old_str", "x"}, {"new_str", "y"}});
        QVERIFY(result.contains("not found") || result.contains("Not found"));
    }

    // ── Tools: apply_changes ────────────────────────────────────────

    void applyChangesReplacesMultipleFiles() {
        QTemporaryDir dir;
        QString a = dir.path() + "/a.txt", b = dir.path() + "/b.txt";
        { QFile f(a); f.open(QIODevice::WriteOnly); f.write("alpha\\nbeta\\n"); }
        { QFile f(b); f.open(QIODevice::WriteOnly); f.write("one\\ntwo\\n"); }
        QString result = Tools::execute("apply_changes", QJsonObject{
            {"changes", QJsonArray{
                QJsonObject{{"path",a},{"operations",QJsonArray{QJsonObject{{"kind","replace"},{"old","beta"},{"new","BETA"}}}}},
                QJsonObject{{"path",b},{"operations",QJsonArray{QJsonObject{{"kind","delete"},{"old","two"}}}}}
            }},
            {"postconditions", QJsonArray{QJsonObject{{"path",a},{"contains","BETA"}}}}
        });
        QVERIFY(result.contains("Applied changes"));
        QFile fa(a); QVERIFY(fa.open(QIODevice::ReadOnly)); QVERIFY(QString::fromUtf8(fa.readAll()).contains("BETA"));
        QFile fb(b); QVERIFY(fb.open(QIODevice::ReadOnly)); QVERIFY(!QString::fromUtf8(fb.readAll()).contains("two"));
    }

    void applyChangesDryRunAndAtomicFailure() {
        QTemporaryDir dir; QString a=dir.path()+"/a.txt", b=dir.path()+"/b.txt";
        { QFile f(a); f.open(QIODevice::WriteOnly); f.write("duplicate\\nduplicate\\n"); }
        { QFile f(b); f.open(QIODevice::WriteOnly); f.write("unchanged"); }
        QString result=Tools::execute("apply_changes", QJsonObject{{"changes",QJsonArray{
            QJsonObject{{"path",a},{"operations",QJsonArray{QJsonObject{{"kind","replace"},{"old","duplicate"},{"new","x"}}}}},
            QJsonObject{{"path",b},{"operations",QJsonArray{QJsonObject{{"kind","replace"},{"old","unchanged"},{"new","changed"}}}}}
        }}});
        QVERIFY(result.contains("no changes applied"));
        QFile fb(b); QVERIFY(fb.open(QIODevice::ReadOnly)); QCOMPARE(fb.readAll(), QByteArray("unchanged"));
        result=Tools::execute("apply_changes", QJsonObject{{"changes",QJsonArray{QJsonObject{{"path",b},{"operations",QJsonArray{QJsonObject{{"kind","replace"},{"old","unchanged"},{"new","changed"}}}}}}},{"dry_run",true}});
        QVERIFY(result.contains("Dry run")); QFile fb2(b); QVERIFY(fb2.open(QIODevice::ReadOnly)); QCOMPARE(fb2.readAll(), QByteArray("unchanged"));
    }

    // ── Tools: directory_tree ───────────────────────────────────────

    void directoryTreeBasic() {
        QTemporaryDir dir;
        QFile(dir.path() + "/file.txt").open(QIODevice::WriteOnly);
        QDir(dir.path()).mkdir("subdir");
        QFile(dir.path() + "/subdir/nested.txt").open(QIODevice::WriteOnly);

        QString result = Tools::execute("directory_tree",
            QJsonObject{{"path", dir.path()}});
        QVERIFY(result.contains("subdir/"));
        QVERIFY(result.contains("file.txt"));
        QVERIFY(result.contains("nested.txt"));
    }

    void directoryTreeNotFound() {
        QString result = Tools::execute("directory_tree",
            QJsonObject{{"path", "/tmp/pengy_nonexistent_dir_12345"}});
        QVERIFY(result.contains("not found") || result.contains("Not found"));
    }

    void directoryTreeHidesHiddenByDefault() {
        QTemporaryDir dir;
        { QFile f(dir.path() + "/.hidden"); f.open(QIODevice::WriteOnly); f.write("secret"); }
        { QFile f(dir.path() + "/visible.txt"); f.open(QIODevice::WriteOnly); f.write("public"); }

        QString result = Tools::execute("directory_tree",
            QJsonObject{{"path", dir.path()}});
        QVERIFY(!result.contains(".hidden"));
        QVERIFY(result.contains("visible.txt"));
    }

    void directoryTreeShowsHiddenWhenRequested() {
        QTemporaryDir dir;
        { QFile f(dir.path() + "/.hidden"); f.open(QIODevice::WriteOnly); f.write("secret"); }

        QString result = Tools::execute("directory_tree",
            QJsonObject{{"path", dir.path()}, {"show_hidden", true}});
        QVERIFY(result.contains(".hidden"));
    }

    // ── Tools: read_multiple_files ──────────────────────────────────

    void readMultipleFilesBasic() {
        QTemporaryDir dir;
        QString p1 = dir.path() + "/a.txt";
        QString p2 = dir.path() + "/b.txt";
        { QFile f(p1); f.open(QIODevice::WriteOnly); f.write("content a"); }
        { QFile f(p2); f.open(QIODevice::WriteOnly); f.write("content b"); }

        QString result = Tools::execute("read_multiple_files",
            QJsonObject{{"paths", QJsonArray{p1, p2}}});
        QVERIFY(result.contains("content a"));
        QVERIFY(result.contains("content b"));
    }

    void readMultipleFilesEmpty() {
        QString result = Tools::execute("read_multiple_files",
            QJsonObject{{"paths", QJsonArray()}});
        QVERIFY(result.contains("no paths"));
    }

    void readMultipleFilesTooMany() {
        QJsonArray paths;
        for (int i = 0; i < 25; ++i)
            paths.append(QString("/tmp/file_%1.txt").arg(i));
        QString result = Tools::execute("read_multiple_files",
            QJsonObject{{"paths", paths}});
        QVERIFY(result.contains("too many"));
    }

    // ── Tools: search_content ───────────────────────────────────────

    void searchContentFindsMatches() {
        QTemporaryDir dir;
        QString path = dir.path() + "/test.rs";
        { QFile f(path); f.open(QIODevice::WriteOnly); f.write("fn main() {\n    println!(\"hello\");\n}\n"); }

        QString result = Tools::execute("search_content",
            QJsonObject{{"pattern", "println"}, {"path", path}});
        QVERIFY(result.contains("println"));
    }

    void searchContentNoMatches() {
        QTemporaryDir dir;
        QString path = dir.path() + "/test.rs";
        { QFile f(path); f.open(QIODevice::WriteOnly); f.write("fn main() {}"); }

        QString result = Tools::execute("search_content",
            QJsonObject{{"pattern", "nonexistent_pattern"}, {"path", path}});
        QVERIFY(result.contains("No matches"));
    }

    void searchContentPathNotFound() {
        QString result = Tools::execute("search_content",
            QJsonObject{{"pattern", "test"},
                        {"path", "/tmp/pengy_nonexistent_12345"}});
        QVERIFY(result.contains("not found") || result.contains("Not found"));
    }

    void searchContentLiteralByDefault() {
        QTemporaryDir dir;
        QString path = dir.path() + "/search.rs";
        { QFile f(path); f.open(QIODevice::WriteOnly); f.write("value = 'a.b'\nvalue2 = 'axb'\n"); }

        // Default: literal match — "a.b" must not match "axb".
        QString literal = Tools::execute("search_content",
            QJsonObject{{"pattern", "a.b"}, {"path", path}});
        QVERIFY(literal.contains("a.b"));
        QVERIFY(!literal.contains("axb"));

        // regex=true: '.' is a wildcard, so it should match "axb" too.
        QString regex = Tools::execute("search_content",
            QJsonObject{{"pattern", "a.b"}, {"path", path}, {"regex", true}});
        QVERIFY(regex.contains("axb"));
    }

    // ── Tools: unknown tool ─────────────────────────────────────────

    void executeUnknownTool() {
        QString result = Tools::execute("nonexistent_tool", QJsonObject());
        QVERIFY(result.contains("Unknown tool"));
    }

    // ── Tools: dispatch via execute ─────────────────────────────────

    void executeDispatchesReadFile() {
        QTemporaryDir dir;
        QString path = dir.path() + "/dispatch_test.txt";
        { QFile f(path); f.open(QIODevice::WriteOnly); f.write("dispatch content"); }

        QString result = Tools::execute("read_file", QJsonObject{{"path", path}});
        QCOMPARE(result, "dispatch content");
    }

    // ── Tools: glob ─────────────────────────────────────────────────

    void globFindsPyFiles() {
        QTemporaryDir dir;
        { QFile f(dir.path() + "/a.py"); f.open(QIODevice::WriteOnly); f.write("x"); }
        { QFile f(dir.path() + "/b.rs"); f.open(QIODevice::WriteOnly); f.write("y"); }
        QDir(dir.path()).mkdir("sub");
        { QFile f(dir.path() + "/sub/c.py"); f.open(QIODevice::WriteOnly); f.write("z"); }

        QString result = Tools::execute("glob",
            QJsonObject{{"pattern", "**/*.py"}, {"path", dir.path()}});
        QVERIFY(result.contains("a.py"));
        QVERIFY(result.contains("sub/c.py"));
        QVERIFY(!result.contains("b.rs"));
    }

    void globNoMatches() {
        QTemporaryDir dir;
        QString result = Tools::execute("glob",
            QJsonObject{{"pattern", "*.xyz"}, {"path", dir.path()}});
        QVERIFY(result.contains("No files matching"));
    }

    void globSkipsHiddenByDefault() {
        QTemporaryDir dir;
        { QFile f(dir.path() + "/.hidden.py"); f.open(QIODevice::WriteOnly); f.write("x"); }
        { QFile f(dir.path() + "/visible.py"); f.open(QIODevice::WriteOnly); f.write("y"); }

        QString result = Tools::execute("glob",
            QJsonObject{{"pattern", "*.py"}, {"path", dir.path()}});
        QVERIFY(result.contains("visible.py"));
        QVERIFY(!result.contains(".hidden.py"));
    }

    // ".env" is in the skip set as a virtualenv *directory* name; matching it
    // against files made the common .env *file* unfindable.
    void globFindsDotenvFile() {
        QTemporaryDir dir;
        { QFile f(dir.path() + "/.env"); f.open(QIODevice::WriteOnly); f.write("SECRET=1"); }

        QString result = Tools::execute("glob",
            QJsonObject{{"pattern", ".env"}, {"path", dir.path()}});
        QVERIFY2(result.contains(".env"), qPrintable(result));
        QVERIFY(!result.contains("No files matching"));
    }

    // The virtualenv case the skip entry exists for must keep working.
    void globStillSkipsDotenvDirectoryContents() {
        QTemporaryDir dir;
        QDir(dir.path()).mkdir(".env");
        { QFile f(dir.path() + "/.env/pyvenv.py"); f.open(QIODevice::WriteOnly); f.write("x"); }
        { QFile f(dir.path() + "/real.py"); f.open(QIODevice::WriteOnly); f.write("y"); }

        QString result = Tools::execute("glob",
            QJsonObject{{"pattern", "**/*.py"}, {"path", dir.path()}});
        QVERIFY2(result.contains("real.py"), qPrintable(result));
        QVERIFY2(!result.contains("pyvenv.py"), qPrintable(result));
    }

    // A pattern whose final component starts with "." wants hidden entries,
    // even when the pattern as a whole starts with "*".
    void globFindsHiddenWhenPatternAsks() {
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("sub");
        { QFile f(dir.path() + "/sub/.config"); f.open(QIODevice::WriteOnly); f.write("x"); }

        QString result = Tools::execute("glob",
            QJsonObject{{"pattern", "**/.config"}, {"path", dir.path()}});
        QVERIFY2(result.contains(".config"), qPrintable(result));
    }

    void globSkipsBuildDirContents() {
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("build");
        { QFile f(dir.path() + "/build/out.py"); f.open(QIODevice::WriteOnly); f.write("x"); }
        { QFile f(dir.path() + "/app.py"); f.open(QIODevice::WriteOnly); f.write("y"); }

        QVERIFY(!Tools::execute("glob",
            QJsonObject{{"pattern", "**/*.py"}, {"path", dir.path()}}).contains("out.py"));
        QVERIFY(!Tools::execute("glob",
            QJsonObject{{"pattern", "*"}, {"path", dir.path()}}).contains("build"));
    }

    // The model picks the download name and may be acting on a fetched page's
    // instructions, so a path component must never leave ~/Downloads.
    void downloadFilenameCannotEscapeDownloads() {
        QCOMPARE(Tools::safeDownloadNameForTest("../../.bashrc"), QString(".bashrc"));
        QCOMPARE(Tools::safeDownloadNameForTest("/etc/passwd"), QString("passwd"));
        QCOMPARE(Tools::safeDownloadNameForTest("a/b/c.txt"), QString("c.txt"));
        QCOMPARE(Tools::safeDownloadNameForTest("..\\..\\evil.exe"), QString("evil.exe"));
        QCOMPARE(Tools::safeDownloadNameForTest(".."), QString("download"));
        QCOMPARE(Tools::safeDownloadNameForTest("."), QString("download"));
        QCOMPARE(Tools::safeDownloadNameForTest(""), QString("download"));
        QCOMPARE(Tools::safeDownloadNameForTest("subdir/"), QString("download"));
        QCOMPARE(Tools::safeDownloadNameForTest("report.pdf"), QString("report.pdf"));
    }

    void globSkipsNodeModules() {
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("node_modules");
        { QFile f(dir.path() + "/node_modules/foo.js"); f.open(QIODevice::WriteOnly); f.write("x"); }
        { QFile f(dir.path() + "/src.js"); f.open(QIODevice::WriteOnly); f.write("y"); }

        QString result = Tools::execute("glob",
            QJsonObject{{"pattern", "**/*.js"}, {"path", dir.path()}});
        QVERIFY(result.contains("src.js"));
        QVERIFY(!result.contains("node_modules"));
    }

    void globDirectoriesShowSlash() {
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("mydir");

        QString result = Tools::execute("glob",
            QJsonObject{{"pattern", "*"}, {"path", dir.path()}});
        QVERIFY(result.contains("mydir/"));
    }

    void globDirPrefixInPattern() {
        QTemporaryDir dir;
        { QFile f(dir.path() + "/a.py"); f.open(QIODevice::WriteOnly); f.write("x"); }
        { QFile f(dir.path() + "/b.rs"); f.open(QIODevice::WriteOnly); f.write("y"); }

        QString pattern = dir.path() + QString("/*.py");
        QString result = Tools::execute("glob", QJsonObject{{"pattern", pattern}});
        QVERIFY(result.contains("a.py"));
        QVERIFY(!result.contains("b.rs"));
    }

    void globExactFileInPattern() {
        QTemporaryDir dir;
        { QFile f(dir.path() + "/target.rs"); f.open(QIODevice::WriteOnly); f.write("content"); }

        QString pattern = dir.path() + QString("/target.rs");
        QString result = Tools::execute("glob", QJsonObject{{"pattern", pattern}});
        QVERIFY(result.contains("target.rs"));
    }

    void globDirPrefixWithRecursive() {
        QTemporaryDir dir;
        { QFile f(dir.path() + "/a.py"); f.open(QIODevice::WriteOnly); f.write("x"); }
        QDir(dir.path()).mkdir("sub");
        { QFile f(dir.path() + "/sub/b.py"); f.open(QIODevice::WriteOnly); f.write("y"); }

        QString pattern = dir.path() + QString("/**/*.py");
        QString result = Tools::execute("glob", QJsonObject{{"pattern", pattern}});
        QVERIFY(result.contains("a.py"));
        QVERIFY(result.contains("sub/b.py"));
    }

    // ── Tools: todowrite ────────────────────────────────────────────

    void todowriteEchoesBackValidTodos() {
        QJsonArray todos;
        todos.append(QJsonObject{{"content", "Find auth code"}, {"status", "in_progress"}});
        todos.append(QJsonObject{{"content", "Add JWT"}, {"status", "pending"}});
        todos.append(QJsonObject{{"content", "Write tests"}, {"status", "pending"}});

        QString result = Tools::execute("todowrite", QJsonObject{{"todos", todos}});
        QVERIFY(result.contains("[→] Find auth code"));
        QVERIFY(result.contains("[ ] Add JWT"));
        QVERIFY(result.contains("[ ] Write tests"));
    }

    void todowriteRejectsMultipleInProgress() {
        QJsonArray todos;
        todos.append(QJsonObject{{"content", "Task A"}, {"status", "in_progress"}});
        todos.append(QJsonObject{{"content", "Task B"}, {"status", "in_progress"}});

        QString result = Tools::execute("todowrite", QJsonObject{{"todos", todos}});
        QVERIFY(result.contains("Error"));
        QVERIFY(result.contains("in_progress"));
    }

    void todowriteRejectsInvalidStatus() {
        QJsonArray todos;
        todos.append(QJsonObject{{"content", "Task A"}, {"status", "done"}});

        QString result = Tools::execute("todowrite", QJsonObject{{"todos", todos}});
        QVERIFY(result.contains("invalid status"));
    }

    void todowriteRejectsEmptyContent() {
        QJsonArray todos;
        todos.append(QJsonObject{{"content", ""}, {"status", "pending"}});

        QString result = Tools::execute("todowrite", QJsonObject{{"todos", todos}});
        QVERIFY(result.contains("content is empty"));
    }

    void todowriteRejectsEmptyList() {
        QString result = Tools::execute("todowrite",
            QJsonObject{{"todos", QJsonArray()}});
        QVERIFY(result.contains("empty"));
    }

    void todowriteAllPendingIsValid() {
        QJsonArray todos;
        todos.append(QJsonObject{{"content", "Task A"}, {"status", "pending"}});
        todos.append(QJsonObject{{"content", "Task B"}, {"status", "pending"}});

        QString result = Tools::execute("todowrite", QJsonObject{{"todos", todos}});
        QVERIFY(!result.contains("Error"));
    }

    void todowriteAllowsAllCompleted() {
        QJsonArray todos;
        todos.append(QJsonObject{{"content", "Task A"}, {"status", "completed"}});
        todos.append(QJsonObject{{"content", "Task B"}, {"status", "completed"}});

        QString result = Tools::execute("todowrite", QJsonObject{{"todos", todos}});
        QVERIFY(result.contains("[✓]"));
    }

    // ── Tools: ask_user_question ────────────────────────────────────

    void askUserQuestionDefinitionExists() {
        bool found = false;
        for (const QJsonValue& v : Tools::toolDefinitions()) {
            if (v.toObject()["function"].toObject()["name"].toString() == "ask_user_question")
                found = true;
        }
        QVERIFY(found);
    }

    void askUserQuestionExecuteReturnsHarnessMessage() {
        QString result = Tools::execute("ask_user_question", QJsonObject());
        QVERIFY(result.contains("harness"));
    }

    void askUserQuestionIsNotReadonly() {
        QVERIFY(!Tools::isReadOnly("ask_user_question"));
    }

    // ── Tools: schema content validation ──────────────────────────

    static QJsonObject findTool(const QString& name) {
        for (const QJsonValue& v : Tools::toolDefinitions()) {
            QJsonObject fn = v.toObject()["function"].toObject();
            if (fn["name"].toString() == name)
                return fn;
        }
        return {};
    }

    void todowriteSchemaItemsAreObjectsNotStrings() {
        QJsonObject fn = findTool("todowrite");
        QVERIFY(!fn.isEmpty());
        QJsonObject params = fn["parameters"].toObject();
        QJsonObject todos = params["properties"].toObject()["todos"].toObject();
        QCOMPARE(todos["type"].toString(), QString("array"));
        QJsonObject items = todos["items"].toObject();
        QCOMPARE(items["type"].toString(), QString("object"));
        QJsonArray req = items["required"].toArray();
        QVERIFY(req.contains("content"));
        QVERIFY(req.contains("status"));
        QJsonObject props = items["properties"].toObject();
        QCOMPARE(props["content"].toObject()["type"].toString(), QString("string"));
        QCOMPARE(props["status"].toObject()["type"].toString(), QString("string"));
        QJsonArray statusEnum = props["status"].toObject()["enum"].toArray();
        QVERIFY(statusEnum.contains("pending"));
        QVERIFY(statusEnum.contains("in_progress"));
        QVERIFY(statusEnum.contains("completed"));
    }

    void applyChangesSchemaHasFullOperationProperties() {
        QJsonObject fn = findTool("apply_changes");
        QVERIFY(!fn.isEmpty());
        QJsonObject params = fn["parameters"].toObject();
        QJsonObject props = params["properties"].toObject();

        // changes array
        QJsonObject changes = props["changes"].toObject();
        QCOMPARE(changes["type"].toString(), QString("array"));
        QJsonObject changeItems = changes["items"].toObject();
        QCOMPARE(changeItems["type"].toString(), QString("object"));
        QJsonArray changeReq = changeItems["required"].toArray();
        QVERIFY(changeReq.contains("path"));
        QVERIFY(changeReq.contains("operations"));

        // operations within each change
        QJsonObject operations =
            changeItems["properties"].toObject()["operations"].toObject();
        QCOMPARE(operations["type"].toString(), QString("array"));
        QJsonObject opItems = operations["items"].toObject();
        QCOMPARE(opItems["type"].toString(), QString("object"));
        QJsonArray opReq = opItems["required"].toArray();
        QVERIFY(opReq.contains("kind"));
        QJsonObject opProps = opItems["properties"].toObject();
        QJsonArray kindEnum = opProps["kind"].toObject()["enum"].toArray();
        QVERIFY(kindEnum.contains("replace"));
        QVERIFY(kindEnum.contains("insert_after"));
        QVERIFY(kindEnum.contains("delete"));
        QCOMPARE(opProps["old"].toObject()["type"].toString(), QString("string"));
        QCOMPARE(opProps["new"].toObject()["type"].toString(), QString("string"));
        QCOMPARE(opProps["anchor"].toObject()["type"].toString(), QString("string"));
        QCOMPARE(opProps["text"].toObject()["type"].toString(), QString("string"));
        QCOMPARE(opProps["expected_matches"].toObject()["type"].toString(), QString("integer"));

        // dry_run
        QJsonObject dryRun = props["dry_run"].toObject();
        QCOMPARE(dryRun["type"].toString(), QString("boolean"));
        QVERIFY(!dryRun["description"].toString().isEmpty());

        // postconditions
        QJsonObject post = props["postconditions"].toObject();
        QCOMPARE(post["type"].toString(), QString("array"));
        QJsonObject postItems = post["items"].toObject();
        QCOMPARE(postItems["type"].toString(), QString("object"));
        QJsonObject postProps = postItems["properties"].toObject();
        QVERIFY(postProps.contains("contains"));
        QVERIFY(postProps.contains("does_not_contain"));
        QCOMPARE(postProps["contains"].toObject()["type"].toString(), QString("string"));
        QCOMPARE(postProps["does_not_contain"].toObject()["type"].toString(), QString("string"));
    }

    // ── Tools: per-run ToolContext isolation ────────────────────────

    void toolContextSudoProviderPerContext() {
        Tools::ToolContext a, b;
        a.setSudoProvider([] { return QString("pw-a"); });
        b.setSudoProvider([] { return QString("pw-b"); });
        QCOMPARE(a.sudoProvider()(), QString("pw-a"));
        QCOMPARE(b.sudoProvider()(), QString("pw-b"));
    }

    void toolContextCachedPasswordNotShared() {
        Tools::ToolContext a, b;
        a.setCachedSudoPassword("secret");
        QVERIFY(b.cachedSudoPassword().isEmpty());
        a.clearSudo();
        QVERIFY(a.cachedSudoPassword().isEmpty());
    }

    void toolContextRunBashRefusesSudoWithoutProvider() {
        // A context with no provider must refuse sudo regardless of the default.
        Tools::ToolContext ctx;
        QString r = Tools::execute("run_bash",
                                   QJsonObject{{"command", "sudo true"}},
                                   nullptr, &ctx);
        QVERIFY(r.contains("no password provider"));
    }

    // ── Tools: sudo askpass ─────────────────────────────────────────

    void sudoRewriteForAskpass() {
        QCOMPARE(Tools::rewriteSudoForAskpass("sudo apt update"),
                 QString("sudo -A apt update"));
        // stdin no longer carries the password, so -S must be replaced.
        QCOMPARE(Tools::rewriteSudoForAskpass("sudo -S apt update"),
                 QString("sudo -A apt update"));
        QCOMPARE(Tools::rewriteSudoForAskpass("sudo -A apt update"),
                 QString("sudo -A apt update"));
        // Every occurrence, not just the first — askpass works per invocation.
        QCOMPARE(Tools::rewriteSudoForAskpass("sudo apt update && sudo apt upgrade"),
                 QString("sudo -A apt update && sudo -A apt upgrade"));
        QCOMPARE(Tools::rewriteSudoForAskpass("echo hi | sudo tee /etc/f"),
                 QString("echo hi | sudo -A tee /etc/f"));
        // Word boundaries: these are not sudo.
        QCOMPARE(Tools::rewriteSudoForAskpass("echo sudoku"), QString("echo sudoku"));
        QCOMPARE(Tools::rewriteSudoForAskpass("ls /dev/pseudo-tty"),
                 QString("ls /dev/pseudo-tty"));
    }

    void sudoAuthenticatesViaAskpass() {
#ifdef Q_OS_UNIX
        // Stub sudo that only succeeds when given -A plus a working askpass.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QString sudoPath = dir.path() + "/sudo";
        QFile f(sudoPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("#!/bin/bash\n"
                "if [ \"$1\" != \"-A\" ]; then echo \"sudo: no tty present\" >&2; exit 1; fi\n"
                "shift\n"
                "pw=\"$(\"$SUDO_ASKPASS\")\"\n"
                "echo \"pw=$pw\"\n"
                "exec \"$@\"\n");
        f.close();
        QFile::setPermissions(sudoPath, QFile::ReadOwner | QFile::WriteOwner |
                                        QFile::ExeOwner | QFile::ReadOther | QFile::ExeOther);

        QByteArray oldPath = qgetenv("PATH");
        qputenv("PATH", dir.path().toUtf8() + ":" + oldPath);

        Tools::ToolContext ctx;
        ctx.setSudoProvider([] { return QString("s3cret"); });

        // Shapes the old stdin-piped `sudo -S` broke on.
        const QStringList commands = {
            "sudo echo hi",
            "echo a; cat > /dev/null; sudo echo hi",
            "sudo echo one; sudo echo two",
            "sudo echo hi < /dev/null",
            "sudo -S echo hi",
            "echo hi | sudo tee /dev/null",
        };
        for (const QString& c : commands) {
            QString r = Tools::execute("run_bash", QJsonObject{{"command", c}},
                                       nullptr, &ctx);
            QVERIFY2(r.contains("pw=s3cret"),
                     qPrintable(QString("%1 -> %2").arg(c, r)));
            QVERIFY2(!r.contains("no tty present"),
                     qPrintable(QString("%1 -> %2").arg(c, r)));
        }

        qputenv("PATH", oldPath);
#endif
    }

    void toolContextKillAllOnlyAffectsOwn() {
#ifdef Q_OS_UNIX
        Tools::ToolContext a, b;
        QProcess proc;
        proc.setProgram("sleep");
        proc.setArguments({"30"});
        proc.setChildProcessModifier([] { setsid(); });
        proc.start();
        QVERIFY(proc.waitForStarted(3000));
        qint64 pid = proc.processId();
        b.registerProcess(pid);

        a.killAll();                       // must NOT kill b's process
        QCOMPARE(proc.state(), QProcess::Running);

        b.killAll();                       // now it should die
        QVERIFY(proc.waitForFinished(5000));
#endif
    }

    // ── Web server: startup ──────────────────────────────────────────

    void webServerBindsPort() {
        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        QVERIFY(server.port() > 0);
    }

    // ── Web server: routing ──────────────────────────────────────────

    void webGetRootRedirects() {
        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        WebResp r = webRequest("GET", server.port(), "/");
        QCOMPARE(r.status, 302);
        QVERIFY2(r.location.contains("/chat/"),
                 qPrintable("Location was: " + r.location));
    }

    void webGetChatPage() {
        QJsonObject chat = chatCreate("Web Test Chat");
        QString chatId   = chat["id"].toString();

        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        WebResp r = webRequest("GET", server.port(), "/chat/" + chatId);
        QCOMPARE(r.status, 200);
        QVERIFY(r.contentType.startsWith("text/html"));
        QVERIFY(r.body.contains("bootstrap"));
        QVERIFY(r.body.contains(chatId.toUtf8()));
    }

    void webGetSettingsPage() {
        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        WebResp r = webRequest("GET", server.port(), "/settings");
        QCOMPARE(r.status, 200);
        QVERIFY(r.contentType.startsWith("text/html"));
        QVERIFY(r.body.contains("base_url"));
        QVERIFY(r.body.contains("api_key"));
        QVERIFY(r.body.contains("tool_confirmation"));
    }

    void webPostSettingsSavesAndRedirects() {
        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        QByteArray form =
            "base_url=http%3A%2F%2Flocalhost%3A8080%2Fv1"
            "&api_key=sk-webtest"
            "&model=web-test-model"
            "&system_message=hi"
            "&tool_confirmation=safe"
            "&tool_timeout=45"
            "&context_keep_turns=3";
        WebResp r = webRequest("POST", server.port(), "/settings", form,
                               "application/x-www-form-urlencoded");
        QCOMPARE(r.status, 302);
        QVERIFY(r.location.contains("/settings"));

        Config cfg = configLoad();
        QCOMPARE(cfg.model, "web-test-model");
        QCOMPARE(cfg.toolConfirmation, "safe");
        QCOMPARE(cfg.toolTimeout, 45);
        QCOMPARE(cfg.contextKeepTurns, 3);
    }

    void webPostNewChatCreatesChat() {
        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        WebResp r = webRequest("POST", server.port(), "/chat/new");
        QCOMPARE(r.status, 302);
        QVERIFY(r.location.contains("/chat/"));

        QJsonArray chats = chatsLoad();
        QCOMPARE(chats.size(), 1);
        QCOMPARE(chats.first().toObject()["title"].toString(), "New Chat");
    }

    void webSendEmptyContentReturns400() {
        QJsonObject chat = chatCreate("Send Test");
        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        WebResp r = webRequest("POST", server.port(),
                               "/chat/" + chat["id"].toString() + "/send",
                               R"({"content":""})", "application/json");
        QCOMPARE(r.status, 400);
    }

    void webUnknownRouteReturns404() {
        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        WebResp r = webRequest("GET", server.port(), "/no/such/path");
        QCOMPARE(r.status, 404);
    }

    void webDeleteChatRemovesIt() {
        QJsonObject chat = chatCreate("Delete Me");
        QString chatId   = chat["id"].toString();
        QCOMPARE(chatsLoad().size(), 1);

        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        WebResp r = webRequest("POST", server.port(), "/chat/" + chatId + "/delete");
        QCOMPARE(r.status, 302);
        QCOMPARE(chatsLoad().size(), 0);
    }

    void webStreamReturnsSSEHeaders() {
        QJsonObject chat = chatCreate("Stream Test");

        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        QNetworkAccessManager mgr;
        QNetworkRequest req(QUrl("http://127.0.0.1:" + QString::number(server.port()) +
                                 "/chat/" + chat["id"].toString() + "/stream"));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
        QNetworkReply* reply = mgr.get(req);

        // metaDataChanged fires when response headers arrive, before the body closes.
        // Perfect for SSE: we get the 200 + content-type without waiting for EOF.
        QEventLoop loop;
        QTimer::singleShot(4000, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, &QEventLoop::quit);
        loop.exec();

        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString ct = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        reply->abort();
        reply->deleteLater();

        QCOMPARE(status, 200);
        QVERIFY2(ct.startsWith("text/event-stream"), qPrintable(ct));
    }

    void webSseReplayResumesAfterCursorWithoutDuplicateToolRequest() {
        WebServer server("127.0.0.1", 0);
        const QString chatId = "sse-replay";
        server.testPushSse(chatId, QJsonObject{{"type", "tool_request"}, {"tool_call_id", "tool-1"}});
        server.testPushSse(chatId, QJsonObject{{"type", "tool_result"}, {"tool_call_id", "tool-1"}});
        server.testPushSse(chatId, QJsonObject{{"type", "final_response"}, {"content", "done"}});

        const QByteArray replay = server.testReplay(chatId, 0);
        QVERIFY(!replay.contains("tool_request"));
        QVERIFY(replay.contains("id: 1\n"));
        QVERIFY(replay.contains("tool_result"));
        QVERIFY(replay.contains("id: 2\n"));
        QVERIFY(replay.contains("final_response"));
    }

    void webSseDisconnectReconnectDeliversMissedFinalExactlyOnce() {
        WebServer server("127.0.0.1", 0);
        const QString chatId = "sse-disconnect";
        server.testPushSse(chatId, QJsonObject{{"type", "tool_request"}, {"tool_call_id", "tool-1"}});
        // The first stream disconnects after event 0. The producer keeps
        // logging events while no socket is attached.
        server.testPushSse(chatId, QJsonObject{{"type", "tool_result"}, {"tool_call_id", "tool-1"}});
        server.testPushSse(chatId, QJsonObject{{"type", "final_response"}, {"content", "done"}});

        const QByteArray replay = server.testReplay(chatId, 0);
        QCOMPARE(replay.count("tool_result"), 1);
        QCOMPARE(replay.count("final_response"), 1);
        QVERIFY(!replay.contains("tool_request"));
    }

    void webSseCompletedLogReplaysTerminalThenCleansUp() {
        WebServer server("127.0.0.1", 0);
        const QString chatId = "sse-cleanup";
        server.testPushSse(chatId, QJsonObject{{"type", "tool_request"}});
        server.testPushSse(chatId, QJsonObject{{"type", "final_response"}, {"content", "done"}});
        server.testMarkCompleted(chatId);
        const QByteArray freshReplay = server.testReplay(chatId, -1);
        QVERIFY(!freshReplay.contains("tool_request"));
        QVERIFY(freshReplay.contains("final_response"));

        server.testCleanupCompleted(chatId);
        QVERIFY(server.testReplay(chatId, -1).isEmpty());
    }

    void webChatTemplateUsesCursorSafeReconnectAndStandardScrollBehavior() {
        QJsonObject chat = chatCreate("Template SSE test");
        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        WebResp r = webRequest("GET", server.port(), "/chat/" + chat["id"].toString());
        QCOMPARE(r.status, 200);
        QVERIFY(r.body.contains("sessionStorage.getItem('pengy_sse_cursor_'"));
        QVERIFY(r.body.contains("?after=' + encodeURIComponent(sseCursor)"));
        QVERIFY(r.body.contains("readyState === EventSource.CLOSED"));
        QVERIFY(r.body.contains("behavior: 'auto'"));
        QVERIFY(!r.body.contains("readyState !== EventSource.OPEN"));
        QVERIFY(!r.body.contains("behavior: 'instant'"));
    }

    void webChatTemplateToolSummaryPresent() {
        QJsonObject chat = chatCreate("Template tool summary");
        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        WebResp r = webRequest("GET", server.port(), "/chat/" + chat["id"].toString());
        QCOMPARE(r.status, 200);
        QVERIFY(r.body.contains("function toolSummary(name, args)"));
        QVERIFY(r.body.contains("tool-summary"));
    }

    // ── Web server: export / rename / command / models / attachments ─
    // Parity tests for the routes shared with the Python and Rust webs.

    void webExportReturnsMarkdown() {
        QJsonObject chat = chatCreate("Export Test");
        QJsonArray msgs;
        msgs.append(userMsg("hello"));
        QJsonObject asst = assistantWithTools({"tc1"});
        asst["content"] = "using a tool";
        msgs.append(asst);
        msgs.append(toolMsg("tc1", "tool output data"));
        msgs.append(assistantMsg("all done"));
        chat["messages"] = msgs;
        chatSave(chat);

        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        WebResp r = webRequest("GET", server.port(),
                               "/chat/" + chat["id"].toString() + "/export");
        QCOMPARE(r.status, 200);
        QVERIFY(r.contentType.contains("markdown"));
        QVERIFY(r.disposition.contains("attachment"));
        QVERIFY(r.disposition.contains("Export Test.md"));
        QString body = QString::fromUtf8(r.body);
        QVERIFY(body.contains("# Export Test"));
        QVERIFY(body.contains("🧑 You"));
        QVERIFY(body.contains("hello"));
        QVERIFY(body.contains("test_tool"));
        QVERIFY(body.contains("tool output data"));
        QVERIFY(body.contains("all done"));
    }

    void webExportUnknownChat404() {
        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        WebResp r = webRequest("GET", server.port(), "/chat/nope/export");
        QCOMPARE(r.status, 404);
    }

    void webRenameUpdatesTitle() {
        QJsonObject chat = chatCreate("Old Title");
        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        WebResp r = webRequest("POST", server.port(),
                               "/chat/" + chat["id"].toString() + "/rename",
                               R"({"title": "Fresh Title"})", "application/json");
        QCOMPARE(r.status, 200);
        QCOMPARE(chatGet(chat["id"].toString())["title"].toString(),
                 QString("Fresh Title"));
    }

    void webRenameEmptyTitle400() {
        QJsonObject chat = chatCreate("Old Title");
        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        WebResp r = webRequest("POST", server.port(),
                               "/chat/" + chat["id"].toString() + "/rename",
                               R"({"title": "  "})", "application/json");
        QCOMPARE(r.status, 400);
    }

    void webCommandYoloPersists() {
        QJsonObject chat = chatCreate("Cmd Test");
        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        WebResp r = webRequest("POST", server.port(),
                               "/chat/" + chat["id"].toString() + "/command",
                               R"({"command": "/yolo safe"})", "application/json");
        QCOMPARE(r.status, 200);
        QJsonObject data = QJsonDocument::fromJson(r.body).object();
        QCOMPARE(data["type"].toString(), QString("config"));
        QVERIFY(data["message"].toString().contains("Safe"));
        QCOMPARE(configLoad().toolConfirmation, QString("safe"));
    }

    void webCommandModelPersists() {
        QJsonObject chat = chatCreate("Cmd Test");
        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        WebResp r = webRequest("POST", server.port(),
                               "/chat/" + chat["id"].toString() + "/command",
                               R"({"command": "/model test-model-9"})",
                               "application/json");
        QJsonObject data = QJsonDocument::fromJson(r.body).object();
        QCOMPARE(data["type"].toString(), QString("config"));
        QCOMPARE(configLoad().model, QString("test-model-9"));
    }

    void webCommandNewRedirects() {
        QJsonObject chat = chatCreate("Cmd Test");
        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        WebResp r = webRequest("POST", server.port(),
                               "/chat/" + chat["id"].toString() + "/command",
                               R"({"command": "/new"})", "application/json");
        QJsonObject data = QJsonDocument::fromJson(r.body).object();
        QCOMPARE(data["type"].toString(), QString("redirect"));
        QVERIFY(data["url"].toString().contains("/chat/"));
    }

    void webCommandRenamePersists() {
        QJsonObject chat = chatCreate("Cmd Test");
        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        WebResp r = webRequest("POST", server.port(),
                               "/chat/" + chat["id"].toString() + "/command",
                               R"({"command": "/rename Renamed Via Cmd"})",
                               "application/json");
        QJsonObject data = QJsonDocument::fromJson(r.body).object();
        QCOMPARE(data["type"].toString(), QString("rename"));
        QCOMPARE(chatGet(chat["id"].toString())["title"].toString(),
                 QString("Renamed Via Cmd"));
    }

    void webCommandHelpAndUnknown() {
        QJsonObject chat = chatCreate("Cmd Test");
        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());

        WebResp help = webRequest("POST", server.port(),
                                  "/chat/" + chat["id"].toString() + "/command",
                                  R"({"command": "/help"})", "application/json");
        QJsonObject helpData = QJsonDocument::fromJson(help.body).object();
        QCOMPARE(helpData["type"].toString(), QString("message"));
        QVERIFY(helpData["message"].toString().contains("/yolo"));

        WebResp unk = webRequest("POST", server.port(),
                                 "/chat/" + chat["id"].toString() + "/command",
                                 R"({"command": "/wat"})", "application/json");
        QJsonObject unkData = QJsonDocument::fromJson(unk.body).object();
        QVERIFY(unkData["message"].toString().contains("Unknown command"));

        WebResp plain = webRequest("POST", server.port(),
                                   "/chat/" + chat["id"].toString() + "/command",
                                   R"({"command": "just text"})", "application/json");
        QCOMPARE(plain.status, 400);
    }

    void webModelsFetchesSortedIds() {
        StubLlmServer stub;
        stub.responses << QByteArray(
            R"({"data": [{"id": "zeta-model"}, {"id": "alpha-model"}]})");
        Config cfg = configLoad();
        cfg.baseUrl = stub.baseUrl();
        configSave(cfg);

        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        WebResp r = webRequest("GET", server.port(), "/models");
        QCOMPARE(r.status, 200);
        QJsonArray models = QJsonDocument::fromJson(r.body).object()["models"].toArray();
        QCOMPARE(models.size(), 2);
        QCOMPARE(models[0].toString(), QString("alpha-model"));
        QCOMPARE(models[1].toString(), QString("zeta-model"));
    }

    void webModelsUnreachable502() {
        Config cfg = configLoad();
        cfg.baseUrl = "http://127.0.0.1:9";  // discard port — refused fast
        configSave(cfg);

        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());
        WebResp r = webRequest("GET", server.port(), "/models");
        QCOMPARE(r.status, 502);
    }

    void webSendInjectsAttachments() {
        StubLlmServer llm;
        llm.responses << llmCompletion("attachment received");
        Config cfg = configLoad();
        cfg.baseUrl = llm.baseUrl();
        cfg.apiKey = "test";
        configSave(cfg);

        QJsonObject chat = chatCreate("Attach Test");
        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());

        QByteArray fileB64 = QByteArray("attachment body").toBase64();
        QByteArray payload = QJsonDocument(QJsonObject{
            {"content", "what is in this file?"},
            {"files", QJsonArray{QJsonObject{
                {"name", "note.txt"},
                {"data", QString::fromUtf8(fileB64)}}}},
        }).toJson(QJsonDocument::Compact);

        WebResp r = webRequest("POST", server.port(),
                               "/chat/" + chat["id"].toString() + "/send",
                               payload, "application/json");
        QCOMPARE(r.status, 200);

        // The worker saves the chat when the conversation finishes.
        QTRY_VERIFY_WITH_TIMEOUT(
            !chatGet(chat["id"].toString())["messages"].toArray().isEmpty(), 5000);

        QJsonArray msgs = chatGet(chat["id"].toString())["messages"].toArray();
        QString userContent = msgs[0].toObject()["content"].toString();
        QVERIFY(userContent.contains("[File: note.txt]"));
        QVERIFY(userContent.contains("attachment body"));
        QVERIFY(userContent.endsWith("what is in this file?"));
        QCOMPARE(msgs.last().toObject()["content"].toString(),
                 QString("attachment received"));
    }

    void webSendFilesOnlyAccepted() {
        StubLlmServer llm;
        llm.responses << llmCompletion("got it");
        Config cfg = configLoad();
        cfg.baseUrl = llm.baseUrl();
        configSave(cfg);

        QJsonObject chat = chatCreate("Files Only");
        WebServer server("127.0.0.1", 0);
        QVERIFY(server.start());

        QByteArray payload = QJsonDocument(QJsonObject{
            {"content", ""},
            {"files", QJsonArray{QJsonObject{
                {"name", "a.txt"},
                {"data", QString::fromUtf8(QByteArray("just the file").toBase64())}}}},
        }).toJson(QJsonDocument::Compact);

        WebResp r = webRequest("POST", server.port(),
                               "/chat/" + chat["id"].toString() + "/send",
                               payload, "application/json");
        QCOMPARE(r.status, 200);
        QTRY_VERIFY_WITH_TIMEOUT(
            !chatGet(chat["id"].toString())["messages"].toArray().isEmpty(), 5000);
    }

    // ── LlmClient conversation-loop tests (stub server) ──────────────
    // Mirrors Pengy's tests/test_llm_loop.py and PengyR's loop_tests —
    // keep scenarios in sync across the three editions.

    void llmFinalResponseNoTools() {
        StubLlmServer stub;
        stub.responses << llmCompletion("hello there");

        LlmParams p;
        p.baseUrl = stub.baseUrl();
        p.apiKey = "test-key";
        p.model = "stub-model";
        p.messages = QJsonArray{userMsg("hi")};
        p.toolConfirmation = "none";

        QList<QJsonObject> events;
        LlmClient().run(p,
            [&](const QJsonObject& ev) { events.append(ev); },
            []() { return std::make_pair(true, false); },
            []() { return false; });

        QCOMPARE(events.size(), 1);
        QCOMPARE(events[0]["type"].toString(), QString("final_response"));
        QCOMPARE(events[0]["content"].toString(), QString("hello there"));
        QCOMPARE(events[0]["usage"].toObject()["total_tokens"].toInt(), 15);

        QCOMPARE(stub.requests.size(), 1);
        QCOMPARE(stub.requests[0]["model"].toString(), QString("stub-model"));
        QCOMPARE(stub.requests[0]["tools"].toArray().size(), 16);
        QVERIFY(!stub.requests[0].contains("reasoning_effort"));
    }

    void llmReasoningEffortSent() {
        StubLlmServer stub;
        stub.responses << llmCompletion("ok");

        LlmParams p;
        p.baseUrl = stub.baseUrl();
        p.model = "stub-model";
        p.messages = QJsonArray{userMsg("hi")};
        p.toolConfirmation = "none";
        p.reasoningEffort = "high";

        LlmClient().run(p, [](const QJsonObject&) {},
            []() { return std::make_pair(true, false); },
            []() { return false; });

        QCOMPARE(stub.requests[0]["reasoning_effort"].toString(), QString("high"));
    }

    void llmAllModeExecutesToolAndFeedsResult() {
        QTemporaryDir dir;
        QString file = dir.path() + "/note.txt";
        { QFile f(file); f.open(QIODevice::WriteOnly); f.write("file body here"); }

        StubLlmServer stub;
        stub.responses
            << llmCompletion("", QJsonArray{llmToolCall("tc1", "read_file",
                                 QJsonObject{{"path", file}})}, 100, 20)
            << llmCompletion("done", {}, 200, 30);

        LlmParams p;
        p.baseUrl = stub.baseUrl();
        p.model = "stub-model";
        p.messages = QJsonArray{userMsg("read it")};
        p.toolConfirmation = "all";

        QList<QJsonObject> events;
        int confirms = 0;
        LlmClient().run(p,
            [&](const QJsonObject& ev) { events.append(ev); },
            [&]() { confirms++; return std::make_pair(true, false); },
            []() { return false; });

        QCOMPARE(confirms, 0);
        QStringList types;
        for (const auto& ev : events) types << ev["type"].toString();
        QCOMPARE(types, QStringList({"assistant_tool_calls", "tool_request",
                                     "tool_result", "final_response"}));
        QVERIFY(events[2]["content"].toString().contains("file body here"));
        QCOMPARE(events[2]["declined"].toBool(), false);

        // Usage accumulated across both round-trips
        QJsonObject usage = events[3]["usage"].toObject();
        QCOMPARE(usage["prompt_tokens"].toInt(), 300);
        QCOMPARE(usage["completion_tokens"].toInt(), 50);

        // Second request carries assistant tool_calls + tool result
        QJsonArray msgs = stub.requests[1]["messages"].toArray();
        QJsonObject last = msgs.last().toObject();
        QCOMPARE(last["role"].toString(), QString("tool"));
        QCOMPARE(last["tool_call_id"].toString(), QString("tc1"));
        QJsonObject secondLast = msgs[msgs.size() - 2].toObject();
        QCOMPARE(secondLast["role"].toString(), QString("assistant"));
        QVERIFY(!secondLast["tool_calls"].toArray().isEmpty());
    }

    // read_image can't return a picture through a role:"tool" message, so the
    // loop attaches it as a follow-up user message.  Mirrors Python's
    // TestReadImageAttachment and Rust's read_image_attaches_* — keep in sync.
    void llmReadImageAttachesPictureAsUserMessage() {
        QTemporaryDir dir;
        QString file = dir.path() + "/shot.png";
        QImage img(48, 32, QImage::Format_RGB32);
        img.fill(QColor(10, 120, 200));
        QVERIFY(img.save(file));

        StubLlmServer stub;
        stub.responses
            << llmCompletion("", QJsonArray{llmToolCall("tc1", "read_image",
                                 QJsonObject{{"path", file}})}, 10, 5)
            << llmCompletion("a blue rectangle", {}, 10, 5);

        LlmParams p;
        p.baseUrl = stub.baseUrl();
        p.model = "stub-model";
        p.messages = QJsonArray{userMsg("what is in it?")};
        p.toolConfirmation = "all";

        QList<QJsonObject> events;
        LlmClient().run(p,
            [&](const QJsonObject& ev) { events.append(ev); },
            [&]() { return std::make_pair(true, false); },
            []() { return false; });

        QVERIFY(events[2]["content"].toString().contains("Loaded shot.png"));
        QVERIFY(events[2]["content"].toString().contains(QString::fromUtf8("48×32")));

        QJsonArray msgs = stub.requests[1]["messages"].toArray();

        // The tool message stays a plain string, directly behind its assistant.
        int toolIdx = -1;
        for (int i = 0; i < msgs.size(); ++i)
            if (msgs[i].toObject()["role"].toString() == "tool") { toolIdx = i; break; }
        QVERIFY(toolIdx > 0);
        QVERIFY(msgs[toolIdx].toObject()["content"].isString());
        QCOMPARE(msgs[toolIdx - 1].toObject()["role"].toString(), QString("assistant"));

        // The picture rides in a trailing user message instead.
        QJsonObject last = msgs.last().toObject();
        QCOMPARE(last["role"].toString(), QString("user"));
        QJsonArray parts = last["content"].toArray();
        int images = 0;
        QString url;
        for (const QJsonValue& v : parts) {
            if (v.toObject()["type"].toString() == "image_url") {
                images++;
                url = v.toObject()["image_url"].toObject()["url"].toString();
            }
        }
        QCOMPARE(images, 1);
        QVERIFY(url.startsWith("data:image/"));
        QVERIFY(url.contains(";base64,"));
    }

    void llmReadImageErrorAttachesNothing() {
        QTemporaryDir dir;
        StubLlmServer stub;
        stub.responses
            << llmCompletion("", QJsonArray{llmToolCall("tc1", "read_image",
                                 QJsonObject{{"path", dir.path() + "/nope.png"}})}, 10, 5)
            << llmCompletion("ok", {}, 10, 5);

        LlmParams p;
        p.baseUrl = stub.baseUrl();
        p.model = "stub-model";
        p.messages = QJsonArray{userMsg("look")};
        p.toolConfirmation = "all";

        LlmClient().run(p, [](const QJsonObject&) {},
            [&]() { return std::make_pair(true, false); },
            []() { return false; });

        for (const QJsonValue& v : stub.requests[1]["messages"].toArray()) {
            if (v.toObject()["role"].toString() == "user")
                QVERIFY(v.toObject()["content"].isString());
        }
    }

    void llmSafeModePausesForWriteTool() {
        QTemporaryDir dir;
        QString target = dir.path() + "/out.txt";

        StubLlmServer stub;
        stub.responses
            << llmCompletion("", QJsonArray{llmToolCall("tc1", "write_file",
                                 QJsonObject{{"path", target}, {"content", "written!"}})})
            << llmCompletion("done");

        LlmParams p;
        p.baseUrl = stub.baseUrl();
        p.model = "stub-model";
        p.messages = QJsonArray{userMsg("write")};
        p.toolConfirmation = "safe";

        int confirms = 0;
        LlmClient().run(p, [](const QJsonObject&) {},
            [&]() { confirms++; return std::make_pair(true, false); },
            []() { return false; });

        QCOMPARE(confirms, 1);
        QFile f(target);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("written!"));
    }

    void llmSafeModeAutoApprovesReadonly() {
        QTemporaryDir dir;
        QString file = dir.path() + "/note.txt";
        { QFile f(file); f.open(QIODevice::WriteOnly); f.write("safe read"); }

        StubLlmServer stub;
        stub.responses
            << llmCompletion("", QJsonArray{llmToolCall("tc1", "read_file",
                                 QJsonObject{{"path", file}})})
            << llmCompletion("done");

        LlmParams p;
        p.baseUrl = stub.baseUrl();
        p.model = "stub-model";
        p.messages = QJsonArray{userMsg("read")};
        p.toolConfirmation = "safe";

        int confirms = 0;
        QList<QJsonObject> events;
        LlmClient().run(p,
            [&](const QJsonObject& ev) { events.append(ev); },
            [&]() { confirms++; return std::make_pair(true, false); },
            []() { return false; });

        QCOMPARE(confirms, 0);
        QVERIFY(events[2]["content"].toString().contains("safe read"));
    }

    void llmDeclineFeedsDeclinedMessage() {
        QTemporaryDir dir;
        QString target = dir.path() + "/out.txt";

        StubLlmServer stub;
        stub.responses
            << llmCompletion("", QJsonArray{llmToolCall("tc1", "write_file",
                                 QJsonObject{{"path", target}, {"content", "x"}})})
            << llmCompletion("understood");

        LlmParams p;
        p.baseUrl = stub.baseUrl();
        p.model = "stub-model";
        p.messages = QJsonArray{userMsg("write")};
        p.toolConfirmation = "none";

        QList<QJsonObject> events;
        LlmClient().run(p,
            [&](const QJsonObject& ev) { events.append(ev); },
            []() { return std::make_pair(false, false); },
            []() { return false; });

        QJsonObject result = events[2];
        QCOMPARE(result["type"].toString(), QString("tool_result"));
        QCOMPARE(result["declined"].toBool(), true);
        QVERIFY(!QFile::exists(target));

        QJsonArray msgs = stub.requests[1]["messages"].toArray();
        QCOMPARE(msgs.last().toObject()["content"].toString(),
                 QString("Tool execution was declined by user."));
    }

    void llmYoloTurnSkipsRemainingConfirms() {
        QTemporaryDir dir;
        QString f1 = dir.path() + "/a.txt";
        QString f2 = dir.path() + "/b.txt";

        StubLlmServer stub;
        stub.responses
            << llmCompletion("", QJsonArray{
                   llmToolCall("tc1", "write_file", QJsonObject{{"path", f1}, {"content", "one"}}),
                   llmToolCall("tc2", "write_file", QJsonObject{{"path", f2}, {"content", "two"}})})
            << llmCompletion("done");

        LlmParams p;
        p.baseUrl = stub.baseUrl();
        p.model = "stub-model";
        p.messages = QJsonArray{userMsg("write both")};
        p.toolConfirmation = "none";

        int confirms = 0;
        LlmClient().run(p, [](const QJsonObject&) {},
            [&]() { confirms++; return std::make_pair(true, true); },  // yolo turn
            []() { return false; });

        QCOMPARE(confirms, 1);  // second tool call must not re-prompt
        QVERIFY(QFile::exists(f1));
        QVERIFY(QFile::exists(f2));
    }

    void llmYoloTurnResetsNextRound() {
        QTemporaryDir dir;
        QString f1 = dir.path() + "/a.txt";
        QString f2 = dir.path() + "/b.txt";

        StubLlmServer stub;
        stub.responses
            << llmCompletion("", QJsonArray{llmToolCall("tc1", "write_file",
                                 QJsonObject{{"path", f1}, {"content", "one"}})})
            << llmCompletion("", QJsonArray{llmToolCall("tc2", "write_file",
                                 QJsonObject{{"path", f2}, {"content", "two"}})})
            << llmCompletion("done");

        LlmParams p;
        p.baseUrl = stub.baseUrl();
        p.model = "stub-model";
        p.messages = QJsonArray{userMsg("write twice")};
        p.toolConfirmation = "none";

        int confirms = 0;
        LlmClient().run(p, [](const QJsonObject&) {},
            [&]() { confirms++; return std::make_pair(true, true); },
            []() { return false; });

        // yolo from round 1 must not leak into round 2
        QCOMPARE(confirms, 2);
        QVERIFY(QFile::exists(f2));
    }

    void llmPreserveReasoningKeepsFields() {
        QTemporaryDir dir;
        QString file = dir.path() + "/a.txt";
        { QFile f(file); f.open(QIODevice::WriteOnly); f.write("data"); }

        StubLlmServer stub;
        stub.responses
            << llmCompletion("", QJsonArray{llmToolCall("tc1", "read_file",
                                 QJsonObject{{"path", file}})}, 10, 5,
                             QJsonObject{{"reasoning_content", "thinking..."}})
            << llmCompletion("done");

        LlmParams p;
        p.baseUrl = stub.baseUrl();
        p.model = "stub-model";
        p.messages = QJsonArray{userMsg("read")};
        p.toolConfirmation = "all";
        p.preserveReasoning = true;

        LlmClient().run(p, [](const QJsonObject&) {},
            []() { return std::make_pair(true, false); },
            []() { return false; });

        QJsonArray msgs = stub.requests[1]["messages"].toArray();
        bool found = false;
        for (const QJsonValue& v : msgs) {
            QJsonObject m = v.toObject();
            if (m["role"].toString() == "assistant" &&
                m["reasoning_content"].toString() == "thinking...") found = true;
        }
        QVERIFY2(found, "reasoning_content should be preserved in follow-up request");
    }

    void llmHttpErrorProducesApiError() {
        StubLlmServer stub;
        stub.responses << QByteArray(R"({"error": {"message": "boom"}})");
        stub.statuses << 500;

        LlmParams p;
        p.baseUrl = stub.baseUrl();
        p.model = "stub-model";
        p.messages = QJsonArray{userMsg("hi")};
        p.toolConfirmation = "none";

        QList<QJsonObject> events;
        LlmClient().run(p,
            [&](const QJsonObject& ev) { events.append(ev); },
            []() { return std::make_pair(true, false); },
            []() { return false; });

        QCOMPARE(events.size(), 1);
        QCOMPARE(events[0]["type"].toString(), QString("final_response"));
        QVERIFY(events[0]["content"].toString().contains("API error"));
        QVERIFY(events[0]["content"].toString().contains("boom"));
    }

    // ── CLI tests (subprocess) ───────────────────────────────────────

    void cliHelp() {
        if (!QFile::exists(cliBin()))
            QSKIP("pengy_cli not built yet");

        QString out = runCli({"/help"});
        QVERIFY2(!out.isEmpty(), "pengy_cli produced no output");
        QVERIFY(out.contains("Commands:"));
        QVERIFY(out.contains("/new"));
        QVERIFY(out.contains("/model"));
        QVERIFY(out.contains("/quit"));
        QVERIFY(out.contains("/yolo"));
    }

    void cliConfig() {
        if (!QFile::exists(cliBin()))
            QSKIP("pengy_cli not built yet");

        QString out = runCli({"/config"});
        QVERIFY(!out.isEmpty());
        QVERIFY(out.contains("Configuration:"));
        QVERIFY(out.contains("base_url"));
        QVERIFY(out.contains("model"));
        QVERIFY(out.contains("tool_confirm"));
        QVERIFY(out.contains("api_key"));
    }

    void cliNewAndList() {
        if (!QFile::exists(cliBin()))
            QSKIP("pengy_cli not built yet");

        // Create a new chat, then list all
        QString out = runCli({"/new", "/list"});
        QVERIFY(!out.isEmpty());
        QVERIFY(out.contains("Chats:"));
        // At minimum the auto-created chat and the /new chat appear
        QVERIFY(out.contains("New Chat"));
    }

    void cliQuitExitsCleanly() {
        if (!QFile::exists(cliBin()))
            QSKIP("pengy_cli not built yet");

        QProcess proc;
        proc.setProgram(cliBin());
        proc.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
        proc.start();
        QVERIFY(proc.waitForStarted(2000));
        proc.write("/quit\n");
        QVERIFY(proc.waitForFinished(4000));
        QCOMPARE(proc.exitCode(), 0);
    }
};

QTEST_MAIN(PengyTests)
#include "tests.moc"
