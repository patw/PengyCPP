#include <QtTest>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QDir>
#include <QSysInfo>

#include "config.h"
#include "chatmanager.h"
#include "tools.h"

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

class PengyTests : public QObject {
    Q_OBJECT

private slots:
    // ── Config ──────────────────────────────────────────────────────

    void configDefaultValues() {
        Config c;
        QCOMPARE(c.baseUrl, "https://api.openai.com/v1");
        QCOMPARE(c.model, "gpt-4o");
        QCOMPARE(c.toolConfirmation, "none");
        QCOMPARE(c.uiScale, 100);
        QCOMPARE(c.toolTimeout, 60);
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
        QCOMPARE(c.toolTimeout, 60);
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
        QVERIFY(Tools::isReadOnly("read_multiple_files"));
        QVERIFY(Tools::isReadOnly("directory_tree"));
        QVERIFY(Tools::isReadOnly("search_content"));
        QVERIFY(Tools::isReadOnly("web_search"));
        QVERIFY(Tools::isReadOnly("fetch_url"));
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

    void toolDefinitionsHasEleven() {
        QCOMPARE(Tools::toolDefinitions().size(), 11);
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
        QCOMPARE(names.size(), 11);
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
        QCOMPARE(parsed.array().size(), 11);
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
};

QTEST_MAIN(PengyTests)
#include "tests.moc"
