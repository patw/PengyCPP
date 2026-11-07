// ChatHistoryWidget sidebar regression tests.
// Mirrors PengyR's gui/tests/test_chathistory.cpp.
//
// Build: cmake -DCMAKE_BUILD_TESTS=ON .. && cmake --build . --target pengy_chathistory_test
#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <iostream>
#include "chathistory.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QThread>
#include <QPushButton>

static void requireEqual(int got, int want, const char* label) {
    if (got != want) {
        std::cerr << "FAIL: " << label << "\nExpected: " << want
                  << "\nGot: " << got << std::endl;
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

static void requireTrue(bool cond, const char* label) {
    if (!cond) {
        std::cerr << "FAIL: " << label << " (expected true)" << std::endl;
        std::exit(1);
    }
}

static void requireFalse(bool cond, const char* label) {
    if (cond) {
        std::cerr << "FAIL: " << label << " (expected false)" << std::endl;
        std::exit(1);
    }
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // addChat() inserts one new row at the top, without touching the
    // existing rows -- the fix for "New Chat feels slow": createNewChat()
    // used to call loadChats() (full clear-and-rebuild of every row) on
    // every click, which scaled with total chat count.
    {
        ChatHistoryWidget w;
        QJsonArray seed;
        QJsonObject a; a["id"] = "old1"; a["title"] = "Old One";
        QJsonObject b; b["id"] = "old2"; b["title"] = "Old Two";
        seed.append(a);
        seed.append(b);
        w.loadChats(seed);
        requireEqual(w.testChatCount(), 2, "seeded chat count");

        w.addChat("new1", "Brand New Chat");
        requireEqual(w.testChatCount(), 3, "count after addChat");
        // The new chat lands at the top (chats sort newest-first).
        requireEqual(w.testChatIdAt(0), QString("new1"), "new chat at top");
        requireEqual(w.testChatIdAt(1), QString("old1"), "existing row 1 untouched");
        requireEqual(w.testChatIdAt(2), QString("old2"), "existing row 2 untouched");
    }

    // Each row's former Save button is now Edit: it emits editRequested(id)
    // (MainWindow opens the Edit Chat dialog) instead of exporting directly.
    {
        ChatHistoryWidget w;
        w.addChat("abc", "A chat");
        QString requested;
        QObject::connect(&w, &ChatHistoryWidget::editRequested,
                         [&requested](const QString& id) { requested = id; });
        QPushButton* edit = nullptr;
        bool hasSave = false;
        for (QPushButton* b : w.findChildren<QPushButton*>()) {
            if (b->property("pengyIcon").toString() == "edit") edit = b;
            if (b->property("pengyIcon").toString() == "save") hasSave = true;
        }
        requireTrue(edit != nullptr, "row has an Edit button");
        requireFalse(hasSave, "row no longer has a Save button");
        edit->click();
        requireEqual(requested, QString("abc"), "Edit emits editRequested with the chat id");
    }

    // removeChat() drops exactly the row for the given id, leaving the rest
    // alone -- the fix for the ghost-row regression: closeTab() and
    // loadIntoNewTab() both delete an abandoned empty "New Chat" from disk,
    // and must also call this or the sidebar keeps showing a row for a chat
    // that no longer exists.
    {
        ChatHistoryWidget w;
        w.addChat("c3", "Third");
        w.addChat("c2", "Second");
        w.addChat("c1", "First");
        requireEqual(w.testChatCount(), 3, "count before removeChat");

        w.removeChat("c2");
        requireEqual(w.testChatCount(), 2, "count after removeChat");
        requireEqual(w.testChatIdAt(0), QString("c1"), "remaining row 0");
        requireEqual(w.testChatIdAt(1), QString("c3"), "remaining row 1");

        // Removing an id that was never shown (or already removed) is a
        // silent no-op, not a crash.
        w.removeChat("never-existed");
        requireEqual(w.testChatCount(), 2, "no-op remove leaves count unchanged");
    }

    // Repeated close-and-recreate must not accumulate ghost rows -- the
    // literal reported regression, reproduced end to end.
    {
        ChatHistoryWidget w;
        for (int i = 0; i < 4; i++) {
            QString id = QString("chat-%1").arg(i);
            w.addChat(id, "New Chat");
            w.removeChat(id);
        }
        requireEqual(w.testChatCount(), 0, "repeated create+close leaves no ghost rows");
    }

    // setModels(): an empty QLabel still claims a line of layout height, so
    // the "no cached model list" hint must be hidden outright once a model
    // list exists, not just text-cleared -- otherwise it leaves a permanent
    // gap above "Tool Confirm:" in the quick-settings panel.
    {
        ChatHistoryWidget w;
        w.setModels({}, "");
        requireFalse(w.testModelHintHidden(), "hint shown when no cached list");
        requireTrue(w.testModelHintText().contains("Fetch"), "hint nudges toward Fetch");

        w.setModels({"gpt-4o", "gpt-4o-mini"}, "gpt-4o");
        requireEqual(w.testModelHintText(), QString(""), "hint text cleared once populated");
        requireTrue(w.testModelHintHidden(), "hint actually hidden, not just text-cleared");
    }

    // Tab refreshes must not emit changes or retain another tab's metrics.
    {
        ChatHistoryWidget w;
        auto* effort = w.findChild<QComboBox*>("effortCombo");
        auto* rate = w.findChild<QLabel*>("responseRateLabel");
        requireTrue(effort && rate, "effort and throughput controls exist");
        requireEqual(effort->findData("minimal"), -1, "legacy Minimal effort is not offered");
        int changes = 0;
        QString selected;
        QObject::connect(&w, &ChatHistoryWidget::effortChanged, [&changes, &selected](const QString& value) {
            ++changes;
            selected = value;
        });
        w.updateQuickSettings("model-a", "high");
        requireEqual(effort->currentData().toString(), QString("high"), "restores tab effort");
        requireEqual(changes, 0, "programmatic update does not commit effort");
        w.updateQuickSettings("model-b", "");
        requireEqual(effort->currentData().toString(), QString(""), "explicit provider default");
        w.updateQuickSettings("model-c", "global");
        requireEqual(effort->currentData().toString(), QString("global"), "global is distinct from provider default");
        effort->setCurrentIndex(effort->findData("max"));
        QMetaObject::invokeMethod(effort, "activated", Q_ARG(int, effort->currentIndex()));
        requireEqual(changes, 1, "user selection commits once");
        requireEqual(selected, QString("max"), "effort signal carries API value");
        w.updateResponseRate(42.25);
        requireTrue(rate->text().contains("42.3"), "rate displayed to one decimal");
        w.updateResponseRate(0);
        requireTrue(rate->text().contains("0.0"), "zero output is a valid measurement");
        w.updateResponseRate(-1);
        requireEqual(rate->text(), QString("Last: — tok/s"), "unmeasured tab clears rate");
        w.updateTokenUsage(100, 30);
        w.updateTokenUsage(0, 0);
        bool cleared = false;
        for (auto* label : w.findChildren<QLabel*>())
            if (label->text() == "Tokens: —") cleared = true;
        requireTrue(cleared, "empty tab clears cumulative usage");
    }

    // Search: typing filters titles, Enter shows message results from the
    // core (real chat files in a scratch PENGY_CONFIG_DIR), a click reports
    // the hit message indexes for jump-to-message, and stale events drop.
    {
        QTemporaryDir cfg;
        qputenv("PENGY_CONFIG_DIR", cfg.path().toUtf8());
        QDir(cfg.path()).mkpath("chats");
        auto writeChat = [&](const QString& id, const QString& title, int second, const QJsonArray& messages) {
            QFile f(cfg.path() + "/chats/" + id + ".json");
            f.open(QIODevice::WriteOnly);
            f.write(QJsonDocument(QJsonObject{
                {"id", id}, {"title", title}, {"messages", messages},
                {"created_at", QString("2026-01-01T00:00:%1").arg(second, 2, 10, QChar('0'))}}).toJson());
        };
        auto msg = [](const char* role, const char* text) { return QJsonObject{{"role", role}, {"content", text}}; };
        writeChat("s1", "Nginx notes", 1, {msg("user", "proxy_pass")});
        writeChat("s2", "Grocery list", 2, {msg("user", "hi"), msg("assistant", "milk and an nginx joke")});
        writeChat("s3", "Plan a trip", 3, {msg("user", "Banff")});

        ChatHistoryWidget w;
        QJsonArray seed;
        for (auto [id, title] : {std::pair{"s3", "Plan a trip"}, {"s2", "Grocery list"}, {"s1", "Nginx notes"}})
            seed.append(QJsonObject{{"id", id}, {"title", title}});
        w.loadChats(seed);
        auto visible = [&]() {
            QStringList ids;
            for (int i = 0; i < w.testChatList()->count(); ++i)
                if (!w.testChatList()->isRowHidden(i)) ids << w.testChatIdAt(i);
            return ids.join(",");
        };
        w.testSearchInput()->setText("NGINX");
        requireEqual(visible(), QString("s1"), "typing filters rows by title");
        w.testSearchInput()->setText("\"plan a\"");
        requireEqual(visible(), QString("s3"), "a quoted phrase is one word");

        w.testSearchInput()->setText("nginx");
        w.runSearch();
        requireEqual(w.testListPage(), 1, "Enter shows the results page");
        QElapsedTimer timer;
        timer.start();
        while (!w.testSearchStatus().contains("Esc") && timer.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        requireTrue(w.testSearchStatus().startsWith("2 chats found"), "search finishes with both chats");
        requireEqual(w.testResults()->item(0)->data(Qt::UserRole).toString(), QString("s1"), "title match first");

        QString openedId;
        QList<int> openedSources;
        QStringList openedTerms;
        QObject::connect(&w, &ChatHistoryWidget::searchHitSelected,
                         [&](const QString& id, const QList<int>& sources, const QStringList& terms) {
                             openedId = id; openedSources = sources; openedTerms = terms;
                         });
        w.testClickResult(1);
        requireEqual(openedId, QString("s2"), "click opens the chat");
        requireTrue(openedSources == QList<int>{1}, "click reports the hit message index");
        requireTrue(openedTerms == QStringList{"nginx"}, "click reports the search words");
        w.testClickResult(0);
        requireTrue(openedSources.isEmpty(), "a title-only match has nothing to jump to");

        const int rows = w.testResults()->count();
        w.testSearchEvent(w.testSearchGeneration() - 1,
                          QJsonObject{{"type", "result"}, {"id", "x"}, {"title", "Stale"}, {"hits", QJsonArray()}});
        requireEqual(w.testResults()->count(), rows, "stale search events are dropped");

        w.testSearchInput()->setText("nginx p");
        requireEqual(w.testListPage(), 0, "editing the query returns to the list");
        w.testSearchInput()->clear();
        requireEqual(visible(), QString("s3,s2,s1"), "clearing shows every chat again");
        qunsetenv("PENGY_CONFIG_DIR");
    }

    std::cout << "All chathistory tests passed." << std::endl;
    return 0;
}
