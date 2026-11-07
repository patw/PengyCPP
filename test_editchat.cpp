// Edit Chat dialog + Bulk Rename dialog, against the canned ChatTitle in
// stub_chattitle.cpp.  Shared verbatim with PengyCPP.
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QString>
#include <iostream>

#include "bulkrename.h"
#include "editchatdialog.h"

static void require(bool cond, const char* label) {
    if (!cond) {
        std::cerr << "FAIL: " << label << std::endl;
        std::exit(1);
    }
}

static void requireEqual(const QString& got, const QString& want, const char* label) {
    if (got != want) {
        std::cerr << "FAIL: " << label << "\nExpected: " << want.toStdString()
                  << "\nGot: " << got.toStdString() << std::endl;
        std::exit(1);
    }
}

static BulkRenameSummary runBulk(const QStringList& ids, QStringList* renamed) {
    BulkRenameDialog dlg(ids, false, [renamed](const QString& id, const QString&) { *renamed << id; });
    dlg.start();
    QElapsedTimer t;
    t.start();
    while (dlg.running() && t.elapsed() < 5000) QApplication::processEvents(QEventLoop::AllEvents, 20);
    require(!dlg.running(), "bulk rename finished");
    return dlg.summary();
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Theme theme = makeTheme("light", "default");

    // titleSource(): untouched keeps the chat's own source, an unedited
    // generated title is "model", anything typed is "user".
    {
        QJsonObject chat{{"id", "x"}, {"title", "Auto"}, {"messages", QJsonArray()}};
        EditChatDialog dlg(chat, "m", theme);
        requireEqual(dlg.titleSource(), QString(), "untouched automatic title keeps no source");
        dlg.testGenerated("Generated");
        requireEqual(dlg.title(), "Generated", "generate fills the field");
        requireEqual(dlg.titleSource(), "model", "unedited generated title");
        dlg.testSetTitle("Generated, edited");
        requireEqual(dlg.titleSource(), "user", "edited title");

        chat["title_source"] = "user";
        EditChatDialog mine(chat, "m", theme);
        requireEqual(mine.titleSource(), "user", "untouched hand-picked title stays user");
    }

    // chatMarkdown(): user + assistant text only.
    {
        QJsonArray msgs{
            QJsonObject{{"role", "user"}, {"content", "q"}},
            QJsonObject{{"role", "assistant"}, {"content", QJsonValue()}, {"tool_calls", QJsonArray{QJsonObject()}}},
            QJsonObject{{"role", "tool"}, {"content", "SECRET"}},
            QJsonObject{{"role", "user"}, {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "parts"}}}}},
            QJsonObject{{"role", "assistant"}, {"content", "a"}},
        };
        const QString md = chatMarkdown(QJsonObject{{"title", "T"}, {"messages", msgs}});
        require(md.startsWith("# T"), "markdown heading");
        require(md.contains("parts"), "list content is exported");
        require(!md.contains("SECRET"), "tool output is not exported");
        require(md.count("**Assistant**") == 1, "empty tool-call turn is skipped");
    }

    // Bulk rename: counts, onRenamed for written titles only.
    {
        QStringList renamed;
        BulkRenameSummary s = runBulk({"ok1", "same1", "kept1", "empty1", "ok2"}, &renamed);
        require(s.renamed == 2 && s.unchanged == 1 && s.kept == 1 && s.skipped == 1, "bulk counts");
        require(!s.aborted && !s.cancelled, "bulk ran to the end");
        require(renamed == QStringList({"ok1", "same1", "ok2"}), "onRenamed for written titles");
        require(bulkRenameSummaryText(s).startsWith("Renamed 2 chats."), "summary text");
    }

    // Five API failures in a row abort the run; a success in between resets.
    {
        QStringList renamed;
        BulkRenameSummary s = runBulk({"fail1", "fail2", "ok1", "fail3", "fail4", "fail5", "fail6",
                                       "fail7", "ok2"}, &renamed);
        require(s.aborted, "aborted after consecutive failures");
        require(s.failed == 7 && s.renamed == 1, "stopped at the fifth failure in a row");
        require(bulkRenameSummaryText(s).contains("endpoint down"), "last error reported");
    }

    std::cout << "editchat tests passed" << std::endl;
    return 0;
}
