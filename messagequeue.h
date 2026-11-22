#pragma once
#include <QList>
#include <QString>
#include <QStringList>

/// One message typed while a turn was running.
///
/// `text` is the composer's payload exactly as it would have been sent (attached
/// text files are already embedded into it by ChatInputWidget::onSubmit);
/// `images` are the composer's remaining attachments, imported at send time.
struct QueuedMessage {
    QString     text;
    QStringList images;
};

/// Per-tab queue of messages typed while a turn is in flight.
///
/// Deliberately pure logic: no widgets, no chat state, no config. The window owns
/// one per tab, the composer renders previews(), and gui/tests drives it directly.
///
/// Nothing here is ever persisted. A message that was never sent must not appear
/// in chats.json as if it had been -- that is the whole reason this is a separate
/// type rather than entries in chat["messages"].
class MessageQueue {
public:
    /// Append. Empty (no text, no images) is ignored so a stray Enter cannot
    /// create a phantom queue entry.
    void enqueue(const QString& text, const QStringList& images = {}) {
        if (text.trimmed().isEmpty() && images.isEmpty()) return;
        m_items.append(QueuedMessage{text, images});
    }

    bool isEmpty() const { return m_items.isEmpty(); }
    int  size() const { return m_items.size(); }

    /// Oldest first.
    const QList<QueuedMessage>& items() const { return m_items; }

    /// Remove and return the oldest: what a finished turn sends next.
    QueuedMessage takeHead() { return m_items.isEmpty() ? QueuedMessage{} : m_items.takeFirst(); }

    /// Remove and return the newest: what Up recalls for editing.
    QueuedMessage takeNewest() { return m_items.isEmpty() ? QueuedMessage{} : m_items.takeLast(); }

    /// Remove and return the oldest, for editing with Down.
    QueuedMessage takeOldest() { return takeHead(); }

    /// Parked because the user pressed Stop: a held queue is not drained when a
    /// turn ends (there is no turn running any more), and the strip says so.
    bool held() const { return m_held; }
    void setHeld(bool held) { m_held = held; }

    /// One single-line, elided summary per item, for the composer's queue strip.
    QStringList previews(int maxChars = 70) const {
        QStringList out;
        for (const QueuedMessage& m : m_items) {
            QString s = m.text.trimmed().section('\n', 0, 0).simplified();
            if (s.isEmpty()) {
                s = m.images.size() == 1
                        ? QStringLiteral("[1 image]")
                        : QStringLiteral("[%1 images]").arg(m.images.size());
            }
            if (maxChars > 1 && s.size() > maxChars)
                s = s.left(maxChars - 1) + QChar(0x2026);  // ellipsis
            out.append(s);
        }
        return out;
    }

    void clear() {
        m_items.clear();
        m_held = false;
    }

private:
    QList<QueuedMessage> m_items;
    bool                 m_held = false;
};
