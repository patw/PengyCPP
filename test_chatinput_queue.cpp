// Standalone regression test for the composer's queue strip and its keys.
//
// Covers the widget half of "type while Pengy works": the strip renders the queued
// previews, Enter on an empty composer asks the window to release the queue
// instead of being swallowed, and Up/Down on an empty composer ask for a recall
// while leaving ordinary cursor movement alone.
//
// Hand-rolled recorders instead of QSignalSpy: Qt6Test is not part of the dev
// guest's Qt install, and this test only needs the three signals below.
#include "chatinput.h"
#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QTextEdit>
#include <iostream>

static void check(bool ok, const char* label) {
    if (!ok) {
        std::cerr << "FAIL: " << label << std::endl;
        std::exit(1);
    }
}

static void press(QWidget* target, int key) {
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(target, &event);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    ChatInputWidget input;
    InputEdit* edit = input.editForTest();
    QLabel* strip = input.queueLabelForTest();
    check(edit && strip, "composer exposes its edit and strip");

    int sent = 0;
    QString lastSentText;
    QObject::connect(&input, &ChatInputWidget::messageSent,
                     [&](const QString& text, const QStringList&) { ++sent; lastSentText = text; });
    QList<int> recalls;
    QObject::connect(&input, &ChatInputWidget::queueRecallRequested,
                     [&](int direction) { recalls.append(direction); });
    int releases = 0;
    QObject::connect(&input, &ChatInputWidget::emptySubmit, [&]() { ++releases; });

    // ── Idle, nothing queued ──────────────────────────────────────
    check(input.composerEmpty(), "composer starts empty");
    check(strip->isHidden(), "strip hidden with nothing queued");
    input.setQueueState({}, false, false);
    check(edit->placeholderText().contains("Enter to send"),
          "idle placeholder says Enter sends");
    check(!edit->placeholderText().contains("queue"), "idle placeholder does not mention queuing");

    // ── A turn is running, still nothing queued ───────────────────
    input.setQueueState({}, false, true);
    check(strip->isHidden(), "strip stays hidden while the queue is empty");
    check(edit->placeholderText().contains("queue"),
          "running placeholder says Enter queues");

    // ── Two queued messages ───────────────────────────────────────
    input.setQueueState({"read the logs", "then summarise"}, false, true);
    check(!strip->isHidden(), "strip appears when something is queued");
    check(strip->text().contains("Queued (2)"), "strip counts the queue");
    check(strip->text().contains("1. read the logs"), "strip numbers from 1");
    check(strip->text().contains("2. then summarise"), "strip lists in order");
    check(strip->text().contains("sends when this turn ends"),
          "running strip says when it sends");

    // Held after Stop says so, and offers the way out.
    input.setQueueState({"parked"}, true, false);
    check(strip->text().contains("held after Stop"), "held strip says it is parked");
    check(strip->text().contains("Enter to send"), "held strip says how to release");

    // Previews are rich text: a message containing markup must render as text.
    input.setQueueState({QString::fromUtf8("<b>not bold</b>")}, false, false);
    check(strip->text().contains("&lt;b&gt;"), "preview markup is escaped, not rendered");
    check(!strip->text().contains("<b>not bold"), "raw markup never reaches the label");

    // ── Emptying the queue hides the strip again ──────────────────
    input.setQueueState({}, false, false);
    check(strip->isHidden(), "strip hides when the queue drains");

    // ── Enter ────────────────────────────────────────────────────
    input.submitForTest();
    check(releases == 1, "Enter on an empty composer asks to release the queue");
    check(sent == 0, "Enter on an empty composer sends nothing");

    input.setComposerText("hello");
    check(!input.composerEmpty(), "composer is non-empty after setComposerText");
    input.submitForTest();
    check(sent == 1, "Enter with text still sends");
    check(lastSentText == "hello", "the queued text is what gets sent");
    check(input.composerEmpty(), "composer clears after a send");

    // ── Up / Down ────────────────────────────────────────────────
    press(edit, Qt::Key_Up);
    check(recalls.size() == 1, "Up on an empty composer asks for the newest queued item");
    check(recalls.value(0) == -1, "Up recalls the newest");
    press(edit, Qt::Key_Down);
    check(recalls.size() == 2, "Down on an empty composer asks for the oldest");
    check(recalls.value(1) == 1, "Down recalls the oldest");

    // With text in the composer, Up/Down must stay ordinary cursor movement.
    input.setComposerText("draft");
    press(edit, Qt::Key_Up);
    press(edit, Qt::Key_Down);
    check(recalls.size() == 2, "Up/Down with a draft open do not touch the queue");

    std::cout << "chatinput queue: all checks passed" << std::endl;
    return 0;
}
