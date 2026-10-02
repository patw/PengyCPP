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

    std::cout << "All chathistory tests passed." << std::endl;
    return 0;
}
