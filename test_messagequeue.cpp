// Standalone regression test for the composer's message queue (messagequeue.h).
//
// The queue is the state machine behind "type while Pengy works": Enter during a
// turn appends here, the finished turn drains the head, and Up/Down recall an
// entry for editing. It has no widgets, so it is tested directly.
#include "messagequeue.h"
#include <iostream>

static void check(bool ok, const char* label) {
    if (!ok) {
        std::cerr << "FAIL: " << label << std::endl;
        std::exit(1);
    }
}

int main() {
    // ── Empty queue ───────────────────────────────────────────────
    {
        MessageQueue q;
        check(q.isEmpty(), "new queue is empty");
        check(q.size() == 0, "new queue size 0");
        check(q.previews().isEmpty(), "no previews when empty");
        check(!q.held(), "not held by default");
        // Taking from an empty queue must be harmless, not a crash.
        check(q.takeHead().text.isEmpty(), "takeHead on empty returns empty");
        check(q.takeNewest().text.isEmpty(), "takeNewest on empty returns empty");
    }

    // ── Order and the three ways out ──────────────────────────────
    {
        MessageQueue q;
        q.enqueue("first");
        q.enqueue("second");
        q.enqueue("third");
        check(q.size() == 3, "three queued");
        check(q.items().first().text == "first", "front is oldest");

        check(q.takeHead().text == "first", "takeHead is FIFO");
        check(q.takeNewest().text == "third", "takeNewest is LIFO");
        check(q.takeOldest().text == "second", "takeOldest follows takeHead");
        check(q.isEmpty(), "drained");
    }

    // ── Empty payloads are not queue entries ──────────────────────
    {
        MessageQueue q;
        q.enqueue("", {});
        q.enqueue("   ", {});
        check(q.isEmpty(), "blank text with no images is ignored");
        q.enqueue("", QStringList{"/tmp/shot.png"});
        check(q.size() == 1, "an image-only message is a real message");
        check(q.items().first().text.isEmpty(), "image-only keeps its empty text");
        check(q.items().first().images.size() == 1, "image path survives");
    }

    // ── Held flag (parked by Stop) ────────────────────────────────
    {
        MessageQueue q;
        q.enqueue("parked");
        q.setHeld(true);
        check(q.held(), "held after Stop");
        q.setHeld(false);
        check(!q.held(), "released");
        q.clear();
        check(q.isEmpty() && !q.held(), "clear() empties and unholds");
    }

    // ── Preview text (what the strip shows) ───────────────────────
    {
        MessageQueue q;
        q.enqueue("read the logs\nand then summarise them");
        q.enqueue("no images here", {});
        q.enqueue("", QStringList{"/tmp/shot.png"});
        q.enqueue("", QStringList{"/tmp/a.png", "/tmp/b.png"});
        const QStringList p = q.previews();
        check(p.size() == 4, "one preview per item");
        check(p.at(0) == "read the logs", "preview is the first line only");
        check(!p.at(0).contains("summarise"), "later lines never leak into the strip");
        check(p.at(2) == "[1 image]", "single image placeholder");
        check(p.at(3) == "[2 images]", "plural image placeholder");

        // Elision is bounded and marked, not silently cut mid-word.
        MessageQueue longq;
        const QString longText(200, 'x');
        longq.enqueue(longText);
        const QString one = longq.previews(20).first();
        check(one.size() == 20, "elided preview honours maxChars");
        check(one.endsWith(QChar(0x2026)), "elision is visible");
        check(longq.previews(0).first().size() == 200, "maxChars 0 disables elision");
    }

    // ── A drained queue stops advertising previews ────────────────
    {
        MessageQueue q;
        q.enqueue("only");
        q.takeHead();
        check(q.previews().isEmpty(), "no previews once drained");
    }

    std::cout << "messagequeue: all checks passed" << std::endl;
    return 0;
}
