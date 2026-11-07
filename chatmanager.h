#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>
#include <QString>

// All data is represented as QJsonObject/QJsonArray to match the existing
// serialization format and minimize conversion boilerplate.

// Sidebar summaries only: id, title, created_at, msg_count, preview.
// Reads one small index file instead of every chat -- prefer this over
// chatsLoad() wherever message bodies aren't actually needed.
QJsonArray  chatsLoadIndex();
// Every chat in full, newest first. Reads every chat file.
QJsonArray  chatsLoad();
// No-op: there is no in-memory cache any more. Kept for API compatibility.
void        chatsInvalidateCache();
// Additive: writes and updates the given chats, never deletes others.
bool        chatsSave(const QJsonArray& chats);
QJsonObject chatCreate(const QString& title);
bool        chatDelete(const QString& id);
bool        chatSave(const QJsonObject& chat);
QJsonObject chatGet(const QString& id);
// A stored chat's file as raw bytes, for chat search's pre-check (which skips
// most files without a JSON parse); empty if missing.  Ids come from the index.
QByteArray  chatReadRaw(const QString& id);
// Where a chat's title came from, stored as chat["title_source"].  Absent is
// the automatic first-message title (and every chat from before the field).
// Bulk Rename only touches absent ones, which makes it resumable and leaves
// hand-picked titles alone.  Same values as the Python and Rust editions.
inline const QString kTitleSourceModel = QStringLiteral("model");
inline const QString kTitleSourceUser  = QStringLiteral("user");
// Change only the title (and, if non-empty, its source) of a stored chat;
// false if it no longer exists.  Re-reads the chat immediately before writing
// instead of saving a copy loaded earlier, so a long-running caller (Bulk
// Rename) cannot write back stale messages over newer ones.
bool        chatSetTitle(const QString& id, const QString& title, const QString& source = {});
QJsonArray  cleanDanglingToolCalls(const QJsonArray& messages);
QJsonArray  elideOldToolResults(const QJsonArray& messages, int keepTurns);
// Context-pruning "undo": pops exactly one raw message off the end,
// repairing any dangling tool_calls it leaves behind. Safe to call
// repeatedly down to an empty array.
QJsonArray  messagesRedactLast(const QJsonArray& messages);
// Accumulates one turn's token usage into chat["usage"] (persisted running
// total, not session-only state) and returns the updated chat. Caller must
// still persist it (chatSave/chatSaveProgress).
QJsonObject chatAddUsage(QJsonObject chat, const QJsonObject& usage);
