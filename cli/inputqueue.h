#pragma once

// The CLI's queue of messages typed while a turn runs.
//
// Plain data + logic so it can be tested without a terminal: the capture thread
// and the prompt wiring live in cli/main.cpp, this is only what they share.
// Nothing here is persisted -- a message that was never sent must not appear in
// chats.json as if it had been. Mirrors cli/src/typing.rs in the Rust edition.

#include <QString>
#include <QStringList>

class InputQueue {
public:
    /// Append. Blank input is ignored so a stray Enter cannot create an entry, and
    /// typing while a turn runs un-parks the queue ("send it when you are done").
    void enqueue(const QString& line) {
        if (line.trimmed().isEmpty()) return;
        m_held = false;
        m_items.append(line);
    }

    bool isEmpty() const { return m_items.isEmpty(); }
    int  len() const { return int(m_items.size()); }

    void setHeld(bool held) { m_held = held; }
    bool isHeld() const { return m_held; }

    /// Oldest first: what a finished turn sends next.
    QString takeOldest() { return m_items.isEmpty() ? QString() : m_items.takeFirst(); }
    /// Newest: what Up recalls for editing.
    QString takeNewest() { return m_items.isEmpty() ? QString() : m_items.takeLast(); }

    void clear() { m_items.clear(); m_held = false; }

    /// One single-line, elided summary per item, for /queue and the prompt label.
    QStringList previews(int maxChars = 72) const {
        QStringList out;
        for (const QString& text : m_items) {
            QString first = text.trimmed().section('\n', 0, 0).trimmed();
            if (maxChars > 1 && first.size() > maxChars)
                first = first.left(maxChars - 1) + QChar(0x2026);
            out.append(first);
        }
        return out;
    }

private:
    QStringList m_items;
    bool        m_held = false;
};
